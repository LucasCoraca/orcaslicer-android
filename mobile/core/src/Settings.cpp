// Settings editing: the option schema, the edited preset's values, value changes and the enable/visible state of
// every option. Print options go through upstream ConfigManipulation (compiled unchanged); the printer and filament
// rules are ports of TabPrinter / TabFilament (slic3r/GUI/Tab.cpp), named after the functions they mirror.
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <cfloat>
#include <boost/format.hpp>

#include "Core.hpp"
#include "Slicer.hpp"
#include "Host.hpp"
#include "I18n.hpp"
#include "Plater.hpp"
#include "Settings.hpp"
#include "slic3r/plugin/PluginConfig.hpp"
#include "libslic3r/BeltTransform.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "slic3r/GUI/ConfigManipulation.hpp"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

static const char *type_str(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float"; case coFloats: return "floats";
    case coInt: return "int"; case coInts: return "ints";
    case coString: return "string"; case coStrings: return "strings";
    case coPercent: return "percent"; case coPercents: return "percents";
    case coFloatOrPercent: return "float_or_percent"; case coFloatsOrPercents: return "floats_or_percents";
    case coPoint: return "point"; case coPoints: return "points"; case coPoint3: return "point3";
    case coBool: return "bool"; case coBools: return "bools";
    case coEnum: return "enum"; case coEnums: return "enums";
    case coIntsGroups: return "ints_groups"; case coPointsGroups: return "points_groups";
    default: return "none";
    }
}

static const char *gui_type_str(ConfigOptionDef::GUIType t)
{
    using G = ConfigOptionDef::GUIType;
    switch (t) {
    case G::i_enum_open: return "i_enum_open"; case G::f_enum_open: return "f_enum_open";
    case G::color: return "color"; case G::select_open: return "select_open"; case G::slider: return "slider";
    case G::legend: return "legend"; case G::one_string: return "one_string"; case G::plugin_picker: return "plugin_picker";
    case G::plugin_config: return "plugin_config"; case G::printer_agent_select: return "printer_agent_select";
    default: return "";
    }
}

// every option of print_config_def, translated: what the generated settings pages render
static json schema(const json &)
{
    json out = json::object();
    for (const auto &[key, def] : print_config_def.options) {
        json j = {{"type", type_str(def.type)},
                  {"label", translate(def.label)},
                  {"full_label", translate(def.full_label)},
                  {"category", translate(def.category)},
                  {"tooltip", translate(def.tooltip)},
                  {"sidetext", translate(def.sidetext)},
                  {"mode", def.mode == comSimple ? "simple" : def.mode == comAdvanced ? "advanced" : def.mode == comExpert ? "expert" : "develop"}};
        if (def.min > -FLT_MAX) j["min"] = def.min;
        if (def.max < FLT_MAX) j["max"] = def.max;
        if (def.max_literal != 1) j["max_literal"] = def.max_literal;
        if (def.nullable) j["nullable"] = true;
        if (def.multiline) j["multiline"] = true;
        if (def.full_width) j["full_width"] = true;
        if (def.is_code) j["is_code"] = true;
        if (def.readonly) j["readonly"] = true;
        if (def.height > 0) j["height"] = def.height;
        if (def.gui_type != ConfigOptionDef::GUIType::undefined) j["gui_type"] = gui_type_str(def.gui_type);
        if (!def.gui_flags.empty()) j["gui_flags"] = def.gui_flags;
        if (!def.ratio_over.empty()) j["ratio_over"] = def.ratio_over;
        if (!def.enum_values.empty()) j["enum_values"] = def.enum_values;
        if (!def.enum_labels.empty()) {
            json labels = json::array();
            for (auto &l : def.enum_labels) labels.push_back(translate(l));
            j["enum_labels"] = labels;
        }
        if (def.default_value) j["default"] = def.default_value->serialize();
        out[key] = std::move(j);
    }
    return out;
}

// value of one option: a string for scalars, a list of element strings for vectors
static json value_json(const ConfigOption *opt)
{
    if (!opt) return nullptr;
    if (opt->is_vector()) return static_cast<const ConfigOptionVectorBase *>(opt)->vserialize();
    // a string as its Field shows it (multi-line G-code with real line breaks), not C-escaped like serialize()
    if (opt->type() == coString) return static_cast<const ConfigOptionString *>(opt)->value;
    return opt->serialize();
}

// -------------------------------------------------------------------------------------------------------------------
// enable / visible state collected from upstream-style toggle_option / toggle_line calls

struct Toggles
{
    json enabled = json::object();   // key or key#idx -> bool
    json visible = json::object();
    json labels  = json::object();
    void field(const std::string &key, bool on, int idx = -1) { enabled[idx >= 0 && idx < 256 ? key + "#" + std::to_string(idx) : key] = on; }
    void line(const std::string &key, bool on, int idx = -1) { visible[idx >= 0 && idx < 256 ? key + "#" + std::to_string(idx) : key] = on; }
};

static GUI::ConfigManipulation make_manipulation(Toggles &t, DynamicPrintConfig *config)
{
    return GUI::ConfigManipulation(
        nullptr, [&t](const std::string &k, bool on, int idx) { t.field(k, on, idx); },
        [&t](const std::string &k, bool on, int idx) { t.line(k, on, idx); }, nullptr, nullptr, nullptr,
        [&t](const std::string &k, const wxString &label, int) { t.labels[k] = std::string(label); });
}

static void reset_option(DynamicPrintConfig &cfg, const char *key, ConfigOption *opt) { cfg.set_key_value(key, opt); }

// ConfigManipulation::show_spiral_mode_settings_dialog for the plate settings (PartPlate::set_spiral_vase_mode)
int show_spiral_mode_settings_dialog(bool is_object_config)
{
    Toggles t;
    return make_manipulation(t, nullptr).show_spiral_mode_settings_dialog(is_object_config);
}

// TabFilament::toggle_options (all pages)
static void toggle_filament(Toggles &t, DynamicPrintConfig *m_config)
{
    PresetBundle *bundle = Core::get().preset_bundle.get();
    const unsigned int variant_index = 0;
    const bool is_BBL_printer = bundle->is_bbl_vendor();
    auto printer_cfg = bundle->printers.get_edited_preset().config;
    auto toggle_option = [&](const std::string &k, bool on, int idx = -1) { t.field(k, on, idx); };
    auto toggle_line   = [&](const std::string &k, bool on, int idx = -1) { t.line(k, on, idx); };

    // Cooling
    bool has_enable_overhang_bridge_fan = m_config->opt_bool("enable_overhang_bridge_fan", 0);
    for (auto el : {"overhang_fan_speed", "overhang_fan_threshold", "internal_bridge_fan_speed"})
        toggle_option(el, has_enable_overhang_bridge_fan);
    toggle_option("dont_slow_down_outer_wall", m_config->opt_bool("slow_down_for_layer_cooling", 0));
    const bool initial_layer_fan_speed_enabled = m_config->opt_int("close_fan_the_first_x_layers", 0) <= 0;
    toggle_line("initial_layer_fan_speed", initial_layer_fan_speed_enabled);
    if (!initial_layer_fan_speed_enabled)
        if (auto *opt = dynamic_cast<const ConfigOptionInts *>(m_config->option("initial_layer_fan_speed")))
            if (std::any_of(opt->values.begin(), opt->values.end(), [](int v) { return v != -1; }))
                reset_option(*m_config, "initial_layer_fan_speed", new ConfigOptionInts(std::vector<int>(opt->values.size(), -1)));
    toggle_line("additional_cooling_fan_speed", printer_cfg.opt_bool("auxiliary_fan"), 256 + variant_index);
    bool support_air_filtration = printer_cfg.opt_bool("support_air_filtration");
    for (auto el : {"activate_air_filtration", "during_print_exhaust_fan_speed", "complete_print_exhaust_fan_speed"})
        toggle_line(el, support_air_filtration);
    if (support_air_filtration) {
        bool activate_air_filtration = m_config->opt_bool("activate_air_filtration", 0);
        toggle_option("activate_air_filtration_during_print", activate_air_filtration);
        toggle_option("during_print_exhaust_fan_speed", activate_air_filtration && m_config->opt_bool("activate_air_filtration_during_print", 0));
        toggle_option("activate_air_filtration_on_completion", activate_air_filtration);
        toggle_option("complete_print_exhaust_fan_speed", activate_air_filtration && m_config->opt_bool("activate_air_filtration_on_completion", 0));
    }

    // Filament
    bool pa = m_config->opt_bool("enable_pressure_advance", variant_index);
    toggle_option("pressure_advance", pa, 256 + variant_index);
    DynamicConfig &proj_cfg = bundle->project_config;
    std::string bed_temp_1st_layer_key;
    if (proj_cfg.has("curr_bed_type"))
        bed_temp_1st_layer_key = get_bed_temp_1st_layer_key(proj_cfg.opt_enum<BedType>("curr_bed_type"));
    const std::vector<std::string> bed_temp_keys = {"supertack_plate_temp_initial_layer", "cool_plate_temp_initial_layer",
                                                    "textured_cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer",
                                                    "textured_plate_temp_initial_layer", "hot_plate_temp_initial_layer"};
    bool support_multi_bed_types = std::find(bed_temp_keys.begin(), bed_temp_keys.end(), bed_temp_1st_layer_key) == bed_temp_keys.end() ||
                                   is_BBL_printer || printer_cfg.opt_bool("support_multi_bed_types");
    for (const auto &key : bed_temp_keys)
        toggle_line(key, support_multi_bed_types || bed_temp_1st_layer_key == key);
    toggle_option("adaptive_pressure_advance", pa, 0);
    toggle_option("adaptive_pressure_advance_overhangs", pa, 0);
    bool has_adaptive_pa = m_config->opt_bool("adaptive_pressure_advance", variant_index);
    toggle_line("adaptive_pressure_advance_overhangs", has_adaptive_pa && pa, 0);
    toggle_line("adaptive_pressure_advance_model", has_adaptive_pa && pa, 0);
    toggle_line("adaptive_pressure_advance_bridges", has_adaptive_pa && pa, 0);
    bool is_pellet_printer = printer_cfg.opt_bool("pellet_modded_printer");
    toggle_line("pellet_flow_coefficient", is_pellet_printer);
    toggle_line("filament_diameter", !is_pellet_printer);
    toggle_line("activate_chamber_temp_control", printer_cfg.opt_bool("support_chamber_temp_control"));
    if (m_config->has("volumetric_speed_coefficients"))
        toggle_option("filament_adaptive_volumetric_speed", m_config->opt_string("volumetric_speed_coefficients", variant_index) != "0 0 0 0 0 0", 256 + variant_index);

    // Setting Overrides: TabFilament::update_filament_overrides_page
    {
        const bool use_firmware_retraction = printer_cfg.opt_bool("use_firmware_retraction");
        const int  extruder_idx = 0;
        auto is_checked = [&](const std::string &k) { auto *o = m_config->option(k); return o && !o->is_nil(); };
        for (const std::string opt_key : {"filament_retraction_length", "filament_z_hop", "filament_z_hop_types", "filament_retract_lift_above",
                                          "filament_retract_lift_below", "filament_retract_lift_enforce", "filament_retraction_speed",
                                          "filament_deretraction_speed", "filament_retract_restart_extra", "filament_retraction_minimum_travel",
                                          "filament_retract_when_changing_layer", "filament_wipe", "filament_wipe_distance",
                                          "filament_retract_before_wipe", "filament_retract_after_wipe", "filament_long_retractions_when_cut",
                                          "filament_retraction_distances_when_cut"}) {
            if (!m_config->has(opt_key)) continue;
            bool is_retraction_option = opt_key == "filament_retraction_length";
            bool enable = !use_firmware_retraction || !is_retraction_option;
            (void)extruder_idx;
            toggle_option(opt_key, enable && is_checked(opt_key));
        }
    }

    // Multimaterial
    toggle_option("filament_minimal_purge_on_wipe_tower", !is_BBL_printer, 256 + variant_index);
    for (auto el : {"filament_loading_speed_start", "filament_loading_speed", "filament_unloading_speed_start", "filament_unloading_speed",
                    "filament_toolchange_delay", "filament_cooling_moves", "filament_cooling_initial_speed", "filament_cooling_final_speed"})
        toggle_option(el, !is_BBL_printer);
    bool multitool_ramming = m_config->opt_bool("filament_multitool_ramming", variant_index);
    toggle_option("filament_multitool_ramming_volume", multitool_ramming, 256 + variant_index);
    toggle_option("filament_multitool_ramming_flow", multitool_ramming, 256 + variant_index);
    bool is_BBL_multi_extruder = is_BBL_printer && printer_cfg.option<ConfigOptionFloats>("nozzle_diameter")->size() > 1;
    toggle_line("long_retractions_when_ec", is_BBL_multi_extruder, 256 + variant_index);
    toggle_line("retraction_distances_when_ec", is_BBL_multi_extruder && m_config->opt_bool("long_retractions_when_ec", variant_index), 256 + variant_index);
}

// TabPrinter::toggle_options (FFF, all pages; the Extruder page is reported per extruder as key#<extruder>)
static void toggle_printer(Toggles &t, DynamicPrintConfig *m_config, int mode)
{
    PresetBundle *bundle = Core::get().preset_bundle.get();
    const bool is_BBL_printer = bundle->is_bbl_vendor();
    auto toggle_option = [&](const std::string &k, bool on, int idx = -1) { t.field(k, on, idx); };
    auto toggle_line   = [&](const std::string &k, bool on, int idx = -1) { t.line(k, on, idx); };
    const bool have_multiple_extruders = true;

    // Basic information
    {
        const auto &printer_cfg = *m_config;
        for (auto el : {"scan_first_layer", "bbl_calib_mark_logo", "bbl_use_printhost"}) toggle_line(el, is_BBL_printer);
        for (auto el : {"use_firmware_retraction", "use_relative_e_distances", "support_multi_bed_types", "pellet_modded_printer", "bed_mesh_max",
                        "bed_mesh_min", "bed_mesh_probe_distance", "adaptive_bed_mesh_margin", "thumbnails"})
            toggle_line(el, !is_BBL_printer);
        bool gcf_is_marlin_firmware = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value == GCodeFlavor::gcfMarlinFirmware;
        toggle_line("enable_power_loss_recovery", is_BBL_printer || gcf_is_marlin_firmware);
        bool is_belt = m_config->opt_bool("belt_printer");
        toggle_option("build_plate_tilt_x", !is_belt);
        toggle_option("build_plate_tilt_y", !is_belt);
        bool expert_or_above = mode >= comExpert;
        toggle_line("belt_printer_infinite_y", is_belt);
        toggle_line("belt_slice_rotation_angle", is_belt);
        toggle_line("belt_slice_rotation", is_belt);
        for (auto el : {"gcode_remap_x", "gcode_remap_y", "gcode_remap_z"}) toggle_line(el, is_belt);
        auto rot_axis = m_config->option<ConfigOptionEnum<BeltRotationAxis>>("belt_slice_rotation")->value;
        toggle_option("belt_slice_rotation_angle", is_belt && rot_axis != BeltRotationAxis::None);
        toggle_line("belt_frame_tilt_decouple", is_belt && expert_or_above);
        toggle_line("belt_frame_tilt_angle", is_belt && expert_or_above && m_config->opt_bool("belt_frame_tilt_decouple"));
        toggle_line("belt_support_floor_offset", is_belt);
        toggle_line("parallel_printheads_count", printer_cfg.opt_bool("support_parallel_printheads"));
        toggle_line("fan_direction", m_config->opt_bool("auxiliary_fan"));
        toggle_line("support_air_filtration", !m_config->opt_bool("support_cooling_filter"));
        toggle_line("cooling_filter_enabled", m_config->opt_bool("support_cooling_filter"));
    }
    // Machine G-code
    {
        std::string printer_type = bundle->printers.get_edited_preset().get_printer_type(bundle);
        toggle_line("wrapping_detection_gcode", DevPrinterConfigUtil::support_wrapping_detection(printer_type));
    }
    // Multimaterial
    {
        const bool is_belt_printer = m_config->opt_bool("belt_printer");
        toggle_line("enable_belt_purge_tower", is_belt_printer);
        const bool supports_wipe_tower_2 = !is_BBL_printer && m_config->opt_enum<WipeTowerType>("wipe_tower_type") == WipeTowerType::Type2;
        toggle_line("wipe_tower_type", !is_BBL_printer && !is_belt_printer);
        for (auto el : {"enable_filament_ramming", "cooling_tube_retraction", "cooling_tube_length", "parking_pos_retraction", "extra_loading_move",
                        "high_current_on_filament_swap"})
            toggle_option(el, supports_wipe_tower_2);
        auto bSEMM = m_config->opt_bool("single_extruder_multi_material");
        if (!bSEMM && m_config->opt_bool("manual_filament_change"))
            reset_option(*m_config, "manual_filament_change", new ConfigOptionBool(false));
        toggle_option("extruders_count", !bSEMM);
        toggle_option("manual_filament_change", bSEMM);
        toggle_option("purge_in_prime_tower", bSEMM && supports_wipe_tower_2);
        bool is_imex = m_config->opt_bool("is_imex");
        for (auto el : {"imex_gantry_count", "imex_tools_per_gantry", "imex_nozzle_clearance_x", "imex_nozzle_clearance_y", "imex_carriage_margin",
                        "imex_tool_layout", "imex_viz_theme", "imex_firmware_managed_zones"})
            toggle_option(el, is_imex);
        const size_t extruders_count = m_config->option<ConfigOptionFloats>("nozzle_diameter")->size();
        toggle_option("tool_change_on_wipe_tower", !bSEMM && supports_wipe_tower_2 && extruders_count > 1);
        toggle_option("wait_for_temp_on_wipe_tower", !bSEMM && supports_wipe_tower_2 && extruders_count > 1);
    }
    // Extruder pages: one per extruder; i is the extruder (the variant index equals it for single-variant printers)
    const size_t extruders = m_config->option<ConfigOptionFloats>("nozzle_diameter")->size();
    for (size_t i = 0; i < extruders; ++i) {
        const int variant_index = int(i);
        bool have_retract_length = m_config->opt_float("retraction_length", variant_index) > 0;
        toggle_option("extruder_printable_area", false, i);
        toggle_line("extruder_printable_area", bundle->get_printer_extruder_count() == 2, i);
        toggle_option("extruder_printable_height", false, i);
        toggle_line("extruder_printable_height", bundle->get_printer_extruder_count() == 2, i);
        bool use_firmware_retraction = m_config->opt_bool("use_firmware_retraction");
        toggle_option("retract_length", !use_firmware_retraction, i);
        toggle_option("retraction_minimum_travel", have_retract_length || use_firmware_retraction, i);
        bool retraction = have_retract_length || use_firmware_retraction;
        for (auto el : {"z_hop", "retract_when_changing_layer"}) toggle_option(el, retraction, i);
        for (auto el : {"retract_lift_above", "retract_lift_below", "retract_lift_enforce"})
            toggle_option(el, retraction && (m_config->opt_float("z_hop", variant_index) > 0), i);
        for (auto el : {"retraction_speed", "deretraction_speed", "retract_before_wipe", "retract_after_wipe", "retract_length",
                        "retract_restart_extra", "wipe_distance"})
            toggle_option(el, retraction && !use_firmware_retraction, i);
        bool   wipe                = retraction && m_config->opt_bool("wipe", variant_index);
        double retract_before_wipe = m_config->option<ConfigOptionPercents>("retract_before_wipe")->get_at(variant_index);
        double retract_after_wipe  = m_config->option<ConfigOptionPercents>("retract_after_wipe")->get_at(variant_index);
        toggle_option("retract_before_wipe", wipe && !is_approx(retract_after_wipe, 100.), i);
        toggle_option("retract_after_wipe", wipe && !is_approx(retract_before_wipe, 100.), i);
        if (use_firmware_retraction && wipe && retract_before_wipe < 100.0) {
            GUI::MessageDialog dialog(nullptr,
                _L("The Retract before wipe option could be only 100% when using the Firmware Retraction mode.\n"
                   "\nShall I set it to 100% in order to enable Firmware Retraction?"),
                _L("Firmware Retraction"), wxICON_WARNING | wxYES | wxNO);
            if (dialog.ShowModal() == wxID_YES) {
                auto w   = static_cast<ConfigOptionBools *>(m_config->option("wipe")->clone());
                auto rbw = static_cast<ConfigOptionPercents *>(m_config->option("retract_before_wipe")->clone());
                for (size_t k = 0; k < w->values.size(); k++) { w->values[k] = false; rbw->values[k] = 100.0; }
                m_config->set_key_value("wipe", w);
                m_config->set_key_value("retract_before_wipe", rbw);
            } else
                m_config->set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
        }
        toggle_option("wipe_distance", wipe, i);
        toggle_option("retract_length_toolchange", have_multiple_extruders, i);
        bool toolchange_retraction = m_config->opt_float("retract_length_toolchange", variant_index) > 0;
        toggle_option("retract_restart_extra_toolchange", have_multiple_extruders && toolchange_retraction, i);
        toggle_option("long_retractions_when_cut", !use_firmware_retraction && m_config->opt_int("enable_long_retraction_when_cut"), i);
        toggle_line("retraction_distances_when_cut", m_config->opt_bool("long_retractions_when_cut", variant_index), i);
        toggle_option("travel_slope", m_config->opt_enum("z_hop_types", variant_index) != ZHopType::zhtNormal, i);
    }
    // Motion ability
    {
        auto gcf = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
        const bool gcf_is_marlin_legacy = gcf == GCodeFlavor::gcfMarlinLegacy, gcf_is_marlin_firmware = gcf == GCodeFlavor::gcfMarlinFirmware;
        const bool gcf_is_klipper = gcf == GCodeFlavor::gcfKlipper, gcf_is_reprap_firmware = gcf == GCodeFlavor::gcfRepRapFirmware;
        bool silent_mode = m_config->opt_bool("silent_mode");
        int  max_field   = silent_mode ? 2 : 1;
        for (int i = 0; i < max_field; ++i) toggle_option("machine_max_acceleration_travel", !gcf_is_marlin_legacy && !gcf_is_klipper, i);
        toggle_line("machine_max_acceleration_travel", !gcf_is_marlin_legacy && !gcf_is_klipper);
        for (int i = 0; i < max_field; ++i) toggle_option("machine_max_junction_deviation", gcf_is_marlin_firmware, i);
        toggle_line("machine_max_junction_deviation", gcf_is_marlin_firmware);
        bool enable_jerk = !gcf_is_marlin_firmware;
        if (gcf_is_marlin_firmware) {
            const auto *jd = m_config->option<ConfigOptionFloats>("machine_max_junction_deviation");
            enable_jerk = !jd || std::all_of(jd->values.begin(), jd->values.end(), [](double v) { return v == 0.0; });
        }
        for (int i = 0; i < max_field; ++i)
            for (auto el : {"machine_max_jerk_x", "machine_max_jerk_y", "machine_max_jerk_z", "machine_max_jerk_e"}) toggle_option(el, enable_jerk, i);
        toggle_option("emit_machine_limits_to_gcode", gcf_is_marlin_legacy || gcf_is_marlin_firmware || gcf_is_reprap_firmware);
        bool resonance_avoidance = m_config->opt_bool("resonance_avoidance");
        toggle_option("min_resonance_avoidance_speed", resonance_avoidance);
        toggle_option("max_resonance_avoidance_speed", resonance_avoidance);
        bool input_shaping_compatible = gcf_is_marlin_firmware || gcf_is_reprap_firmware;
        for (auto is : {"input_shaping_emit", "input_shaping_type", "input_shaping_freq_x", "input_shaping_freq_y", "input_shaping_damp_x", "input_shaping_damp_y"})
            toggle_line(is, input_shaping_compatible);
        if (input_shaping_compatible) {
            bool emit = m_config->opt_bool("emit_machine_limits_to_gcode");
            toggle_option("input_shaping_emit", emit);
            bool ise = emit && m_config->opt_bool("input_shaping_emit");
            toggle_option("input_shaping_type", ise);
            toggle_option("input_shaping_freq_x", ise);
            toggle_option("input_shaping_freq_y", ise && !gcf_is_reprap_firmware);
            toggle_option("input_shaping_damp_x", ise);
            toggle_option("input_shaping_damp_y", ise && !gcf_is_reprap_firmware);
        }
    }
}

// TabPrinter::update_fff: values derived from others
static void update_printer_fff(DynamicPrintConfig *m_config)
{
    if (m_config->opt_bool("belt_printer")) {
        auto rot_axis = m_config->option<ConfigOptionEnum<BeltRotationAxis>>("belt_slice_rotation")->value;
        const auto tilt = BeltTransformPipeline::physical_tilt(rot_axis, m_config->opt_float("belt_slice_rotation_angle"));
        if (m_config->opt_float("build_plate_tilt_x") != tilt.tilt_x_deg) m_config->set_key_value("build_plate_tilt_x", new ConfigOptionFloat(tilt.tilt_x_deg));
        if (m_config->opt_float("build_plate_tilt_y") != tilt.tilt_y_deg) m_config->set_key_value("build_plate_tilt_y", new ConfigOptionFloat(tilt.tilt_y_deg));
    }
}

// TabPrint::toggle_options: enum lists that depend on other values
static json enum_restrictions(DynamicPrintConfig *m_config)
{
    json out = json::object();
    PresetBundle *bundle = Core::get().preset_bundle.get();
    auto restrict = [&](const char *key, const std::vector<int> &set) {
        const ConfigOptionDef *def = print_config_def.get(key);
        json vals = json::array();
        for (int i : set) if (i < int(def->enum_values.size())) vals.push_back(def->enum_values[i]);
        out[key] = vals;
    };
    if (m_config->has("support_type")) {
        auto support_type = m_config->opt_enum<SupportType>("support_type");
        restrict("support_style", is_tree(support_type) ? std::vector<int>{smsDefault, smsTreeSlim, smsTreeStrong, smsTreeHybrid, smsTreeOrganic}
                                                        : std::vector<int>{smsDefault, smsGrid, smsSnug});
    }
    if (m_config->has("brim_type")) {
        const auto *belt_opt = bundle->printers.get_edited_preset().config.option<ConfigOptionBool>("belt_printer");
        bool offer = (belt_opt && belt_opt->value) || m_config->opt_enum<BrimType>("brim_type") == btLeadingEdgeOnly;
        const ConfigOptionDef *def = print_config_def.get("brim_type");
        json vals = json::array();
        for (auto &v : def->enum_values) if (v != "leading_edge_only" || offer) vals.push_back(v);
        out["brim_type"] = vals;
    }
    if (m_config->has("wipe_tower_wall_type"))
        restrict("wipe_tower_wall_type", bundle->is_bbl_vendor() ? std::vector<int>{wtwRectangle, wtwRib} : std::vector<int>{wtwRectangle, wtwCone, wtwRib});
    return out;
}

// -------------------------------------------------------------------------------------------------------------------

static int current_mode()
{
    const std::string m = Core::get().app_config->get("user_mode");
    return m == "simple" ? comSimple : m == "advanced" ? comAdvanced : m == "develop" ? comDevelop : comExpert;
}

static json config_state(Preset::Type type)
{
    Core &core = Core::get();
    PresetCollection &presets = collection(type);
    Preset &edited = presets.get_edited_preset();
    DynamicPrintConfig *config = &edited.config;

    Toggles t;
    if (type == Preset::TYPE_PRINT) {
        auto cm = make_manipulation(t, config);
        cm.set_is_BBL_Printer(core.preset_bundle->is_bbl_vendor());
        cm.toggle_print_fff_options(config, 0, true);
    } else if (type == Preset::TYPE_FILAMENT)
        toggle_filament(t, config);
    else
        toggle_printer(t, config, current_mode());

    json values = json::object();
    for (const std::string &key : config->keys()) values[key] = value_json(config->option(key));

    json sys_diff = json::array(), dirty = json::array();
    for (auto &k : presets.current_dirty_options(true)) dirty.push_back(k);
    for (auto &k : presets.current_different_from_parent_options(true)) sys_diff.push_back(k);
    if (type == Preset::TYPE_PRINTER) {
        // TabPrinter's synthetic "extruders_count": the number of nozzles, dirty / not system like the Tab marks it
        auto count = [](const DynamicPrintConfig &c) { auto *nd = c.option<ConfigOptionFloats>("nozzle_diameter"); return nd ? nd->values.size() : size_t(0); };
        const size_t n = count(*config);
        values["extruders_count"] = std::to_string(n);
        if (n != count(presets.get_selected_preset().config)) dirty.push_back("extruders_count");
        if (const Preset *par = presets.get_selected_preset_parent(); par && n != count(par->config)) sys_diff.push_back("extruders_count");
    }
    const Preset *parent = presets.get_selected_preset_parent();
    json st = {{"type", type == Preset::TYPE_PRINT ? "print" : type == Preset::TYPE_FILAMENT ? "filament" : "printer"},
               {"preset", edited.name},
               {"label", edited.label(false)},
               {"is_system", edited.is_system},
               {"is_default", edited.is_default},
               {"is_user", edited.is_user()},
               {"has_parent", parent != nullptr},
               {"parent", parent ? parent->name : ""},
               {"dirty", dirty},
               {"sys_diff", sys_diff},
               {"enabled", t.enabled},
               {"visible", t.visible},
               {"labels", t.labels},
               {"values", values},
               {"mode", current_mode()},
               {"extruders", core.preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->size()}};
    if (type == Preset::TYPE_PRINT) st["enums"] = enum_restrictions(config);
    return st;
}

// GUI.cpp change_opt_value, from a string: whole option, or one element of a vector option
static void set_value(DynamicPrintConfig &config, const std::string &key, const json &value, int idx)
{
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
    ConfigOption *opt = config.option(key);
    if (!opt) throw CommandError("unknown option " + key);
    if (value.is_array()) {   // a whole vector as elements
        std::vector<std::string> v;
        for (auto &e : value) v.push_back(e.get<std::string>());
        std::string s = opt->type() == coStrings ? escape_strings_cstyle(v) : boost::algorithm::join(v, ",");
        config.set_deserialize_strict(key, s);
    } else if (opt->is_vector() && idx >= 0) {
        auto *vec = static_cast<ConfigOptionVectorBase *>(opt);
        std::vector<std::string> v = vec->vserialize();
        if (size_t(idx) >= v.size()) v.resize(idx + 1, v.empty() ? std::string() : v.back());
        v[idx] = value.get<std::string>();
        std::string s = opt->type() == coStrings ? escape_strings_cstyle(v) : boost::algorithm::join(v, ",");
        config.set_deserialize_strict(key, s);
    } else if (opt->type() == coString)
        static_cast<ConfigOptionString *>(opt)->value = value.get<std::string>();   // as the Field gives it
    else
        config.set_deserialize_strict(key, value.get<std::string>());
}

static json config_get(const json &a) { return config_state(preset_type(str_arg(a, "type"))); }

// Field change -> Tab::on_value_change -> Tab::update (TabPrint / TabFilament / TabPrinter)
static json config_set(const json &a)
{
    Core &core = Core::get();
    Preset::Type type = preset_type(str_arg(a, "type"));
    PresetCollection &presets = collection(type);
    DynamicPrintConfig *m_config = &presets.get_edited_preset().config;
    const std::string key = str_arg(a, "key");
    const int idx = a.value("index", -1);
    if (type == Preset::TYPE_PRINTER && key == "extruders_count") {
        // TabPrinter::extruders_count_changed + on_value_change("extruders_count"): the printer's per-extruder options
        // resized, and on a multi-tool printer the filament list follows (one filament per extruder)
        const int n = std::clamp(std::stoi(a["value"].is_string() ? a["value"].get<std::string>() : a["value"].dump()), 1, int(MAXIMUM_EXTRUDER_NUMBER));
        PresetBundle &bundle = *core.preset_bundle;
        if (n != int(m_config->option<ConfigOptionFloats>("nozzle_diameter")->values.size())) {
            bundle.on_extruders_count_changed(n);
            Plater::get().changed();
        } else if (n == 1 && bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() > 1)
            bundle.update_multi_material_filament_presets();
        if (!m_config->opt_bool("single_extruder_multi_material")) {
            size_t physical = bundle.num_physical_filaments();
            while (physical != size_t(n)) {
                bundle.set_num_filaments(unsigned(physical < size_t(n) ? physical + 1 : physical - 1),
                                         physical < size_t(n) ? next_filament_color() : std::string());
                const size_t updated = bundle.num_physical_filaments();
                if (updated == physical) break;
                physical = updated;
            }
        }
        presets.update_dirty();
        core.invalidate_full_config();
        presets_changed();
        Host::event("presets", presets_state());
        return config_state(type);
    }
    set_value(*m_config, key, a["value"], idx);

    Toggles t;
    auto cm = make_manipulation(t, m_config);
    cm.set_is_BBL_Printer(core.preset_bundle->is_bbl_vendor());

    // Tab::on_value_change (the parts that change values)
    // the preset's "plugins" manifest follows a plugin picker, so full_config() and save_to_json() find resolved
    // "name;uuid;capability" references; the overrides of a capability no longer referenced are dropped
    if (const ConfigOptionDef *opt_def = m_config->def()->get(key); opt_def && opt_def->is_plugin_backed()) {
        m_config->update_plugin_manifest();
        prune_stale_plugin_overrides(*m_config, Preset::plugin_overrides_key(type));
    }
    if (key == "pellet_flow_coefficient")
        m_config->set_key_value("filament_diameter", new ConfigOptionFloats{Preset::convert_pellet_flow_to_filament_diameter(m_config->opt_float("pellet_flow_coefficient", 0))});
    if (key == "filament_diameter")
        m_config->set_key_value("pellet_flow_coefficient", new ConfigOptionFloats{Preset::convert_filament_diameter_to_pellet_flow(m_config->opt_float("filament_diameter", 0))});
    auto ask_keep = [&](const char *msg, const char *opt, bool restore) {
        GUI::MessageDialog dlg(nullptr, translate(msg), translate("Warning"), wxICON_WARNING | wxYES | wxNO);
        if (dlg.ShowModal() == wxID_NO) m_config->set_key_value(opt, new ConfigOptionBool(restore));
    };
    if (key == "enable_prime_tower" && m_config->has("timelapse_type")) {
        const bool on = m_config->opt_bool("enable_prime_tower");
        bool timelapse_enabled = m_config->opt_enum<TimelapseType>("timelapse_type") == TimelapseType::tlSmooth;
        bool enable_wrapping = m_config->opt_bool("enable_wrapping_detection");
        if (!on) {
            auto *mnc = core.preset_bundle->printers.get_edited_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
            if (mnc && std::any_of(mnc->values.begin(), mnc->values.end(), [](int v) { return v > 1 && v != ConfigOptionIntsNullable::nil_value(); }))
                ask_keep("Prime tower is required for nozzle changing. There may be flaws on the model without prime tower. Are you sure you want to disable prime tower?", "enable_prime_tower", true);
        }
        if (!m_config->opt_bool("enable_prime_tower") && (timelapse_enabled || enable_wrapping)) {
            if (timelapse_enabled)
                ask_keep("A prime tower is required for smooth timelapse mode. There may be flaws on the model without a prime tower. Are you sure you want to disable the prime tower?", "enable_prime_tower", true);
            if (enable_wrapping && !m_config->opt_bool("enable_prime_tower"))
                ask_keep("A prime tower is required for clumping detection. There may be flaws on the model without prime tower. Are you sure you want to disable prime tower?", "enable_prime_tower", true);
        }
        if (m_config->opt_bool("enable_prime_tower") && on && m_config->opt_bool("precise_z_height"))
            ask_keep("Enabling both precise Z height and the prime tower may cause slicing errors. Do you still want to enable?", "enable_prime_tower", false);
    }
    if (key == "enable_wrapping_detection" && m_config->opt_bool("enable_wrapping_detection") && !m_config->opt_bool("enable_prime_tower"))
        ask_keep("A prime tower is required for clumping detection. There may be flaws on the model without prime tower. Do you still want to enable clumping detection?", "enable_wrapping_detection", false);
    if (key == "precise_z_height" && m_config->opt_bool("precise_z_height") && m_config->has("enable_prime_tower") && m_config->opt_bool("enable_prime_tower"))
        ask_keep("Enabling both precise Z height and the prime tower may cause slicing errors. Do you still want to enable precise Z height?", "precise_z_height", false);

    // TabPrinter / TabFilament::on_value_change: retract before + after wipe <= 100 %
    if (idx >= 0 && (key == "retract_after_wipe" || key == "retract_before_wipe" || key == "filament_retract_after_wipe" || key == "filament_retract_before_wipe")) {
        const std::string prefix = boost::starts_with(key, "filament_") ? "filament_" : "";
        auto *before = m_config->option<ConfigOptionPercents>(prefix + "retract_before_wipe");
        auto *after  = m_config->option<ConfigOptionPercents>(prefix + "retract_after_wipe");
        if (before && after && !before->is_nil(idx) && !after->is_nil(idx)) {
            before->values[idx] = std::clamp(before->values[idx], 0., 100.);
            after->values[idx]  = std::clamp(after->values[idx], 0., 100.);
            if (after->values[idx] > 100. - before->values[idx]) after->values[idx] = 100. - before->values[idx];
        }
    }

    // Tab::update
    if (type == Preset::TYPE_PRINT) {
        if (!cm.is_initialized_support_material_overhangs_queried()) {
            const Preset &sel = presets.get_selected_preset();
            cm.initialize_support_material_overhangs_queried(!sel.is_system && !sel.is_dirty && m_config->opt_bool("enable_support") &&
                                                             !m_config->opt_bool("detect_overhang_wall"));
        }
        cm.update_print_fff_config(m_config, true, false);
    } else if (type == Preset::TYPE_FILAMENT) {
        cm.check_filament_max_volumetric_speed(m_config);
        cm.check_filament_ironing_spacing(m_config);
        if (key == "nozzle_temperature" || key == "nozzle_temperature_range_low" || key == "nozzle_temperature_range_high")
            cm.check_nozzle_temperature_range(m_config, 0);
        if (key == "nozzle_temperature_initial_layer")
            cm.check_nozzle_temperature_initial_layer_range(m_config, 0);
        if (key == "chamber_temperatures" || key == "chamber_temperature")
            cm.check_chamber_temperature(m_config);
        if (key == "adaptive_pressure_advance_model")
            cm.check_adaptive_pressure_advance_model(m_config);
    } else
        update_printer_fff(m_config);

    presets.update_dirty();
    core.invalidate_full_config();
    if (type == Preset::TYPE_PRINTER && (key == "nozzle_diameter" || key == "extruders_count"))
        Host::event("presets", presets_state());
    schedule_auto_reslice_if_needed();   // Plater::on_config_change
    return config_state(type);
}

// the "reset to system value" / "reset to saved value" buttons of a row (Tab::on_roll_back_value)
static json config_reset(const json &a)
{
    Preset::Type type = preset_type(str_arg(a, "type"));
    PresetCollection &presets = collection(type);
    const std::string key = str_arg(a, "key");
    const bool to_system = a.value("to", "saved") == "system";
    const Preset *ref = to_system ? presets.get_selected_preset_parent() : &presets.get_selected_preset();
    if (!ref) throw CommandError("no reference preset");
    std::string base = key.substr(0, key.find('#'));
    DynamicPrintConfig &cfg = presets.get_edited_preset().config;
    if (const ConfigOption *src = ref->config.option(base)) {
        if (key.find('#') != std::string::npos && src->is_vector()) {
            int idx = std::stoi(key.substr(key.find('#') + 1));
            static_cast<ConfigOptionVectorBase *>(cfg.option(base))->set_at(src, idx, idx);
        } else
            cfg.set_key_value(base, src->clone());
    }
    presets.update_dirty();
    Core::get().invalidate_full_config();
    return config_state(type);
}

static json set_mode(const json &a)
{
    Core::get().app_config->set("user_mode", str_arg(a, "mode"));
    Core::get().app_config->save();
    return true;
}

// lists the generated layout expands at run time (gen_settings_layout.py RUNTIME_LISTS)
static json layout_lists(const json &)
{
    auto keys = [](const std::vector<PublishablePrinterOption> &v) {
        json a = json::array();
        for (auto &o : v) a.push_back(o.key);
        return a;
    };
    return {{"publishable_printer_retraction_options", keys(publishable_printer_retraction_options())},
            {"publishable_printer_z_hop_options", keys(publishable_printer_z_hop_options())}};
}

// ---------------------------------------------------------------------------------------------------------------
// Objects mode: the settings of one object / part / modifier (TabPrintObject / TabPrintPart, TabPrintModel): the
// process config with the item's overrides applied, limited to the keys such an item can override; a change is
// stored in the item's ModelConfig, "reset to system value" removes the override.

// a height range of an object ({"obj", "range": [min, max]}): TabPrintLayer
static bool is_layer_item(const json &a) { return a.contains("range") && a["range"].is_array(); }
static t_layer_height_range layer_range(const json &a) { return {a["range"][0].get<double>(), a["range"][1].get<double>()}; }

static ModelConfig *item_config(const json &a, bool &is_part)
{
    Model &m = Plater::get().model();
    int o = a.at("obj"), v = a.value("vol", -1);
    if (o < 0 || o >= int(m.objects.size())) throw CommandError("bad object");
    if (is_layer_item(a)) {
        is_part = true;   // a layer range overrides region options like a part
        auto &ranges = m.objects[o]->layer_config_ranges;
        auto it = ranges.find(layer_range(a));
        if (it == ranges.end()) throw CommandError("bad height range");
        return &it->second;
    }
    is_part = v >= 0;
    if (is_part) return &m.objects[o]->volumes.at(v)->config;
    return &m.objects[o]->config;
}

static std::vector<std::string> item_keys(bool is_part, bool is_layer = false)
{
    // TabPrintModel: intersect(Preset::print_options(), keys); TabPrintLayer: layer_height + the region keys
    std::vector<std::string> keys = PrintRegionConfig().keys();
    if (is_layer) keys.push_back("layer_height");
    if (!is_part) { auto ok = PrintObjectConfig().keys(); keys.insert(keys.end(), ok.begin(), ok.end()); }
    const auto &po = Preset::print_options();
    std::vector<std::string> out;
    for (auto &k : keys) if (std::find(po.begin(), po.end(), k) != po.end()) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

static json item_state(const json &a)
{
    Core &core = Core::get();
    bool is_part = false;
    ModelConfig *local = item_config(a, is_part);
    std::vector<std::string> keys = item_keys(is_part, is_layer_item(a));
    // TabPrintModel::update_model_config: the parent tab's config, then the local values
    DynamicPrintConfig config = core.preset_bundle->prints.get_edited_preset().config;
    // the object an item belongs to applies first (a part shows its object's values as its defaults)
    if (is_part) {
        const ModelObject *mo = Plater::get().model().objects[a.at("obj").get<int>()];
        config.apply(mo->config.get(), true);
    }
    config.apply(local->get(), true);
    Toggles t;
    auto cm = make_manipulation(t, &config);
    cm.set_is_BBL_Printer(core.preset_bundle->is_bbl_vendor());
    cm.toggle_print_fff_options(&config, 0, true);
    json values = json::object(), overridden = json::array();
    for (const std::string &k : keys)
        if (const ConfigOption *opt = config.option(k)) values[k] = value_json(opt);
    for (const std::string &k : local->keys())
        if (std::binary_search(keys.begin(), keys.end(), k)) overridden.push_back(k);
    return {{"type", "print"},
            {"preset", core.preset_bundle->prints.get_edited_preset().name},
            {"label", core.preset_bundle->prints.get_edited_preset().label(false)},
            {"item", is_layer_item(a) ? "layer" : is_part ? "part" : "object"},
            {"dirty", json::array()},
            {"sys_diff", overridden},
            {"has_parent", true},
            {"enabled", t.enabled},
            {"visible", t.visible},
            {"labels", t.labels},
            {"values", values},
            {"mode", current_mode()},
            {"enums", enum_restrictions(&config)},
            {"extruders", core.preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->size()}};
}

// TabPrintModel::on_value_change
static json item_set(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    bool is_part = false;
    ModelConfig *local = item_config(a, is_part);
    const std::string key = str_arg(a, "key");
    std::vector<std::string> keys = item_keys(is_part, is_layer_item(a));
    if (!std::binary_search(keys.begin(), keys.end(), key)) throw CommandError("not an object setting: " + key);
    p.take_snapshot((boost::format("Change Option %s") % key).str());
    DynamicPrintConfig config = core.preset_bundle->prints.get_edited_preset().config;
    config.apply(local->get(), true);
    set_value(config, key, a.at("value"), a.value("index", -1));
    local->apply_only(config, {key});
    local->touch();
    p.changed();
    return item_state(a);
}

static json item_reset(const json &a)
{
    Plater &p = Plater::get();
    bool is_part = false;
    ModelConfig *local = item_config(a, is_part);
    const std::string key = str_arg(a, "key");
    p.take_snapshot(std::string("Reset Options"));
    if (a.value("all", false)) {   // TabPrintModel::reset_model_config
        for (auto &k : item_keys(is_part, is_layer_item(a))) if (local->has(k)) local->erase(k);
    } else
        local->erase(key.substr(0, key.find('#')));
    if (is_layer_item(a) && !local->has("layer_height")) {
        // TabPrintLayer::notify_changed: a range always has its layer height, the object's by default
        const ModelObject *mo = Plater::get().model().objects[a.at("obj").get<int>()];
        const double lh = mo->config.has("layer_height") ? mo->config.opt_float("layer_height")
                                                          : Core::get().preset_bundle->prints.get_edited_preset().config.opt_float("layer_height");
        local->set_key_value("layer_height", new ConfigOptionFloat(lh));
    }
    p.changed();
    return item_state(a);
}

// BedShapePanel::load_stl: the STL projected onto the XY plane, its one outline as the bed shape
static json bedshape_load_stl(const json &a)
{
    const std::string file_name = str_arg(a, "path");
    if (!boost::algorithm::iends_with(a.value("name", file_name), ".stl")) throw CommandError(_u8L("Invalid file format."));
    Model model;
    try {
        model = Model::read_from_file(file_name);
    } catch (std::exception &) {
        throw CommandError(_u8L("Error: invalid model"));
    }
    auto mesh = model.mesh();
    auto expolygons = mesh.horizontal_projection();
    if (expolygons.size() == 0) throw CommandError(_u8L("The selected file contains no geometry."));
    if (expolygons.size() > 1) throw CommandError(_u8L("The selected file contains several disjointed areas. This is not supported."));
    auto polygon = expolygons[0].contour;
    polygon.make_counter_clockwise();
    json points = json::array();
    for (auto pt : polygon.points) {
        const Vec2d p = unscale(pt);
        points.push_back({p.x(), p.y()});
    }
    return points;
}

// the strings of BedShapeDialog
static json bedshape_strings(const json &)
{
    return {{"title", _u8L("Bed Shape")}, {"shape", _u8L("Shape")}, {"settings", _u8L("Settings")},
            {"rectangle", _u8L("Rectangular")}, {"circle", _u8L("Circular")}, {"custom", _u8L("Custom")},
            {"load_shape", _u8L("Load shape from STL...")}, {"texture", _u8L("Texture")}, {"model", _u8L("Model")},
            {"load", _u8L("Load...")}, {"remove", _u8L("Remove")}, {"none", _u8L("None")}, {"not_found", _u8L("Not found:")},
            {"invalid", _u8L("Invalid file format.")}};
}

static Commands reg({
    {"objconfig.get", item_state},
    {"objconfig.set", item_set},
    {"objconfig.reset", item_reset},
    {"layout.lists", layout_lists},
    {"bedshape.load_stl", bedshape_load_stl},
    {"bedshape.strings", bedshape_strings},
    {"config.schema", schema},
    {"config.get", config_get},
    {"config.set", config_set},
    {"config.reset", config_reset},
    {"config.set_mode", set_mode},
});

} // namespace orca
