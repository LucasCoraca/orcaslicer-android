// The Preferences dialog (PreferencesDialog): its tabs and items are read from upstream's create_items() at build time
// (tools/gen_preferences.py); here they get translated, their current app config values, and the side effects a change
// has on the desktop (backup interval, units, ...). Items that make no sense on a touch device (window buttons,
// mouse buttons, keyboard, single instance, file association, OpenGL benchmark) are left out.
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

namespace {

#include "slic3r/GUI/Preferences.inc"

// desktop-only rows
const std::set<std::string> hidden_keys = {"single_instance", "show_splash_screen", "window_buttons_on_left", "left_mouse_drag_action",
                                           "middle_mouse_drag_action", "right_mouse_drag_action", "camera_navigation_style",
                                           "reverse_mouse_wheel_zoom", "zoom_to_mouse", "enable_speed_dial", "speed_dial_recent_count",
                                           "opengl_antialiasing_samples", "opengl_fxaa_enabled", "opengl_fps_cap", "opengl_show_render_timings",
                                           "use_encrypted_token_file", "enable_multi_machine"};
const std::set<std::string> hidden_vars = {"item_downloads"};
const std::set<std::string> hidden_kinds = {"link_association", "network_plugin_version", "bambu_cloud", "region"};

json tr(const json &v)
{
    if (v.is_object() && v.contains("tr")) {
        std::string s = Slic3r::GUI::I18N::translate_utf8(v["tr"].get<std::string>().c_str());
        if (v.value("dots", false)) s += "...";
        return s;
    }
    if (v.is_object() && v.contains("concat")) {
        std::string s;
        for (const auto &p : v["concat"]) s += tr(p).get<std::string>();
        return s;
    }
    if (v.is_array()) {
        json out = json::array();
        for (const auto &e : v) out.push_back(tr(e));
        return out;
    }
    return v;
}

// the languages with a catalog in resources/i18n (create_item_language_combobox)
json languages()
{
    json out = json::array();
    out.push_back({{"value", "en"}, {"label", "English"}});
    static const std::map<std::string, std::string> names = {
        {"ca", "Català"}, {"cs", "Čeština"}, {"de", "Deutsch"}, {"es", "Español"}, {"eu", "Euskara"}, {"fr", "Français"}, {"hu", "Magyar"},
        {"it", "Italiano"}, {"ja", "日本語"}, {"ko", "한국어"}, {"lt", "Lietuvių"}, {"nl", "Nederlands"}, {"pl", "Polski"},
        {"pt_BR", "Português (Brasil)"}, {"ru", "Русский"}, {"sv", "Svenska"}, {"th", "ไทย"}, {"tr", "Türkçe"}, {"uk", "Українська"},
        {"vi", "Tiếng Việt"}, {"zh_CN", "简体中文"}, {"zh_TW", "繁體中文"}};
    boost::system::error_code ec;
    for (fs::directory_iterator it(fs::path(resources_dir()) / "i18n", ec), end; it != end && !ec; it.increment(ec)) {
        const std::string l = it->path().filename().string();
        if (l == "en" || !fs::is_directory(it->path())) continue;
        auto n = names.find(l);
        out.push_back({{"value", l}, {"label", n != names.end() ? n->second : l}});
    }
    return out;
}

json prefs_get(const json &)
{
    AppConfig &cfg = *Core::get().app_config;
    json tabs = json::array();
    for (const json &t : json::parse(k_preferences_layout)) {
        json items = json::array();
        for (json it : t["items"]) {
            const std::string kind = it["kind"], key = it.value("key", std::string()), var = it.value("var", std::string());
            if (hidden_keys.count(key) || hidden_vars.count(var) || hidden_kinds.count(kind)) continue;
            for (const char *k : {"label", "tooltip", "secondary", "side", "button", "options"})
                if (it.contains(k)) it[k] = tr(it[k]);
            if (it.value("tooltip", json()).is_string() && it["tooltip"].get<std::string>().empty()) it["tooltip"] = it.value("label", json());
            if (!key.empty()) it["value"] = cfg.get(key);
            if (kind == "language") {
                it["options"] = languages();
                it["value"] = cfg.get("language");
            } else if (kind == "backup") {
                it["value"] = cfg.get("backup_switch") == "true";
                it["interval"] = cfg.get("backup_interval").empty() ? std::string("10") : cfg.get("backup_interval");
                it["side"] = _u8L("sec");
            } else if (kind == "auto_reslice") {
                it["value"] = cfg.get_bool("auto_slice_after_change");
                it["interval"] = cfg.get("auto_slice_change_delay_seconds");
            } else if (kind == "darkmode") {
                it["value"] = cfg.get("dark_color_mode");
            } else if (kind == "loglevel") {
                it["options"] = json::array({_u8L("fatal"), _u8L("error"), _u8L("warning"), _u8L("info"), _u8L("debug"), _u8L("trace")});
                it["value"] = std::to_string(level_string_to_boost(cfg.get("log_severity_level")));
            } else if (kind == "combobox" && it["options"].empty())
                continue;   // filled by GUI code (the preview's view types)
            items.push_back(it);
        }
        // drop titles left without rows
        json kept = json::array();
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i]["kind"] != "title" || (i + 1 < items.size() && items[i + 1]["kind"] != "title")) kept.push_back(items[i]);
        if (!kept.empty()) tabs.push_back({{"tab", tr(t["tab"])}, {"items", kept}});
    }
    return {{"tabs", tabs}, {"title", _u8L("Preferences")}, {"restart", _u8L("(Requires restart)")}};
}

// the rows' changes and their side effects
json prefs_set(const json &a)
{
    AppConfig &cfg = *Core::get().app_config;
    const std::string kind = a.value("kind", std::string("checkbox"));
    if (kind == "backup") {
        if (a.contains("value")) cfg.set_bool("backup_switch", a["value"].get<bool>());
        if (a.contains("interval")) cfg.set("backup_interval", a["interval"].get<std::string>());
        const bool on = cfg.get("backup_switch") == "true";
        long interval = 10;
        try { interval = std::stol(cfg.get("backup_interval")); } catch (...) {}
        set_backup_interval(on ? interval : 0);
    } else if (kind == "auto_reslice") {
        if (a.contains("value")) cfg.set_bool("auto_slice_after_change", a["value"].get<bool>());
        if (a.contains("interval")) cfg.set("auto_slice_change_delay_seconds", a["interval"].get<std::string>());
    } else if (kind == "darkmode") {
        cfg.set("dark_color_mode", a.at("value").get<std::string>());
    } else if (kind == "loglevel") {
        // PreferencesDialog::create_item_loglevel_combobox: the level's name is stored
        const std::string level = get_string_logging_level(unsigned(std::stoi(a.at("value").get<std::string>())));
        set_logging_level(level_string_to_boost(level));
        cfg.set("log_severity_level", level);
    } else if (kind == "language") {
        cfg.set("language", a.at("value").get<std::string>());
    } else if (kind == "button") {
        if (a.contains("clear")) cfg.set(a["clear"].get<std::string>(), "");
        if (a.contains("erase")) cfg.erase(a["erase"][0].get<std::string>(), a["erase"][1].get<std::string>());
    } else {
        const std::string key = a.at("key");
        const json &v = a.at("value");
        cfg.set(key, v.is_string() ? v.get<std::string>() : v.is_boolean() ? std::string(v.get<bool>() ? "true" : "false") : v.dump());
        if (key == "use_inches") Host::event("project", Plater::get().state());   // the units of the panels
    }
    cfg.save();
    Viewport::get().update_view_presets();   // the Graphics rows (realistic view) apply at once
    return prefs_get(a);
}

} // namespace

// Core::init: the user's language (Preferences) wins over the device's
std::string preferred_language(const std::string &device)
{
    const std::string l = Core::get().app_config->get("language");
    return l.empty() ? device : l;
}

// GUI_App::on_init_inner (not Windows): dark_color_mode is the system appearance, sent at start and on every change
static json system_appearance(const json &a)
{
    AppConfig &cfg = *Core::get().app_config;
    const std::string mode = a.value("dark", false) ? "1" : "0";
    if (cfg.get("dark_color_mode") != mode) {
        cfg.set("dark_color_mode", mode);
        cfg.save();
        Host::event("redraw", json::object());
    }
    return nullptr;
}

static Commands reg({
    {"prefs.get", prefs_get},
    {"prefs.set", prefs_set},
    {"app.appearance", system_appearance},
});

} // namespace orca
