// File > Export > Export Preset Bundle (ExportConfigsDialog): printer config bundles (.orca_printer), filament bundles
// (.orca_filament), or printer / filament / process presets (.zip). The preset collection and archiving are compiled
// from upstream CreatePresetsDialog.cpp; the dialog's radio boxes and check boxes are the app's.
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrinterBedAssets.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace orca {

namespace {

#include "slic3r/GUI/CreatePresetsHelpers.inc"

// ::CheckBox of the dialog: the app's choice
struct CheckBox { bool value{false}; bool GetValue() const { return value; } };

// ExportConfigsDialog without its widgets
struct ExportConfigs
{
    enum ExportCase { INITIALIZE_FAIL = 0, ADD_FILE_FAIL, ADD_BUNDLE_STRUCTURE_FAIL, FINALIZE_FAIL, OPEN_ZIP_WRITTEN_FILE, EXPORT_CANCEL, EXPORT_SUCCESS, CASE_COUNT };

    void        data_init();
    bool        earse_preset_fields_for_safe(Preset *preset);
    std::string initial_file_path(const wxString &path, const std::string &sub_file_path);
    std::string initial_file_name(const wxString &path, const std::string file_name);
    mz_bool     initial_zip_archive(mz_zip_archive &zip_archive, const std::string &file_path);
    ExportCase  save_zip_archive_to_file(mz_zip_archive &zip_archive);
    ExportCase  save_presets_to_zip(const std::string &export_file, const std::vector<std::pair<std::string, std::string>> &config_paths);
    ExportCase  archive_preset_bundle_to_file(const wxString &path, bool include_bed_assets);
    ExportCase  archive_filament_bundle_to_file(const wxString &path);
    ExportCase  archive_printer_preset_to_file(const wxString &path);
    ExportCase  archive_filament_preset_to_file(const wxString &path);
    ExportCase  archive_process_preset_to_file(const wxString &path);

    ~ExportConfigs()
    {
        for (auto &p : m_printer_presets) delete p.second;
        for (auto &p : m_filament_presets) for (Preset *f : p.second) delete f;
        for (auto &p : m_process_presets) for (Preset *f : p.second) delete f;
        for (auto &p : m_filament_name_to_presets) for (auto &f : p.second) delete f.second;
        for (auto &c : m_preset) delete c.first;
        for (auto &c : m_printer_name) delete c.first;
    }

    std::vector<std::pair<::orca::CheckBox *, Preset *>>                           m_preset;
    std::vector<std::pair<::orca::CheckBox *, std::string>>                        m_printer_name;
    std::unordered_map<std::string, Preset *>                                       m_printer_presets;
    std::unordered_map<std::string, std::vector<Preset *>>                          m_filament_presets;
    std::unordered_map<std::string, std::vector<Preset *>>                          m_process_presets;
    std::unordered_map<std::string, std::vector<std::pair<std::string, Preset *>>> m_filament_name_to_presets;
};

#define CheckBox orca::CheckBox
#include "slic3r/GUI/ExportConfigs.inc"
#undef CheckBox

// select_curr_radiobox: what each export type lists
json export_choices(ExportConfigs &e, const std::string &type)
{
    json items = json::array();
    if (type == "preset_bundle" || type == "printer_preset") {
        for (auto &[name, preset] : e.m_printer_presets) {
            if (type == "preset_bundle" && e.m_filament_presets.find(name) == e.m_filament_presets.end() &&
                e.m_process_presets.find(name) == e.m_process_presets.end() && preset->is_system) continue;
            if (type == "printer_preset" && preset->is_system) continue;
            items.push_back(name);
        }
    } else if (type == "filament_bundle" || type == "filament_preset") {
        for (auto &[name, presets] : e.m_filament_name_to_presets)
            if (!presets.empty()) items.push_back(name);
    } else if (type == "process_preset") {
        for (auto &[name, presets] : e.m_process_presets)
            if (!presets.empty()) items.push_back(name);
    }
    std::vector<std::string> sorted = items.get<std::vector<std::string>>();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

json bundle_state(const json &a)
{
    ExportConfigs e;
    e.data_init();
    json types = json::array();
    for (const auto &[key, label, desc] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"preset_bundle", _u8L("Printer config bundle(.orca_printer)"),
              _u8L("Printer and all the filament and process presets that belongs to the printer.\nCan be shared with others.")},
             {"filament_bundle", _u8L("Filament bundle(.orca_filament)"), _u8L("User's filament preset set.\nCan be shared with others.")},
             {"printer_preset", _u8L("Printer presets(.zip)"), ""},
             {"filament_preset", _u8L("Filament presets(.zip)"), ""},
             {"process_preset", _u8L("Process presets(.zip)"), ""}})
        types.push_back({{"key", key}, {"label", label}, {"description", desc}, {"items", export_choices(e, key)}});
    return {{"title", _u8L("Export Preset Bundle")}, {"types", types}, {"presets_label", _u8L("Presets")},
            {"include_bed", _u8L("Include custom bed texture and model")},
            {"none_selected", _u8L("Please select at least one printer or filament.")},
            {"hint_bundle", _u8L("Only display printers with changes to printer, filament, and process presets are displayed.")},
            {"hint_filament", _u8L("Only display the filament names with changes to filament presets.")},
            {"hint_process", _u8L("Only printers with changed process presets are displayed.")}};
}

// the OK button: the chosen items archived into "dir"
json bundle_export(const json &a)
{
    ExportConfigs e;
    e.data_init();
    const std::string type = a.at("type");
    std::set<std::string> chosen;
    for (const auto &n : a.at("items")) chosen.insert(n.get<std::string>());
    if (type == "preset_bundle" || type == "printer_preset") {
        for (auto &[name, preset] : e.m_printer_presets)
            e.m_preset.push_back({new CheckBox{chosen.count(name) > 0}, preset});
    } else
        for (const std::string &name : chosen) e.m_printer_name.push_back({new CheckBox{true}, name});
    const wxString dir = a.at("dir").get<std::string>();
    ExportConfigs::ExportCase r = type == "preset_bundle" ? e.archive_preset_bundle_to_file(dir, a.value("include_bed", true))
                                : type == "filament_bundle" ? e.archive_filament_bundle_to_file(dir)
                                : type == "printer_preset" ? e.archive_printer_preset_to_file(dir)
                                : type == "filament_preset" ? e.archive_filament_preset_to_file(dir)
                                                            : e.archive_process_preset_to_file(dir);
    // show_export_result
    std::string text;
    switch (r) {
    case ExportConfigs::INITIALIZE_FAIL: text = _u8L("initialize fail"); break;
    case ExportConfigs::ADD_FILE_FAIL: text = _u8L("add file fail"); break;
    case ExportConfigs::ADD_BUNDLE_STRUCTURE_FAIL: text = _u8L("add bundle structure file fail"); break;
    case ExportConfigs::FINALIZE_FAIL: text = _u8L("finalize fail"); break;
    case ExportConfigs::OPEN_ZIP_WRITTEN_FILE: text = _u8L("open zip written fail"); break;
    case ExportConfigs::EXPORT_SUCCESS: text = _u8L("Export successful"); break;
    default: break;
    }
    return {{"ok", r == ExportConfigs::EXPORT_SUCCESS}, {"canceled", r == ExportConfigs::EXPORT_CANCEL}, {"message", text}};
}

} // namespace

static Commands reg({
    {"bundle.state", bundle_state},
    {"bundle.export", bundle_export},
});

} // namespace orca
