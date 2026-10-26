// Speed Dial's registry: ActionRegistry (persisted favourites / run stats / frecency order, "Go to Plate N", the recent
// projects) and the NativeCommands catalog, compiled from upstream. The catalog's runners and the settings jump are
// the app's ("speed_dial_run" / "jump_to_option" events); the setting actions come from the app's settings layout
// (speeddial.settings), which is what upstream's settings index is built from.
#include "SpeedDial.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <tuple>
#include <unordered_set>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Menu.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"

namespace orca { namespace sd {
using namespace Slic3r;
using Slic3r::GUI::I18N::translate_utf8;
using Slic3r::GUI::from_u8;
namespace I18N = Slic3r::GUI::I18N;

ConfigOptionMode get_mode()
{
    // Settings.cpp current_mode: the mode the settings pages show
    const std::string m = Core::get().app_config->get("user_mode");
    return m == "simple" ? comSimple : m == "advanced" ? comAdvanced : m == "develop" ? comDevelop : comExpert;
}
void save_mode(ConfigOptionMode mode)
{
    const char *m = mode == comSimple ? "simple" : mode == comAdvanced ? "advanced" : mode == comDevelop ? "develop" : "expert";
    Core::get().app_config->set("user_mode", m);
    Core::get().app_config->save();
    Host::event("mode_changed", {{"mode", m}});
}
void enable_developer_mode()
{
    Core::get().app_config->set_bool("developer_mode", true);
    save_mode(comDevelop);
}

// ---- what the extracted bodies reach through wxGetApp()
struct PartPlate
{
    std::string name;
    std::string get_plate_name() const { return name; }
};
struct PartPlateList
{
    std::vector<PartPlate>   plates;
    std::vector<PartPlate *> ptrs;
    const std::vector<PartPlate *> &get_plate_list() const { return ptrs; }
};
struct Plater
{
    PartPlateList  list;
    PrinterTechnology printer_technology() const { return ptFFF; }
    bool           only_gcode_mode() const { return orca::Plater::get().only_gcode; }
    // PartPlateList of the project
    PartPlateList &get_partplate_list()
    {
        list.plates.clear();
        list.ptrs.clear();
        for (const Plate &p : orca::Plater::get().plates) list.plates.push_back({p.name});
        for (PartPlate &p : list.plates) list.ptrs.push_back(&p);
        return list;
    }
};
// a plugin's page: the app's main tab of it (PluginPages::select_page)
struct SdPluginPages
{
    void select_page(const PluginCapabilityId &id) { Host::event("plugin_page_select", {{"plugin", id.plugin_key}, {"name", id.name}}); }
};
// MainFrame::open_recent_project / Sidebar::jump_to_option: the app's
struct MainFrame
{
    SdPluginPages  pages;
    SdPluginPages &plugin_pages() { return pages; }
    void open_recent_project(size_t, const wxString &path) { Host::event("speed_dial_run", {{"key", "open_recent_project"}, {"param", path.ToStdString()}}); }
};
struct SdApp
{
    AppConfig       *app_config{nullptr};
    MainFrame        main;
    MainFrame       *mainframe{&main};
    Plater           plater_;
    Plater          *plater() { return &plater_; }
    ConfigOptionMode get_mode() const { return sd::get_mode(); }
    bool             is_closing() const { return false; }
    template<class F> void CallAfter(F &&f) { Core::call_after(std::forward<F>(f)); }
};
SdApp &wxGetApp()
{
    static SdApp app;
    app.app_config = Core::get().app_config.get();
    return app;
}
constexpr bool wxTheApp = true;

void jump_to_option(const std::string &opt_key, Preset::Type type, const std::wstring &category)
{
    const char *t = type == Preset::TYPE_FILAMENT ? "filament" : type == Preset::TYPE_PRINTER ? "printer" : "print";
    Host::event("jump_to_option", {{"key", opt_key}, {"type", t}, {"category", boost::nowide::narrow(category)}});
}

// ---- NativeCommands
struct NativeCommand
{
    std::string key, title, group, input, icon;
    std::function<AppActionRunResult(const std::string &param)> runner;
};
namespace MenuFactory {
using HandyModel = orca::HandyModel;
inline const std::vector<HandyModel> &handy_models() { return orca::handy_models(); }
} // namespace MenuFactory
// The catalog entries the app has no counterpart for (AMS, the desktop's window layout) are left out of the palette.
bool unsupported(const std::string &key)
{
    static const std::unordered_set<std::string> keys{"sync_ams", "reset_window_layout"};
    return keys.count(key) > 0;
}
namespace NativeCommands {
const std::vector<NativeCommand> &catalog();
std::unique_ptr<AppAction>        make_action(const NativeCommand &command);
// the app runs the command (its menus / toolbars / dialogs do the same)
AppActionRunResult run(const std::string &key, const std::string &param = {})
{
    Host::event("speed_dial_run", {{"key", key}, {"param", param}});
    return {AppActionRunResult::Level::Success};
}
} // namespace NativeCommands

constexpr const char *kCommandPrefix = "orca_command";
namespace {
#include "slic3r/GUI/SpeedDialCatalog.inc"
} // namespace

const std::vector<NativeCommand> &NativeCommands::catalog()
{
    static const std::vector<NativeCommand> commands = [] {
        std::vector<NativeCommand> all = build_command_catalog(), out;
        for (NativeCommand &c : all)
            if (!unsupported(c.key)) out.push_back(std::move(c));
        return out;
    }();
    return commands;
}
std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand &command) { return std::make_unique<CommandAction>(command); }

// ---- ActionRegistry
constexpr const char *kConfigSection       = "speed_dial";
constexpr const char *kSettingPrefix       = "orca_setting";
constexpr const char *kPlateGotoPrefix     = "orca_plate_goto";
constexpr const char *kRecentProjectPrefix = "orca_recent_project";
#include "slic3r/GUI/SpeedDialRegistry.inc"

ActionRegistry::~ActionRegistry() = default;
#include "slic3r/GUI/SpeedDialRegistryFns.inc"


// the settings rows of the app's settings pages (Sidebar::settings_index): what speeddial.settings last sent
static nlohmann::json g_setting_rows = nlohmann::json::array();

// materialize_setting_actions from the app's rows: {key, type, title, title_en, page, page_en, group, group_en,
// full_label, full_label_en, mode, icon, tooltip, wiki}
void ActionRegistry::materialize_setting_actions()
{
    nlohmann::json           stats;
    std::vector<std::string> favs;
    load_persisted(stats, favs);
    std::unordered_set<std::string> seen;
    for (const nlohmann::json &r : g_setting_rows) {
        const std::string t    = r.value("type", "print");
        const Preset::Type type = t == "filament" ? Preset::TYPE_FILAMENT : t == "printer" ? Preset::TYPE_PRINTER : Preset::TYPE_PRINT;
        const std::string key  = r.value("key", "");
        const std::string id   = SettingAction::id_for(key, type);
        if (key.empty() || seen.count(id)) continue;
        seen.insert(id);
        // the breadcrumb "Process : Quality : Layers", localized and English (setting_text)
        auto path = [](std::string type_name, const std::string &category, const std::string &group) {
            if (!category.empty()) type_name += " : " + category;
            if (!group.empty()) type_name += " : " + group;
            return type_name;
        };
        const char *type_context = setting_type_context(type);
        const std::string mode   = r.value("mode", "simple");
        const ConfigOptionMode m = mode == "advanced" ? comAdvanced : mode == "expert" ? comExpert : mode == "develop" ? comDevelop : comSimple;
        const std::string page   = r.value("page_en", "");
        auto action = std::make_unique<SettingAction>(key, type, r.value("title", key), std::string(), boost::nowide::widen(page),
                                                      path(translate_utf8(type_context), r.value("page", ""), r.value("group", "")), m);
        action->full_label    = r.value("full_label", "");
        action->title_en      = r.value("title_en", "");
        action->full_label_en = r.value("full_label_en", "");
        action->source_en     = path(type_context, page, r.value("group_en", ""));
        action->icon          = r.value("icon", "");
        action->tooltip       = r.value("tooltip", "");
        if (const std::string wiki = r.value("wiki", ""); !wiki.empty())
            action->help_url = "https://www.orcaslicer.com/wiki/" + wiki;   // OptionsGroup::get_url
        seed_from(stats, favs, id, *action);
        const std::string action_id = action->id();
        m_actions.insert_or_assign(action_id, std::shared_ptr<AppAction>(std::move(action)));
    }
    drop_stale(m_actions, kSettingPrefix, seen);
}

// "Go to tab...": the main tabs of the app
nlohmann::json ActionRegistry::tab_options() const
{
    nlohmann::json out = nlohmann::json::array();
    for (auto [id, title, icon] : std::initializer_list<std::tuple<const char *, const char *, const char *>>{
             {"home", L("Home"), "tab_home_active"}, {"prepare", L("Prepare"), "tab_3d_active"}, {"preview", L("Preview"), "tab_preview_active"},
             {"monitor", L("Device"), "tab_monitor_active"}, {"project", L("Project"), "tab_auxiliary_active"}}) {
        nlohmann::json tab({{"id", id}, {"title", translate_utf8(title)}, {"icon", icon}});
        if (tab["title"] != title) tab["title_en"] = title;
        out.push_back(std::move(tab));
    }
    return out;
}

ActionRegistry &registry()
{
    static ActionRegistry r;
    static bool started = (r.init(), true);
    (void) started;
    return r;
}

static nlohmann::json set_settings(const nlohmann::json &a)
{
    if (a.contains("rows") && a["rows"].is_array()) g_setting_rows = a["rows"];
    return nullptr;
}

// "toggle_developer_mode": Preferences > Developer > Developer mode, flipped
static nlohmann::json toggle_developer_mode(const nlohmann::json &)
{
    AppConfig *cfg = Core::get().app_config.get();
    const bool on = !cfg->get_bool("developer_mode");
    cfg->set_bool("developer_mode", on);
    cfg->save();
    return on ? _u8L("Developer mode enabled.") : _u8L("Developer mode disabled.");
}

}} // namespace orca::sd

namespace orca {
static Commands reg({{"speeddial.settings", sd::set_settings}, {"speeddial.toggle_developer_mode", sd::toggle_developer_mode}});
} // namespace orca
