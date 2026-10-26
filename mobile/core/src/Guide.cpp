// The setup wizard: Orca's web guide (resources/web/guide) with the message handling of the desktop GuideFrame
// (slic3r/GUI/WebGuideDialog.cpp). The app shows the pages in a WebView and forwards every window.wx.postMessage()
// to "guide.message"; the answer lists the scripts to run in the page and whether the wizard closed.
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "I18n.hpp"
#include "Settings.hpp"
#include "libslic3r/PresetCacheFormat.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace orca {

namespace {

struct Guide
{
    json                    profile;          // GuideFrame::m_ProfileJson
    std::unique_ptr<AppConfig> appconfig_new; // GuideFrame::m_appconfig_new (created on open: AppConfig needs libslic3r initialised)
    fs::path                vendor_dir, rsrc_vendor_dir;
    std::string             region;
    bool                    stealth_mode{false};
    bool                    loaded{false};
} g;

std::string file_url(const fs::path &p) { return "file://" + p.generic_string(); }

std::string handle_studio(const json &res) { return "HandleStudio(" + res.dump(-1, ' ', true, json::error_handler_t::ignore) + ")"; }

// guide_strings(): the page's translated strings (window.ORCA_UI_STRINGS)
json guide_strings()
{
    return {
        {"loading", _u8L("Loading…")},
        {"custom_vendor", _u8L_CONTEXT("Custom", "Printer vendor")},
        {"welcome", _u8L("Welcome to Orca Slicer")},
        {"setup_intro", _u8L("Orca Slicer will be set up in several steps. Let's start!")},
        {"get_started", _u8L("Get Started")},
        {"login_region", _u8L("Please select your login region")},
        {"asia_pacific", _u8L("Asia-Pacific")},
        {"china", _u8L("China")},
        {"europe", _u8L("Europe")},
        {"north_america", _u8L("North America")},
        {"others", _u8L_CONTEXT("Others", "Region")},
        {"back", _u8L_CONTEXT("Back", "Navigation")},
        {"next", _u8L("Next")},
        {"finish", _u8L("Finish")},
        {"confirm", _u8L("Confirm")},
        {"cancel", _u8L("Cancel")},
        {"ok", _u8L("OK")},
        {"yes", _u8L("Yes")},
        {"no", _u8L("No")},
        {"error", _u8L("Error")},
        {"information", _u8L("Information")},
        {"printer_selection", _u8L("Printer Selection")},
        {"vendors", _u8L("Vendors")},
        {"search", _u8L("Search")},
        {"custom_printer", _u8L("Custom Printer")},
        {"nozzle", _u8L("Nozzle")},
        {"create", _u8L("Create")},
        {"check_new_printers", _u8L("Check for new printers")},
        {"printer_required", _u8L("At least one printer must be selected.")},
        {"new_printers_found", _u8L("New printers found")},
        {"new_vendors_installed", _u8L("New printer vendors installed: %s")},
        {"vendors_declined", _u8L("New printer vendors were found, but installation was cancelled.")},
        {"no_new_printers", _u8L("No new printers found.")},
        {"filament_selection", _u8L("Filament Selection")},
        {"system_filaments", _u8L("System Filaments")},
        {"custom_filaments", _u8L("Custom Filaments")},
        {"create_new", _u8L("Create New")},
        {"printer", _u8L("Printer")},
        {"filament_type", _u8L("Filament type")},
        {"vendor", _u8L("Vendor")},
        {"all", _u8L("All")},
        {"no_items", _u8L("No items")},
        {"filter_items", _u8L("Filter items")},
        {"checked", _u8L("Checked")},
        {"unchecked", _u8L("Unchecked")},
        {"select_filtered", _u8L("Select filtered")},
        {"select_visible", _u8L("Select visible")},
        {"clear_all", _u8L("Clear all")},
        {"filament_required", _u8L("At least one filament must be selected.")},
        {"use_default_filament", _u8L("Do you want to use the default filament?")},
        {"stealth_mode", _u8L("Stealth Mode")},
        {"stealth_mode_desc", _u8L("This disables all cloud features, including Orca Cloud profile syncing. Users who prefer to work entirely offline can enable this option.")},
        {"stealth_mode_note", _u8L("Note: When Stealth Mode is enabled, your user profiles will not be backed up to Orca Cloud.")},
        {"enable_stealth_mode", _u8L("Enable Stealth Mode.")},
        {"proprietary_plugins", _u8L("Proprietary Plugins")},
        {"proprietary_plugins_desc", _u8L("Some printer vendors require proprietary plugins for communication with their printers. Please select the corresponding plugin if you use such printers.")},
        {"proprietary_plugins_risk", _u8L("Please be aware that these plugins are not developed or maintained by OrcaSlicer. They should be used at your own discretion and risk.")},
        {"install_bambu_plugin", _u8L("Install Bambu Network plug-in")},
    };
}

void reset_profile_json()
{
    g.profile["model"] = json::array();
    g.profile["machine"] = json::object();
    g.profile["filament"] = json::object();
    g.profile["process"] = json::array();
}

void init_guide_paths()
{
    g.profile = json::object();
    reset_profile_json();
    g.vendor_dir = (fs::path(data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    g.rsrc_vendor_dir = (fs::path(resources_dir()) / "profiles").make_preferred();
}

// GuideFrame::BuildProfileJson
bool build_profile_json(const PresetBundle &bundle, bool require_all_resource_vendors)
{
    try {
        for (const auto &[vendor_id, vp] : bundle.vendors)
            for (const auto &model : vp.models) {
                std::string nozzle_str;
                for (const auto &v : model.variants) { if (!nozzle_str.empty()) nozzle_str += ";"; nozzle_str += v.name; }
                fs::path cover = (fs::path(resources_dir()) / "profiles" / vp.id / (model.id + "_cover.png")).make_preferred();
                if (!fs::exists(cover)) cover = (fs::path(resources_dir()) / "web/image/printer" / (model.id + "_cover.png")).make_preferred();
                json entry;
                entry["model"] = model.id;
                entry["name"] = model.name;
                entry["vendor"] = vp.id;
                entry["nozzle_diameter"] = nozzle_str;
                entry["materials"] = boost::algorithm::join(model.default_materials, ";");
                entry["cover"] = file_url(cover);
                entry["nozzle_selected"] = "";
                entry["sub_path"] = "";
                g.profile["model"].push_back(entry);
            }
        for (const Preset &p : bundle.printers()) {
            if (!p.is_system || !p.vendor) continue;
            const auto *pm = p.config.option<ConfigOptionString>("printer_model");
            const auto *pv = p.config.option<ConfigOptionString>("printer_variant");
            if (!pm || pm->value.empty() || !pv) continue;
            g.profile["machine"][p.name] = {{"model", pm->value}, {"nozzle", pv->value}};
        }
        const json &machines = g.profile["machine"];
        for (const Preset &p : bundle.filaments()) {
            if (!p.is_system || !p.vendor) continue;
            const auto *fv = p.config.option<ConfigOptionStrings>("filament_vendor");
            const auto *ft = p.config.option<ConfigOptionStrings>("filament_type");
            const auto *cp = p.config.option<ConfigOptionStrings>("compatible_printers");
            std::string model_list;
            if (cp)
                for (const std::string &pname : cp->values) {
                    auto it = machines.find(pname);
                    if (it != machines.end()) model_list += "[" + (*it)["model"].get<std::string>() + "++" + (*it)["nozzle"].get<std::string>() + "]";
                }
            g.profile["filament"][p.name] = {{"name", p.name}, {"sub_path", p.file},
                                             {"vendor", fv && !fv->values.empty() ? fv->values[0] : ""},
                                             {"type", ft && !ft->values.empty() ? ft->values[0] : ""},
                                             {"models", model_list}, {"selected", 0}};
        }
        for (const Preset &p : bundle.prints()) {
            if (!p.is_system || !p.vendor || !p.is_visible) continue;
            g.profile["process"].push_back({{"name", p.name}, {"sub_path", p.file}});
        }
        if (require_all_resource_vendors)
            for (const std::string &name : vendor_names_in(g.rsrc_vendor_dir))
                if (bundle.vendors.find(name) == bundle.vendors.end()) { reset_profile_json(); return false; }
        return !g.profile["machine"].empty();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "guide: " << e.what();
        reset_profile_json();
        return false;
    }
}

// GuideFrame::BuildProfileDataFromVendors: every vendor (installed or shipped) in a scratch bundle, cached on disk
bool build_profile_from_vendors()
{
    std::map<std::string, fs::path> sources;
    for (const fs::path &dir : {g.vendor_dir, g.rsrc_vendor_dir}) {
        boost::system::error_code ec;
        if (fs::exists(dir, ec))
            for (const std::string &name : vendor_names_in(dir)) sources.emplace(name, dir);
    }
    std::vector<PresetBundle::VendorSource> ordered;
    json stamps = json::array();
    auto add = [&](const std::string &name, const fs::path &dir) {
        const fs::path profile = dir / (name + ".json");
        std::string version;
        if (fs::exists(profile)) {
            const Semver v = get_version_from_json(profile.string());
            if (!v.valid()) return;
            version = v.to_string();
        } else
            version = VendorCacheFile::peek_version((dir / (name + ".opc")).string(), name);
        ordered.push_back({name, dir});
        stamps.push_back({name, version});
    };
    const std::string lib(PresetBundle::ORCA_FILAMENT_LIBRARY);
    if (auto it = sources.find(lib); it != sources.end()) add(lib, it->second);
    for (const auto &[name, dir] : sources)
        if (name != lib) add(name, dir);
    if (ordered.empty()) return false;
    const fs::path cache_file = fs::path(data_dir()) / "cache" / "wizard_profile_data.json";
    try {
        boost::nowide::ifstream ifs(cache_file.string(), std::ios::binary);
        if (ifs.is_open()) {
            const std::string text{std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
            json cached = json::parse(text);
            if (cached.value("format", 0) == 1 && cached["vendors"] == stamps && !cached["profile"]["machine"].empty()) {
                for (const char *key : {"model", "machine", "filament", "process"}) g.profile[key] = std::move(cached["profile"][key]);
                return true;
            }
        }
    } catch (...) {}
    Host::event("progress", {{"text", _u8L("Loading printer & filament profiles") + "…"}, {"percent", 10}});
    PresetBundle bundle;
    std::vector<std::string> failed;
    bundle.load_vendors(ordered, ForwardCompatibilitySubstitutionRule::EnableSilent, true, nullptr, &failed);
    Host::event("progress", {{"text", ""}, {"percent", 100}});
    if (bundle.vendors.empty() || !failed.empty()) return false;
    if (!build_profile_json(bundle, false)) return false;
    try {
        json out = {{"format", 1}, {"vendors", std::move(stamps)}};
        for (const char *key : {"model", "machine", "filament", "process"}) out["profile"][key] = g.profile[key];
        fs::create_directories(cache_file.parent_path());
        write_file_atomically(cache_file.string(), out.dump(-1, ' ', false, json::error_handler_t::ignore), true);
    } catch (...) {}
    return true;
}

// GuideFrame::SaveProfileData: what is selected now
void save_profile_data()
{
    AppConfig &app = *Core::get().app_config;
    const auto enabled_filaments = app.has_section(AppConfig::SECTION_FILAMENTS) ? app.get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();
    g.appconfig_new->set_vendors(app);
    g.appconfig_new->set_section(AppConfig::SECTION_FILAMENTS, enabled_filaments);
    for (auto &m : g.profile["model"]) {
        if (!m.is_object()) continue;
        std::string model_name = m["model"], vendor_name = m["vendor"], nozzle_diameter = m["nozzle_diameter"], selected;
        std::vector<std::string> nozzles;
        boost::split(nozzles, nozzle_diameter, boost::is_any_of(";"));
        for (auto n : nozzles) {
            boost::trim(n);
            if (!n.empty() && g.appconfig_new->get_variant(vendor_name, model_name, n)) selected += (selected.empty() ? "" : ";") + n;
        }
        m["nozzle_selected"] = selected;
    }
    if (g.profile["model"].size() == 1) g.profile["model"][0]["nozzle_selected"] = g.profile["model"][0]["nozzle_diameter"];
    for (auto it = g.profile["filament"].begin(); it != g.profile["filament"].end(); ++it)
        if (enabled_filaments.count(it.key())) it.value()["selected"] = 1;
    g.region = app.get("region");
    g.profile["region"] = g.region;
    g.profile["network_plugin_install"] = app.get("app", "installed_networking");
    g.profile["network_plugin_compability"] = "0";   // the Bambu network plug-in is a desktop binary
    g.stealth_mode = app.get_bool("app", "stealth_mode");
    g.profile["stealth_mode"] = g.stealth_mode;
}

// GuideFrame::SaveProfile
void save_profile()
{
    AppConfig &app = *Core::get().app_config;
    app.set("region", g.region);
    app.set_bool("stealth_mode", g.stealth_mode);
    app.set("firstguide", "finish", true);
    app.save();
    std::map<std::string, std::string> section_new;
    g.appconfig_new->clear_section(AppConfig::SECTION_FILAMENTS);
    for (auto it = g.profile["filament"].begin(); it != g.profile["filament"].end(); ++it)
        if (it.value()["selected"] == 1) section_new[it.key()] = "true";
    g.appconfig_new->set_section(AppConfig::SECTION_FILAMENTS, section_new);
    g.appconfig_new->set_vendors(AppConfig::VendorMap());
    for (auto &m : g.profile["model"]) {
        if (!m.is_object()) continue;
        std::string model_name = m["model"], vendor_name = m["vendor"], selected = m["nozzle_selected"];
        std::vector<std::string> nozzles;
        boost::split(nozzles, selected, boost::is_any_of(";"));
        for (auto n : nozzles) { boost::trim(n); if (!n.empty()) g.appconfig_new->set_variant(vendor_name, model_name, n, true); }
    }
}

std::set<std::string> added_aliases(const std::map<std::string, std::string> &old_data, const std::map<std::string, std::string> &new_data)
{
    auto aliases = [](const std::map<std::string, std::string> &d) {
        std::set<std::string> out;
        for (auto &it : d) { size_t pos = it.first.find("@"); out.emplace(pos == std::string::npos ? it.first : it.first.substr(0, pos - 1)); }
        return out;
    };
    std::set<std::string> o = aliases(old_data), n = aliases(new_data), diff;
    std::set_difference(n.begin(), n.end(), o.begin(), o.end(), std::inserter(diff, diff.begin()));
    return diff;
}

// GuideFrame::apply_config
bool apply_config()
{
    Core &core = Core::get();
    AppConfig *app_config = core.app_config.get();
    PresetBundle *preset_bundle = core.preset_bundle.get();
    const auto enabled_vendors = g.appconfig_new->vendors();
    const auto old_enabled_vendors = app_config->vendors();
    const auto enabled_filaments = g.appconfig_new->has_section(AppConfig::SECTION_FILAMENTS) ? g.appconfig_new->get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();

    // install the newly enabled vendors' profiles from resources (PresetUpdater::install_bundles_rsrc)
    std::vector<std::string> install;
    for (const auto &it : enabled_vendors)
        if (!it.second.empty() && !is_vendor_installed(it.first)) install.push_back(it.first);
    if (!install.empty()) install_vendor_bundles_from_resources(install);

    std::string preferred_model, preferred_variant;
    auto get_preferred_printer_model = [&](const std::string &bundle_name, std::string &variant) {
        const auto config = enabled_vendors.find(bundle_name);
        if (config == enabled_vendors.end()) return std::string();
        const VendorProfile &printer_profile = preset_bundle->vendors[bundle_name];
        for (const auto &model_it : config->second) {
            if (model_it.second.empty()) continue;
            variant = *model_it.second.begin();
            if (model_it.second.size() > 1) {
                if (!printer_profile.models.empty()) {
                    auto pm = std::find_if(printer_profile.models.begin(), printer_profile.models.end(), [id = model_it.first](auto &m) { return m.id == id; });
                    if (pm != printer_profile.models.end())
                        for (auto &vt : pm->variants)
                            if (model_it.second.count(vt.name)) { variant = vt.name; break; }
                } else if (variant != PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT && model_it.second.count(PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT))
                    variant = PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT;
            }
            const auto config_old = old_enabled_vendors.find(bundle_name);
            if (config_old == old_enabled_vendors.end()) return model_it.first;
            const auto model_it_old = config_old->second.find(model_it.first);
            if (model_it_old == config_old->second.end()) return model_it.first;
            if (model_it_old->second != model_it.second)
                for (const auto &var : model_it.second)
                    if (!model_it_old->second.count(var)) { variant = var; return model_it.first; }
        }
        variant.clear();
        return std::string();
    };
    if (preferred_model = get_preferred_printer_model(PresetBundle::ORCA_DEFAULT_BUNDLE, preferred_variant); preferred_model.empty())
        for (const auto &bundle : enabled_vendors) {
            if (bundle.first == PresetBundle::ORCA_DEFAULT_BUNDLE) continue;
            if (preferred_model = get_preferred_printer_model(bundle.first, preferred_variant); !preferred_model.empty()) break;
        }
    // the vendors must be loaded before their presets can be selected
    preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
    if (!preset_bundle->apply_vendor_config(enabled_vendors, enabled_filaments, app_config, true, preferred_model, preferred_variant)) return false;
    app_config->set_legacy_datadir(false);
    app_config->save();
    presets_changed();
    return true;
}

} // namespace

// open the wizard: the profile data and the page to load (GuideFrame::SetStartPage)
static json guide_open(const json &a)
{
    init_guide_paths();
    g.appconfig_new = std::make_unique<AppConfig>();
    if (!build_profile_json(*Core::get().preset_bundle, true) && !build_profile_from_vendors())
        throw CommandError(_u8L("The printer profiles could not be loaded."));
    save_profile_data();
    const std::string page = a.value("page", "welcome");
    const std::string target = page == "welcome" ? "1" : page == "region" ? "11" : page == "filaments" ? (g.profile["model"].empty() ? "21" : "22")
                             : page == "filament_only" ? "23" : page == "models_only" ? "24" : "21";
    std::string url = file_url(fs::path(resources_dir()) / "web/guide/0/index.html") + "?target=" + target;
    if (!current_language().empty()) url += "&lang=" + current_language();
    return {{"url", url}, {"script", "window.ORCA_UI_STRINGS = " + guide_strings().dump(-1, ' ', false, json::error_handler_t::ignore) + ";"}};
}

// GuideFrame::OnScriptMessage
static json guide_message(const json &a)
{
    json out = {{"scripts", json::array()}, {"close", nullptr}};
    json j = json::parse(a.value("message", std::string("{}")), nullptr, false);
    if (j.is_discarded() || !j.contains("command")) return out;
    const std::string cmd = j["command"];
    auto run = [&](const json &res) { out["scripts"].push_back(handle_studio(res)); };
    if (cmd == "userguide_loading_page_ready")
        run({{"command", "userguide_profile_load_finish"}, {"sequence_id", "10001"}});
    else if (cmd == "close_page" || cmd == "user_guide_cancel")
        out["close"] = "cancel";
    else if (cmd == "request_userguide_profile")
        run({{"command", "response_userguide_profile"}, {"sequence_id", "10001"}, {"response", g.profile}});
    else if (cmd == "request_custom_filaments")
        run({{"command", "update_custom_filaments"}, {"sequence_id", "2000"}, {"data", json::array()}});
    else if (cmd == "save_userguide_models") {
        json sel = j["data"];
        for (auto &m : g.profile["model"]) {
            m["nozzle_selected"] = "";
            for (auto &one : sel) {
                if (m["model"] != one["model"]) continue;
                m["nozzle_selected"] = m["nozzle_diameter"];
                // ConfigWizard::select_default_materials_for_printer_model
                std::string materials_str;
                if (m.contains("materials") && m["materials"].is_string()) materials_str = m["materials"];
                boost::trim(materials_str);
                std::vector<std::string> materials;
                if (unescape_strings_cstyle(materials_str, materials))
                    for (auto &mat : materials)
                        if (!mat.empty() && g.profile["filament"].contains(mat)) g.profile["filament"][mat]["selected"] = 1;
                break;
            }
        }
    } else if (cmd == "save_userguide_filaments") {
        for (auto it = g.profile["filament"].begin(); it != g.profile["filament"].end(); ++it) it.value()["selected"] = 0;
        for (auto &f : j["data"]["filament"]) g.profile["filament"][f.get<std::string>()]["selected"] = 1;
    } else if (cmd == "check_for_new_printers") {
        json response = {{"command", "check_new_printers_result"}, {"sequence_id", j.value("sequence_id", json(""))},
                         {"vendors", json::array()}, {"declined", false}};
        run(response);
    } else if (cmd == "user_guide_finish") {
        save_profile();
        out["close"] = apply_config() ? "ok" : "cancel";
    } else if (cmd == "user_guide_create_printer") {
        out["close"] = "create_printer";
    } else if (cmd == "save_region")
        g.region = j.value("region", std::string());
    else if (cmd == "save_stealth_mode")
        g.stealth_mode = j["data"].value("action", std::string()) == "yes";
    else if (cmd == "create_custom_filament")
        out["close"] = "create_filament";
    return out;
}

// GuideFrame::run, cancelled with only the default printer: select the default bundle's printer
static json guide_cancelled(const json &)
{
    Core &core = Core::get();
    if (core.preset_bundle->printers.only_default_printers()) {
        core.app_config->set_variant(PresetBundle::ORCA_DEFAULT_BUNDLE, PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT, true);
        core.app_config->clear_section(AppConfig::SECTION_FILAMENTS);
        core.preset_bundle->load_selections(*core.app_config, {PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT,
                                                               PresetBundle::ORCA_DEFAULT_FILAMENT, std::string()});
        core.app_config->set_legacy_datadir(false);
        core.app_config->save();
        presets_changed();
    }
    return presets_state();
}

static Commands reg({
    {"guide.open", guide_open},
    {"guide.message", guide_message},
    {"guide.cancelled", guide_cancelled},
});

} // namespace orca
