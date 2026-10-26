#include <condition_variable>
#include <chrono>
#include "Core.hpp"
#include "slic3r/GUI/MainFrame.hpp"

#include <cctype>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "Host.hpp"
#include "Viewport.hpp"
#include "I18n.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace Slic3r { namespace GUI {
// what upstream GUI logic compiled into the core reaches through wxGetApp() (shim GUI_App.hpp)
GUI_App &wxGetApp()
{
    static GUI_App app;
    return app;
}
}} // namespace Slic3r::GUI

namespace orca {

Core &Core::get()
{
    static Core core;
    return core;
}

std::map<std::string, Core::Command> &Core::commands()
{
    static std::map<std::string, Command> map;
    return map;
}

void Core::add_command(const std::string &name, Command fn) { commands()[name] = std::move(fn); }
std::map<std::string, Core::Command> &Core::pure_commands()
{
    static std::map<std::string, Command> c;
    return c;
}
void Core::add_pure_command(const std::string &name, Command fn) { pure_commands()[name] = std::move(fn); }
json Core::call_pure(const std::string &cmd, const json &args)
{
    auto it = pure_commands().find(cmd);
    if (it == pure_commands().end()) throw CommandError("unknown command " + cmd);
    return it->second(args);
}

static std::mutex                         s_after_mutex;
static std::condition_variable            s_after_cv;
static std::vector<std::function<void()>> s_after;

void Core::call_after(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> l(s_after_mutex);
        s_after.push_back(std::move(fn));
    }
    s_after_cv.notify_all();
    Host::event("call_after", json::object());
}

void Core::pump_call_after(int timeout_ms)
{
    std::vector<std::function<void()>> fns;
    {
        std::unique_lock<std::mutex> l(s_after_mutex);
        s_after_cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [] { return !s_after.empty(); });
        fns.swap(s_after);
    }
    for (auto &fn : fns) {
        try { fn(); } catch (const std::exception &ex) { BOOST_LOG_TRIVIAL(error) << "call_after: " << ex.what(); }
    }
}

static json run_call_after(const json &)
{
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> l(s_after_mutex);
        fns.swap(s_after);
    }
    for (auto &fn : fns) {
        try { fn(); } catch (const std::exception &ex) { BOOST_LOG_TRIVIAL(error) << "call_after: " << ex.what(); }
    }
    return nullptr;
}
static Commands reg_after({{"core.call_after", run_call_after}});

Preset::Type preset_type(const std::string &name)
{
    if (name == "print") return Preset::TYPE_PRINT;
    if (name == "filament") return Preset::TYPE_FILAMENT;
    if (name == "printer") return Preset::TYPE_PRINTER;
    throw CommandError("unknown preset type " + name);
}

PresetCollection &collection(Preset::Type type)
{
    PresetBundle &b = *Core::get().preset_bundle;
    switch (type) {
    case Preset::TYPE_PRINT: return b.prints;
    case Preset::TYPE_FILAMENT: return b.filaments;
    case Preset::TYPE_PRINTER: return b.printers;
    default: throw CommandError("unsupported preset type");
    }
}

// Orca: copy/update the vendor profiles from resources to the system folder
// (upstream: PresetUpdater::priv::check_installed_vendor_profiles, slic3r/Utils/PresetUpdater.cpp).
static void check_installed_vendor_profiles(AppConfig &app_config)
{
    const auto            enabled_vendors = app_config.vendors();
    std::set<std::string> bundles;
    for (const std::string &vendor_name : vendor_names_in(fs::path(resources_dir()) / "profiles")) {
        const bool is_vendor_enabled = vendor_name == PresetBundle::ORCA_DEFAULT_BUNDLE || vendor_name == PresetBundle::ORCA_FILAMENT_LIBRARY ||
                                       enabled_vendors.find(vendor_name) != enabled_vendors.end();
        if (is_vendor_installed(vendor_name)) {
            if (is_vendor_enabled) {
                if (installed_vendor_version(vendor_name) < resource_vendor_version(vendor_name))
                    bundles.insert(vendor_name);
            } else
                remove_installed_vendor(vendor_name);
        } else if (is_vendor_enabled)
            bundles.insert(vendor_name);
    }
    if (!bundles.empty())
        install_vendor_bundles_from_resources(std::vector<std::string>(bundles.begin(), bundles.end()));
}

void backup_init();
void cloud_init();
void plugins_init();
std::string preferred_language(const std::string &device);

json Core::init(const std::string &resources, const std::string &data, const std::string &language)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // the engine thread plays the desktop's UI thread: AppConfig::save() and others only run there
    save_main_thread_id();
    set_resources_dir(resources);
    set_var_dir((fs::path(resources) / "images").string());
    set_local_dir((fs::path(resources) / "i18n").string());
    set_sys_shapes_dir((fs::path(resources) / "shapes").string());
    set_custom_gcodes_dir((fs::path(resources) / "custom_gcodes").string());
    fs::create_directories(data);
    set_data_dir(data);
    fs::create_directories(fs::path(data) / "tmp");
    set_temporary_dir((fs::path(data) / "tmp").string());
    fs::create_directories(fs::path(data) / "log");
    set_log_path_and_level("debug", 3);

    app_config = std::make_unique<AppConfig>();
    const bool app_conf_exists = app_config->exists();
    if (app_conf_exists) {
        std::string error = app_config->load();
        if (!error.empty())
            BOOST_LOG_TRIVIAL(error) << "app config: " << error;
    }
    // GUI_App::on_init_inner: the stored log level (earlier app versions stored its index)
    if (std::string lvl = app_config->get("log_severity_level"); lvl.size() == 1 && std::isdigit((unsigned char) lvl[0]))
        app_config->set("log_severity_level", get_string_logging_level(unsigned(lvl[0] - '0')));
    set_logging_level(level_string_to_boost(app_config->get("log_severity_level")));

    // the language chosen in Preferences, else the device's
    install_translate_callback();
    std::string lang = preferred_language(language);
    // a full locale ("de_DE", as the desktop stores it) falls back to its language's catalog ("de") like wxLocale does
    if (!lang.empty() && !fs::exists(resources + "/i18n/" + lang) && lang.find('_') != std::string::npos)
        lang = lang.substr(0, lang.find('_'));
    if (!lang.empty() && lang != "en")
        load_catalog(resources + "/i18n/" + lang + "/OrcaSlicer_" + lang + ".po", lang);
    // the mobile app opens with the orthographic camera; the user's later choice (View menu) is kept in Orca's own key
    if (app_config->get("mobile_camera_default").empty()) {
        app_config->set_bool("use_perspective_camera", false);
        app_config->set("mobile_camera_default", "1");
    }

    preset_bundle = std::make_unique<PresetBundle>();
    preset_bundle->setup_directories();
    check_installed_vendor_profiles(*app_config);
    PresetsConfigSubstitutions subs;
    std::string                load_error;
    try {
        subs = preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
    } catch (const std::exception &ex) {
        load_error = ex.what();
    }

    auto &app = GUI::wxGetApp();
    app.preset_bundle = preset_bundle.get();
    app.app_config    = app_config.get();
    app.plater_       = &plater;
    static GUI::MainFrame mainframe;   // the app's main window, as upstream logic checks for it
    app.mainframe     = &mainframe;
    app.model_        = &model;
    invalidate_full_config();
    plater.set_config(&full_config());
    backup_init();
    Viewport::get().update_view_presets();
    cloud_init();     // GUI_App::on_init_network: the OrcaCloud agent, a stored login comes back
    plugins_init();   // GUI_App::on_init_inner: "Loading Plugins" (Plugins.cpp, once the app drains the queue)

    return {{"version", SoftFever_VERSION},
            {"first_run", !app_conf_exists || preset_bundle->printers.only_default_printers()},
            {"error", load_error}};
}

const DynamicPrintConfig &Core::full_config()
{
    if (!m_full_config_valid) {
        m_full_config       = preset_bundle->full_config();
        m_full_config_valid = true;
    }
    return m_full_config;
}

json Core::call(const std::string &cmd, const json &args)
{
    auto it = commands().find(cmd);
    if (it == commands().end())
        throw CommandError("unknown command " + cmd);
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return it->second(args);
}

// app config keys (AppConfig::get / set), e.g. the "do not show again" flags of tip dialogs
static Commands reg({
    {"app.get", [](const json &a) { return json(Core::get().app_config->get(str_arg(a, "key"))); }},
    {"app.set", [](const json &a) {
        Core::get().app_config->set(str_arg(a, "key"), str_arg(a, "value"));
        return json(nullptr);
    }},
});

} // namespace orca
