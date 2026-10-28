// The printer combo's "Create printer" (CreatePrinterPresetDialog, opened by Sidebar::create_printer_preset): a new printer
// preset, or a nozzle for an existing printer, with filament and process presets cloned from a vendor's or from templates.
// The input checks, the vendor / model tables and the preset loading are compiled from upstream CreatePresetsDialog.cpp;
// the dialog's combo boxes, text fields, check boxes and radio boxes are stand-ins the app's dialog fills.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <openssl/evp.h>
#include <openssl/md5.h>

#include "Core.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "wx/arrstr.h"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/Utils/FileHelp.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace orca {

void presets_changed();   // Presets.cpp

namespace {

#include "slic3r/GUI/CreatePresetsHelpers.inc"
#include "slic3r/GUI/CreatePrinterTables.inc"

// the dialog's widgets, as far as the compiled methods read them
using wxColour = int;
#define DEFAULT_PROMPT_TEXT_COLOUR 0
const int  black_     = 0;
const int *wxBLACK    = &black_;
struct Combo
{
    wxArrayString items;
    int           sel{-1};
    wxString GetStringSelection() const { return sel >= 0 && sel < int(items.size()) ? items[sel] : wxString(); }
    void     Set(const wxArrayString &a) { items = a; sel = -1; }
    void     SetSelection(int i) { sel = i; }
    void     SetLabelColor(wxColour) {}
    void     select(const std::string &s) { sel = items.Index(s); }
};
struct Text { wxString value; wxString GetValue() const { return value; } };
struct Input { Text text; Text *GetTextCtrl() { return &text; } };
struct Check { bool value{false}; bool GetValue() const { return value; } };
struct RadioBox
{
    bool value{false}, enabled{true};
    bool GetValue() const { return value; }
    void SetValue(bool v) { value = v; }
    void Enable() { enabled = true; }
    void Disable() { enabled = false; }
};
// on_select_printer_model's event: where the call comes from
struct wxCommandEvent
{
    long extra{0};
    void SetExtraLong(long v) { extra = v; }
    long GetExtraLong() const { return extra; }
    void Skip() {}
};

struct CreatePrinter
{
    struct CreateType { wxString create_printer, create_nozzle, base_template, base_curr_printer; } m_create_type;

    Combo   select_vendor_, select_model_, select_printer_, nozzle_diameter_, printer_vendor_, printer_model_;
    Combo  *m_select_vendor{&select_vendor_}, *m_select_model{&select_model_}, *m_select_printer{&select_printer_},
           *m_nozzle_diameter{&nozzle_diameter_}, *m_printer_vendor{&printer_vendor_}, *m_printer_model{&printer_model_};
    Text    custom_vendor_, custom_model_, custom_nozzle_;
    Text   *m_custom_vendor_text_ctrl{&custom_vendor_}, *m_custom_model_text_ctrl{&custom_model_}, *m_custom_nozzle_diameter_ctrl{&custom_nozzle_};
    Input   bed_x_, bed_y_, origin_x_, origin_y_, height_;
    Input  *m_bed_size_x_input{&bed_x_}, *m_bed_size_y_input{&bed_y_}, *m_bed_origin_x_input{&origin_x_}, *m_bed_origin_y_input{&origin_y_},
           *m_print_height_input{&height_};
    Check   can_not_find_vendor_, can_not_find_nozzle_;
    Check  *m_can_not_find_vendor_combox{&can_not_find_vendor_}, *m_can_not_find_nozzle_checkbox{&can_not_find_nozzle_};
    RadioBox   type_printer_, type_nozzle_, base_template_, base_printer_;
    std::vector<std::pair<RadioBox *, wxString>> m_create_type_btns, m_create_presets_btns;

    std::string                                               m_custom_texture, m_custom_model;
    Preset                                                   *m_printer_preset{nullptr};
    VendorProfile                                             m_printer_preset_vendor_selected;
    VendorProfile::PrinterModel                               m_printer_preset_model_selected;
    std::unordered_map<std::string, std::shared_ptr<Preset>> m_printer_name_to_preset;
    std::vector<std::pair<Check *, Preset *>>                 m_filament_preset, m_process_preset;
    bool                                                      rewritten{false};
    bool                                                      closed{false};   // validate_input_valid switched to a system preset

    CreatePrinter()
    {
        m_create_type.create_printer    = _L("Create Printer");
        m_create_type.create_nozzle     = _L("Create Nozzle for Existing Printer");
        m_create_type.base_template     = _L("Create from Template");
        m_create_type.base_curr_printer = _L("Create Based on Current Printer");
        m_create_type_btns    = {{&type_printer_, m_create_type.create_printer}, {&type_nozzle_, m_create_type.create_nozzle}};
        m_create_presets_btns = {{&base_template_, m_create_type.base_template}, {&base_printer_, m_create_type.base_curr_printer}};
        type_printer_.value = base_template_.value = true;
        // create_printer_item / create_nozzle_diameter_item / create_bed_size_item ...: the combos' lists and the fields' defaults
        for (const std::string &vendor : printer_vendors) select_vendor_.items.Add(vendor);
        for (const std::string &nozzle : nozzle_diameter_vec) nozzle_diameter_.items.Add(nozzle + " mm");
        nozzle_diameter_.sel = 0;
        bed_x_.text.value = bed_y_.text.value = height_.text.value = "200";
        origin_x_.text.value = origin_y_.text.value = "0";
        VendorMap vendors;
        printer_vendor_.Set(get_exist_vendor_choices(vendors));
        set_current_visible_printer();
    }
    ~CreatePrinter()
    {
        clear_preset_combobox();
        delete m_printer_preset;
    }

    void select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx)
    {
        for (size_t i = 0; i < radiobox_list.size(); ++i) radiobox_list[i].first->SetValue(int(i) == btn_idx);
    }
    void update_preset_list_size() {}
    void clear_preset_combobox()
    {
        for (auto *list : {&m_filament_preset, &m_process_preset}) {
            for (auto &p : *list) { delete p.first; delete p.second; }
            list->clear();
        }
    }
    // update_presets_list: the compatible filament and process presets of the chosen printer, as check boxes
    void update_presets_list(bool just_template = false)
    {
        PresetBundle temp_preset_bundle;
        if (!load_system_and_user_presets_with_curr_model(temp_preset_bundle, just_template)) return;
        clear_preset_combobox();
        for (const Preset &filament_preset : temp_preset_bundle.filaments.get_presets())
            if (filament_preset.is_compatible && !filament_preset.is_default) m_filament_preset.emplace_back(new Check, new Preset(filament_preset));
        for (const Preset &process_preset : temp_preset_bundle.prints.get_presets())
            if (process_preset.is_compatible && !process_preset.is_default) m_process_preset.emplace_back(new Check, new Preset(process_preset));
    }

    bool        load_system_and_user_presets_with_curr_model(PresetBundle &temp_preset_bundle, bool just_template = false);
    void        generate_process_presets_data(std::vector<Preset const *> presets, std::string nozzle);
    std::string get_printer_vendor() const;
    std::string get_printer_model() const;
    std::string get_nozzle_diameter() const;
    std::string get_custom_printer_model() const;
    std::string get_custom_printer_name() const;
    void        set_current_visible_printer();
    wxArrayString printer_preset_sort_with_nozzle_diameter(const VendorProfile &vendor_profile, float nozzle_diameter);
    bool        save_printable_area_config(Preset *preset);
    bool        check_printable_area();
    bool        validate_input_valid();
    wxString    curr_create_preset_type() const;
    wxString    curr_create_printer_type() const;
    void        on_select_printer_model(wxCommandEvent &e);
    void        on_preset_model_value_change(wxCommandEvent &e);
    bool        data_init();
};

#include "slic3r/GUI/CreatePrinter.inc"

std::unique_ptr<CreatePrinter> dlg;

CreatePrinter &current()
{
    if (!dlg) throw CommandError("the Create printer dialog is not open");
    return *dlg;
}

json strings(const wxArrayString &a) { return json(std::vector<std::string>(a.begin(), a.end())); }

// what the app's dialog shows: both pages' choices and the current selections
json state()
{
    CreatePrinter &d = current();
    json models = json::object();
    for (const auto &[vendor, list] : printer_model_map) models[vendor] = list;
    json printers = json::array();
    for (const auto &[name, preset] : d.m_printer_name_to_preset) printers.push_back(name);
    std::sort(printers.begin(), printers.end());
    auto presets = [](const std::vector<std::pair<Check *, Preset *>> &list) {
        json out = json::array();
        for (const auto &[check, preset] : list) out.push_back({{"name", preset->name}, {"checked", check->value}});
        return out;
    };
    return {{"vendors", strings(d.select_vendor_.items)},
            {"models", models},
            {"printers", printers},
            {"nozzles", strings(d.nozzle_diameter_.items)},
            {"preset_vendors", strings(d.printer_vendor_.items)},
            {"preset_vendor", d.printer_vendor_.GetStringSelection()},
            {"preset_models", strings(d.printer_model_.items)},
            {"preset_model", d.printer_model_.GetStringSelection()},
            {"template_enabled", d.base_template_.enabled},
            {"from_template", d.base_template_.value},
            {"filaments", presets(d.m_filament_preset)},
            {"processes", presets(d.m_process_preset)}};
}

json open(const json &)
{
    dlg = std::make_unique<CreatePrinter>();
    return state();
}

// page 1's OK (create_page1_dialog_buttons): the inputs checked, page 2 set up
json page1(const json &a)
{
    CreatePrinter &d = current();
    d.type_printer_.value = a.value("type", std::string("printer")) == "printer";
    d.type_nozzle_.value  = !d.type_printer_.value;
    d.can_not_find_vendor_.value = a.value("custom_printer", false);
    d.select_vendor_.select(a.value("vendor", std::string()));
    d.select_model_.Set({});
    if (auto it = printer_model_map.find(a.value("vendor", std::string())); it != printer_model_map.end())
        for (const std::string &m : it->second) d.select_model_.items.Add(m);
    d.select_model_.select(a.value("model", std::string()));
    d.custom_vendor_.value = a.value("custom_vendor", std::string());
    d.custom_model_.value  = a.value("custom_model", std::string());
    d.select_printer_.Set({});
    for (const auto &[name, preset] : d.m_printer_name_to_preset) d.select_printer_.items.Add(name);
    d.select_printer_.select(a.value("printer", std::string()));
    d.can_not_find_nozzle_.value = a.value("custom_nozzle", false);
    d.nozzle_diameter_.select(a.value("nozzle", std::string()));
    d.custom_nozzle_.value = a.value("nozzle_value", std::string());
    d.bed_x_.text.value    = a.value("bed_x", std::string("200"));
    d.bed_y_.text.value    = a.value("bed_y", std::string("200"));
    d.origin_x_.text.value = a.value("origin_x", std::string("0"));
    d.origin_y_.text.value = a.value("origin_y", std::string("0"));
    d.height_.text.value   = a.value("height", std::string("200"));
    d.m_custom_model   = a.value("bed_model", std::string());
    d.m_custom_texture = a.value("bed_texture", std::string());
    d.closed = false;
    if (!d.validate_input_valid()) {
        const bool closed = d.closed;   // switched to the existing system preset
        if (closed) dlg.reset();
        return {{"ok", false}, {"closed", closed}};
    }
    d.data_init();
    json out = state();
    out["ok"] = true;
    return out;
}

// page 2's vendor, model and "Presets" radio boxes
json page2(const json &a)
{
    CreatePrinter &d = current();
    wxCommandEvent e;
    if (a.contains("from_template")) {
        d.select_curr_radiobox(d.m_create_presets_btns, a["from_template"].get<bool>() ? 0 : 1);
        d.on_preset_model_value_change(e);
    }
    if (a.contains("vendor")) {
        d.printer_vendor_.select(a["vendor"].get<std::string>());
        e.SetExtraLong(1);
        d.on_select_printer_model(e);
    }
    if (a.contains("model")) {
        d.printer_model_.select(a["model"].get<std::string>());
        d.on_preset_model_value_change(e);
    }
    return state();
}

// page 2's Create (create_page2_dialog_buttons' OK)
json create(const json &a)
{
    CreatePrinter &d = current();
    PresetBundle  *preset_bundle = wxGetApp().preset_bundle;
    auto checked = [&a](std::vector<std::pair<Check *, Preset *>> &list, const char *key) {
        const std::set<std::string> names = a.value(key, std::set<std::string>());
        for (auto &[check, preset] : list) check->value = names.count(preset->name) > 0;
    };
    checked(d.m_filament_preset, "filaments");
    checked(d.m_process_preset, "processes");
    auto info = [](const wxString &text) { MessageDialog(nullptr, text, wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxOK | wxCENTRE).ShowModal(); };
    auto failed = json{{"ok", false}};

    if (!d.m_printer_preset) {
        info(_L("You have not yet chosen which printer preset to create based on. Please choose the vendor and model of the printer"));
        return failed;
    }
    if (!d.save_printable_area_config(d.m_printer_preset)) {
        info(_L("You have entered a disallowed character in the printable area section on the first page. Please use only numbers."));
        return {{"ok", false}, {"page1", true}};
    }

    std::string printer_model_name  = d.get_custom_printer_model();
    std::string printer_nozzle_name = d.get_nozzle_diameter();
    std::replace(printer_nozzle_name.begin(), printer_nozzle_name.end(), ',', '.');
    std::string printer_preset_name = printer_model_name + " " + printer_nozzle_name + " nozzle";

    if (!d.rewritten && preset_bundle->printers.find_preset(printer_preset_name)) {
        MessageDialog dlg(nullptr, _L("The printer preset you created already has a preset with the same name. Do you want to overwrite it\?\n\tYes: Overwrite the printer preset with the same name, and filament and process presets with the same preset name will be recreated \nand filament and process presets without the same preset name will be reserved.\n\tCancel: Do not create a preset; return to the creation interface."),
                          wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxCANCEL | wxYES_DEFAULT | wxCENTRE);
        if (dlg.ShowModal() != wxID_YES) return failed;
        d.rewritten = true;
    }

    bool filament_preset_is_exist = false;
    std::vector<Preset const *> selected_filament_presets;
    for (auto &[check, preset] : d.m_filament_preset) {
        if (check->GetValue()) selected_filament_presets.push_back(preset);
        if (!filament_preset_is_exist && preset_bundle->filaments.find_preset(preset->alias + " @ " + printer_preset_name) != nullptr) filament_preset_is_exist = true;
    }
    if (selected_filament_presets.empty() && !filament_preset_is_exist) {
        info(_L("You need to select at least one filament preset."));
        return failed;
    }
    bool process_preset_is_exist = false;
    std::vector<Preset const *> selected_process_presets;
    for (auto &[check, preset] : d.m_process_preset) {
        if (check->GetValue()) selected_process_presets.push_back(preset);
        if (!process_preset_is_exist && preset_bundle->prints.find_preset(preset->alias + " @" + printer_preset_name) != nullptr) process_preset_is_exist = true;
    }
    if (selected_process_presets.empty() && !process_preset_is_exist) {
        info(_L("You need to select at least one process preset."));
        return failed;
    }

    PresetBundle filament_id_sources;
    if (!selected_filament_presets.empty()) load_filament_id_sources(filament_id_sources);
    auto create_filament_id = [&filament_id_sources](std::string vendor_typr_serial) { return get_filament_id(vendor_typr_serial, filament_id_sources); };

    // the clones; on a name clash the user may overwrite (the two branches upstream differ only in the process data)
    auto clone = [&](PresetCollection &presets, std::vector<Preset const *> &selected, const wxString &what) {
        if (selected.empty()) return true;
        std::vector<std::string> failures;
        if (presets.clone_presets_for_printer(selected, failures, printer_preset_name, create_filament_id, d.rewritten)) return true;
        std::string message;
        for (const std::string &failure : failures) message += "\t" + failure + "\n";
        MessageDialog dlg(nullptr, what + from_u8(message) + _L("\nDo you want to rewrite it?"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                          wxYES | wxYES_DEFAULT | wxCENTRE);
        if (dlg.ShowModal() != wxID_YES) return false;
        presets.clone_presets_for_printer(selected, failures, printer_preset_name, create_filament_id, true);
        return true;
    };
    if (!clone(preset_bundle->filaments, selected_filament_presets, _L("Create filament presets failed. As follows:\n"))) return failed;
    if (d.curr_create_preset_type() == d.m_create_type.base_template && !selected_process_presets.empty())
        d.generate_process_presets_data(selected_process_presets, printer_nozzle_name);
    if (!clone(preset_bundle->prints, selected_process_presets, _L("Create process presets failed. As follows:\n"))) return failed;

    try {
        if (auto printer_model = dynamic_cast<ConfigOptionString *>(d.m_printer_preset->config.option("printer_model", true)))
            printer_model->value = printer_model_name;
        if (auto printer_variant = dynamic_cast<ConfigOptionString *>(d.m_printer_preset->config.option("printer_variant", true)))
            printer_variant->value = printer_nozzle_name;
        if (auto nozzle_diameter = dynamic_cast<ConfigOptionFloats *>(d.m_printer_preset->config.option("nozzle_diameter", true))) {
            auto iter = nozzle_diameter_map.find(printer_nozzle_name);
            std::fill(nozzle_diameter->values.begin(), nozzle_diameter->values.end(),
                      iter != nozzle_diameter_map.end() ? iter->second : my_stof(d.get_nozzle_diameter()));
        }
    } catch (...) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " bisic info is not rewritten, may be printer_model, printer_variant, or nozzle_diameter";
    }
    preset_bundle->printers.save_current_preset(printer_preset_name, true, false, d.m_printer_preset);
    preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
    dlg.reset();
    presets_changed();   // Sidebar::create_printer_preset: load_current_presets, the combos

    // CreatePresetSuccessfulDialog
    MessageDialog done(nullptr, _L("Printer Created") + "\n\n" + _L("Please go to printer settings to edit your presets"), _L("Printer Created Successfully"),
                       wxOK | wxCANCEL | wxCENTRE);
    done.SetButtonLabel(wxID_OK, _L("Printer Setting"));
    return {{"ok", true}, {"edit", done.ShowModal() == wxID_OK}};
}

} // namespace

static Commands reg({
    {"create_printer.open", open},
    {"create_printer.page1", page1},
    {"create_printer.page2", page2},
    {"create_printer.create", create},
    {"create_printer.close", [](const json &) -> json { dlg.reset(); return nullptr; }},
});

} // namespace orca
