// The SVG gizmo (GLGizmoSVG): a volume embossed from the paths of an SVG file — created from the part / object menus or
// by importing an .svg, edited in the panel (file: reload / change / forget path / bake / save as, depth, size with the
// ratio lock, use surface, distance from the surface, rotation, keep up, mirror, face the camera, operation) and moved
// over the object's surface by a drag. The shape loading and the warnings are compiled from upstream GLGizmoSVG.cpp.
#include <fstream>
#include <mutex>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "EmbossCommon.hpp"
#include "EmbossPlace.hpp"
#include "Plater.hpp"
#include "SvgGizmo.hpp"
#include "Viewport.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace orca::emboss;

namespace orca {

namespace {

using DataBasePtr = std::unique_ptr<DataBase>;
// the desktop's message boxes become the error of the panel
void show_error(void *, const std::string &message) { throw JobException(message); }
// the app picks the file (a document copied into the cache) and passes its path
std::string choose_svg_file() { return {}; }
const float warning_preccission = 1e-4f;
double get_tesselation_tolerance(double scale);
EmbossShape select_shape(std::string_view filepath = "", double tesselation_tolerance_in_mm = get_tesselation_tolerance(1.));

#include "slic3r/GUI/SvgGizmo.inc"

// GLGizmoSVG limits
const struct Limits
{
    MinMax<double> depth{0.01, 1e4};
    MinMax<float>  ui_size{5.f, 100.f};
    MinMax<float>  ui_size_in{.1f, 4.f};
    MinMax<double> relative_scale_ratio{1e-5, 1e4};
} limits;

constexpr double mm_to_in = 0.0393700787, in_to_mm = 25.4;

ModelVolumeType type_of(const std::string &t)
{
    return t == "negative" ? ModelVolumeType::NEGATIVE_VOLUME : t == "modifier" ? ModelVolumeType::PARAMETER_MODIFIER : ModelVolumeType::MODEL_PART;
}

class SvgGizmo
{
public:
    static SvgGizmo &get() { static SvgGizmo g; return g; }

    json open(const json &a);
    void close();
    json state();
    json set(const json &a);
    json file(const json &a);
    bool active() { std::lock_guard<std::recursive_mutex> l(m_mutex); return m_obj >= 0; }
    bool drag_begin(const Camera &cam, double px, double py);
    // m_rotate_gizmo: its frame (under the core lock) and the rotation dragged on it
    bool ring(Transform3d &frame, double &radius)
    {
        ModelVolume *mv = volume();
        if (mv == nullptr) return false;
        emboss::rotate_ring(*mv, m_inst, frame, radius);
        return true;
    }
    // on_stop_dragging: dragging_rotate_gizmo's angle, applied as do_local_z_rotate
    void ring_rotate(double delta)
    {
        std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
        double a = m_angle.value_or(0.f) + delta;
        Geometry::to_range_pi_pi(a);
        set({{"rotation", -a * 180. / M_PI}, {"final", true}});
    }
    void drag_move(const Camera &cam, double px, double py);
    void drag_end();

private:
    ModelVolume *volume();
    void set_volume(int obj, int inst, int vol);
    void calculate_scale();
    float scale_for_tolerance() const { return std::max(m_scale_width.value_or(1.f), m_scale_height.value_or(1.f)); }
    bool process(bool snapshot);
    void volume_transformation_changed();
    json result(json st);

    std::recursive_mutex m_mutex;
    int                  m_obj{-1}, m_inst{0};
    size_t               m_volume_id{0};
    unsigned             m_session{0};
    EmbossShape          m_volume_shape;   // a copy of the volume's shape, edited by the panel
    std::vector<std::string> m_shape_warnings;
    BoundingBox          m_shape_bb;
    std::optional<float> m_scale_width, m_scale_height, m_scale_depth;
    std::optional<float> m_angle, m_distance;
    bool                 m_keep_ratio{true}, m_keep_up{true};
    std::string          m_error;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    SurfaceDrag          m_drag;
};

ModelVolume *SvgGizmo::volume()
{
    const Model &m = Plater::get().model();
    if (m_obj < 0 || m_obj >= int(m.objects.size())) return nullptr;
    for (ModelVolume *v : m.objects[m_obj]->volumes)
        if (v->id().id == m_volume_id) return v;
    return nullptr;
}

// GLGizmoSVG::calculate_scale: the scale of the volume's world matrix along its axes
void SvgGizmo::calculate_scale()
{
    ModelVolume *mv = volume();
    if (mv == nullptr) return;
    Transform3d to_world = mv->get_object()->instances[m_inst]->get_matrix() * mv->get_matrix();
    if (mv->emboss_shape && mv->emboss_shape->fix_3mf_tr) to_world = to_world * mv->emboss_shape->fix_3mf_tr->inverse();
    const auto linear = to_world.linear();
    auto calc = [&linear](const Vec3d &axe, std::optional<float> &scale) {
        const double norm_sq = (linear * axe).squaredNorm();
        if (is_approx(norm_sq, 1.)) scale.reset();
        else scale = float(std::sqrt(norm_sq));
    };
    calc(Vec3d::UnitX(), m_scale_width);
    calc(Vec3d::UnitY(), m_scale_height);
    calc(Vec3d::UnitZ(), m_scale_depth);
}

// GLGizmoSVG::set_volume_by_selection
void SvgGizmo::set_volume(int obj, int inst, int vol)
{
    ModelVolume *v = Plater::get().model().objects[obj]->volumes[vol];
    if (m_cancel) m_cancel->store(true);
    m_cancel.reset();
    m_obj = obj;
    m_inst = inst;
    m_volume_id = v->id().id;
    ++m_session;
    m_error.clear();
    calculate_scale();
    EmbossShape &es = *v->emboss_shape;
    EmbossShape::SvgFile &svg_file = *es.svg_file;
    if (svg_file.image == nullptr && init_image(svg_file) == nullptr) { m_obj = -1; return; }
    if (es.shapes_with_ids.empty()) {
        NSVGLineParams params{get_tesselation_tolerance(scale_for_tolerance())};
        es.shapes_with_ids = create_shape_with_ids(*svg_file.image, params);
    }
    m_volume_shape = es;
    m_shape_warnings = create_shape_warnings(es, scale_for_tolerance());
    m_angle = calc_angle(*v, inst);
    m_distance = calc_distance(*v, inst);
    m_shape_bb = get_extents(m_volume_shape.shapes_with_ids);
}

// GLGizmoSVG::process
bool SvgGizmo::process(bool snapshot)
{
    ModelVolume *mv = volume();
    if (mv == nullptr || !mv->emboss_shape.has_value()) return false;
    if (m_cancel) m_cancel->store(true);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    EmbossShape shape = m_volume_shape;
    DataBase base(mv->name, m_cancel, std::move(shape));
    base.is_outside = mv->type() == ModelVolumeType::MODEL_PART;
    try {
        if (!emboss::update(*mv, m_inst, base, snapshot ? _u8L("Emboss attribute change") : std::string())) return false;
    } catch (const JobException &e) {
        m_error = e.what();
        return false;
    }
    m_volume_id = mv->id().id;
    m_error.clear();
    return true;
}

// GLGizmoSVG::volume_transformation_changed
void SvgGizmo::volume_transformation_changed()
{
    ModelVolume *mv = volume();
    if (mv == nullptr) return;
    if (!m_keep_up) m_angle = calc_angle(*mv, m_inst);
    if (mv->emboss_shape->projection.use_surface) process(false);
    else Plater::get().changed();
    calculate_scale();
}

// create_volume (the menus, a file import) or, with an SVG volume selected, edit it
json SvgGizmo::open(const json &a)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    Plater &p = Plater::get();
    const Model &m = p.model();
    m_error.clear();
    if (a.contains("path")) {
        const std::string t = a.value("type", std::string("part"));
        if (t == "invalid") p.selection.clear();
        const ModelVolumeType type = type_of(t);
        std::optional<Vec2d> screen;
        if (a.contains("x")) screen = Vec2d(a["x"].get<double>(), a["y"].get<double>());
        try {
            DataBasePtr base = create_emboss_data_base(m_cancel, type, a["path"].get<std::string>());
            if (!base) return result(p.state());
            const Created c = emboss::create(*base, type, screen, std::nullopt, std::nullopt, _u8L("Add Emboss text object"),
                                             _u8L("Add Emboss text Volume"));
            set_volume(c.obj, c.inst, c.vol);
        } catch (const JobException &e) {
            m_error = e.what();
            m_obj = -1;
            json st = p.state();
            st["error"] = m_error;
            return st;
        }
        return result(p.state());
    }
    // the selected single SVG volume
    for (const SelItem &s : p.selection) {
        if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
        const ModelObject *mo = m.objects[s.obj];
        int vol = s.vol;
        if (vol < 0 && mo->volumes.size() == 1) vol = 0;
        if (vol >= 0 && vol < int(mo->volumes.size()) && mo->volumes[vol]->is_svg()) { set_volume(s.obj, std::max(0, s.inst), vol); break; }
    }
    return result(p.state());
}

json SvgGizmo::result(json st)
{
    st["svg_active"] = m_obj >= 0;
    return st;
}

void SvgGizmo::close()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    m_obj = -1;
    m_drag.reset();
    m_volume_shape = {};
}

json SvgGizmo::state()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr || !mv->is_svg() || !m_volume_shape.svg_file) { m_obj = -1; return {{"active", false}}; }
    const bool use_inch = Core::get().app_config->get_bool("use_inches");
    const EmbossShape::SvgFile &svg = *m_volume_shape.svg_file;
    // draw_filename
    std::string name = !svg.path.empty() ? get_file_name(svg.path) : !svg.path_in_3mf.empty() ? get_file_name(svg.path_in_3mf) : std::string();
    if (name.empty()) name = _u8L("Unknown filename");
    json warnings = json::array();
    for (const std::string &w : m_shape_warnings)
        if (!w.empty()) warnings.push_back(w);
    // draw_preview: the shapes, in the unit square (y down) for the app to fill
    json preview = json::array();
    const ExPolygons shapes = union_ex(m_volume_shape.shapes_with_ids);
    const BoundingBox bb = get_extents(shapes);
    const double size = std::max(bb.size().x(), bb.size().y());
    if (size > 0) {
        const double tolerance = size / 400.;
        for (const ExPolygon &ex : shapes)
            for (const Slic3r::Polygon &poly : to_polygons(ex.simplify(tolerance))) {
                json pts = json::array();
                for (const Point &pt : poly.points) {
                    pts.push_back(std::round((pt.x() - bb.min.x()) / size * 1e4) / 1e4);
                    pts.push_back(std::round((bb.max.y() - pt.y()) / size * 1e4) / 1e4);
                }
                preview.push_back(pts);
            }
    }
    const Point sz = m_shape_bb.size();
    double width = sz.x() * m_volume_shape.scale * m_scale_width.value_or(1.f);
    double height = sz.y() * m_volume_shape.scale * m_scale_height.value_or(1.f);
    if (use_inch) { width *= mm_to_in; height *= mm_to_in; }
    double depth = m_volume_shape.projection.depth * m_scale_depth.value_or(1.f);
    if (use_inch) depth *= mm_to_in;
    size_t count_points = 0;
    for (const auto &s : m_volume_shape.shapes_with_ids) count_points += Slic3r::count_points(s.expoly);
    const bool only_part = mv->is_the_only_one_part();
    const EmbossProjection &projection = mv->emboss_shape->projection;
    return {
        {"active", true},
        {"session", m_session},
        {"name", name},
        {"path", svg.path},
        {"warnings", warnings},
        {"error", m_error},
        {"preview", preview},
        {"preview_ratio", bb.size().x() > 0 ? double(bb.size().y()) / bb.size().x() : 1.},
        {"can_reload", !svg.path.empty()},
        {"can_forget", !mv->emboss_shape->svg_file->path.empty()},
        {"units", use_inch ? "in" : "mm"},
        {"depth", depth},
        {"width", width},
        {"height", height},
        {"size_range", use_inch ? json{limits.ui_size_in.min, limits.ui_size_in.max} : json{limits.ui_size.min, limits.ui_size.max}},
        {"keep_ratio", m_keep_ratio},
        {"scaled", m_scale_width.has_value() || m_scale_height.has_value()},
        {"count_points", count_points},
        {"use_surface", m_volume_shape.projection.use_surface},
        {"can_use_surface", projection.use_surface || !only_part},
        {"distance", m_distance.value_or(0.f) * (use_inch ? mm_to_in : 1.)},
        {"distance_set", m_distance.has_value()},
        {"distance_range", {-2 * projection.depth * (use_inch ? mm_to_in : 1.), 2 * projection.depth * (use_inch ? mm_to_in : 1.)}},
        {"can_distance", !projection.use_surface && !only_part},
        {"rotation", -m_angle.value_or(0.f) * 180. / M_PI},
        {"rotation_set", m_angle.has_value()},
        {"keep_up", m_keep_up},
        {"show_keep_up", !only_part},
        {"type", mv->type() == ModelVolumeType::NEGATIVE_VOLUME ? "negative" : mv->type() == ModelVolumeType::PARAMETER_MODIFIER ? "modifier" : "part"},
        {"show_type", !only_part},
        {"labels", {{"depth", _u8L("Depth")}, {"size", _u8L("Size")}, {"use_surface", _u8L("Use surface")}, {"distance", _u8L("From surface")},
                    {"rotation", _u8L("Rotation")}, {"mirror", _u8L("Mirror")}, {"face_camera", _u8L("Face the camera")}, {"operation", _u8L("Operation")},
                    {"join", _u8L("Join")}, {"cut", _u8L("Cut")}, {"modifier", _u8L("Modifier")}, {"reload", _u8L("Reload SVG file from disk.")},
                    {"change_file", _u8L("Change file") + "..."}, {"forget", _u8L("Forget the file path")}, {"bake", _u8L("Bake")},
                    {"bake_tip", _u8L("Bake into model as uneditable part")}, {"save_as", _u8L("Save as") + "..."},
                    {"forget_tip", _u8L("Do NOT save local path to 3MF file.\nAlso disables 'reload from disk' option.")},
                    {"lock_ratio", _u8L("Lock/unlock the aspect ratio of the SVG.")}, {"reset_scale", _u8L("Reset scale")},
                    {"reset_distance", _u8L("Reset distance")}, {"reset_rotation", _u8L("Reset rotation")},
                    {"keep_up", _u8L("Lock/unlock rotation angle when dragging above the surface.")},
                    {"mirror_x", _u8L("Mirror vertically")}, {"mirror_y", _u8L("Mirror horizontally")},
                    {"points", GUI::format(_L("Scale also changes amount of curve samples (%1%)"), count_points)},
                    {"path", GUI::format(_L("SVG file path is \"%1%\""), svg.path)}, {"done", _u8L("Done")}}}};
}

// draw_depth / draw_size / draw_use_surface / draw_distance / draw_rotation / draw_mirroring / draw_face_the_camera /
// draw_model_type
json SvgGizmo::set(const json &a)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr) return state();
    Plater &p = Plater::get();
    const bool use_inch = Core::get().app_config->get_bool("use_inches");
    const bool final_ = a.value("final", true);
    if (a.contains("depth")) {
        double input = a["depth"].get<double>();
        if (use_inch) input *= in_to_mm / m_scale_depth.value_or(1.f);
        else if (m_scale_depth) input /= *m_scale_depth;
        input = std::clamp(input, limits.depth.min, limits.depth.max);
        if (!is_approx(input, m_volume_shape.projection.depth, 1e-4)) { m_volume_shape.projection.depth = input; process(true); }
    }
    if (a.contains("keep_ratio")) m_keep_ratio = a["keep_ratio"];
    std::optional<Vec3d> new_relative_scale;
    {
        const Point sz = m_shape_bb.size();
        double width = sz.x() * m_volume_shape.scale * m_scale_width.value_or(1.f);
        double height = sz.y() * m_volume_shape.scale * m_scale_height.value_or(1.f);
        if (use_inch) { width *= mm_to_in; height *= mm_to_in; }
        auto is_valid_scale_ratio = [](double ratio) {
            return !(std::fabs(ratio - 1.) < limits.relative_scale_ratio.min || ratio > limits.relative_scale_ratio.max || ratio < 1e-4);
        };
        if (a.contains("width") && width > 0) {
            const double ratio = a["width"].get<double>() / width;
            if (is_valid_scale_ratio(ratio)) {
                m_scale_width = m_scale_width.value_or(1.f) * ratio;
                if (m_keep_ratio) m_scale_height = m_scale_height.value_or(1.f) * ratio;
                new_relative_scale = Vec3d(ratio, m_keep_ratio ? ratio : 1., 1.);
            }
        } else if (a.contains("height") && height > 0) {
            const double ratio = a["height"].get<double>() / height;
            if (is_valid_scale_ratio(ratio)) {
                m_scale_height = m_scale_height.value_or(1.f) * ratio;
                new_relative_scale = Vec3d(1., ratio, 1.);
            }
        }
        if (a.contains("reset_scale")) new_relative_scale = Vec3d(1. / m_scale_width.value_or(1.f), 1. / m_scale_height.value_or(1.f), 1.);
    }
    if (new_relative_scale) {   // selection.scale in the volume's (or the embossed object's instance) local coordinates
        if (final_) p.take_snapshot(_u8L("Set Scale"));
        const Transform3d s = Geometry::scale_transform(*new_relative_scale);
        if (mv->is_the_only_one_part()) {
            ModelInstance *mi = mv->get_object()->instances[m_inst];
            mi->set_transformation(Geometry::Transformation(mi->get_matrix() * mv->get_matrix() * s * mv->get_matrix().inverse()));
        } else
            mv->set_transformation(mv->get_matrix() * s);
        mv->get_object()->invalidate_bounding_box();
        if (mv->type() == ModelVolumeType::MODEL_PART) mv->get_object()->ensure_on_bed();
        calculate_scale();
        if (const NSVGimage *img = m_volume_shape.svg_file->image.get()) {
            NSVGLineParams params{get_tesselation_tolerance(scale_for_tolerance())};
            m_volume_shape.shapes_with_ids = create_shape_with_ids(*img, params);
            m_volume_shape.final_shape = {};
            process(false);
        } else
            p.changed();
    }
    if (a.contains("use_surface")) { m_volume_shape.projection.use_surface = a["use_surface"]; process(true); }
    if (a.contains("distance") || a.contains("reset_distance")) {
        const float prev = m_distance.value_or(0.f);
        if (a.contains("reset_distance")) m_distance.reset();
        else {
            m_distance = float(a["distance"].get<double>() * (use_inch ? in_to_mm : 1.));
            if (std::abs(*m_distance) < 1e-6f) m_distance.reset();
        }
        if (final_) p.take_snapshot(_u8L("SVG move"));
        local_z_move(*mv, m_distance.value_or(0.f) - prev);
        p.changed();
    }
    if (a.contains("rotation") || a.contains("reset_rotation")) {
        const float angle = m_angle.value_or(0.f);
        double angle_rad = a.contains("reset_rotation") ? 0. : -a["rotation"].get<double>() * M_PI / 180.0;
        Geometry::to_range_pi_pi(angle_rad);
        if (final_) p.take_snapshot(_u8L("SVG rotate"));
        local_z_rotate(*mv, m_inst, angle_rad - angle);
        m_angle = calc_angle(*mv, m_inst);
        if (mv->emboss_shape->projection.use_surface) process(false);
        else p.changed();
    }
    if (a.contains("keep_up")) m_keep_up = a["keep_up"];
    if (a.contains("mirror")) {   // draw_mirroring: selection.mirror in local coordinates
        const int axis = a["mirror"].get<std::string>() == "x" ? 0 : 1;
        p.take_snapshot(_u8L("Set Mirror"));
        Vec3d m3 = Vec3d::Ones();
        m3[axis] = -1.;
        const Transform3d s = Geometry::scale_transform(m3);
        if (mv->is_the_only_one_part()) {
            ModelInstance *mi = mv->get_object()->instances[m_inst];
            mi->set_transformation(Geometry::Transformation(mi->get_matrix() * mv->get_matrix() * s * mv->get_matrix().inverse()));
        } else
            mv->set_transformation(mv->get_matrix() * s);
        if (m_keep_up) m_angle = calc_angle(*mv, m_inst);
        volume_transformation_changed();
    }
    if (a.contains("face_camera")) {
        p.take_snapshot(_u8L("Face the camera"));
        face_camera(*mv, m_inst, Viewport::get().camera(), m_keep_up);
        volume_transformation_changed();
    }
    if (a.contains("type")) {
        if (change_type(*mv, m_inst, type_of(a["type"]), _u8L("Change SVG Type"))) process(false);
    }
    Viewport::get().request_redraw();
    return state();
}

// draw_filename's buttons and menu
json SvgGizmo::file(const json &a)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr) return state();
    const std::string what = a.value("what", std::string());
    bool file_changed = false;
    if (what == "reload") {
        if (!boost::filesystem::exists(m_volume_shape.svg_file->path)) m_volume_shape.svg_file->path.clear();
        else file_changed = true;
    } else if (what == "change") {
        EmbossShape::SvgFile svg_file_new;
        svg_file_new.path = a.value("path", std::string());
        if (!svg_file_new.path.empty()) { m_volume_shape.svg_file = svg_file_new; file_changed = true; }
    } else if (what == "forget") {
        mv->emboss_shape->svg_file->path.clear();
        m_volume_shape.svg_file->path.clear();
    } else if (what == "bake") {
        Plater::get().take_snapshot(_u8L("Bake"));
        mv->emboss_shape.reset();
        Plater::get().changed();
        m_obj = -1;
        json st = state();
        st["close"] = true;
        return st;
    } else if (what == "data") {   // Save as: the app writes the file
        const EmbossShape::SvgFile &svg = *m_volume_shape.svg_file;
        return {{"data", svg.file_data ? *svg.file_data : std::string()},
                {"name", get_file_name(!svg.path.empty() ? svg.path : !svg.path_in_3mf.empty() ? svg.path_in_3mf : std::string("/shape.svg")) + ".svg"}};
    } else if (what == "saved") {   // the file it was saved to becomes the source
        m_volume_shape.svg_file->path = a.value("path", std::string());
        m_volume_shape.svg_file->path_in_3mf.clear();
        mv->emboss_shape->svg_file = m_volume_shape.svg_file;
    }
    if (file_changed) {
        try {
            const float scale = scale_for_tolerance();
            EmbossShape es_ = select_shape(m_volume_shape.svg_file->path, get_tesselation_tolerance(scale));
            m_volume_shape.svg_file = std::move(es_.svg_file);
            m_volume_shape.shapes_with_ids = std::move(es_.shapes_with_ids);
            m_volume_shape.final_shape = {};
            m_shape_warnings = create_shape_warnings(m_volume_shape, scale);
            m_shape_bb = get_extents(m_volume_shape.shapes_with_ids);
            process(true);
        } catch (const JobException &e) {
            m_error = e.what();
        }
    }
    return state();
}

bool SvgGizmo::drag_begin(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr || !m_drag.begin(*mv, m_inst, cam, px, py, m_keep_up)) return false;
    if (m_cancel) m_cancel->store(true);
    return true;
}

void SvgGizmo::drag_move(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr) return;
    m_drag.move(*mv, m_inst, cam, px, py, m_keep_up);
    calculate_scale();
    if (!m_keep_up) m_angle = calc_angle(*mv, m_inst);
}

void SvgGizmo::drag_end()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    ModelVolume *mv = volume();
    if (mv == nullptr) { m_drag.reset(); return; }
    m_drag.end(*mv);
    if (mv->emboss_shape->projection.use_surface) process(false);
    else Plater::get().changed();
    m_distance = calc_distance(*mv, m_inst);
    calculate_scale();
}

} // namespace

bool svg_gizmo_ring(Transform3d &frame, double &radius) { return SvgGizmo::get().active() && SvgGizmo::get().ring(frame, radius); }
void svg_gizmo_ring_rotate(double delta) { SvgGizmo::get().ring_rotate(delta); }
bool svg_gizmo_active() { return SvgGizmo::get().active(); }
void svg_gizmo_close() { SvgGizmo::get().close(); }
bool svg_gizmo_drag_begin(const Camera &cam, double px, double py) { return SvgGizmo::get().drag_begin(cam, px, py); }
void svg_gizmo_drag_move(const Camera &cam, double px, double py) { SvgGizmo::get().drag_move(cam, px, py); }
void svg_gizmo_drag_end() { SvgGizmo::get().drag_end(); }

static Commands reg({
    {"svg.open", [](const json &a) { return SvgGizmo::get().open(a); }},
    {"svg.state", [](const json &) { return SvgGizmo::get().state(); }},
    {"svg.set", [](const json &a) { return SvgGizmo::get().set(a); }},
    {"svg.file", [](const json &a) { return SvgGizmo::get().file(a); }},
});

} // namespace orca
