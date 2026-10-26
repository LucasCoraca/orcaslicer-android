// The Calibration menu (MainFrame::run_calibration): the dialogs' fields are described for the app (calib.dialog), and
// OK runs the dialog's on_start — compiled from upstream calib_dlg.cpp — which validates the values and calls the
// Plater's calib_* function, also compiled from upstream Plater.cpp, against the small Plater stand-in below.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>
#include <boost/filesystem.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Slicer.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/calib.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

void presets_changed();

// what the slicer applies to each print (Print::set_calib_params of the desktop's background process)
static Calib_Params g_calib_params;
const Calib_Params &calib_params() { return g_calib_params; }
void reset_calib_params() { g_calib_params = Calib_Params(); Plater::get().model().calib_pa_pattern.reset(); }

namespace calib {

using Slic3r::GUI::MessageDialog;
using Slic3r::GUI::wxGetApp;
using Slic3r::GUI::format_wxstr;
using Slic3r::GUI::format;

#define wxCHECK_RET(cond, msg) if (!(cond)) return
struct wxBusyCursor {};
struct wxCommandEvent { int selection{0}; int GetSelection() const { return selection; } void Skip() {} };
constexpr int TAB_ID_PREPARE = 1;
const wxString wxEmptyString;

// ---- the widgets of the dialogs, holding what the app sent
struct TextCtrl
{
    wxString value;
    wxString GetValue() const { return value; }
    void     SetValue(const wxString &v) { value = v; }
};
struct TextInput { TextCtrl ctrl; bool enabled{true}; TextCtrl *GetTextCtrl() { return &ctrl; } void Enable(bool e) { enabled = e; } };
struct CheckBox { bool value{false}; bool enabled{true}; bool GetValue() const { return value; } void SetValue(bool v) { value = v; } void Enable(bool e) { enabled = e; } };
struct RadioGroup { int sel{0}; int GetSelection() const { return sel; } void SetSelection(int s) { sel = s; } };
using ComboBox = RadioGroup;

class Plater;

// ---- the Plater of the calib_* functions
struct NotificationManager { void push_notification(const std::string &text) { Host::event("toast", {{"text", text}}); } };
struct Tab { void update_dirty() {} void reload_config() {} void update_ui_from_settings() {} };
struct ObjectList
{
    void load_generic_subobject(const std::string &type, ModelVolumeType) { Core::get().call("object.add_shape", {{"shape", type}}); }
    void update_name_for_items() {}
    void add_object_to_list(size_t) {}
    void update_selections() {}
};
struct Sidebar { ObjectList list; ObjectList *obj_list() { return &list; } };

// PartPlate / PartPlateList, over the core's plates
struct PartPlate
{
    int index;
    Vec3d get_origin() const { return orca::Plater::get().plate_origin(index); }
    bool  empty() const { return orca::Plater::get().plates[index].instances.empty(); }
    std::vector<ModelObject *> get_objects_on_this_plate() const
    {
        orca::Plater &p = orca::Plater::get();
        std::set<int> objs;
        for (auto &[o, i] : p.plates[index].instances) objs.insert(o);
        std::vector<ModelObject *> out;
        for (int o : objs) out.push_back(p.model().objects[o]);
        return out;
    }
};
struct PartPlateList
{
    std::vector<PartPlate> plates;
    PartPlate *get_plate(int i)
    {
        orca::Plater &p = orca::Plater::get();
        if (i < 0 || i >= int(p.plates.size())) return nullptr;
        if (plates.size() < p.plates.size()) { plates.clear(); for (int k = 0; k < int(p.plates.size()); ++k) plates.push_back({k}); }
        return &plates[i];
    }
    PartPlate *get_curr_plate() { return get_plate(orca::Plater::get().current_plate); }
    int        get_curr_plate_index() const { return orca::Plater::get().current_plate; }
    int        create_plate()
    {
        orca::Plater &p = orca::Plater::get();
        p.resize_plates(p.plates.size() + 1);
        return int(p.plates.size()) - 1;
    }
    void add_to_plate(size_t, int, int) { orca::Plater::get().update_plates(); }
};

// Selection::scale of the objects just loaded (adjust_settings_for_flowrate_calib), around the selection's centre
struct TransformationType { void set_relative() {} };
struct Selection
{
    void setup_cache() {}
    void scale(const Vec3d &s, const TransformationType &)
    {
        Model &m = orca::Plater::get().model();
        BoundingBoxf3 bb;
        for (ModelObject *o : m.objects) bb.merge(o->instance_bounding_box(0));
        const Vec3d c = bb.center();
        for (ModelObject *o : m.objects)
            for (ModelInstance *i : o->instances) {
                i->set_scaling_factor(i->get_scaling_factor().cwiseProduct(s));
                Vec3d off = i->get_offset();
                i->set_offset(Vec3d(c.x() + (off.x() - c.x()) * s.x(), c.y() + (off.y() - c.y()) * s.y(), off.z()));
            }
        for (ModelObject *o : m.objects) { o->invalidate_bounding_box(); o->ensure_on_bed(); }
    }
};
struct GLCanvas3D { Selection sel; Selection &get_selection() { return sel; } void do_scale(const std::string &) {} };
static GLCanvas3D *calib_canvas() { static GLCanvas3D c; return &c; }
static Tab *get_tab(Preset::Type) { static Tab t; return &t; }

#include "slic3r/GUI/CalibUtils.inc"
#include "slic3r/GUI/ShaperTypes.inc"

class Plater
{
public:
    void calib_pa(const Calib_Params &params);
    void calib_flowrate(bool is_linear, int pass, InfillPattern pattern);
    void calib_temp(const Calib_Params &params);
    void calib_max_vol_speed(const Calib_Params &params);
    void calib_retraction(const Calib_Params &params);
    void calib_VFA(const Calib_Params &params);
    void calib_input_shaping_freq(const Calib_Params &params);
    void calib_input_shaping_damp(const Calib_Params &params);
    void Calib_Cornering(const Calib_Params &params);
    void _calib_pa_pattern_gen_gcode();

private:
    void _calib_apply_belt_mode();
    void _calib_pa_pattern(const Calib_Params &params);
    void _calib_pa_tower(const Calib_Params &params);
    void _calib_temp_belt_sectioned(const Calib_Params &params, double belt_angle_rad);
    void cut_horizontal(size_t obj_idx, size_t instance_idx, double z, ModelObjectCutAttributes attributes);

    // Plater::new_project: an empty project named after the test (the unsaved one saved first when wanted)
    int new_project(bool skip_confirm, bool, const wxString &project_name)
    {
        if (Core::get().call("project.new", {{"skip_confirm", skip_confirm}}).value("canceled", false)) return wxID_CANCEL;
        orca::Plater::get().project_name = project_name;
        reset_calib_params();
        return wxID_YES;
    }
    // Plater::add_model: the model only, no project config, one object
    bool add_model(bool, const std::string &path)
    {
        try {
            Core::get().call("project.load", {{"paths", {path}}, {"load_config", false}, {"ask_multi", false}});
        } catch (const std::exception &e) {
            Host::event("toast", {{"text", e.what()}});
            return false;
        }
        return !model().objects.empty();
    }
    Model &model() { return orca::Plater::get().model(); }
    void   changed_objects(const std::vector<size_t> &obj_idxs)
    {
        for (size_t i : obj_idxs)
            if (i < model().objects.size()) model().objects[i]->invalidate_bounding_box();
        orca::Plater::get().changed();
    }
    // Plater::apply_cut_object_to_model
    void apply_cut_object_to_model(size_t obj_idx, const ModelObjectPtrs &new_objects)
    {
        model().delete_object(obj_idx);
        for (ModelObject *o : new_objects) { model().add_object(*o); model().objects.back()->ensure_on_bed(); }
        orca::Plater::get().selection = {{0, -1, -1}};
        orca::Plater::get().changed();
    }
    void _calib_pa_select_added_objects() { orca::Plater::get().selection = {{0, -1, -1}}; }
    void select_prepare() { Host::event("calib_prepare", json::object()); }
    void set_calib_params(const Calib_Params &params) { g_calib_params = params; }
    PartPlateList &get_partplate_list() { static PartPlateList l; return l; }
    Sidebar &sidebar() { static Sidebar s; return s; }
    NotificationManager *get_notification_manager() { static NotificationManager n; return &n; }
};

#include "slic3r/GUI/CalibPlater.inc"

// ---- the dialogs (only what on_start reads)
struct PA_Calibration_Dlg
{
    Calib_Params m_params; RadioGroup *m_rbExtruderType, *m_rbMethod; TextInput *m_tiStartPA, *m_tiEndPA, *m_tiPAStep;
    CheckBox *m_cbPrintNum; TextInput *m_tiBMAccels, *m_tiBMSpeeds; Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
    void reset_params();
};
struct Temp_Calibration_Dlg
{
    Calib_Params m_params; RadioGroup *m_rbFilamentType, *m_rbModel; TextInput *m_tiStart, *m_tiEnd, *m_tiStep; CheckBox *m_cbResize;
    Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
    void on_filament_type_changed(wxCommandEvent &event);
};
struct MaxVolumetricSpeed_Test_Dlg
{
    Calib_Params m_params; TextInput *m_tiStart, *m_tiEnd, *m_tiStep; Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};
struct VFA_Test_Dlg
{
    Calib_Params m_params; TextInput *m_tiStart, *m_tiEnd, *m_tiStep; CheckBox *m_cbAutoAdjust, *m_cbResize; Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};
struct Retraction_Test_Dlg
{
    Calib_Params m_params; TextInput *m_tiStart, *m_tiEnd, *m_tiStep; Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};
struct Input_Shaping_Freq_Test_Dlg
{
    Calib_Params m_params; RadioGroup *m_rbModel, *m_rbType; TextInput *m_tiFreqStartX, *m_tiFreqEndX, *m_tiFreqStartY, *m_tiFreqEndY, *m_tiDampingFactor;
    Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};
struct Input_Shaping_Damp_Test_Dlg
{
    Calib_Params m_params; RadioGroup *m_rbModel, *m_rbType; TextInput *m_tiFreqX, *m_tiFreqY, *m_tiDampingFactorStart, *m_tiDampingFactorEnd;
    Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};
struct Cornering_Test_Dlg
{
    Calib_Params m_params; RadioGroup *m_rbModel; TextInput *m_tiJDStart, *m_tiJDEnd; Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};
struct FlowRateCalibrationDialog
{
    RadioGroup *m_rbType; ComboBox *m_rbPattern; Plater *m_plater; bool closed{false};
    void on_start(wxCommandEvent &event);
};

#include "slic3r/GUI/CalibDialogs.inc"

// ---- the dialogs' fields for the app (the constructors' labels and defaults)
json text(const std::string &key, const std::string &label, const std::string &value, const std::string &unit)
{
    return {{"kind", "text"}, {"key", key}, {"label", label}, {"value", value}, {"unit", unit}};
}
json radio(const std::string &key, const std::string &label, std::vector<std::string> options, int sel = 0)
{
    return {{"kind", "radio"}, {"key", key}, {"label", label}, {"options", options}, {"value", sel}};
}
json check(const std::string &key, const std::string &label, bool value, const std::string &tip = "")
{
    return {{"kind", "check"}, {"key", key}, {"label", label}, {"value", value}, {"tooltip", tip}};
}
json note(const std::string &textv) { return {{"kind", "note"}, {"label", textv}}; }

std::string fmt3(double v) { char b[32]; snprintf(b, sizeof(b), "%.3f", v); return b; }
std::string num(double v) { return wxString::FromDouble(v); }

const DynamicPrintConfig &printer_cfg() { return Core::get().preset_bundle->printers.get_edited_preset().config; }
GCodeFlavor flavor()
{
    const auto *o = printer_cfg().option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    return o ? o->value : gcfMarlinLegacy;
}
bool junction_deviation()
{
    const auto *jd = printer_cfg().option<ConfigOptionFloats>("machine_max_junction_deviation");
    return flavor() == gcfMarlinFirmware && jd && !jd->values.empty() && jd->values[0] > 0;
}
std::string firmware_note()
{
    switch (flavor()) {
    case gcfMarlinFirmware:
    case gcfMarlinLegacy: return _u8L("Marlin version => 2.1.2\nFixed-Time motion not yet implemented.");
    case gcfKlipper: return _u8L("Klipper version => 0.9.0");
    case gcfRepRapFirmware: return _u8L("RepRap firmware version => 3.4.0\nCheck your firmware documentation for supported shaper types.");
    default: return _u8L("Please ensure the selected type is compatible with your firmware version.");
    }
}
const std::string scale_tip()
{
    return _u8L("This model is designed around a 0.4 mm nozzle with a 0.2 mm layer height. \n"
                "When the scaling option is enabled (recommended), it dynamically resizes to match your current nozzle diameter"
                " and an appropriate layer height, making the test both accurate and easy to read.\n"
                "Turn scaling off only if you wish to print the reference model exactly as-is.");
}

json dialog(const json &a)
{
    const std::string kind = str_arg(a, "kind");
    json fields = json::array();
    std::string title, wiki;
    const bool belt = is_belt_printer_selected();
    if (kind == "pa") {
        title = _u8L("PA Calibration"); wiki = "https://www.orcaslicer.com/wiki/pressure_advance_calib";
        fields.push_back(radio("extruder_type", _u8L("Extruder type"), {_u8L("DDE"), _u8L("Bowden")}));
        fields.push_back(radio("method", _u8L("Method"), {_u8L("PA Tower"), _u8L("PA Line"), _u8L("PA Pattern")}));
        // reset_params: the defaults per method / extruder type (the app applies them)
        fields.push_back(text("start", _u8L("Start PA: "), "0", ""));
        fields.push_back(text("end", _u8L("End PA: "), "0.1", ""));
        fields.push_back(text("step", _u8L("PA step: "), "0.002", ""));
        fields.push_back(check("print_numbers", _u8L("Print numbers"), false));
        fields.push_back(text("accels", _u8L("Accelerations: "), "", ""));
        fields.push_back(text("speeds", _u8L("Speeds: "), "", ""));
        if (belt) fields.push_back(note(_u8L("Not enabled yet on belt printers — use the PA Tower method instead.")));
    } else if (kind == "temp") {
        title = _u8L("Temperature calibration"); wiki = "https://www.orcaslicer.com/wiki/temp_calib";
        fields.push_back(radio("filament_type", _u8L("Filament type"),
                               {_u8L("PLA"), _u8L("ABS/ASA"), _u8L("PETG"), _u8L("PCTG"), _u8L("TPU"), _u8L("PA-CF"), _u8L("PET-CF"), _u8L("Custom")}));
        if (belt) fields.push_back(radio("model", _u8L("Test model"), {_u8L("Standard"), _u8L("Overhang")}));
        fields.push_back(text("start", _u8L("Start temp: "), "230", "℃"));
        fields.push_back(text("end", _u8L("End temp: "), "190", "℃"));
        fields.push_back(text("step", _u8L("Temp step: "), "5", "℃"));
        fields.push_back(check("resize", _u8L("Auto-scale for nozzle"), true, scale_tip()));
    } else if (kind == "max_flowrate") {
        title = _u8L("Max volumetric speed test"); wiki = "https://www.orcaslicer.com/wiki/volumetric_speed_calib";
        fields.push_back(text("start", _u8L("Start volumetric speed: "), "5", "mm³/s"));
        fields.push_back(text("end", _u8L("End volumetric speed: "), "20", "mm³/s"));
        fields.push_back(text("step", _u8L_CONTEXT("Step", "Increment") + ": ", "0.5", "mm³/s"));
    } else if (kind == "vfa") {
        title = _u8L("VFA test"); wiki = "https://www.orcaslicer.com/wiki/vfa_calib";
        fields.push_back(text("start", _u8L("Start speed: "), "40", "mm/s"));
        fields.push_back(text("end", _u8L("End speed: "), "200", "mm/s"));
        fields.push_back(text("step", _u8L_CONTEXT("Step", "Increment") + ": ", "10", "mm/s"));
        fields.push_back(check("resize", _u8L("Auto-scale for nozzle"), true, scale_tip()));
        fields.push_back(check("auto_adjust", _u8L("Auto-adjust to max volumetric speed"), true));
    } else if (kind == "retraction") {
        title = _u8L("Retraction"); wiki = "https://www.orcaslicer.com/wiki/retraction_calib";
        fields.push_back(text("start", _u8L("Start retraction length: "), "0", "mm"));
        fields.push_back(text("end", _u8L("End retraction length: "), "2", "mm"));
        fields.push_back(text("step", _u8L_CONTEXT("Step", "Increment") + ": ", "0.1", "mm"));
    } else if (kind == "is_freq" || kind == "is_damp") {
        const bool rrf = flavor() == gcfRepRapFirmware;
        title = kind == "is_freq" ? _u8L("Input shaping Frequency test") : _u8L("Input shaping Damp test");
        wiki = "https://www.orcaslicer.com/wiki/input_shaping_calib";
        fields.push_back(radio("model", _u8L("Test model"), {_u8L("Ringing Tower"), _u8L("Fast Tower")}));
        std::vector<std::string> types = get_shaper_type_values();
        fields.push_back(radio("type", _u8L("Input shaper type"), types));
        fields.push_back(note(firmware_note()));
        if (kind == "is_freq") {
            fields.push_back(text("x_start", rrf ? _u8L("Frequency (Start / End): ") : _u8L("X Start / End: "), "15", "Hz"));
            fields.push_back(text("x_end", "", "110", "Hz"));
            if (!rrf) { fields.push_back(text("y_start", _u8L("Y Start / End: "), "15", "Hz")); fields.push_back(text("y_end", "", "110", "Hz")); }
            fields.push_back(text("damp", _u8L("Damp: "), fmt3(0.15), ""));
            fields.push_back(note(_u8L("Recommended: Set Damp to 0.\nThis will use the printer's default or saved value.")));
        } else {
            fields.push_back(text("x", rrf ? _u8L("Frequency: ") : _u8L("Frequency X / Y: "), "30", "Hz"));
            if (!rrf) fields.push_back(text("y", "", "30", "Hz"));
            fields.push_back(text("damp_start", _u8L("Damp (Start / End): "), fmt3(0.0), ""));
            fields.push_back(text("damp_end", "", fmt3(0.4), ""));
        }
    } else if (kind == "cornering") {
        title = _u8L("Cornering test"); wiki = "https://www.orcaslicer.com/wiki/cornering_calib";
        fields.push_back(radio("model", _u8L("Test model"), {_u8L("Ringing Tower"), _u8L("Fast Tower"), _u8L("SCV-V2")}));
        const bool jd = junction_deviation();
        fields.push_back(text("start", _u8L("Start: "), jd ? fmt3(0.0) : fmt3(1.0), jd ? "mm" : "mm/s"));
        fields.push_back(text("end", _u8L("End: "), jd ? fmt3(0.25) : fmt3(15.0), jd ? "mm" : "mm/s"));
        std::string n = _u8L("Note: Lower values = sharper corners but slower speeds.");
        if (flavor() == gcfMarlinFirmware)
            n += "\n" + (jd ? _u8L("Marlin 2 Junction Deviation detected:\nTo test Classic Jerk, set 'Maximum Junction Deviation' in Motion ability to 0.")
                            : _u8L("Marlin 2 Classic Jerk detected:\nTo test Junction Deviation, set 'Maximum Junction Deviation' in Motion ability to a value > 0."));
        else if (flavor() == gcfRepRapFirmware)
            n += "\n" + _u8L("RepRap detected: Jerk in mm/s.\nOrcaSlicer will convert the values to mm/min when necessary.");
        fields.push_back(note(n));
    } else if (kind == "flowrate") {
        title = _u8L("Flow Ratio Calibration"); wiki = "https://www.orcaslicer.com/wiki/flow_rate_calib";
        fields.push_back(radio("type", _u8L("Calibration Test Type"),
                               {_u8L("Pass 1 (Coarse)"), _u8L("Pass 2 (Fine)"), _u8L("YOLO (Recommended)"), _u8L("YOLO (Perfectionist)")}, 2));
        fields.push_back(radio("pattern", _u8L("Top Surface Pattern"), {_u8L("Archimedean Chords"), _u8L("Monotonic")}));
    } else
        throw CommandError("unknown calibration " + kind);
    return {{"kind", kind}, {"title", title}, {"fields", fields}, {"wiki", wiki}, {"ok", _u8L("OK")}, {"wiki_label", _u8L("Wiki Guide")}};
}

// a choice that changes other fields: PA_Calibration_Dlg::reset_params (extruder type, method),
// Temp_Calibration_Dlg::on_filament_type_changed
json update(const json &a)
{
    const std::string kind = str_arg(a, "kind");
    json v = a.at("values");
    auto t = [&](const char *k) { auto *x = new TextInput(); if (v.contains(k)) x->ctrl.value = v[k].get<std::string>(); return std::unique_ptr<TextInput>(x); };
    auto c = [&](const char *k) { auto *x = new CheckBox(); x->value = v.value(k, false); return std::unique_ptr<CheckBox>(x); };
    auto r = [&](const char *k) { auto *x = new RadioGroup(); x->sel = v.value(k, 0); return std::unique_ptr<RadioGroup>(x); };
    json enabled = json::object();
    if (kind == "pa") {
        auto et = r("extruder_type"), m = r("method");
        auto s0 = t("start"), e = t("end"), st = t("step"), a1 = t("accels"), sp = t("speeds");
        auto pn = c("print_numbers");
        PA_Calibration_Dlg d{{}, et.get(), m.get(), s0.get(), e.get(), st.get(), pn.get(), a1.get(), sp.get(), nullptr};
        d.reset_params();
        v["start"] = s0->ctrl.value; v["end"] = e->ctrl.value; v["step"] = st->ctrl.value; v["print_numbers"] = pn->value;
        enabled = {{"print_numbers", pn->enabled}, {"accels", a1->enabled}, {"speeds", sp->enabled}};
    } else if (kind == "temp") {
        auto s0 = t("start"), e = t("end");
        Temp_Calibration_Dlg d{{}, nullptr, nullptr, s0.get(), e.get(), nullptr, nullptr, nullptr};
        wxCommandEvent ev;
        ev.selection = v.value("filament_type", 0);
        d.on_filament_type_changed(ev);
        v["start"] = s0->ctrl.value; v["end"] = e->ctrl.value;
    }
    return {{"values", v}, {"enabled", enabled}};
}

// OK: the dialog's on_start with the app's values
json run(const json &a)
{
    const std::string kind = str_arg(a, "kind");
    const json &v = a.at("values");
    auto t = [&](const char *k) { auto *x = new TextInput(); if (v.contains(k)) x->ctrl.value = v[k].get<std::string>(); return x; };
    auto c = [&](const char *k) { auto *x = new CheckBox(); x->value = v.value(k, false); return x; };
    auto r = [&](const char *k) { auto *x = new RadioGroup(); x->sel = v.value(k, 0); return x; };
    Plater plater;
    wxCommandEvent ev;
    bool closed = false;
    std::vector<std::unique_ptr<void, void (*)(void *)>> keep;
    if (kind == "pa") {
        // MainFrame::m_pa_calib_dlg lives on: the PA pattern keeps a reference to the dialog's m_params
        static PA_Calibration_Dlg d;
        d = PA_Calibration_Dlg{{}, r("extruder_type"), r("method"), t("start"), t("end"), t("step"), c("print_numbers"), t("accels"), t("speeds"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "temp") {
        Temp_Calibration_Dlg d{{}, r("filament_type"), v.contains("model") ? r("model") : nullptr, t("start"), t("end"), t("step"), c("resize"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "max_flowrate") {
        MaxVolumetricSpeed_Test_Dlg d{{}, t("start"), t("end"), t("step"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "vfa") {
        VFA_Test_Dlg d{{}, t("start"), t("end"), t("step"), c("auto_adjust"), c("resize"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "retraction") {
        Retraction_Test_Dlg d{{}, t("start"), t("end"), t("step"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "is_freq") {
        Input_Shaping_Freq_Test_Dlg d{{}, r("model"), r("type"), t("x_start"), t("x_end"), t("y_start"), t("y_end"), t("damp"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "is_damp") {
        Input_Shaping_Damp_Test_Dlg d{{}, r("model"), r("type"), t("x"), t("y"), t("damp_start"), t("damp_end"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "cornering") {
        Cornering_Test_Dlg d{{}, r("model"), t("start"), t("end"), &plater};
        d.on_start(ev); closed = d.closed;
    } else if (kind == "flowrate") {
        FlowRateCalibrationDialog d{r("type"), r("pattern"), &plater};
        d.on_start(ev); closed = d.closed;
    } else
        throw CommandError("unknown calibration " + kind);
    // the Tabs' reload_config: the edited presets changed
    if (closed) presets_changed();
    json st = orca::Plater::get().state();
    st["closed"] = closed;
    return st;
}

} // namespace calib

// Plater::reslice: the PA pattern's custom G-code is generated for the plate before slicing
void calib_before_slice()
{
    if (Plater::get().model().calib_pa_pattern) {
        calib::Plater p;
        p._calib_pa_pattern_gen_gcode();
    }
}

static Commands reg({
    {"calib.dialog", calib::dialog},
    {"calib.run", calib::run},
    {"calib.update", calib::update},
});

} // namespace orca
