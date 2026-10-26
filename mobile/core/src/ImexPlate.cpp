// The IDEX/IQEX overlay of a plate (PartPlate::render_imex_zones and the ghost volumes): the copy / mirror zones, the
// carriage danger strips and the safety-margin bands, and a ghost of each object where a Copy / Mirror tool prints it.
// PartPlate's calc_imex_zones / calc_imex_ghosts and the head colours are compiled from upstream on a stand-in plate
// whose GLModels keep their polygon and whose ghost GLVolumes keep their transformation and mesh.
#include "ImexPlate.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "PrimeTower.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "wx/string.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/IMEXZones.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Tesselate.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace orca { namespace imexplate {
using namespace Slic3r;

// GLModel of a zone: the clipped polygon
struct GLModel
{
    ExPolygon poly;
    bool      init{false};
    std::shared_ptr<const TriangleMesh> mesh;
    void init_from(const TriangleMesh &m) { mesh = std::make_shared<const TriangleMesh>(m); init = true; }
};
static bool init_model_from_poly(GLModel &m, const ExPolygon &poly, float) { m.poly = poly; m.init = true; return true; }
static constexpr float GROUND_Z = 0.f;

// a ghost GLVolume: its transformation, mesh and colour
struct GLVolume
{
    static inline const ColorRGBA UNPRINTABLE_COLOR{0.0f, 0.0f, 0.0f, 0.5f};
    struct CompositeID
    {
        CompositeID(int o, int v, int i) : object_id(o), volume_id(v), instance_id(i) {}
        int object_id, volume_id, instance_id;
    };
    Transform3d xf{Transform3d::Identity()};
    GLModel     model;
    CompositeID composite_id{0, 0, 0};
    ColorRGBA   color;
    int force_transparent{0}, force_native_color{0}, disabled{0}, zoom_to_volumes{0}, shader_outside_printer_detection_enabled{0}, picking{0};
    void set_instance_transformation(const Transform3d &t) { xf = t; }
};

struct ImexApp { PresetBundle *preset_bundle{nullptr}; };
static ImexApp &wxGetApp()
{
    static ImexApp app;
    app.preset_bundle = Core::get().preset_bundle.get();
    return app;
}

struct ImexPlate
{
    static constexpr int IMEX_GHOST_COMPOSITE_ID_BASE = -100000;
    static constexpr int IMEX_GHOST_MAX_HEADS        = 256;
    static int imex_ghost_composite_id_for_head(int physical_head) { return IMEX_GHOST_COMPOSITE_ID_BASE - physical_head; }
    static int imex_ghost_head_from_composite_id(int object_id) { return IMEX_GHOST_COMPOSITE_ID_BASE - object_id; }

    DynamicPrintConfig                       m_config;
    void                                    *m_plater{this};   // the GUI plater exists
    Model                                   *m_model{nullptr};
    Pointfs                                  m_shape;
    std::set<std::pair<int, int>>            obj_to_instance_set;

    std::vector<GLModel>                     m_imex_copy_zones, m_imex_mirror_zones, m_imex_collision_overlay, m_imex_margin_overlay;
    std::vector<BoundingBoxf3>               m_imex_secondary_zone_boxes, m_imex_collision_zones;
    std::optional<BoundingBoxf>              m_imex_primary_zone_box;
    std::map<int, Vec2d>                     m_imex_head_zone_centers;
    int                                      m_imex_primary_head{-1};
    std::vector<std::unique_ptr<GLVolume>>   m_imex_ghost_volumes;

    std::string             get_imex_mode() const;
    std::string             get_effective_imex_mode() const;
    std::map<int, int>      get_imex_head_filament_map() const;
    void                    calc_imex_zones();
    void                    ensure_imex_zones() {}   // calc_imex_ghosts runs after calc_imex_zones here
    bool                    resolve_active_mode_tools(std::string &out_tools_str, int &out_primary_phys) const;
    void                    calc_imex_ghosts();
    ColorRGBA               get_imex_head_filament_color(int physical_head, const ConfigOptionInts &pem, const std::map<int, int> &plate_map) const;
    bool                    has_imex_multimaterial_conflict() const;
    void                    set_imex_head_filament_map(const std::map<int, int> &m);
    void                    update_slice_result_valid_state(bool) {}   // the caller marks the plate changed
    // collect_imex_warnings' view of the plate: its filaments (get_extruders(true)) and its config
    int  plate_index{0};
    std::vector<int> get_extruders(bool, bool expand_mixed_slots = true) const
    {
        Core &core = Core::get();
        return PlateTower(plate_index).get_extruders(true, core.full_config(), core.preset_bundle->project_config, expand_mixed_slots);
    }
    const DynamicPrintConfig *config() const { return &m_config; }
    // the plate outline (PartPlate::generate_print_polygon)
    void generate_print_polygon(ExPolygon &poly) const
    {
        for (const Vec2d &p : m_shape) poly.contour.append(Point::new_scale(p.x(), p.y()));
    }
};

#include "slic3r/GUI/ImexPlate.inc"

using PartPlate = ImexPlate;
#include "slic3r/GUI/ImexPlate.inc.warn"

// GCodeViewer::resolve_imex_marker_plan on a stand-in viewer (the plan is plain data)
struct GCodeViewer
{
    struct ImexMarkerPlan
    {
        struct Carriage
        {
            int   phys_head    = -1;
            bool  mirror_x     = false;
            bool  mirror_y     = false;
            float x_term       = 0.0f;
            float y_term       = 0.0f;
            float box_offset_x = 0.0f;
            float box_offset_y = 0.0f;
        };
        std::vector<Carriage> carriages;
        int   pri_head = -1;
        float pri_box_offset_x = 0.0f;
        float pri_box_offset_y = 0.0f;
        float box_wx = 0.0f;
        float box_wy = 0.0f;
    };
    static ImexMarkerPlan resolve_imex_marker_plan(const DynamicPrintConfig &printer_cfg, const std::string &mode, const BoundingBoxf &bed_extents);
};
#include "slic3r/GUI/ImexPlate.inc.marker"

// PartPlate::render_imex_zones' themes (Standard, Deuteranopia / Protanopia, Tritanopia, High Contrast)
struct Theme { ColorRGBA copy, mirror, danger, margin; };
static const Theme &theme(const DynamicPrintConfig &pcfg)
{
    static const Theme k_standard = {{0.337f, 0.706f, 0.914f, 0.22f}, {0.902f, 0.624f, 0.000f, 0.22f}, {0.850f, 0.100f, 0.100f, 0.40f}, {0.300f, 0.900f, 0.200f, 0.20f}};
    static const Theme k_deuteranopia = {{0.000f, 0.447f, 0.698f, 0.50f}, {0.941f, 0.894f, 0.259f, 0.50f}, {0.200f, 0.100f, 0.800f, 0.65f}, {0.800f, 0.475f, 0.655f, 0.45f}};
    static const Theme k_tritanopia = {{0.835f, 0.369f, 0.000f, 0.50f}, {0.800f, 0.475f, 0.655f, 0.50f}, {0.850f, 0.100f, 0.100f, 0.55f}, {0.000f, 0.700f, 0.600f, 0.45f}};
    static const Theme k_high_contrast = {{0.000f, 0.600f, 1.000f, 0.65f}, {1.000f, 0.600f, 0.000f, 0.65f}, {1.000f, 0.000f, 0.000f, 0.75f}, {0.000f, 1.000f, 0.200f, 0.60f}};
    switch (imex_cfg_enum<ImexVizTheme>(pcfg, "imex_viz_theme")) {
    case ImexVizTheme::Deuteranopia: return k_deuteranopia;
    case ImexVizTheme::Tritanopia: return k_tritanopia;
    case ImexVizTheme::HighContrast: return k_high_contrast;
    default: return k_standard;
    }
}

static constexpr float IMEX_GHOST_ALPHA = 0.55f;

} // namespace imexplate

// under the core lock
ImexOverlay imex_overlay(int plate)
{
    using namespace imexplate;
    ImexOverlay out;
    Core &core = Core::get();
    const DynamicPrintConfig &pcfg = core.preset_bundle->printers.get_edited_preset().config;
    if (!imex_cfg_bool(pcfg, "is_imex")) return out;
    Plater &p = Plater::get();
    if (plate < 0 || plate >= int(p.plates.size())) return out;
    ImexPlate pp;
    pp.m_config = p.plates[plate].config;
    pp.m_model = &p.model();
    const Vec3d origin = p.plate_origin(plate);
    for (const Vec2d &v : p.bed_shape()) pp.m_shape.push_back(v + Vec2d(origin.x(), origin.y()));
    pp.obj_to_instance_set = p.plates[plate].instances;
    pp.plate_index = plate;
    pp.calc_imex_zones();
    pp.calc_imex_ghosts();
    const Theme &t = theme(pcfg);
    auto fills = [&](const std::vector<GLModel> &zones, const ColorRGBA &c) {
        for (const GLModel &z : zones) {
            if (!z.init) continue;
            std::vector<float> tri;
            for (const Vec2d &v : triangulate_expolygon_2d(z.poly)) tri.insert(tri.end(), {float(v.x()), float(v.y()), 0.f});
            out.fills.push_back({std::move(tri), c});
        }
    };
    fills(pp.m_imex_copy_zones, t.copy);
    fills(pp.m_imex_mirror_zones, t.mirror);
    fills(pp.m_imex_collision_overlay, t.danger);
    fills(pp.m_imex_margin_overlay, t.margin);
    // update_imex_ghost_colors: the filament of each head, translucent
    const ConfigOptionInts pem = effective_physical_extruder_map(*core.preset_bundle);
    const auto plate_map = pp.get_imex_head_filament_map();
    for (auto &g : pp.m_imex_ghost_volumes) {
        if (!g || !g->model.mesh) continue;
        ColorRGBA c = pp.get_imex_head_filament_color(ImexPlate::imex_ghost_head_from_composite_id(g->composite_id.object_id), pem, plate_map);
        c.a(IMEX_GHOST_ALPHA);
        out.ghosts.push_back({g->xf, g->model.mesh, c, ImexPlate::imex_ghost_head_from_composite_id(g->composite_id.object_id)});
    }
    // PLATE_IMEX_MODE_ID: the plate's mode button, cycling imex_plate_mode_choices
    out.mode = pp.get_imex_mode();
    // the warning badge on the mode icon, and the line its tooltip adds
    out.mm_conflict = pp.has_imex_multimaterial_conflict();
    out.choices = imex_plate_mode_choices(pcfg);
    return out;
}

// GCodeViewer::render_scene: the plate's mode (its own beats the process preset's), the plan over the plate's bed and
// the carriage colours
ImexMarkers imex_markers(int plate)
{
    using namespace imexplate;
    ImexMarkers out;
    Core &core = Core::get();
    const DynamicPrintConfig &pcfg = core.preset_bundle->printers.get_edited_preset().config;
    if (!imex_cfg_bool(pcfg, "is_imex")) return out;
    Plater &p = Plater::get();
    if (plate < 0 || plate >= int(p.plates.size())) return out;
    ImexPlate pp;
    pp.m_config = p.plates[plate].config;
    pp.m_model = &p.model();
    const Vec3d origin = p.plate_origin(plate);
    for (const Vec2d &v : p.bed_shape()) pp.m_shape.push_back(v + Vec2d(origin.x(), origin.y()));
    pp.obj_to_instance_set = p.plates[plate].instances;
    pp.plate_index = plate;
    auto *mode_opt = core.preset_bundle->prints.get_edited_preset().config.opt<ConfigOptionString>("imex_parallel_mode");
    std::string mode = mode_opt ? mode_opt->value : kImexPrimaryMode;
    if (const std::string plate_mode = pp.get_effective_imex_mode(); plate_mode != kImexPrimaryMode) mode = plate_mode;
    const GCodeViewer::ImexMarkerPlan plan = GCodeViewer::resolve_imex_marker_plan(pcfg, mode, get_extents(pp.m_shape));
    if (plan.carriages.empty()) return out;
    static const std::array<ColorRGBA, 4> s_carriage_colors = {{
        {1.000f, 1.000f, 1.000f, 0.65f},   // primary      — white
        {0.941f, 0.894f, 0.259f, 0.65f},   // secondary 1  — yellow
        {0.835f, 0.369f, 0.000f, 0.65f},   // secondary 2  — vermilion
        {0.800f, 0.475f, 0.655f, 0.65f},   // secondary 3  — reddish purple
    }};
    const ConfigOptionInts pem = effective_physical_extruder_map(*core.preset_bundle);
    const auto plate_map = pp.get_imex_head_filament_map();
    // a head with no filament keeps the palette; a filament colour takes the palette's translucency
    auto filament = [&](int head, const ColorRGBA &fallback) {
        if (head < 0) return fallback;
        ColorRGBA c = pp.get_imex_head_filament_color(head, pem, plate_map);
        if (c == GLVolume::UNPRINTABLE_COLOR) return fallback;
        c.a(fallback.a());
        return c;
    };
    out.pri_palette = s_carriage_colors[0];
    out.pri_filament = filament(plan.pri_head, out.pri_palette);
    out.pri_box_offset_x = plan.pri_box_offset_x;
    out.pri_box_offset_y = plan.pri_box_offset_y;
    out.box_wx = plan.box_wx;
    out.box_wy = plan.box_wy;
    for (size_t i = 0; i < plan.carriages.size(); ++i) {
        const auto &c = plan.carriages[i];
        ImexMarkers::Carriage m;
        m.mirror_x = c.mirror_x; m.mirror_y = c.mirror_y;
        m.x_term = c.x_term; m.y_term = c.y_term;
        m.box_offset_x = c.box_offset_x; m.box_offset_y = c.box_offset_y;
        m.palette = s_carriage_colors[(i + 1) % s_carriage_colors.size()];
        m.filament = filament(c.phys_head, m.palette);
        out.carriages.push_back(m);
    }
    return out;
}

// a tap on a ghost (GLCanvas3D::_render_imex_ghost_tooltip, Plater::format_imex_ghost_tooltip): the head, the filament
// it prints with its swatch; on a head with several lanes, the lanes IMEXFilamentPickerPopover offers
static json imex_ghost(const json &a)
{
    using namespace imexplate;
    Core &core = Core::get();
    Plater &p = Plater::get();
    const int head = a.at("head").get<int>();
    const int plate = p.current_plate;
    json out = {{"head", head}};
    if (plate < 0 || plate >= int(p.plates.size())) {
        out["label"] = Slic3r::format(_u8L("T%1% -> (no plate)"), head);
        return out;
    }
    ImexPlate pp;
    pp.m_config = p.plates[plate].config;
    pp.m_model = &p.model();
    pp.plate_index = plate;
    const ConfigOptionInts pem = effective_physical_extruder_map(*core.preset_bundle);
    const auto map = pp.get_imex_head_filament_map();
    const int logical = resolve_filament_for_head(map, pem, head);
    if (logical < 0) {
        // TRN IDEX/IQEX ghost tooltip. %1% is the number of the physical print head.
        out["label"] = Slic3r::format(_u8L("T%1% — no filament routed\nAdd an extruder in the Machine tab so this head has a filament slot."), head);
        return out;
    }
    ColorRGBA swatch = pp.get_imex_head_filament_color(head, pem, map);
    swatch.a(1.0f);
    out["swatch"] = encode_color(swatch);
    // TRN IDEX/IQEX ghost tooltip. %1% is the number of the physical print head, %2% the filament it prints.
    out["label"] = Slic3r::format(_u8L("T%1% -> filament %2%"), head, logical + 1);
    // Plater::on_imex_ghost_click: a single-lane head has nothing to pick
    std::vector<int> lanes;
    for (size_t L = 0; L < pem.values.size(); ++L)
        if (pem.values[L] == head) lanes.push_back(int(L));
    if (lanes.size() >= 2) {
        auto it = map.find(head);
        const int selected = it != map.end() ? it->second : lanes.front() + 1;
        std::vector<std::string> colors;
        if (auto *fc = core.full_config().option<ConfigOptionStrings>("filament_colour")) colors = fc->values;
        json items = json::array();
        for (int L : lanes)
            items.push_back({{"slot", L + 1}, {"label", Slic3r::format(_u8L("Filament %d"), L + 1)},
                             {"color", L < int(colors.size()) ? colors[L] : std::string()}, {"checked", L + 1 == selected}});
        out["lanes"] = items;
    }
    return out;
}

// IMEXFilamentPickerPopover::on_filament_selected (the pem default erases the override) and the commit callback
static json imex_ghost_set(const json &a)
{
    using namespace imexplate;
    Core &core = Core::get();
    Plater &p = Plater::get();
    const int head = a.at("head").get<int>(), slot = a.at("slot").get<int>();
    const int plate = p.current_plate;
    if (plate < 0 || plate >= int(p.plates.size())) throw CommandError("no such plate");
    ImexPlate pp;
    pp.m_config = p.plates[plate].config;
    const ConfigOptionInts pem = effective_physical_extruder_map(*core.preset_bundle);
    auto map = pp.get_imex_head_filament_map();
    const int default_logical = first_filament_for_physical_head(pem, head);
    if (default_logical >= 0 && slot == default_logical + 1) map.erase(head);
    else map[head] = slot;
    p.take_snapshot("edit imex head filament");
    pp.set_imex_head_filament_map(map);
    p.plates[plate].config = pp.m_config;
    p.changed();
    return p.state();
}

// Plater::select_plate_by_hover_id(PLATE_IMEX_MODE_ID): a tap cycles the plate's mode, the menu (a long press) sets one
static json plate_imex_mode(const json &a)
{
    using namespace Slic3r;
    Core &core = Core::get();
    Plater &p = Plater::get();
    const int plate = a.value("plate", p.current_plate);
    if (plate < 0 || plate >= int(p.plates.size())) throw CommandError("no such plate");
    const std::vector<std::string> modes = imex_plate_mode_choices(core.preset_bundle->printers.get_edited_preset().config);
    DynamicPrintConfig &cfg = p.plates[plate].config;
    std::string current = kImexPrimaryMode;
    if (auto *opt = cfg.option<ConfigOptionString>("imex_parallel_mode"); opt && !opt->value.empty()) current = opt->value;
    if (a.value("menu", false)) {
        json items = json::array();
        for (const std::string &m : modes)
            items.push_back({{"mode", m}, {"label", m == kImexPrimaryMode ? _u8L("Primary") : m}, {"checked", m == current}});
        imexplate::ImexPlate pp;
        pp.plate_index = plate;
        pp.m_config = cfg;
        pp.m_model = &p.model();
        json out = {{"items", items}};
        if (pp.has_imex_multimaterial_conflict())
            out["warning"] = _u8L("Warning: this plate uses a parallel IDEX/IQEX mode with multi-material objects. Proceed with caution — verify your G-code handles this combination correctly.");
        return out;
    }
    std::string next;
    if (a.contains("mode")) next = a["mode"].get<std::string>();
    else if (!modes.empty()) {
        auto it = std::find(modes.begin(), modes.end(), current);
        next = modes[it == modes.end() ? 0 : (it - modes.begin() + 1) % modes.size()];
    }
    if (next.empty() || next == current) return p.state();
    p.take_snapshot("set imex mode");
    // PartPlate::set_imex_mode: Primary is the absence of a mode
    if (next == kImexPrimaryMode) cfg.erase("imex_parallel_mode");
    else cfg.set_key_value("imex_parallel_mode", new ConfigOptionString(next));
    p.changed();
    return p.state();
}

// Plater::priv::on_action_slice_plate / on_action_slice_all: the IDEX/IQEX concerns of the plates about to be sliced,
// asked before slicing unless turned off (false: the user stopped)
bool imex_pre_slice_check(const std::vector<int> &plates)
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;
    Core &core = Core::get();
    if (core.app_config->get("imex_pre_slice_warnings") == "false") return true;
    if (!imex_cfg_bool(core.preset_bundle->printers.get_edited_preset().config, "is_imex")) return true;
    Plater &p = Plater::get();
    auto warnings_of = [&](int i) {
        imexplate::ImexPlate pp;
        pp.plate_index = i;
        pp.m_config = p.plates[i].config;
        pp.m_model = &p.model();
        return imexplate::collect_imex_warnings(&pp);
    };
    wxString msg;
    if (plates.size() == 1) {
        const std::vector<wxString> warnings = warnings_of(plates[0]);
        if (warnings.empty()) return true;
        msg = wxString::Format(_L("Plate %d has IDEX/IQEX parallel mode active with the following concerns:\n\n"), plates[0] + 1);
        for (const wxString &w : warnings) msg += "\u2022 " + w + "\n\n";
    } else {
        wxString combined_msg;
        for (int i : plates) {
            const std::vector<wxString> warnings = warnings_of(i);
            if (warnings.empty()) continue;
            combined_msg += wxString::Format(_L("Plate %d:\n"), i + 1);
            for (const wxString &w : warnings) combined_msg += "  \u2022 " + w + "\n";
            combined_msg += "\n";
        }
        if (combined_msg.empty()) return true;
        msg = _L("The following IDEX/IQEX parallel mode concerns were detected:\n\n") + combined_msg;
    }
    msg += _L("Continue slicing?");
    RichMessageDialog dlg(nullptr, msg, _L("IDEX/IQEX Parallel Mode Warning"), wxICON_WARNING | wxYES | wxNO);
    dlg.ShowCheckBox(_L("Don't show these warnings again"));
    const int result = dlg.ShowModal();
    if (dlg.IsCheckBoxChecked()) core.app_config->set("imex_pre_slice_warnings", "false");
    return result == wxID_YES;
}

static Commands reg({{"plate.imex_mode", plate_imex_mode}, {"imex.ghost", imex_ghost}, {"imex.ghost_set", imex_ghost_set}});

} // namespace orca
