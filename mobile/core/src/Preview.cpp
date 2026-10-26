#include "Preview.hpp"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <cstring>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Realistic.hpp"
#include "Slicer.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/nowide/cstdio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <fstream>

using namespace Slic3r;
using json = nlohmann::json;

namespace orca {

Preview &Preview::get()
{
    static Preview p;
    return p;
}

void Preview::gl_created()
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_viewer = std::make_unique<libvgcode::Viewer>();
    try {
        m_viewer->init(reinterpret_cast<const char *>(glGetString(GL_VERSION)));
    } catch (const std::exception &ex) {
        __android_log_print(ANDROID_LOG_ERROR, "orcacore", "libvgcode init: %s", ex.what());
    }
    m_loaded_id = ~0u;
    m_loaded_plate = -1;
    m_has_data = false;
    m_pending = true;
}

// GCodeViewer::load: tool colors from the filaments, color print colors, then the converted result
void Preview::load()
{
    int plate = m_plate;
    PlateSlice *ps = Slicer::get().slice(plate);
    std::lock_guard<std::mutex> rl(Slicer::get().result_mutex);
    if (!ps || !ps->valid) {
        const bool had = m_has_data;
        if (m_has_data) m_viewer->reset();
        m_has_data = false;
        m_loaded_plate = plate;
        if (had) Host::event("preview", info());   // the legend goes with the toolpaths (GCodeViewer::reset)
        return;
    }
    if (m_loaded_plate == plate && m_loaded_id == ps->result_id && m_has_data) return;
    std::vector<std::string> tool_colors;
    {
        std::unique_lock<std::recursive_mutex> lock(Core::get().mutex);
        if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour")) tool_colors = fc->values;
        // GCodeViewer::load: what the legend needs besides the viewer
        LegendData &d = m_legend;
        d.custom_gcodes.clear();
        auto &pcg = Plater::get().model().plates_custom_gcodes;
        if (auto it = pcg.find(plate); it != pcg.end()) d.custom_gcodes = it->second.gcodes;   // get_curr_plate_custom_gcodes
        d.nozzles = int(Core::get().full_config().option<ConfigOptionFloats>("nozzle_diameter")->size());
    }
    {
        LegendData &d = m_legend;
        const GCodeProcessorResult &r = ps->result;
        d.stats = r.print_statistics;
        d.diameters = r.filament_diameters;
        d.densities = r.filament_densities;
        d.extruders_count = r.filaments_count;
        d.filename = ps->gcode_path.empty() ? r.filename : ps->gcode_path;
        d.lines_ends = r.lines_ends;
        d.counts.fill(0);
        for (auto &t : d.times) t.fill(0.f);
        d.distances.fill(0.f);
        for (const GCodeProcessorResult::MoveVertex &move : r.moves) {
            if (move.internal_only) continue;
            const size_t move_type = size_t(move.type);
            if (move_type < d.counts.size()) {
                ++d.counts[move_type];
                for (size_t mode = 0; mode < move.time.size() && mode < d.times[move_type].size(); ++mode) d.times[move_type][mode] += move.time[mode];
                if (move.type == EMoveType::Retract || move.type == EMoveType::Unretract) d.distances[move_type] += std::fabs(move.delta_extruder);
                else d.distances[move_type] += move.travel_dist;
            }
        }
        if (ps->print) {
            const PrintStatistics &st = ps->print->print_statistics();
            d.total_weight = st.total_weight;
            d.total_used_filament = st.total_used_filament;
            d.total_cost = st.total_cost;
        }
    }
    if (tool_colors.empty()) tool_colors.push_back("#FFFFFF");
    libvgcode::GCodeInputData data = libvgcode::convert(ps->result, tool_colors, {}, *m_viewer);
    m_viewer->reset_default_extrusion_roles_colors();
    m_viewer->load(std::move(data));
    m_show_marker = false;
    m_z_offset = ps->result.z_offset;
    m_loaded_id = ps->result_id;
    m_loaded_plate = plate;
    m_has_data = true;
    // all layers visible, like a fresh preview
    m_viewer->set_layers_view_range(0, std::max<size_t>(1, m_viewer->get_layers_count()) - 1);
    Host::event("preview", info());
}

void Preview::update()
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_viewer) return;
    if (m_pending.exchange(false)) {
        try { load(); } catch (const std::exception &ex) { __android_log_print(ANDROID_LOG_ERROR, "orcacore", "preview load: %s", ex.what()); }
    }
    if (m_pending_set.is_array() && !m_pending_set.empty()) {
        json list = std::move(m_pending_set);
        m_pending_set = json::array();
        for (auto &a : list) apply(a);
        Host::event("preview", info());
    }
}

// GLCanvas3D::_render_shadows' toolpath casters: the paths' box cut at the top shown layer, and
// GCodeViewer::shadow_casters_signature
bool Preview::shadow_casters(BoundingBoxf3 &bb, size_t &signature)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_viewer || !m_has_data) return false;
    // GCodeViewer::load's m_paths_bounding_box (the extrusions of the editor)
    using R = libvgcode::EGCodeExtrusionRole;
    const libvgcode::AABox box = m_viewer->get_extrusion_bounding_box({
        R::Perimeter, R::ExternalPerimeter, R::OverhangPerimeter, R::InternalInfill, R::SolidInfill, R::TopSolidInfill, R::Ironing,
        R::BridgeInfill, R::GapFill, R::Skirt, R::SupportMaterial, R::SupportMaterialInterface, R::WipeTower, R::BottomSurface,
        R::InternalBridgeInfill, R::Brim, R::SupportTransition, R::Mixed});
    const Vec3d mn(box[0][0], box[0][1], box[0][2]), mx(box[1][0], box[1][1], box[1][2]);
    bb = BoundingBoxf3();
    // Merged corner by corner: a single layer print has zero Z extent
    if ((mn.array() <= mx.array()).all()) { bb.merge(mn); bb.merge(mx); }
    const std::vector<float> zs = m_viewer->get_layers_zs();
    if (!zs.empty()) {
        const size_t top = std::min<size_t>(m_viewer->get_layers_view_range()[1], zs.size() - 1);
        bb.max.z() = std::max(bb.min.z(), std::min(bb.max.z(), double(zs[top])));
    }
    size_t hash = m_viewer->get_vertices_count();
    auto combine = [&hash](size_t v) { hash ^= v + 0x9e3779b9 + (hash << 6) + (hash >> 2); };
    const libvgcode::Interval &visible = m_viewer->get_view_visible_range();
    const libvgcode::Interval &layers  = m_viewer->get_layers_view_range();
    for (size_t v : {size_t(visible[0]), size_t(visible[1]), size_t(layers[0]), size_t(layers[1])}) combine(v);
    for (double v : {mn.x(), mn.y(), mn.z(), mx.x(), mx.y(), mx.z()}) combine(std::hash<double>()(v));
    combine(m_viewer->is_top_layer_only_view_range());
    for (size_t i = 0; i < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++i) combine(m_viewer->is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole(i)));
    combine(m_viewer->is_option_visible(libvgcode::EOptionType::Travels));
    combine(m_viewer->is_option_visible(libvgcode::EOptionType::Wipes));
    signature = hash;
    return bb.defined;
}

bool Preview::tool_marker(Vec3f &pos, float &z_offset, bool &color_by_filament)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_viewer || !m_has_data || !m_show_marker) return false;
    const auto &p = m_viewer->get_current_vertex().position;
    pos = Vec3f(p[0], p[1], p[2]);
    z_offset = m_z_offset;
    color_by_filament = m_viewer->get_view_type() == libvgcode::EViewType::ColorPrint;
    return true;
}

void Preview::set_clipping_plane(const std::array<float, 4> &plane)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_viewer) m_viewer->set_clipping_plane(plane);
}

void Preview::render_shadow_casters(const Eigen::Matrix4f &light_view, const Eigen::Matrix4f &light_proj, const Vec3d &eye)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_viewer || !m_has_data) return;
    libvgcode::Mat4x4 v, p;
    std::memcpy(v.data(), light_view.data(), 64);
    std::memcpy(p.data(), light_proj.data(), 64);
    m_viewer->render_shadow_casters(v, p, {float(eye.x()), float(eye.y()), float(eye.z())});
}

bool Preview::render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    update();
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_viewer || !m_has_data) return false;
    // GLCanvas3D::_render_gcode: the toolpaths receive the shadow map they were drawn into, and the tone
    Realistic &rv = Realistic::get();
    libvgcode::Mat4x4 lvp;
    std::memcpy(lvp.data(), rv.light_vp().data(), 64);
    const bool receive = rv.shadow_valid() && rv.texture() != 0;
    if (receive) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, rv.texture());
        glActiveTexture(GL_TEXTURE0);
        m_viewer->set_shadow_map(4, lvp, 0.35f, rv.texel());
    } else {
        libvgcode::Mat4x4 id;
        const Eigen::Matrix4f e = Eigen::Matrix4f::Identity();
        std::memcpy(id.data(), e.data(), 64);
        m_viewer->set_shadow_map(4, id, 0.f, 0.f);
    }
    // the lighting term leaves the print duller than the legend: saturation always, brightness with a lossy pass
    const bool lossy = rv.enabled(true) && (rv.shadow_mode(true) != 0 || rv.ssao(true));
    m_viewer->set_tone(lossy ? 1.1f : 1.0f, 1.15f);
    Vec3f light(-0.4574957f, 0.4574957f, 0.7624929f);
    rv.static_light_eye(true, view, light);
    m_viewer->set_light_top_dir({light.x(), light.y(), light.z()});
    libvgcode::Mat4x4 v, p;
    std::memcpy(v.data(), view.data(), 64);
    std::memcpy(p.data(), proj.data(), 64);
    m_viewer->render(v, p);
    m_show_marker = m_show_marker || m_viewer->get_view_visible_range()[1] != m_viewer->get_view_full_range()[1];
    if (receive) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    return false;
}

static json position_window(const libvgcode::Viewer &viewer);

json Preview::info()
{
    if (!m_viewer || !m_has_data) return {{"loaded", false}};
    const auto &lr = m_viewer->get_layers_view_range();
    const auto &vr = m_viewer->get_view_visible_range();
    const auto &fr = m_viewer->get_view_full_range();
    json zs = json::array();
    for (float z : m_viewer->get_layers_zs()) zs.push_back(z);
    json roles = json::array();
    for (auto r : m_viewer->get_extrusion_roles()) {
        const auto &c = m_viewer->get_extrusion_role_color(r);
        roles.push_back({{"role", int(r)}, {"visible", m_viewer->is_extrusion_role_visible(r)}, {"color", {c[0], c[1], c[2]}},
                         {"time", m_viewer->get_extrusion_role_estimated_time(r)}});
    }
    json options = json::array();
    for (auto o : m_viewer->get_options()) options.push_back({{"option", int(o)}, {"visible", m_viewer->is_option_visible(o)}});
    json range = nullptr;
    auto vt = m_viewer->get_view_type();
    if (vt != libvgcode::EViewType::FeatureType && vt != libvgcode::EViewType::ColorPrint && vt != libvgcode::EViewType::Tool && vt != libvgcode::EViewType::Summary) {
        const auto &cr = m_viewer->get_color_range(vt);
        json values = json::array(), colors = json::array();
        for (float x : cr.get_values()) { values.push_back(x); auto c = cr.get_color_at(x); colors.push_back({c[0], c[1], c[2]}); }
        range = {{"min", cr.get_range()[0]}, {"max", cr.get_range()[1]}, {"values", values}, {"colors", colors}};
    }
    json tools = json::array();
    for (auto &c : m_viewer->get_tool_colors()) tools.push_back({c[0], c[1], c[2]});
    json used = json::array();
    for (auto e : m_viewer->get_used_extruders_ids()) used.push_back(e);
    return {{"loaded", true},
            {"plate", m_loaded_plate},
            {"layers", zs},
            {"layer_range", {lr[0], lr[1]}},
            {"view_range", {vr[0], vr[1]}},
            {"full_range", {fr[0], fr[1]}},
            {"view_type", int(vt)},
            {"roles", roles},
            {"options", options},
            {"range", range},
            {"tool_colors", tools},
            {"used_extruders", used},
            {"time", m_viewer->get_estimated_time()},
            {"travel_time", m_viewer->get_travels_estimated_time()},
            {"layer_times", m_viewer->get_layers_estimated_times()},
            {"top_layer_only", m_viewer->is_top_layer_only_view_range()},
            {"current_line", m_viewer->get_current_vertex().gcode_id},
            // m_show_marker: once the moves slider leaves the end
            {"position", vr[1] != fr[1] ? position_window(*m_viewer) : json(nullptr)},
            {"legend", legend()}};
}


// ---------------------------------------------------------------------------------------------------------------------
// GCodeViewer::render_legend as data: the view type list, then sections the app lays out like the ImGui window
// (headers, coloured rows with an eye toggle, titled ranges, label: value pairs)

static const double in_to_mm = 25.4, oz_to_g = 28.34952;   // GizmoObjectManipulation

static std::string format_compact_weight(double value_in_grams, bool imperial_units)
{
    char buffer[64];
    if (imperial_units) {
        ::sprintf(buffer, "%.2f oz", value_in_grams / oz_to_g);
        return buffer;
    }
    const double abs_value = value_in_grams < 0.0 ? -value_in_grams : value_in_grams;
    const char *unit = "g";
    double scaled_value = abs_value;
    if (scaled_value >= 1000000.0) { scaled_value /= 1000000.0; unit = "t"; }
    else if (scaled_value >= 1000.0) { scaled_value /= 1000.0; unit = "kg"; }
    ::sprintf(buffer, "%s%.2f%s", value_in_grams < 0.0 ? "-" : "", scaled_value, unit);
    return buffer;
}

static std::string view_type_string(libvgcode::EViewType view_type)
{
    using V = libvgcode::EViewType;
    switch (view_type) {
    case V::Summary: return _u8L("Summary");
    case V::FeatureType: return _u8L("Line Type");
    case V::Height: return _u8L("Layer Height");
    case V::Width: return _u8L("Line Width");
    case V::Speed: return _u8L("Speed");
    case V::ActualSpeed: return _u8L("Actual Speed");
    case V::Acceleration: return _u8L("Acceleration");
    case V::Jerk: return _u8L("Jerk");
    case V::FanSpeed: return _u8L("Fan Speed");
    case V::Temperature: return _u8L("Temperature");
    case V::VolumetricFlowRate: return _u8L("Flow");
    case V::ActualVolumetricFlowRate: return _u8L("Actual Flow");
    case V::Tool: return _u8L("Tool");
    case V::ColorPrint: return _u8L("Filament");
    case V::LayerTimeLinear: return _u8L("Layer Time");
    case V::LayerTimeLogarithmic: return _u8L("Layer Time (log)");
    case V::PressureAdvance: return _u8L("Pressure Advance");
    default: return "";
    }
}

static json rgb(const libvgcode::Color &c) { return json::array({c[0], c[1], c[2]}); }


// SequentialView::Marker::render_position_window: the current move's position, speed and the view's detail, and the
// properties table behind the fold button
static std::string move_type_string(libvgcode::EMoveType type)
{
    using M = libvgcode::EMoveType;
    switch (type) {
    case M::Noop: return _u8L("Noop");
    case M::Retract: return _u8L("Retract");
    case M::Unretract: return _u8L("Unretract");
    case M::Seam: return _u8L("Seam");
    case M::ToolChange: return _u8L("Tool Change");
    case M::ColorChange: return _u8L("Color Change");
    case M::PausePrint: return _u8L("Pause Print");
    case M::CustomGCode: return _u8L("Custom G-code");
    case M::Travel: return _u8L("Travel");
    case M::Wipe: return _u8L("Wipe");
    case M::Extrude: return _u8L("Extrude");
    default: return _u8L("Unknown");
    }
}

static std::string role_string(libvgcode::EGCodeExtrusionRole role)
{
    using R = libvgcode::EGCodeExtrusionRole;
    switch (role) {
    case R::None: return _u8L("Unknown");
    case R::Perimeter: return _u8L("Inner wall");
    case R::ExternalPerimeter: return _u8L("Outer wall");
    case R::OverhangPerimeter: return _u8L("Overhang wall");
    case R::InternalInfill: return _u8L("Sparse infill");
    case R::SolidInfill: return _u8L("Internal solid infill");
    case R::TopSolidInfill: return _u8L("Top surface");
    case R::Ironing: return _u8L("Ironing");
    case R::BridgeInfill: return _u8L("Bridge");
    case R::GapFill: return _u8L("Gap infill");
    case R::Skirt: return _u8L("Skirt");
    case R::SupportMaterial: return _u8L("Support");
    case R::SupportMaterialInterface: return _u8L("Support interface");
    case R::WipeTower: return _u8L("Prime tower");
    case R::Custom: return _u8L("Custom");
    case R::BottomSurface: return _u8L("Bottom surface");
    case R::InternalBridgeInfill: return _u8L("Internal bridge");
    case R::Brim: return _u8L("Brim");
    case R::SupportTransition: return _u8L("Support transition");
    case R::Mixed: return _u8L("Mixed");
    default: return _u8L("Unknown");
    }
}

static json position_window(const libvgcode::Viewer &viewer)
{
    using V = libvgcode::EViewType;
    const std::string NA_TXT = _u8L("N/A");
    const V view_type = viewer.get_view_type();
    libvgcode::PathVertex vertex = viewer.get_current_vertex();
    size_t vertex_id = viewer.get_current_vertex_id();
    if (view_type != V::FeatureType && vertex.type == libvgcode::EMoveType::Seam) {
        const libvgcode::Interval &visible_range = viewer.get_view_visible_range();
        if (visible_range[1] > 0) { vertex_id = size_t(visible_range[1]) - 1; vertex = viewer.get_vertex_at(vertex_id); }
    }
    const bool is_extrusion = vertex.is_extrusion();
    char detail[256] = "";
    switch (view_type) {
    case V::FeatureType:
        snprintf(detail, sizeof detail, "%s", is_extrusion ? role_string(vertex.role).c_str() : vertex.type != libvgcode::EMoveType::Noop ? move_type_string(vertex.type).c_str() : NA_TXT.c_str());
        break;
    case V::Height: is_extrusion ? snprintf(detail, sizeof detail, "%s%.2f", _u8L("Height: ").c_str(), vertex.height) : snprintf(detail, sizeof detail, "%s%s", _u8L("Height: ").c_str(), NA_TXT.c_str()); break;
    case V::Width: is_extrusion ? snprintf(detail, sizeof detail, "%s%.2f", _u8L("Width: ").c_str(), vertex.width) : snprintf(detail, sizeof detail, "%s%s", _u8L("Width: ").c_str(), NA_TXT.c_str()); break;
    case V::VolumetricFlowRate: is_extrusion ? snprintf(detail, sizeof detail, "%s%.2f", _u8L("Flow: ").c_str(), vertex.volumetric_rate()) : snprintf(detail, sizeof detail, "%s%s", _u8L("Flow: ").c_str(), NA_TXT.c_str()); break;
    case V::FanSpeed: snprintf(detail, sizeof detail, "%s%.0f", _u8L("Fan: ").c_str(), vertex.fan_speed); break;
    case V::Temperature: snprintf(detail, sizeof detail, "%s%.0f", _u8L("Temperature: ").c_str(), vertex.temperature); break;
    case V::LayerTimeLinear:
    case V::LayerTimeLogarithmic: snprintf(detail, sizeof detail, "%s%.1f", _u8L("Layer Time: ").c_str(), vertex.layer_duration); break;
    case V::Tool: snprintf(detail, sizeof detail, "%s%d", _u8L("Tool: ").c_str(), vertex.extruder_id + 1); break;
    case V::ColorPrint: snprintf(detail, sizeof detail, "%s%d", _u8L("Color: ").c_str(), vertex.color_id + 1); break;
    case V::Acceleration: snprintf(detail, sizeof detail, "%s%.0f", _u8L("Acceleration: ").c_str(), vertex.acceleration); break;
    case V::Jerk: snprintf(detail, sizeof detail, "%s%.1f", _u8L("Jerk: ").c_str(), vertex.jerk); break;
    case V::PressureAdvance: snprintf(detail, sizeof detail, "%s%.4f", _u8L("PA: ").c_str(), vertex.pressure_advance); break;
    default: break;
    }
    // the properties table
    json rows = json::array();
    auto add = [&rows](const std::string &l, const std::string &v) { rows.push_back({l, v}); };
    char buff[256];
    add(_u8L("Type"), move_type_string(vertex.type));
    add(_u8L("Line Type"), is_extrusion ? role_string(vertex.role) : NA_TXT);
    if (is_extrusion) snprintf(buff, sizeof buff, ("%.3f " + _u8L("mm")).c_str(), vertex.width); else snprintf(buff, sizeof buff, "%s", NA_TXT.c_str());
    add(_u8L("Width"), buff);
    if (is_extrusion) snprintf(buff, sizeof buff, ("%.3f " + _u8L("mm")).c_str(), vertex.height); else snprintf(buff, sizeof buff, "%s", NA_TXT.c_str());
    add(_u8L("Height"), buff);
    if (vertex_id > 0 && (is_extrusion || vertex.is_travel() || vertex.is_wipe())) {
        const size_t vertices_count = viewer.get_vertices_count();
        size_t first_id = vertex_id, last_id = vertex_id;
        while (first_id > 0 && viewer.get_vertex_at(first_id - 1).gcode_id == vertex.gcode_id) --first_id;
        while (last_id + 1 < vertices_count && viewer.get_vertex_at(last_id + 1).gcode_id == vertex.gcode_id) ++last_id;
        float length = 0.0f;
        for (size_t i = std::max<size_t>(first_id, 1); i <= last_id; ++i) {
            const auto &a = viewer.get_vertex_at(i).position, &b = viewer.get_vertex_at(i - 1).position;
            length += std::sqrt(sqr(a[0] - b[0]) + sqr(a[1] - b[1]) + sqr(a[2] - b[2]));
        }
        snprintf(buff, sizeof buff, ("%.3f " + _u8L("mm")).c_str(), length);
    } else snprintf(buff, sizeof buff, "%s", NA_TXT.c_str());
    add(_u8L("Length"), buff);
    add(_u8L("Layer"), std::to_string(vertex.layer_id + 1));
    snprintf(buff, sizeof buff, ("%.1f " + _u8L("mm/s")).c_str(), vertex.feedrate); add(_u8L("Speed"), buff);
    snprintf(buff, sizeof buff, ("%.0f " + _u8L("mm/s²")).c_str(), vertex.acceleration); add(_u8L("Acceleration"), buff);
    snprintf(buff, sizeof buff, ("%.1f " + _u8L("mm/s")).c_str(), vertex.jerk); add(_u8L("Jerk"), buff);
    if (is_extrusion) snprintf(buff, sizeof buff, ("%.3f " + _u8L("mm³/s")).c_str(), vertex.volumetric_rate()); else snprintf(buff, sizeof buff, "%s", NA_TXT.c_str());
    add(_u8L("Flow rate"), buff);
    snprintf(buff, sizeof buff, "%.0f %%", vertex.fan_speed); add(_u8L("Fan speed"), buff);
    snprintf(buff, sizeof buff, ("%.0f " + _u8L("℃")).c_str(), vertex.temperature); add(_u8L("Temperature"), buff);
    snprintf(buff, sizeof buff, "%.4f", vertex.pressure_advance); add(_u8L("Pressure Advance"), buff);
    snprintf(buff, sizeof buff, "%s (%.3fs)", get_time_dhms(viewer.get_estimated_time_at(vertex_id)).c_str(), vertex.times[size_t(viewer.get_time_mode())]);
    add(_u8L("Time"), buff);
    // the axes, with less precision on big prints
    const auto &p = vertex.position;
    const int max_value = int(std::round(std::max(std::max(p[0], p[1]), p[2])));
    const char *precision = max_value > 9999 ? "%.1f" : max_value > 999 ? "%.2f" : "%.3f";
    char x[32], y[32], z[32];
    snprintf(x, sizeof x, precision, p[0]); snprintf(y, sizeof y, precision, p[1]); snprintf(z, sizeof z, precision, p[2]);
    char speed[64];
    snprintf(speed, sizeof speed, "%s%.0f", _u8L("Speed: ").c_str(), vertex.feedrate);
    return {{"x", x}, {"y", y}, {"z", z}, {"speed", speed}, {"detail", detail}, {"properties", rows}};
}

json Preview::legend()
{
    using V = libvgcode::EViewType;
    const LegendData &d = m_legend;
    const bool imperial_units = Core::get().app_config->get("use_inches") == "1";
    const auto &time_mode = d.stats.modes[size_t(m_viewer->get_time_mode())];
    const V curr_view_type = m_viewer->get_view_type();
    const size_t current_time_mode = size_t(m_viewer->get_time_mode());
    const float total_estimated_time = time_mode.time > 0.0f ? time_mode.time : m_viewer->get_estimated_time();

    json out;
    // update_by_mode: the view types of the combo, in the desktop's order
    json types = json::array();
    for (V v : {V::Summary, V::FeatureType, V::ColorPrint, V::Speed, V::ActualSpeed, V::Acceleration, V::Jerk, V::Height, V::Width,
                V::VolumetricFlowRate, V::ActualVolumetricFlowRate, V::LayerTimeLinear, V::LayerTimeLogarithmic, V::FanSpeed, V::Temperature,
                V::PressureAdvance})
        types.push_back({{"id", int(v)}, {"name", view_type_string(v)}});
    out["view_types"] = types;
    out["view_type"] = int(curr_view_type);
    json sections = json::array();
    auto headers = [&](std::vector<std::string> h) { sections.push_back({{"headers", h}}); };
    auto title = [&](const std::string &t) { sections.push_back({{"title", t}}); };
    json rows = json::array();
    auto flush_rows = [&]() { if (!rows.empty()) { sections.push_back({{"rows", rows}}); rows = json::array(); } };
    auto row = [&](const json &color, std::vector<std::string> cols, json toggle = nullptr, json visible = nullptr) {
        rows.push_back({{"color", color}, {"cols", cols}, {"toggle", toggle}, {"visible", visible}});
    };
    json pairs = json::array();
    auto flush_pairs = [&]() { if (!pairs.empty()) { sections.push_back({{"pairs", pairs}}); pairs = json::array(); } };

    auto format_compact_count = [](unsigned long long value) {
        static constexpr const char *suffixes[] = {"", "K", "M", "B", "T", "P", "E"};
        if (value < 1000) return std::to_string(value);
        size_t suffix_index = 0;
        unsigned long long divisor = 1;
        while (suffix_index + 1 < 7 && value / divisor >= 1000) { divisor *= 1000; ++suffix_index; }
        const unsigned long long whole = value / divisor, tenths = (value % divisor) * 10 / divisor;
        std::string ret = std::to_string(whole);
        if (tenths != 0) ret += "." + std::to_string(tenths);
        return ret + suffixes[suffix_index];
    };
    auto format_percent = [](float percent) {
        if (percent == 0.0f) return std::string("0");
        char buffer[64];
        percent > 0.001f ? ::sprintf(buffer, "%.1f", percent * 100.0f) : ::sprintf(buffer, "<0.1");
        return std::string(buffer);
    };
    auto format_distance = [imperial_units](float distance_mm) {
        char buffer[64];
        if (imperial_units) ::sprintf(buffer, "%.2fin", distance_mm / in_to_mm);
        else if (std::fabs(distance_mm) < 1000.0f) ::sprintf(buffer, "%.0fmm", distance_mm);
        else ::sprintf(buffer, "%.2fm", distance_mm / 1000.0f);
        return std::string(buffer);
    };
    auto used_filament_per_role = [&](ExtrusionRole role) {
        auto it = d.stats.used_filaments_per_role.find(role);
        if (it == d.stats.used_filaments_per_role.end()) return std::make_pair(0.0, 0.0);
        double koef = imperial_units ? in_to_mm / 1000.0 : 1.0, unit_conver = imperial_units ? oz_to_g : 1;
        return std::make_pair(it->second.first / koef, it->second.second / unit_conver);
    };
    auto get_used_filament_from_volume = [&](double volume, int extruder_id) {
        double koef = imperial_units ? 1.0 / in_to_mm : 0.001;
        const double dia = extruder_id < int(d.diameters.size()) ? d.diameters[extruder_id] : 1.75;
        const double den = extruder_id < int(d.densities.size()) ? d.densities[extruder_id] : 1.24;
        return std::make_pair(koef * volume / (PI * sqr(0.5 * dia)), volume * den * 0.001);
    };
    auto dhms = [](float t) { return short_time(get_time_dhms(t)); };

    // used filament statistics
    std::vector<double> model_m, model_g, flushed_m, flushed_g, tower_m, tower_g, support_m, support_g;
    double total_model_m = 0, total_model_g = 0, total_flushed_m = 0, total_flushed_g = 0, total_tower_m = 0, total_tower_g = 0,
           total_support_m = 0, total_support_g = 0;
    enum { Model = 1, Flushed = 2, WipeTower = 4, Support = 8 };
    int displayed_columns = 0;
    auto collect = [&](const std::map<size_t, double> &volumes, std::vector<double> &m, std::vector<double> &g, double &tm, double &tg, int flag) {
        for (size_t extruder_id : m_viewer->get_used_extruders_ids()) {
            auto it = volumes.find(extruder_id);
            if (it == volumes.end()) { m.push_back(0.0); g.push_back(0.0); continue; }
            auto [um, ug] = get_used_filament_from_volume(it->second, int(extruder_id));
            m.push_back(um); g.push_back(ug); tm += um; tg += ug;
            displayed_columns |= flag;
        }
    };
    collect(d.stats.model_volumes_per_extruder, model_m, model_g, total_model_m, total_model_g, Model);
    collect(d.stats.wipe_tower_volumes_per_extruder, tower_m, tower_g, total_tower_m, total_tower_g, WipeTower);
    collect(d.stats.flush_per_filament, flushed_m, flushed_g, total_flushed_m, total_flushed_g, Flushed);
    collect(d.stats.support_volumes_per_extruder, support_m, support_g, total_support_m, total_support_g, Support);
    const double koef = imperial_units ? in_to_mm : 1000.0;

    auto append_range = [&](V type, int decimals) {
        const libvgcode::ColorRange &range = m_viewer->get_color_range(type);
        auto item = [&](int i, float value) {
            char buf[64];
            ::sprintf(buf, "%.*f", decimals, value);
            row(rgb(range.get_palette()[i]), {buf});
        };
        const std::vector<float> values = range.get_values();
        if (values.size() == 1) item(0, values.front());
        else if (values.size() == 2) { item(int(range.get_palette().size()) - 1, values.back()); item(0, values.front()); }
        else for (int i = int(range.get_palette().size()) - 1; i >= 0 && i < int(values.size()); --i) item(i, values[i]);
        flush_rows();
    };
    // append_option_item: the option rows with their time / % / distance / count
    auto option_row = [&](libvgcode::EOptionType type, bool full_layout) {
        std::string label;
        libvgcode::EMoveType move_type = libvgcode::EMoveType::Noop;
        bool has_move_type = true;
        switch (type) {
        case libvgcode::EOptionType::Travels: label = _u8L("Travel"); has_move_type = false; break;
        case libvgcode::EOptionType::Seams: label = _u8L("Seams"); move_type = libvgcode::EMoveType::Seam; break;
        case libvgcode::EOptionType::Retractions: label = _u8L("Retract"); move_type = libvgcode::EMoveType::Retract; break;
        case libvgcode::EOptionType::Unretractions: label = _u8L("Unretract"); move_type = libvgcode::EMoveType::Unretract; break;
        case libvgcode::EOptionType::ToolChanges: label = _u8L("Filament changes"); move_type = libvgcode::EMoveType::ToolChange; break;
        case libvgcode::EOptionType::Wipes: label = _u8L("Wipe"); move_type = libvgcode::EMoveType::Wipe; break;
        default: return;
        }
        std::vector<std::string> cols{label};
        if (has_move_type && full_layout) {
            const size_t idx = size_t(move_type);
            float time = d.times[idx][current_time_mode];
            if (type == libvgcode::EOptionType::ToolChanges)
                time = d.stats.total_filament_load_time + d.stats.total_filament_unload_time + d.stats.total_tool_change_time;
            const float seam_distance = d.stats.total_seam_gap_distance + d.stats.total_seam_scarf_distance;
            const float distance = (type == libvgcode::EOptionType::Seams && seam_distance > 0.0f) ? seam_distance : d.distances[idx];
            const bool has_distance = type == libvgcode::EOptionType::Wipes || type == libvgcode::EOptionType::Retractions ||
                                      type == libvgcode::EOptionType::Unretractions || type == libvgcode::EOptionType::Seams;
            cols.push_back(time > 0.0f ? dhms(time) : "");
            cols.push_back(total_estimated_time > 0.0f ? format_percent(time / total_estimated_time) : "");
            cols.push_back(has_distance ? format_distance(distance) : "");
            cols.push_back(format_compact_count(d.counts[idx]));
        }
        row(rgb(m_viewer->get_option_color(type)), cols, {{"option", int(type)}}, m_viewer->is_option_visible(type));
    };
    auto travel_options = [&]() {   // the Options / Travel block of the speed / acceleration / jerk views
        headers({_u8L("Options")});
        row(rgb(m_viewer->get_option_color(libvgcode::EOptionType::Travels)), {_u8L("Travel")}, {{"option", int(libvgcode::EOptionType::Travels)}},
            m_viewer->is_option_visible(libvgcode::EOptionType::Travels));
        flush_rows();
    };

    switch (curr_view_type) {
    case V::FeatureType: {
        headers({_u8L("Line Type"), _u8L("Time"), "%", _u8L("Usage"), ""});
        for (libvgcode::EGCodeExtrusionRole role : m_viewer->get_extrusion_roles()) {
            if (size_t(role) >= libvgcode::GCODE_EXTRUSION_ROLES_COUNT) continue;
            const float time = m_viewer->get_extrusion_role_estimated_time(role);
            const float percent = total_estimated_time > 0.0f ? time / total_estimated_time : 0.0f;
            char buffer[64];
            if (percent == 0) ::sprintf(buffer, "0");
            else percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
            auto [um, ug] = used_filament_per_role(convert(role));
            char len[64];
            ::sprintf(len, imperial_units ? "%.2fin" : "%.2fm", um);
            row(rgb(m_viewer->get_extrusion_role_color(role)),
                {_u8L(ExtrusionEntity::role_to_string(convert(role))), time > 0.0f ? dhms(time) : "", buffer, len, format_compact_weight(ug, imperial_units)},
                {{"role", int(role)}}, m_viewer->is_extrusion_role_visible(role));
        }
        for (libvgcode::EOptionType item : m_viewer->get_options()) {
            if (item != libvgcode::EOptionType::Travels) option_row(item, true);
            else {   // the travel time in the Line Type view
                const float time = m_viewer->get_travels_estimated_time();
                const float percent = total_estimated_time > 0.0f ? time / total_estimated_time : 0.0f;
                char buffer[64];
                if (percent == 0) ::sprintf(buffer, "0");
                else percent > 0.001 ? ::sprintf(buffer, "%.1f", percent * 100) : ::sprintf(buffer, "<0.1");
                row(rgb(m_viewer->get_option_color(item)),
                    {_u8L("Travel"), time > 0.0f ? dhms(time) : "", buffer, format_distance(d.stats.total_travel_distance), format_compact_count(d.stats.total_travel_moves)},
                    {{"option", int(item)}}, m_viewer->is_option_visible(item));
            }
        }
        flush_rows();
        break;
    }
    case V::Height: title(_u8L("Layer height (mm)")); append_range(V::Height, 2); break;
    case V::Width: title(_u8L("Line width (mm)")); append_range(V::Width, 2); break;
    case V::Speed: title(_u8L("Speed (mm/s)")); append_range(V::Speed, 0); travel_options(); break;
    case V::ActualSpeed: title(_u8L("Actual Speed (mm/s)")); append_range(V::ActualSpeed, 0); travel_options(); break;
    case V::Acceleration: title(_u8L("Acceleration (mm/s²)")); append_range(V::Acceleration, 0); travel_options(); break;
    case V::Jerk: title(_u8L("Jerk (mm/s)")); append_range(V::Jerk, 1); travel_options(); break;
    case V::FanSpeed: title(_u8L("Fan speed (%)")); append_range(V::FanSpeed, 0); break;
    case V::Temperature: title(_u8L("Temperature (℃)")); append_range(V::Temperature, 0); break;
    case V::PressureAdvance: title(_u8L("Pressure Advance")); append_range(V::PressureAdvance, 3); break;
    case V::VolumetricFlowRate: title(_u8L("Volumetric flow rate (mm³/s)")); append_range(V::VolumetricFlowRate, 2); break;
    case V::ActualVolumetricFlowRate: title(_u8L("Actual volumetric flow rate (mm³/s)")); append_range(V::ActualVolumetricFlowRate, 2); break;
    case V::LayerTimeLinear: title(_u8L("Layer Time")); append_range(V::LayerTimeLinear, 1); break;
    case V::LayerTimeLogarithmic: title(_u8L("Layer Time (log)")); append_range(V::LayerTimeLogarithmic, 1); break;
    case V::Tool: {
        headers({_u8L("Filament"), _u8L("Usage")});
        size_t i = 0;
        for (uint8_t extruder_id : m_viewer->get_used_extruders_ids()) {
            char buf[64];
            ::sprintf(buf, imperial_units ? "%.2f in    %s" : "%.2f m    %s", i < model_m.size() ? model_m[i] : 0.,
                      format_compact_weight(i < model_g.size() ? model_g[i] : 0., imperial_units).c_str());
            row(rgb(m_viewer->get_tool_colors()[extruder_id]), {_u8L("Extruder") + " " + std::to_string(extruder_id + 1), buf});
            ++i;
        }
        flush_rows();
        break;
    }
    case V::Summary: {
        char buf[64];
        ::sprintf(buf, imperial_units ? "%.2f in / %s" : "%.2f m / %s", d.total_used_filament / koef, format_compact_weight(d.total_weight, imperial_units).c_str());
        pairs.push_back({_u8L("Total") + ":", buf});
        ::sprintf(buf, "%.2f", d.total_cost);
        pairs.push_back({_u8L("Cost") + ":", buf});
        pairs.push_back({_u8L("Total time") + ":", dhms(time_mode.time)});
        flush_pairs();
        break;
    }
    case V::ColorPrint: {
        std::vector<std::string> h{_u8L("Filament")};
        if (displayed_columns & Model) h.push_back(_u8L("Model"));
        if (displayed_columns & Support) h.push_back(_u8L("Support"));
        if (displayed_columns & Flushed) h.push_back(_u8L("Flushed"));
        if (displayed_columns & WipeTower) h.push_back(_u8L("Tower"));
        if ((displayed_columns & ~Model) > 0) h.push_back(_u8L("Total"));
        headers(h);
        const bool multi = (displayed_columns & ~Model) > 0;
        auto cell = [&](double m, double g, bool two_lines) {
            char buf[64];
            ::sprintf(buf, imperial_units ? (two_lines ? "%.2f in\n%s" : "%.2f in    %s") : (two_lines ? "%.2f m\n%s" : "%.2f m    %s"), m,
                      format_compact_weight(g, imperial_units).c_str());
            return std::string(buf);
        };
        size_t i = 0;
        const auto &used = m_viewer->get_used_extruders_ids();
        const auto &tool_colors = m_viewer->get_tool_colors();
        for (auto extruder_idx : used) {
            if (i < model_m.size()) {
                std::vector<std::string> cols{std::to_string(extruder_idx + 1)};
                double sm = 0, sg = 0;
                if (displayed_columns & Model) { cols.push_back(cell(model_m[i], model_g[i], multi)); sm += model_m[i]; sg += model_g[i]; }
                if (displayed_columns & Support) { cols.push_back(cell(support_m[i], support_g[i], true)); sm += support_m[i]; sg += support_g[i]; }
                if (displayed_columns & Flushed) { cols.push_back(cell(flushed_m[i], flushed_g[i], true)); sm += flushed_m[i]; sg += flushed_g[i]; }
                if (displayed_columns & WipeTower) { cols.push_back(cell(tower_m[i], tower_g[i], true)); sm += tower_m[i]; sg += tower_g[i]; }
                if (multi) cols.push_back(cell(sm, sg, true));
                row(rgb(tool_colors[extruder_idx]), cols);
            }
            ++i;
        }
        if (used.size() > 1) {   // the sum of all rows
            std::vector<std::string> cols{_u8L("Total")};
            if (displayed_columns & Model) cols.push_back(cell(total_model_m, total_model_g, multi));
            if (displayed_columns & Support) cols.push_back(cell(total_support_m, total_support_g, true));
            if (displayed_columns & Flushed) cols.push_back(cell(total_flushed_m, total_flushed_g, true));
            if (displayed_columns & WipeTower) cols.push_back(cell(total_tower_m, total_tower_g, true));
            if (multi) cols.push_back(cell(total_model_m + total_support_m + total_flushed_m + total_tower_m, total_model_g + total_support_g + total_flushed_g + total_tower_g, true));
            row(nullptr, cols);
        }
        flush_rows();
        pairs.push_back({_u8L("Filament change times") + ":", format_compact_count(d.stats.total_filament_changes)});
        pairs.push_back({_u8L("Tool changes") + ":", format_compact_count(d.stats.total_extruder_changes)});
        char buf[64];
        ::sprintf(buf, "%.2f", d.total_cost);
        pairs.push_back({_u8L("Cost") + ":", buf});
        flush_pairs();
        break;
    }
    default: break;
    }

    // the plate's custom G-codes with their layer and time
    if (!d.custom_gcodes.empty()) {
        headers({_u8L("Custom G-code"), _u8L("Layer"), _u8L("Time")});
        const std::vector<float> zs = m_viewer->get_layers_zs();
        const std::vector<float> layer_times = m_viewer->get_layers_estimated_times();
        for (const CustomGCode::Item &item : d.custom_gcodes) {
            std::string name;
            switch (item.type) {
            case CustomGCode::PausePrint: name = _u8L("Pause"); break;
            case CustomGCode::Template: name = _u8L("Template"); break;
            case CustomGCode::ToolChange: name = _u8L("Tool Change"); break;
            case CustomGCode::Custom: name = _u8L("Custom"); break;
            default: name = _u8L("Unknown"); break;
            }
            int layer = -1;   // find_close_layer_idx
            for (size_t k = 0; k < zs.size(); ++k)
                if (std::abs(zs[k] - item.print_z) < EPSILON) { layer = int(k); break; }
            float t = 0;
            for (int k = 0; k < layer && k < int(layer_times.size()); ++k) t += layer_times[k];
            row(nullptr, {name, std::to_string(layer + 1), dhms(t)});
        }
        flush_rows();
    }

    // the time estimation block
    std::string time_title = curr_view_type == V::FeatureType ? _u8L("Total estimation") : _u8L("Time Estimation");
    std::vector<std::string> time_strs;
    for (const auto &m : d.stats.modes)
        if (m.time > 0.0f && std::find(time_strs.begin(), time_strs.end(), dhms(m.time)) == time_strs.end()) time_strs.push_back(dhms(m.time));
    const bool can_show_mode_button = time_strs.size() > 1;
    if (can_show_mode_button && m_viewer->get_time_mode() == libvgcode::ETimeMode::Normal) time_title += " [" + _u8L("Normal mode") + "]";
    title(time_title);
    if (curr_view_type == V::FeatureType) {
        char buf[64];
        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", d.total_used_filament / koef);
        pairs.push_back({_u8L("Total Filament") + ":", std::string(buf) + "  " + format_compact_weight(d.total_weight, imperial_units)});
        const double exclude_m = total_support_m + total_flushed_m + total_tower_m, exclude_g = total_support_g + total_flushed_g + total_tower_g;
        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", d.total_used_filament / koef - exclude_m);
        pairs.push_back({_u8L("Model Filament") + ":", std::string(buf) + "  " + format_compact_weight(d.total_weight - exclude_g, imperial_units)});
        ::sprintf(buf, "%.2f", d.total_cost);
        pairs.push_back({_u8L("Cost") + ":", buf});
    }
    if (time_mode.prepare_time != 0.0f) pairs.push_back({_u8L("Prepare time") + ":", dhms(time_mode.prepare_time)});
    pairs.push_back({_u8L("Model printing time") + ":", dhms(time_mode.time - time_mode.prepare_time)});
    pairs.push_back({_u8L("Total time") + ":", dhms(time_mode.time)});
    flush_pairs();
    if (can_show_mode_button)
        sections.push_back({{"button", m_viewer->get_time_mode() == libvgcode::ETimeMode::Normal ? _u8L("Show stealth mode") : _u8L("Show normal mode")},
                            {"time_mode", m_viewer->get_time_mode() == libvgcode::ETimeMode::Normal ? 1 : 0}});
    if (curr_view_type == V::ColorPrint) {
        headers({_u8L("Options")});
        for (auto item : m_viewer->get_options()) option_row(item, false);
        flush_rows();
    }
    out["sections"] = sections;
    return out;
}

// GCodeWindow::render's lines: `count` lines centred on the current move's G-code line
json Preview::gcode_lines(int count)
{
    std::lock_guard<std::mutex> l(m_mutex);
    const LegendData &d = m_legend;
    if (!m_viewer || !m_has_data || d.lines_ends.empty() || count <= 0) return nullptr;
    const uint64_t curr_line_id = m_viewer->get_current_vertex().gcode_id;
    if (curr_line_id == 0) return nullptr;
    const uint64_t lines_count = uint64_t(count), half = lines_count / 2;
    uint64_t start_id = curr_line_id >= half ? curr_line_id - half : 0;
    uint64_t end_id = start_id + lines_count - 1;
    if (end_id >= uint64_t(d.lines_ends.size())) {
        end_id = uint64_t(d.lines_ends.size()) - 1;
        start_id = end_id >= lines_count - 1 ? end_id - lines_count + 1 : 0;
    }
    std::ifstream f(d.filename, std::ios::binary);
    if (!f) return nullptr;
    json lines = json::array();
    for (uint64_t id = std::max<uint64_t>(start_id, 1); id <= end_id; ++id) {
        const size_t start = id == 1 ? 0 : d.lines_ends[id - 2], end = d.lines_ends[id - 1];
        const size_t original_len = end > start ? end - start : 0, len = std::min(original_len, size_t(55 * 4));
        std::string gline(len, '\0');
        f.seekg(std::streamoff(start));
        f.read(gline.data(), std::streamsize(len));
        while (!gline.empty() && (gline.back() == '\n' || gline.back() == '\r')) gline.pop_back();
        if (gline.size() > 55) gline = gline.substr(0, 52) + "...";   // Orca: truncate long lines
        std::string command, parameters, comment;
        const size_t comment_start = gline.find(';');
        if (comment_start == std::string::npos) command = gline;
        else { command = gline.substr(0, comment_start); comment = gline.substr(comment_start); }
        if (!command.empty()) {
            const size_t sp = command.find(' ');
            if (sp != std::string::npos) { parameters = command.substr(sp); command = command.substr(0, sp); }
        }
        lines.push_back({{"id", id}, {"command", command}, {"parameters", parameters}, {"comment", comment}, {"current", id == curr_line_id}});
    }
    return lines;
}

void Preview::set(const json &a)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_pending_set.is_array()) m_pending_set = json::array();
    m_pending_set.push_back(a);
}

// layer range / move range / view type / role and option visibility (the legend and the sliders); GL thread
void Preview::apply(const json &a)
{
    if (!m_viewer || !m_has_data) return;
    if (a.contains("layer_range")) {
        size_t n = m_viewer->get_layers_count();
        if (n > 0) m_viewer->set_layers_view_range(std::min(a["layer_range"][0].get<size_t>(), n - 1), std::min(a["layer_range"][1].get<size_t>(), n - 1));
    }
    if (a.contains("view_range")) {
        const auto &en = m_viewer->get_view_enabled_range();
        size_t lo = std::clamp(a["view_range"][0].get<size_t>(), en[0], en[1]), hi = std::clamp(a["view_range"][1].get<size_t>(), en[0], en[1]);
        m_viewer->set_view_visible_range(lo, std::max(lo, hi));
    }
    if (a.contains("view_type")) m_viewer->set_view_type(libvgcode::EViewType(a["view_type"].get<int>()));
    if (a.contains("toggle_role")) m_viewer->toggle_extrusion_role_visibility(libvgcode::EGCodeExtrusionRole(a["toggle_role"].get<int>()));
    if (a.contains("toggle_option")) m_viewer->toggle_option_visibility(libvgcode::EOptionType(a["toggle_option"].get<int>()));
    if (a.contains("top_layer_only") && a["top_layer_only"].get<bool>() != m_viewer->is_top_layer_only_view_range())
        m_viewer->toggle_top_layer_only_view_range();
    if (a.contains("time_mode")) m_viewer->set_time_mode(libvgcode::ETimeMode(a["time_mode"].get<int>()));
    if (a.contains("view_type") && libvgcode::EViewType(a["view_type"].get<int>()) == libvgcode::EViewType::FeatureType)
        for (auto role : m_viewer->get_extrusion_roles())   // reset_visible
            if (!m_viewer->is_extrusion_role_visible(role)) m_viewer->toggle_extrusion_role_visibility(role);
}

} // namespace orca

namespace orca {
static Commands reg({
    {"preview.gcode_lines", [](const json &a) { return Preview::get().gcode_lines(a.value("count", 20)); }},
});
namespace {
using namespace Slic3r;
#include "slic3r/GUI/ToolpathsObjExporter.inc"
const float ToolpathsObjExporter::Cap_Rounding_Factor = 0.25f;
} // namespace

bool Preview::export_toolpaths_to_obj(const std::string &path)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_viewer || !m_has_data) return false;
    ToolpathsObjExporter exporter(*m_viewer);
    exporter.export_to(path);
    return true;
}

static Commands reg_export({
    {"preview.export_obj", [](const json &a) {
        if (!Preview::get().export_toolpaths_to_obj(str_arg(a, "path"))) throw CommandError("no toolpaths");
        return json(nullptr);
    }},
});

size_t Preview::layers_count() const { return m_viewer && m_has_data ? m_viewer->get_layers_count() : 0; }
size_t Preview::vertices_count() const { return m_viewer && m_has_data ? m_viewer->get_vertices_count() : 0; }
void Preview::set_top_layer(size_t top)
{
    if (!m_viewer || !m_has_data) return;
    m_viewer->set_layers_view_range(m_viewer->get_layers_view_range()[0], top);
}
void Preview::notify()
{
    if (m_viewer && m_has_data) Host::event("preview", info());
}

std::array<size_t, 2> Preview::layers_view_range() const
{
    if (!m_viewer || !m_has_data) return {0, 0};
    const auto &r = m_viewer->get_layers_view_range();
    return {size_t(r[0]), size_t(r[1])};
}

} // namespace orca
