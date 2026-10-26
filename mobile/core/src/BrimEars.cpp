#include "BrimEars.hpp"

#include <GLES3/gl3.h>

#include "Core.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

static const ColorRGBA DEF_COLOR      = {0.7f, 0.7f, 0.7f, 1.f};
static const ColorRGBA SELECTED_COLOR = {0.0f, 0.5f, 0.5f, 1.0f};
static const ColorRGBA ERR_COLOR      = {1.0f, 0.3f, 0.3f, 0.5f};
static constexpr float BRIM_EAR_RADIUS_MIN = 0.1f;
static constexpr float BRIM_EAR_RADIUS_MAX = 100.f;
static constexpr float RenderPointScale    = 1.f;

using CacheEntry = BrimEarsGizmo::CacheEntry;
#include "slic3r/GUI/BrimEars.inc"

BrimEarsGizmo &BrimEarsGizmo::get()
{
    static BrimEarsGizmo g;
    return g;
}

bool BrimEarsGizmo::active()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    return m_active;
}

Geometry::Transformation BrimEarsGizmo::instance_transformation() const
{
    return Plater::get().model().objects[m_obj]->instances[m_inst]->get_transformation();
}

// get_brim_default_radius: 8 first-layer line widths
float BrimEarsGizmo::get_brim_default_radius() const
{
    PresetBundle &b = *Core::get().preset_bundle;
    const double nozzle_diameter = b.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
    const DynamicPrintConfig &print_cfg = b.prints.get_edited_preset().config;
    return std::clamp(float(print_cfg.get_abs_value("initial_layer_line_width", nozzle_diameter) * 8.0), BRIM_EAR_RADIUS_MIN, BRIM_EAR_RADIUS_MAX);
}

void BrimEarsGizmo::update_model_object()
{
    ModelObject *mo = m_obj >= 0 && m_obj < int(Plater::get().model().objects.size()) ? Plater::get().model().objects[m_obj] : nullptr;
    if (mo) {
        mo->brim_points.clear();
        for (const CacheEntry &ce : m_editing_cache) mo->brim_points.emplace_back(ce.brim_point);
        Plater::get().dirty = true;
        // the slice uses the ears (EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS)
        for (auto &pl : Plater::get().plates) pl.slice_valid = false;
    }
    Viewport::get().request_redraw();
}

void BrimEarsGizmo::reload_cache()
{
    const ModelObject *mo = Plater::get().model().objects[m_obj];
    m_editing_cache.clear();
    for (const BrimPoint &point : mo->brim_points) m_editing_cache.emplace_back(point);
    find_single();
}

// first_layer_slicer: the model parts minus the negative parts at 0.1 mm
void BrimEarsGizmo::first_layer_slicer()
{
    const ModelObject *mo = Plater::get().model().objects[m_obj];
    std::vector<float>  slice_height(1, 0.1);
    MeshSlicingParamsEx params;
    params.mode           = MeshSlicingParams::SlicingMode::Regular;
    params.closing_radius = 0.1f;
    params.extra_offset   = 0.05f;
    params.resolution     = 0.01;
    ExPolygons part_ex;
    ExPolygons negative_ex;
    for (const ModelVolume *model_volume : mo->volumes) {
        if (model_volume->type() == ModelVolumeType::MODEL_PART || model_volume->type() == ModelVolumeType::NEGATIVE_VOLUME) {
            indexed_triangle_set volume_its = model_volume->mesh().its;
            if (volume_its.indices.size() <= 0) continue;
            Transform3d         trsf = mo->instances[m_inst]->get_matrix() * model_volume->get_matrix();
            MeshSlicingParamsEx params_ex(params);
            params_ex.trafo = params_ex.trafo * trsf;
            if (params_ex.trafo.rotation().determinant() < 0.) its_flip_triangles(volume_its);
            ExPolygons sliced_layer = slice_mesh_ex(volume_its, slice_height, params_ex).front();
            if (model_volume->type() == ModelVolumeType::MODEL_PART)
                part_ex = union_ex(part_ex, sliced_layer);
            else
                negative_ex = union_ex(negative_ex, sliced_layer);
        }
    }
    m_first_layer = diff_ex(part_ex, negative_ex);
    get_detection_radius_max();
}

json BrimEarsGizmo::open()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    Plater &p = Plater::get();
    const Model &m = p.model();
    // on_is_activable: a single full instance
    std::set<std::pair<int, int>> insts;
    bool ok = !p.selection.empty();
    for (const SelItem &s : p.selection) {
        if (s.vol >= 0 || s.obj < 0 || s.obj >= int(m.objects.size())) { ok = false; break; }
        if (s.inst < 0) for (int i = 0; i < int(m.objects[s.obj]->instances.size()); ++i) insts.insert({s.obj, i});
        else insts.insert({s.obj, s.inst});
    }
    m_active = ok && insts.size() == 1;
    if (m_active) {
        std::tie(m_obj, m_inst) = *insts.begin();
        if (m_new_point_head_radius == 0.f) m_new_point_head_radius = get_brim_default_radius();   // on_init
        m_selection_empty = true;
        first_layer_slicer();
        reload_cache();
    }
    Viewport::get().request_redraw();
    return state();
}

void BrimEarsGizmo::close()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return;
    m_active = false;
    if (m_obj < int(Plater::get().model().objects.size())) {
        Plater::get().take_snapshot("Brim ears edit");
        update_model_object();
        Plater::get().changed();
    }
    m_editing_cache.clear();
    m_single_brim.clear();
    Viewport::get().request_redraw();
}

json BrimEarsGizmo::state()
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return {{"active", false}};
    const ModelObject *mo = Plater::get().model().objects[m_obj];
    // undo / redo changed the model's points: GLGizmoBrimEars::on_load restores the cache
    std::vector<BrimPoint> cached;
    for (const CacheEntry &ce : m_editing_cache) cached.push_back(ce.brim_point);
    if (cached.size() != mo->brim_points.size() || !std::equal(cached.begin(), cached.end(), mo->brim_points.begin())) reload_cache();
    const DynamicPrintConfig &obj_cfg = mo->config.get();
    const DynamicPrintConfig &glb_cfg = Core::get().preset_bundle->prints.get_edited_preset().config;
    const bool brim_not_painted = obj_cfg.option("brim_type") ? obj_cfg.opt_enum<BrimType>("brim_type") != btPainted : glb_cfg.opt_enum<BrimType>("brim_type") != btPainted;
    bool any_selected = false;
    for (const CacheEntry &ce : m_editing_cache) any_selected |= ce.selected;
    const int n_invalid = int(m_single_brim.size());
    return {{"active", true}, {"radius", m_new_point_head_radius}, {"max_angle", m_max_angle}, {"detection_radius", m_detection_radius},
            {"detection_radius_max", m_detection_radius_max}, {"count", m_editing_cache.size()}, {"any_selected", any_selected},
            {"brim_not_painted", brim_not_painted}, {"invalid", n_invalid},
            {"labels", {{"title", _u8L("Brim Ears")}, {"brim_ear_radius", _u8L("Brim ear radius")}, {"max_angle", _u8L("Max angle")},
                        {"detection_radius", _u8L("Detection radius")}, {"remove", _u8L("Remove")}, {"remove_selected", _u8L("Selected")},
                        {"remove_all", _u8L("All")}, {"create", _u8L("Create")}, {"auto_generate", _u8L("Auto-generate")}, {"done", _u8L("Done")},
                        {"warning_painted", _u8L("Warning: The brim type is not set to \"painted\", the brim ears will not take effect!")},
                        {"set_painted", _u8L("Set the brim type of this object to \"painted\"")},
                        {"invalid", _u8L("Warning") + ": " + (boost::format(_L_PLURAL("%d invalid brim ear", "%d invalid brim ears", n_invalid)) % n_invalid).str()}}}};
}

json BrimEarsGizmo::set(const json &a)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return state();
    if (a.contains("radius")) {
        // begin_radius_change / update_cache_radius / apply_radius_change: one "Change brim ear radius" snapshot at the end
        const float r = std::clamp(a["radius"].get<float>(), BRIM_EAR_RADIUS_MIN, BRIM_EAR_RADIUS_MAX);
        if (a.value("final", false)) Plater::get().take_snapshot("Change brim ear radius");
        m_new_point_head_radius = r;
        for (auto &cache_entry : m_editing_cache)
            if (cache_entry.selected) cache_entry.brim_point.head_front_radius = m_new_point_head_radius;
        find_single();
        update_model_object();
    }
    if (a.contains("max_angle")) m_max_angle = std::clamp(a["max_angle"].get<float>(), 0.f, 180.f);
    if (a.contains("detection_radius")) m_detection_radius = std::clamp(a["detection_radius"].get<float>(), 0.f, float(m_detection_radius_max));
    if (a.value("painted", false)) {   // the "Set the brim type of this object to painted" link
        ModelObject *mo = Plater::get().model().objects[m_obj];
        DynamicPrintConfig new_conf = mo->config.get();
        new_conf.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btPainted));
        mo->config.assign_config(new_conf);
        Plater::get().changed();
    }
    return state();
}

void BrimEarsGizmo::delete_selected_points()
{
    Plater::get().take_snapshot("Delete brim ear");
    for (unsigned int idx = 0; idx < m_editing_cache.size(); ++idx)
        if (m_editing_cache[idx].selected) m_editing_cache.erase(m_editing_cache.begin() + (idx--));
    select_point(NoPoints);
    find_single();
    update_model_object();
}

json BrimEarsGizmo::action(const std::string &what)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return state();
    if (what == "auto_generate") {
        const Transform3d trsf = instance_transformation().get_matrix();
        const Vec3f normal = (instance_transformation().get_matrix_no_offset().inverse() * m_world_normal).cast<float>();
        auto add_point = [this, &trsf, &normal](const Point &p) {
            Vec3d world_pos  = {float(p.x() * SCALING_FACTOR), float(p.y() * SCALING_FACTOR), -0.0001};
            Vec3d object_pos = trsf.inverse() * world_pos;
            add_point_to_cache(object_pos.cast<float>(), m_new_point_head_radius, false, normal);
        };
        for (const ExPolygon &ex_poly : m_first_layer) {
            Polygon  out_poly   = ex_poly.contour;
            Polygons inner_poly = ex_poly.holes;
            polygons_reverse(inner_poly);
            Plater::get().take_snapshot("Auto generate brim ear");
            Points out_points = generate_points(out_poly, m_detection_radius, m_max_angle, true);
            for (Point &p : out_points) add_point(p);
            for (Polygon &pl : inner_poly) {
                Points inner_points = generate_points(pl, m_detection_radius, m_max_angle, false);
                for (Point &p : inner_points) add_point(p);
            }
        }
        find_single();
    } else if (what == "remove_selected") {
        delete_selected_points();
    } else if (what == "remove_all") {
        if (!m_editing_cache.empty()) {
            select_point(AllPoints);
            delete_selected_points();
        }
    } else if (what == "select_all" && !m_editing_cache.empty())
        select_point(AllPoints);
    Viewport::get().request_redraw();
    return state();
}

// unproject_on_mesh2: the closest hit over the model parts, in object (instance) coordinates
bool BrimEarsGizmo::unproject_on_mesh2(const Camera &cam, double px, double py, Vec3f &pos)
{
    const ModelObject *mo = Plater::get().model().objects[m_obj];
    const Transform3d inst = mo->instances[m_inst]->get_matrix();
    Vec3d o, d;
    cam.ray(px, py, o, d);
    double best = std::numeric_limits<double>::max();
    bool hit = false;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        const Transform3d world = inst * mv->get_matrix();
        const Transform3d inv = world.inverse();
        const Vec3d lo = inv * o, ld = inv.linear() * d;
        const indexed_triangle_set &its = mv->mesh().its;
        for (const Vec3i32 &f : its.indices) {
            const Vec3d p0 = its.vertices[f[0]].cast<double>(), e1 = its.vertices[f[1]].cast<double>() - p0, e2 = its.vertices[f[2]].cast<double>() - p0;
            const Vec3d pv = ld.cross(e2);
            const double det = e1.dot(pv);
            if (std::abs(det) < 1e-12) continue;
            const Vec3d tv = lo - p0;
            const double u = tv.dot(pv) / det;
            if (u < 0 || u > 1) continue;
            const Vec3d q = tv.cross(e1);
            const double v = ld.dot(q) / det;
            if (v < 0 || u + v > 1) continue;
            const double t = e2.dot(q) / det;
            if (t <= 0) continue;
            const Vec3d local = lo + t * ld;
            const double dist = (world * local - o).norm();
            if (dist < best) {
                best = dist;
                hit = true;
                pos = (mv->get_matrix() * local).cast<float>();   // position_on_model: volume transformation applied
            }
        }
    }
    return hit;
}

// the ear under the finger (the grabbers' raycasters): the disc's screen circle grown to a finger's size
int BrimEarsGizmo::pick_point(const Camera &cam, double px, double py)
{
    const Transform3d inst = instance_transformation().get_matrix();
    const Eigen::Matrix4d vp = cam.projection(1., 10000.) * cam.view();
    auto project = [&](const Vec3d &w) {
        Eigen::Vector4d c = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.);
        return Vec2d((c.x() / c.w() * 0.5 + 0.5) * cam.width, (0.5 - c.y() / c.w() * 0.5) * cam.height);
    };
    int best = -1;
    double best_d = std::numeric_limits<double>::max();
    for (int i = 0; i < int(m_editing_cache.size()); ++i) {
        const Vec3d w = inst * m_editing_cache[i].brim_point.pos.cast<double>();
        const Vec2d c = project(w);
        const double r = (project(w + Vec3d(m_editing_cache[i].brim_point.head_front_radius, 0, 0)) - c).norm();
        const double d = (c - Vec2d(px, py)).norm();
        if (d <= std::max(r, 40.) && d < best_d) { best_d = d; best = i; }
    }
    return best;
}

json BrimEarsGizmo::tap(const Camera &cam, double px, double py, bool remove)
{
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return state();
    const int hover = pick_point(cam, px, py);
    if (remove) {   // RightDown on a point: delete it
        if (hover >= 0) {
            select_point(NoPoints);
            select_point(hover);
            delete_selected_points();
        }
    } else if (hover >= 0) {   // a click on a point selects it (on_start_dragging)
        select_point(NoPoints);
        select_point(hover);
    } else if (m_selection_empty) {   // LeftDown on the mesh: a new ear on the bottom, facing up
        Vec3f pos;
        if (unproject_on_mesh2(cam, px, py, pos)) {
            const Transform3d trsf = instance_transformation().get_matrix();
            const Transform3d inverse_trsf = instance_transformation().get_matrix_no_offset().inverse();
            Vec3d world_pos  = trsf * pos.cast<double>();
            world_pos[2]     = -0.0001;
            Vec3d object_pos = trsf.inverse() * world_pos;
            Plater::get().take_snapshot("Add brim ear");
            add_point_to_cache(object_pos.cast<float>(), m_new_point_head_radius, false, (inverse_trsf * m_world_normal).cast<float>());
            find_single();
        }
    } else
        select_point(NoPoints);
    Viewport::get().request_redraw();
    return state();
}

bool BrimEarsGizmo::drag_begin(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);   // UI thread: the model is read / changed
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active) return false;
    m_drag = pick_point(cam, px, py);
    if (m_drag < 0) return false;
    select_point(NoPoints);
    select_point(m_drag);
    m_point_before_drag = m_editing_cache[m_drag];
    return true;
}

void BrimEarsGizmo::drag_move(const Camera &cam, double px, double py)
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);   // UI thread: the model is read / changed
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (m_drag < 0 || m_drag >= int(m_editing_cache.size())) return;
    Vec3f pos;
    if (!unproject_on_mesh2(cam, px, py, pos)) return;
    m_editing_cache[m_drag].brim_point.pos[0] = pos.x();
    m_editing_cache[m_drag].brim_point.pos[1] = pos.y();
    m_editing_cache[m_drag].normal = Vec3f(0, 0, 1);
    find_single();
    Viewport::get().request_redraw();
}

void BrimEarsGizmo::drag_end()
{
    std::lock_guard<std::recursive_mutex> core(Core::get().mutex);   // UI thread: the model is read / changed
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (m_drag < 0 || m_drag >= int(m_editing_cache.size())) { m_drag = -1; return; }
    CacheEntry backup = m_editing_cache[m_drag];
    if (backup.brim_point.pos != m_point_before_drag.brim_point.pos) {   // moved, not just selected
        m_editing_cache[m_drag] = m_point_before_drag;
        update_model_object();
        Plater::get().take_snapshot("Move support point");
        m_editing_cache[m_drag] = backup;
        update_model_object();
    }
    m_drag = -1;
}

// render_points: a flat cylinder (radius = ear radius, 0.2 high) per ear, in the state colours
void BrimEarsGizmo::render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    static std::shared_ptr<const TriangleMesh> cylinder = std::make_shared<const TriangleMesh>(its_make_cylinder(1.0, 1.0, 2. * PI / 16.));
    std::lock_guard<std::recursive_mutex> l(m_mutex);
    if (!m_active || m_obj >= int(Plater::get().model().objects.size())) return;
    const Geometry::Transformation it = instance_transformation();
    const Transform3d instance_scaling_matrix_inverse = it.get_scaling_factor_matrix().inverse();
    const Transform3d instance_matrix = it.get_matrix();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    for (size_t i = 0; i < m_editing_cache.size(); ++i) {
        const CacheEntry &ce = m_editing_cache[i];
        const ColorRGBA c = int(i) == m_drag ? ColorRGBA(0.f, 1.f, 1.f, 1.f) : ce.selected ? SELECTED_COLOR : ce.is_error ? ERR_COLOR : DEF_COLOR;
        Eigen::Quaterniond q;
        q.setFromTwoVectors(Vec3d{0., 0., 1.}, instance_scaling_matrix_inverse * ce.normal.cast<double>());
        const double radius = (double) ce.brim_point.head_front_radius * RenderPointScale;
        const Transform3d center_matrix = instance_matrix * Geometry::translation_transform(ce.brim_point.pos.cast<double>()) *
                                          instance_scaling_matrix_inverse * q * Geometry::scale_transform(Vec3d{radius, radius, .2});
        const float col[4] = {c.r(), c.g(), c.b(), c.a()};
        gl_draw_lit(cylinder, center_matrix.matrix().cast<float>(), col, view, proj);
    }
    glDisable(GL_BLEND);
}

static Commands reg({
    {"brim_ears.state", [](const json &) { return BrimEarsGizmo::get().state(); }},
    {"brim_ears.set", [](const json &a) { return BrimEarsGizmo::get().set(a); }},
    {"brim_ears.action", [](const json &a) { return BrimEarsGizmo::get().action(str_arg(a, "what")); }},
    {"brim_ears.tap", [](const json &a) {
        return BrimEarsGizmo::get().tap(Viewport::get().camera(), a.at("x").get<double>(), a.at("y").get<double>(), a.value("remove", false));
    }},
});

} // namespace orca
