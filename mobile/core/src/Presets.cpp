// Preset commands: listing, selection, editing, saving and deleting printer / filament / process presets.
// The logic follows the desktop Tab (slic3r/GUI/Tab.cpp: select_preset, may_discard_current_dirty_preset, save_preset,
// delete_preset) and SavePresetDialog; the function names below match the upstream ones they mirror.
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Viewport.hpp"
#include "Slicer.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "PrimeTower.hpp"
#include "I18n.hpp"
#include "Settings.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

void cloud_delete_preset(const std::string &setting_id, const std::string &file);   // Cloud.cpp

static const char *type_name(Preset::Type t)
{
    return t == Preset::TYPE_PRINT ? "print" : t == Preset::TYPE_FILAMENT ? "filament" : "printer";
}

// Tab::m_cache_config / m_cache_options: changes carried over to the next selected preset ("Transfer").
static std::map<Preset::Type, std::pair<DynamicPrintConfig, std::vector<std::string>>> g_cache;

static json preset_json(const PresetCollection &presets, const Preset &p)
{
    json j = {{"name", p.name},
              {"label", p.label(false)},
              {"alias", p.alias},
              {"system", p.is_system},
              {"default", p.is_default},
              {"user", p.is_user()},
              {"project", p.is_project_embedded},
              {"bundle", p.is_from_bundle()},
              {"compatible", p.is_compatible},
              {"visible", p.is_visible},
              {"dirty", p.is_dirty},
              {"inherits", p.inherits()}};
    if (const VendorProfile *v = presets.get_preset_with_vendor_profile(p).vendor)
        j["vendor"] = v->name;
    if (presets.type() == Preset::TYPE_FILAMENT) {
        if (auto *t = p.config.option<ConfigOptionStrings>("filament_type"); t && !t->values.empty()) j["filament_type"] = t->values.front();
        if (auto *c = p.config.option<ConfigOptionStrings>("filament_colour"); c && !c->values.empty()) j["color"] = c->values.front();
        if (auto *v = p.config.option<ConfigOptionStrings>("filament_vendor"); v && !v->values.empty()) j["filament_vendor"] = v->values.front();
    }
    if (presets.type() == Preset::TYPE_PRINTER)
        if (auto *m = p.config.option<ConfigOptionString>("printer_model")) j["printer_model"] = m->value;
    return j;
}

// Plater preset combos + Tab combos: everything visible, with flags the app groups by.
static json list_presets(const json &a)
{
    PresetCollection &presets = collection(preset_type(str_arg(a, "type")));
    const bool        all     = a.value("all", false);   // incompatible ones too ("Show incompatible presets")
    json              out     = json::array();
    const size_t sel = presets.get_selected_idx();
    for (size_t i = 0; i < presets.get_presets().size(); ++i) {
        const Preset &p = presets.get_presets()[i];
        const bool selected = i == sel;
        if (!p.is_visible || (!all && !p.is_compatible && !selected)) continue;
        if (p.is_default && presets.size() > presets.num_default_presets() && !selected) continue;
        json j = preset_json(presets, p);
        j["selected"] = selected;
        out.push_back(std::move(j));
    }
    return out;
}

json filaments_state();                 // Filaments.cpp
bool ensure_belt_purge_tower();         // BeltPurge.cpp
void filaments_auto_flush(int idx);
void filaments_refresh_mixed();

// What the plater sidebar shows: the selected printer / process and one filament per extruder slot.
json presets_state()
{
    Core         &core = Core::get();
    PresetBundle &b    = *core.preset_bundle;
    json filaments = json::array();
    const auto *colors = core.full_config().option<ConfigOptionStrings>("filament_colour");
    for (size_t i = 0; i < b.filament_presets.size(); ++i) {
        const Preset *p = b.filaments.find_preset(b.filament_presets[i], false, true);
        const auto *mixed = b.project_config.option<ConfigOptionBools>("filament_is_mixed");
        filaments.push_back({{"mixed", mixed && i < mixed->values.size() && mixed->values[i]},
                             {"name", b.filament_presets[i]},
                             {"label", p ? p->label(false) : b.filament_presets[i]},
                             {"color", colors && i < colors->values.size() ? colors->values[i] : "#FFFFFF"},
                             {"type", p && p->config.has("filament_type") ? p->config.opt_string("filament_type", 0u) : ""},
                             {"compatible", p ? p->is_compatible : false}});
    }
    auto sel = [&](PresetCollection &c) {
        json j = preset_json(c, c.get_edited_preset());
        j["dirty"] = c.current_is_dirty();
        return j;
    };
    // Sidebar printer card: the model's cover picture and its variants (the nozzle diameters of the same model)
    json printer = sel(b.printers);
    const Preset &pp = b.printers.get_edited_preset();
    std::string model = pp.config.opt_string("printer_model"), variant = pp.config.opt_string("printer_variant");
    json variants = json::array();
    if (const VendorProfile *vp = b.printers.get_preset_with_vendor_profile(pp).vendor) {
        fs::path cover = fs::path(resources_dir()) / "profiles" / vp->id / (model + "_cover.png");
        if (!fs::exists(cover)) cover = fs::path(resources_dir()) / "web/image/printer" / (model + "_cover.png");
        if (fs::exists(cover)) printer["cover"] = cover.string();
    }
    for (const Preset &p : b.printers.get_presets())
        if (p.is_visible && p.config.opt_string("printer_model") == model && !model.empty())
            variants.push_back({{"name", p.name}, {"variant", p.config.opt_string("printer_variant")}, {"selected", p.name == pp.name}});
    printer["model"] = model;
    printer["variant"] = variant;
    printer["variants"] = variants;
    const auto *nd = pp.config.option<ConfigOptionFloats>("nozzle_diameter");
    printer["nozzle_diameter"] = nd && !nd->values.empty() ? nd->values[0] : 0.4;
    return {{"printer", printer},
            {"print", sel(b.prints)},
            {"filament", sel(b.filaments)},
            {"filaments", filaments},
            {"filament_area", filaments_state()},
            {"technology", b.printers.get_edited_preset().printer_technology() == ptSLA ? "SLA" : "FFF"},
            {"extruders", b.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->size()},
            {"bbl", b.is_bbl_vendor()}};
}

// GUI_App::load_current_presets + Plater::on_config_change: everything that follows a preset change.
void presets_changed()
{
    Core &core = Core::get();
    core.invalidate_full_config();
    core.preset_bundle->export_selections(*core.app_config);
    core.app_config->save();
    ensure_belt_purge_tower();   // a belt printer's purge prism follows the printer / filaments
    ++Plater::get().revision;   // the 3D view reloads (filament colours, prime tower)
    Viewport::get().update_view_presets();   // the overhang angle and plate tilt of the view
    Host::event("presets", presets_state());
    schedule_auto_reslice_if_needed();
}

// Tab::may_discard_current_dirty_preset: asks the user what to do with unsaved changes (UnsavedChangesDialog).
// Returns false when cancelled.
static bool may_discard_current_dirty_preset(PresetCollection &presets, const std::string &new_name, bool no_transfer)
{
    std::vector<std::string> dirty = presets.current_dirty_options(true);
    json options = json::array();
    const Preset &edited = presets.get_edited_preset(), &saved = presets.get_selected_preset();
    for (const std::string &key : dirty) {
        std::string base = key.substr(0, key.find('#'));
        const ConfigOptionDef *def = edited.config.def()->get(base);
        options.push_back({{"key", key},
                           {"label", def ? translate(def->label.empty() ? def->full_label : def->label) : key},
                           {"category", def ? translate(def->category) : ""},
                           {"old", saved.config.has(base) ? saved.config.opt_serialize(base) : ""},
                           {"new", edited.config.has(base) ? edited.config.opt_serialize(base) : ""}});
    }
    json ans = Host::ask({{"kind", "unsaved_changes"},
                          {"type", type_name(presets.type())},
                          {"preset", edited.name},
                          {"new_preset", new_name},
                          {"options", options},
                          {"can_transfer", !no_transfer},
                          {"can_save", !edited.is_default}});
    const std::string action = ans.value("action", "cancel");
    if (action == "cancel")
        return false;
    std::vector<std::string> selected;
    for (auto &k : ans.value("selected", json::array())) selected.push_back(k.get<std::string>());
    if (!ans.contains("selected")) selected = dirty;
    if (action == "save") {
        std::vector<std::string> unselected;
        for (auto &k : dirty) if (std::find(selected.begin(), selected.end(), k) == selected.end()) unselected.push_back(k);
        Core::get().preset_bundle->save_changes_for_preset(str_arg(ans, "name"), presets.type(), unselected, ans.value("to_project", false));
    } else if (action == "transfer") {
        // Tab::cache_config_diff
        auto &c = g_cache[presets.type()];
        c.first.apply_only(edited.config, selected);
        c.second = selected;
    }
    return true;
}

// Tab::apply_config_from_cache
static void apply_config_from_cache(PresetCollection &presets)
{
    auto it = g_cache.find(presets.type());
    if (it == g_cache.end() || it->second.second.empty()) return;
    presets.get_edited_preset().config.apply_only(it->second.first, it->second.second);
    presets.get_edited_preset().set_dirty(presets.get_selected_preset().config.diff(presets.get_edited_preset().config).size() > 0);
    g_cache.erase(it);
}

// Tab::select_preset (without the SLA paths and the cloud sync)
static json select_preset(const json &a)
{
    Core             &core   = Core::get();
    PresetBundle     &bundle = *core.preset_bundle;
    Preset::Type      type   = preset_type(str_arg(a, "type"));
    PresetCollection &presets = collection(type);
    std::string       preset_name = str_arg(a, "name");
    const bool        delete_current = a.value("delete_current", false);
    const bool        force_select   = a.value("force", false);

    // the plater's filament combo of slot N (PlaterPresetComboBox::on_select): only that slot changes
    if (type == Preset::TYPE_FILAMENT && a.contains("slot")) {
        size_t slot = a["slot"].get<size_t>();
        if (slot >= bundle.filament_presets.size()) throw CommandError("bad filament slot");
        bundle.set_filament_preset(slot, preset_name);
        if (slot == 0 || presets.get_edited_preset().name != preset_name) {
            // the Filament tab shows the first filament (Sidebar::on_select_preset -> select_preset of the tab)
            if (!presets.current_is_dirty() || may_discard_current_dirty_preset(presets, preset_name, true)) {
                if (presets.current_is_dirty()) presets.discard_current_changes();
                presets.select_preset_by_name(preset_name, true);
            }
        }
        bundle.update_compatible(PresetSelectCompatibleType::Never);
        // Plater::priv::on_select_preset: with "all", the slot's flushing volumes (whether or not it changed to / from a
        // support filament, which upstream checks first under the same setting)
        if (core.app_config->get("auto_calculate_flush") == "all") filaments_auto_flush(int(slot));
        presets_changed();
        return presets_state();
    }

    if (preset_name.empty() && delete_current) {
        const std::deque<Preset> &list = presets.get_presets();
        size_t idx_current = presets.get_idx_selected();
        preset_name = list[idx_current].inherits();
        if (preset_name.empty()) {
            size_t idx_new = idx_current + 1;
            if (idx_new < list.size())
                for (; idx_new < list.size() && !list[idx_new].is_visible; ++idx_new);
            if (idx_new == list.size())
                for (idx_new = idx_current - 1; idx_new > 0 && !list[idx_new].is_visible; --idx_new);
            preset_name = list[idx_new].name;
        }
    }

    bool current_dirty = !delete_current && presets.current_is_dirty();
    bool print_tab     = type == Preset::TYPE_PRINT;
    bool printer_tab   = type == Preset::TYPE_PRINTER;
    bool canceled      = false;
    bool no_transfer   = printer_tab;
    bool technology_changed = false;
    const std::string old_printer = bundle.printers.get_edited_preset().name;

    if (type == Preset::TYPE_FILAMENT) {
        if (Preset *to = presets.find_preset(preset_name, false, true)) {
            auto *cur_t = presets.get_edited_preset().config.option<ConfigOptionStrings>("filament_type");
            auto *to_t  = to->config.option<ConfigOptionStrings>("filament_type");
            if ((cur_t && !cur_t->values.empty() ? cur_t->values[0] : "") != (to_t && !to_t->values.empty() ? to_t->values[0] : ""))
                no_transfer = true;
        }
    }

    if (current_dirty && !may_discard_current_dirty_preset(presets, preset_name, no_transfer) && !force_select)
        canceled = true;
    else if (print_tab) {
        PresetWithVendorProfile printer_profile = bundle.printers.get_edited_preset_with_vendor_profile();
        PresetCollection       &dependent       = bundle.filaments;
        const Preset           *to_select       = presets.find_preset(preset_name, true);
        bool old_preset_dirty      = dependent.current_is_dirty();
        bool new_preset_compatible = is_compatible_with_print(dependent.get_edited_preset_with_vendor_profile(),
                                                              presets.get_preset_with_vendor_profile(*to_select), printer_profile);
        if (!canceled)
            canceled = old_preset_dirty && !may_discard_current_dirty_preset(dependent, preset_name, false) && !new_preset_compatible && !force_select;
        if (!canceled && old_preset_dirty && !new_preset_compatible)
            dependent.discard_current_changes();
    } else if (printer_tab) {
        const Preset &new_printer = *presets.find_preset(preset_name, true);
        const PresetWithVendorProfile new_printer_vp = presets.get_preset_with_vendor_profile(new_printer);
        PrinterTechnology old_tech = presets.get_edited_preset().printer_technology();
        PrinterTechnology new_tech = new_printer.printer_technology();
        for (PresetCollection *pc : {&bundle.prints, &bundle.filaments}) {
            bool old_dirty  = old_tech == ptFFF && pc->current_is_dirty();
            bool compatible = new_tech == ptFFF && is_compatible_with_printer(pc->get_edited_preset_with_vendor_profile(), new_printer_vp);
            if (!canceled)
                canceled = old_dirty && !may_discard_current_dirty_preset(*pc, preset_name, false) && !compatible && !force_select;
            if (!canceled && old_dirty && !compatible)
                pc->discard_current_changes();
        }
        if (!canceled)
            technology_changed = old_tech != new_tech;
    }

    if (!canceled && delete_current) {
        try {
            // Tab::delete_preset: a synced preset leaves OrcaCloud too (GUI_App::delete_preset_from_cloud)
            const Preset &current = presets.get_selected_preset();
            if (!current.setting_id.empty()) cloud_delete_preset(current.setting_id, current.file);
            presets.delete_current_preset();
        } catch (const std::exception &ex) {
            canceled = true;
            BOOST_LOG_TRIVIAL(error) << "delete preset: " << ex.what();
        }
    }

    if (!canceled) {
        if (current_dirty)
            presets.discard_current_changes();
        presets.select_preset_by_name(preset_name, false);
        auto update_compatible_type = [delete_current](bool technology_changed, bool on_page, bool show_incompatible) {
            return (delete_current || technology_changed) ? PresetSelectCompatibleType::Always :
                   on_page                                ? PresetSelectCompatibleType::Never :
                   show_incompatible                      ? PresetSelectCompatibleType::OnlyIfWasCompatible : PresetSelectCompatibleType::Always;
        };
        if (current_dirty || delete_current || print_tab || printer_tab)
            bundle.update_compatible(update_compatible_type(technology_changed, print_tab, false),
                                     update_compatible_type(technology_changed, false, false));
        apply_config_from_cache(presets);
        if (printer_tab && core.app_config->get_bool("remember_printer_config"))
            bundle.update_selections(*core.app_config);
        if (type == Preset::TYPE_FILAMENT && !bundle.filament_presets.empty())
            bundle.set_filament_preset(0, presets.get_edited_preset().name);
        // Plater::priv::on_select_preset: another printer recalculates every flushing volume with "all"
        if (printer_tab && old_printer != preset_name && core.app_config->get("auto_calculate_flush") == "all") filaments_auto_flush(-1);
    }
    presets_changed();
    // Plater::priv::update_objects_position_when_select_preset: re-clamp the prime towers to the new bed
    if (!canceled && printer_tab) set_default_wipe_tower_pos_all();
    json st = presets_state();
    st["canceled"] = canceled;
    return st;
}

// SavePresetDialog::Item::update: is a name usable for saving the edited preset?
static json validate_name(const json &a)
{
    PresetCollection &presets = collection(preset_type(str_arg(a, "type")));
    const std::string name    = str_arg(a, "name");
    const char *unusable_symbols = "<>[]:/\\|?*\"";
    std::string info, level = "ok";
    for (const char *c = unusable_symbols; *c; ++c)
        if (name.find(*c) != std::string::npos) {
            info = translate("Name is invalid;") + "\n" + translate("illegal characters:") + " " + unusable_symbols;
            level = "error";
            break;
        }
    if (level == "ok" && name.find(PresetCollection::get_suffix_modified()) != std::string::npos)
        info = translate("Name is invalid;") + "\n" + translate("illegal suffix:") + "\n\t" + PresetCollection::get_suffix_modified(), level = "error";
    if (level == "ok" && (name == "Default Setting" || name == PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER || name == "Default Printer"))
        info = translate("Name is unavailable."), level = "error";
    const Preset *existing = presets.find_preset(name, false);
    if (level == "ok" && existing && !existing->can_overwrite())
        info = translate("Overwriting a system profile is not allowed."), level = "error";
    if (level == "ok" && existing && name != presets.get_selected_preset_name()) {
        info = (boost::format(existing->is_compatible ? translate("Preset \"%1%\" already exists.")
                                                      : translate("Preset \"%1%\" already exists and is incompatible with the current printer.")) % name).str();
        info += "\n" + translate("Please note that saving will overwrite the current preset.");
        level = "warning";
    }
    if (level == "ok" && name.empty()) info = translate("The name field is not allowed to be empty."), level = "error";
    if (level == "ok" && name.find_first_of(' ') == 0) info = translate("The name is not allowed to start with a space."), level = "error";
    if (level == "ok" && name.find_last_of(' ') == name.length() - 1) info = translate("The name is not allowed to end with a space."), level = "error";
    if (level == "ok" && presets.get_preset_name_by_alias(name) != name) info = translate("The name cannot be the same as a preset alias name."), level = "error";
    return {{"level", level}, {"info", info}, {"existing_project", existing && existing->is_project_embedded}};
}

// SavePresetDialog::Item::Item: the proposed name
static json suggest_name(const json &a)
{
    PresetCollection &presets = collection(preset_type(str_arg(a, "type")));
    const Preset &sel = presets.get_selected_preset();
    std::string suffix = a.value("detach", false) ? translate("Detached") : translate_ctx("Copy", "PresetName");
    std::string name = sel.is_default ? translate("Untitled") :
                       sel.is_system ? (boost::format("%1% - %2%") % sel.name % suffix).str() :
                       sel.is_from_bundle() && !sel.alias.empty() ? sel.alias : sel.name;
    if (boost::iends_with(name, ".ini")) name.resize(name.size() - 4);
    return name;
}

// Tab::save_preset
static json save_preset(const json &a)
{
    Core             &core    = Core::get();
    Preset::Type      type    = preset_type(str_arg(a, "type"));
    PresetCollection &presets = collection(type);
    const std::string name    = str_arg(a, "name");
    const bool detach = a.value("detach", false), save_to_project = a.value("to_project", false);

    Preset *existing = presets.find_preset(name, false);
    if (existing && existing->name != presets.get_selected_preset_name() && existing->can_overwrite()) {
        // SavePresetDialog::Item::accept (overwrite warning confirmed); the overwritten one leaves OrcaCloud too
        if (!existing->setting_id.empty()) cloud_delete_preset(existing->setting_id, existing->file);
        presets.delete_preset(name);
    }
    existing = presets.find_preset(name, false);

    Preset &edited = presets.get_edited_preset();
    if (type == Preset::TYPE_FILAMENT && !existing && edited.is_system) {
        const Preset *printer = &core.preset_bundle->printers.get_selected_preset_base();
        auto *compatible_printers = edited.config.option<ConfigOptionStrings>("compatible_printers");
        if (printer && compatible_printers && compatible_printers->values.empty())
            compatible_printers->values.push_back(printer->name);
    }
    const std::string old_name = edited.name;
    presets.save_current_preset(name, detach, save_to_project, nullptr);
    Preset *new_preset = presets.find_preset(name, false, true);
    if (!new_preset) throw CommandError("saving the preset failed");
    new_preset->sync_info = existing ? "update" : "create";
    new_preset->save_info();
    core.preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    if (type == Preset::TYPE_FILAMENT)   // Sidebar::update_presets_from_to
        for (auto &f : core.preset_bundle->filament_presets) if (f == old_name) f = name;
    presets_changed();
    return presets_state();
}

// Tab::delete_preset (the confirmation is asked by the app)
static json delete_preset(const json &a)
{
    json args = a;
    args["name"] = "";
    args["delete_current"] = true;
    args["force"] = true;
    return select_preset(args);
}

static json discard_changes(const json &a)
{
    PresetCollection &presets = collection(preset_type(str_arg(a, "type")));
    presets.discard_current_changes();
    presets_changed();
    return presets_state();
}

// Plater::get_next_color_for_filament: the colours new filament slots get, in turn
std::string next_filament_color()
{
    static int curr = 0;
    static const char *colors[] = {"#00C1AE", "#F4E2C1", "#ED1C24", "#00FF7F", "#F26722", "#FFEB31", "#7841CE", "#115877", "#ED1E79",
                                   "#2EBDEF", "#345B2F", "#800080", "#FA8173", "#800000", "#F7B763", "#A4C41E"};
    return colors[curr++ % (sizeof(colors) / sizeof(colors[0]))];
}

// Sidebar add / remove filament (PresetBundle::set_num_filaments)
static json filament_count(const json &a)
{
    PresetBundle &b = *Core::get().preset_bundle;
    int n = std::clamp(int(b.filament_presets.size()) + a.value("delta", 1), 1, int(MAXIMUM_EXTRUDER_NUMBER));
    b.set_num_filaments(unsigned(n), a.value("delta", 1) > 0 ? next_filament_color() : std::string());
    presets_changed();
    return presets_state();
}

// the colour picker of a filament slot (PlaterPresetComboBox: project filament_colour)
static json filament_color(const json &a)
{
    PresetBundle &b = *Core::get().preset_bundle;
    size_t slot = a.at("slot");
    auto *colors = b.project_config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || slot >= colors->values.size()) throw CommandError("bad filament slot");
    colors->values[slot] = str_arg(a, "color");
    if (slot < b.ams_multi_color_filment.size()) b.ams_multi_color_filment[slot].clear();
    // Plater::priv::on_filament_color_changed
    if (Core::get().app_config->get("auto_calculate_flush") != "disabled") filaments_auto_flush(int(slot));
    else filaments_refresh_mixed();
    presets_changed();
    return presets_state();
}

static Commands reg({
    {"presets.filament_count", filament_count},
    {"presets.filament_color", filament_color},
    {"presets.list", list_presets},
    {"presets.state", [](const json &) { return presets_state(); }},
    {"presets.select", select_preset},
    {"presets.validate_name", validate_name},
    {"presets.suggest_name", suggest_name},
    {"presets.save", save_preset},
    {"presets.delete", delete_preset},
    {"presets.discard", discard_changes},
});

} // namespace orca
