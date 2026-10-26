// The Python plugin system (slic3r/plugin, compiled from upstream): started like GUI_App::on_init_inner does it (the
// manager with its libslic3r hooks, the cloud plugin folder, discovery, the GUI wiring of init_plugin_gui_wiring,
// the enabled plugins loaded), off the start-up path on the engine thread.
#include "slic3r/plugin/host/PluginResourceTextures.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include <pybind11/embed.h>
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/plugin/host/PluginHostUi.hpp"
#include "slic3r/plugin/host/PluginPages.hpp"

using namespace Slic3r;

namespace Slic3r { namespace GUI { class WebViewHostDialog; } }
namespace orca { namespace webdlg { Slic3r::GUI::WebViewHostDialog *open_gui_dialog(const std::string &name); } }
void refresh_open_plugins_dialog();   // below: the dialog's own header

namespace orca {

PluginPages &plugin_pages()
{
    static PluginPages pages;
    return pages;
}

// GUI_App::init_plugin_gui_wiring: what follows a plugin's load and unload (its windows closed, the Plugins dialog
// and the pages refreshed); the Python printer agents need the Bambu device stack, which is not on Android
static void init_plugin_gui_wiring()
{
    PluginManager &plugin_mgr = PluginManager::instance();
    // GUI_App: m_plugins_dlg->update_plugin_dialog_ui() on the UI thread
    auto refresh_plugins_dialog = [] { Core::call_after([] { refresh_open_plugins_dialog(); }); };
    plugin_mgr.subscribe_on_unload_callback(PluginHostUi::close_windows_for_plugin);
    plugin_mgr.subscribe_on_load_callback([refresh_plugins_dialog](const std::string &) { refresh_plugins_dialog(); });
    plugin_mgr.subscribe_on_unload_callback([refresh_plugins_dialog](const std::string &) { refresh_plugins_dialog(); });
    plugin_mgr.subscribe_on_load_callback([](const std::string &plugin_key) { plugin_pages().on_plugin_register(plugin_key); });
    plugin_mgr.subscribe_on_unload_callback([](const std::string &plugin_key) { plugin_pages().on_plugin_deregister(plugin_key); });
    plugin_mgr.subscribe_on_capability_load_callback([refresh_plugins_dialog](const PluginCapabilityId &capability) {
        refresh_plugins_dialog();
        // a newly loaded capability may satisfy a missing-plugin notification of the plate
        Core::call_after([] { Host::event("project", json::object()); });
        if (capability.type == PluginCapabilityType::Pages) plugin_pages().on_cap_register(capability);
    });
    plugin_mgr.subscribe_on_capability_unload_callback([refresh_plugins_dialog](const PluginCapabilityId &capability) {
        if (capability.type == PluginCapabilityType::Pages) plugin_pages().on_cap_deregister(capability);
        refresh_plugins_dialog();
    });
    // the texture packs (ResourceTexture capabilities) of the Texture displacement library
    plugin_mgr.subscribe_on_load_callback([](const std::string &plugin_key) {
        PluginResourceTextures::instance().on_plugin_register(plugin_key);
        Slic3r::GUI::bump_texture_library_generation();
        Core::call_after([] { Host::event("gizmo_changed", json::object()); });   // an open texture picker lists it
    });
    plugin_mgr.subscribe_on_unload_callback([](const std::string &plugin_key) {
        PluginResourceTextures::instance().on_plugin_deregister(plugin_key);
        Slic3r::GUI::bump_texture_library_generation();
        Core::call_after([] { Host::event("gizmo_changed", json::object()); });   // an open texture picker lists it
    });
    plugin_mgr.subscribe_on_capability_load_callback([](const PluginCapabilityId &capability) {
        if (capability.type != PluginCapabilityType::ResourceTexture) return;
        PluginResourceTextures::instance().on_cap_register(capability);
        Slic3r::GUI::bump_texture_library_generation();
        Core::call_after([] { Host::event("gizmo_changed", json::object()); });   // an open texture picker lists it
    });
    plugin_mgr.subscribe_on_capability_unload_callback([](const PluginCapabilityId &capability) {
        if (capability.type != PluginCapabilityType::ResourceTexture) return;
        PluginResourceTextures::instance().on_cap_deregister(capability);
        Slic3r::GUI::bump_texture_library_generation();
        Core::call_after([] { Host::event("gizmo_changed", json::object()); });   // an open texture picker lists it
    });
    // packs already loaded before this wiring
    PluginResourceTextures::instance().initial_sweep();
    Slic3r::GUI::bump_texture_library_generation();
}

// GUI_App::on_init_inner: "Loading Plugins"
void plugins_init()
{
    Core::call_after([] {
        try {
            PluginManager &plugin_mgr = PluginManager::instance();
            plugin_mgr.initialize();
            // pybind11 creates its internals on the first thread that needs them and stores that thread's state in
            // them; created under a temporary PyGILState state, the slot keeps a freed thread state, and the next
            // gil_scoped_acquire on that thread (a plugin loader worker) resumes it. Created here, on the engine
            // thread, with the slot cleared.
            if (PythonInterpreter::instance().is_initialized()) {
                const PyGILState_STATE gil = PyGILState_Ensure();
                pybind11::detail::get_internals().tstate.reset();
                PyGILState_Release(gil);
            }
            // the cloud plugins of the last session's account are found before the network agent is ready
            const std::string preset_folder = Core::get().app_config->get("preset_folder");
            if (!preset_folder.empty()) plugin_mgr.set_cloud_user(preset_folder);
            plugin_mgr.discover_plugins(false, true);
            init_plugin_gui_wiring();
            for (const std::string &plugin_key : plugin_mgr.get_enabled_plugin_keys()) {
                if (!plugin_mgr.is_plugin_loaded(plugin_key)) {
                    plugin_mgr.load_plugin(plugin_key, false);
                    BOOST_LOG_TRIVIAL(info) << "Auto-loading plugin on startup: " << plugin_key;
                }
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "plugins: " << e.what();
        }
    });
}

static Commands reg({
    // the interpreter and the plugins found (a first check; the Plugins dialog lists them)
    {"plugins.status", [](const json &) -> json {
         PluginManager &m = PluginManager::instance();
         json list = json::array();
         for (const std::string &key : m.get_enabled_plugin_keys()) list.push_back({{"key", key}, {"loaded", m.is_plugin_loaded(key)}});
         return {{"python", PythonInterpreter::instance().is_initialized()},
                 {"error", PythonInterpreter::instance().last_error()},
                 {"enabled", list}};
     }},
});

} // namespace orca

// ---- PluginPages: a plugin's pages are tabs of the app's main tab bar (the "plugin_pages" event lists them, the app asks
// plugins.pages); a shown page gets its HTML from the capability (PluginPage::page_html) and talks to it through the
// page bridge of PluginPages.cpp, its messages going to on_message and the capability's post_message back to the page
#include "slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp"
#include "slic3r/GUI/Widgets/WebHosting.hpp"
#include "slic3r/GUI/Widgets/WebViewHostDialog.hpp"
namespace Slic3r {
namespace {
std::vector<PluginCapabilityId> g_page_order;   // PluginPages::m_order
std::string page_tab_id(const PluginCapabilityId &id) { return "plugin." + id.plugin_key + "." + id.name; }

std::shared_ptr<PagesPluginCapability> get_pages_cap(const PluginCapabilityId &id)
{
    auto capability = PluginManager::instance().get_plugin_capability(id, /*only_enabled=*/false);
    if (!capability || !capability->is_enabled() || capability->type() != PluginCapabilityType::Pages) return nullptr;
    return std::dynamic_pointer_cast<PagesPluginCapability>(capability);
}

void pages_changed() { orca::Host::event("plugin_pages", orca::json::object()); }

// PluginPages.cpp's PLUGIN_PAGE_BRIDGE_JS: window.orca with postMessage / onMessage over window.wx
constexpr char PLUGIN_PAGE_BRIDGE_JS[] = R"JS(
(function () {
  if (window.top !== window.self) return;
  if (window.orca) return;
  var handlers = [];
  function deliver(payload, attempts) {
    try {
      if (window.wx && typeof window.wx.postMessage === 'function') {
        window.wx.postMessage(payload);
        return;
      }
    } catch (e) { /* retry while the native handler is being registered */ }
    if (attempts < 100)
      window.setTimeout(function () { deliver(payload, attempts + 1); }, 25);
  }
  function send(data) {
    deliver(JSON.stringify({
      channel: 'orca', kind: 'message', data: (data === undefined ? null : data)
    }), 0);
  }
  window.orca = {
    postMessage: function (data) { send(data); },
    onMessage: function (callback) {
      if (typeof callback === 'function') handlers.push(callback);
    }
  };
  window.__orcaDispatch = function (payload) {
    var data = payload ? payload.data : null;
    for (var i = 0; i < handlers.length; i++) {
      try { handlers[i](data); } catch (e) {}
    }
  };
})();
)JS";
} // namespace

void PluginPages::on_cap_register(const PluginCapabilityId &id)
{
    if (std::find(g_page_order.begin(), g_page_order.end(), id) != g_page_order.end() || !get_pages_cap(id)) return;
    g_page_order.push_back(id);
    pages_changed();
}
void PluginPages::on_cap_deregister(const PluginCapabilityId &id)
{
    if (auto cap = get_pages_cap(id)) cap->clear_message_sender();
    g_page_order.erase(std::remove(g_page_order.begin(), g_page_order.end(), id), g_page_order.end());
    pages_changed();
}
void PluginPages::on_plugin_register(const std::string &plugin_key)
{
    for (const auto &capability : PluginManager::instance().get_plugin_capabilities(plugin_key, PluginCapabilityType::Pages))
        if (capability) on_cap_register(capability->identity());
}
void PluginPages::on_plugin_deregister(const std::string &plugin_key)
{
    for (const PluginCapabilityId &id : std::vector<PluginCapabilityId>(g_page_order))
        if (id.plugin_key == plugin_key) on_cap_deregister(id);
}
void PluginPages::set_visible_page_count(int) { pages_changed(); }
void PluginPages::select_page(const PluginCapabilityId &id) { orca::Host::event("plugin_page_select", {{"id", page_tab_id(id)}}); }
} // namespace Slic3r

namespace orca {
static PluginCapabilityId page_arg(const json &a)
{
    const std::string id = a.at("id");
    for (const PluginCapabilityId &c : Slic3r::g_page_order)
        if (Slic3r::page_tab_id(c) == id) return c;
    throw CommandError("no plugin page " + id);
}

static Commands reg_pages({
    // the tabs: in order, with how many the tab bar shows before its overflow menu (Preferences)
    {"plugins.pages", [](const json &) -> json {
         json pages = json::array();
         for (const PluginCapabilityId &id : Slic3r::g_page_order) {
             std::string icon;
             if (auto cap = Slic3r::get_pages_cap(id)) {
                 try { icon = cap->get_icon(); } catch (...) {}
             }
             pages.push_back({{"id", Slic3r::page_tab_id(id)}, {"name", id.name}, {"plugin", id.plugin_key}, {"icon", icon}});
         }
         return {{"pages", pages}, {"visible", Core::get().app_config->get_plugin_pages_visible_count()}};
     }},
    // PluginPage: the page's HTML (get_ui) with the bridge, and the capability's post_message routed to it
    {"plugins.page_open", [](const json &a) -> json {
         const PluginCapabilityId id = page_arg(a);
         auto cap                    = Slic3r::get_pages_cap(id);
         if (!cap) throw CommandError("the page is not available");
         std::string html;
         try {
             html = cap->get_ui();
         } catch (const std::exception &e) {
             throw CommandError(std::string("Failed to load plugin page: ") + e.what());
         }
         const std::string tab = Slic3r::page_tab_id(id);
         cap->set_message_sender([tab](const std::string &message) {
             // PluginPage::push_message: JSON as it is, anything else as a string literal
             const std::string data = json::accept(message) ? message : json(message).dump(-1, ' ', false, json::error_handler_t::replace);
             Host::event("plugin_page", {{"id", tab}, {"script", "__orcaDispatch({\"data\":" + data + "})"}});
         });
         return {{"html", html},
                 {"base_url", Slic3r::GUI::web_hosting::content_base_url().ToStdString()},
                 {"script", Slic3r::GUI::WebViewHostDialog::theme_user_script() + "\n" + Slic3r::PLUGIN_PAGE_BRIDGE_JS}};
     }},
    // PluginPage::on_page_message (queued: the app sends it outside the engine queue)
    {"plugins.page_message", [](const json &a) -> json {
         const PluginCapabilityId id = page_arg(a);
         json payload = a.at("message").is_string() ? json::parse(a["message"].get<std::string>(), nullptr, false) : a["message"];
         Core::call_after([id, payload] {
             auto cap = Slic3r::get_pages_cap(id);
             if (!cap || payload.value("channel", std::string()) != "orca" || payload.value("kind", std::string()) != "message") return;
             try {
                 cap->on_message((payload.contains("data") ? payload["data"] : json()).dump(-1, ' ', false, json::error_handler_t::replace));
             } catch (const std::exception &e) {
                 BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << id.name << "': " << e.what();
             }
         });
         return nullptr;
     }},
    {"plugins.page_close", [](const json &a) -> json {
         if (auto cap = Slic3r::get_pages_cap(page_arg(a))) cap->clear_message_sender();
         return nullptr;
     }},
});
} // namespace orca

// ---- the Plugins dialog (PluginsDialog.cpp, compiled from upstream on the WebViewHostDialog stand-in)
#include "slic3r/GUI/PluginsDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace orca { namespace webdlg {
// GUI_App::open_plugins_dialog: the dialog and its terminal opener
std::unique_ptr<Slic3r::GUI::WebViewHostDialog> make_plugins_dialog()
{
    auto d = std::make_unique<Slic3r::GUI::PluginsDialog>(nullptr);
    d->set_open_terminal_dlg_fn();
    return d;
}
}} // namespace orca::webdlg

// GUI_App::open_terminal_dialog: the plugins' Python terminal (TerminalDialog), a web dialog of the app; an open one
// is brought to the front by the app
#include "slic3r/GUI/TerminalDialog.hpp"
void Slic3r::GUI::GUI_App::open_terminal_dialog() { orca::Host::event("open_web_dialog", {{"name", "terminal"}}); }
namespace orca { namespace webdlg {
std::unique_ptr<Slic3r::GUI::WebViewHostDialog> make_terminal_dialog()
{
    return std::make_unique<Slic3r::GUI::TerminalDialog>(nullptr, wxID_ANY, _L("Plugin Terminal"), wxDefaultPosition, wxSize(820, 600));
}
}} // namespace orca::webdlg

void Slic3r::GUI::desktop_open_any_folder(const std::string &path) { orca::Host::event("toast", {{"text", path}}); }

void refresh_open_plugins_dialog()
{
    if (auto *d = dynamic_cast<Slic3r::GUI::PluginsDialog *>(orca::webdlg::open_gui_dialog("plugins"))) d->update_plugin_dialog_ui();
}


// ---- GUI_App's plugin actions (the Speed Dial's Refresh Plugins / Install Local Plugin)
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "wx/filedlg.h"
namespace Slic3r { namespace GUI {
#include "slic3r/GUI/PluginsApp.inc"
}} // namespace Slic3r::GUI

static orca::Commands reg_app({
    {"plugins.refresh", [](const orca::json &) -> orca::json { Slic3r::GUI::wxGetApp().refresh_plugins(); return nullptr; }},
    {"plugins.open_hub", [](const orca::json &) -> orca::json { Slic3r::GUI::open_plugin_hub(); return nullptr; }},
    {"plugins.install_local", [](const orca::json &) -> orca::json { Slic3r::GUI::wxGetApp().install_local_plugin(); return nullptr; }},
});

// ---- the plugin settings of the presets: OptionsGroup::pick_plugin (PluginField's selector) with PluginPickerDialog as
// the app's choice dialog, and PluginsConfigDialog (PluginConfigField's button) as a web dialog of the app
#include <boost/log/trivial.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/PluginsConfigDialog.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
namespace Slic3r { namespace GUI {
namespace {
struct PluginPickerDialog
{
    struct CapabilityEntry
    {
        std::string plugin_key;
        std::string name;
        wxString    label;
        wxString    description;
    };
    PluginPickerDialog(void *, const wxString &plugin_type_label, std::vector<CapabilityEntry> capabilities)
        : m_label(plugin_type_label), m_entries(std::move(capabilities)) {}
    int ShowModal()
    {
        orca::json items = orca::json::array();
        for (const CapabilityEntry &e : m_entries) items.push_back({{"label", e.label.ToStdString()}, {"description", e.description.ToStdString()}});
        const orca::json r = orca::Host::ask({{"kind", "plugin_picker"},
                                              {"title", _u8L("Select a plugin")},
                                              {"type", m_label.ToStdString()},
                                              {"items", items},
                                              {"ok", _u8L("OK")},
                                              {"cancel", _u8L("Cancel")}});
        m_selection = r.value("index", -1);
        return r.value("ok", false) && m_selection >= 0 && m_selection < int(m_entries.size()) ? wxID_OK : wxID_CANCEL;
    }
    CapabilityEntry selected_capability() const { return m_selection >= 0 && m_selection < int(m_entries.size()) ? m_entries[m_selection] : CapabilityEntry{}; }
    void CenterOnParent() {}

private:
    wxString                     m_label;
    std::vector<CapabilityEntry> m_entries;
    int                          m_selection{-1};
};
struct PickGroup
{
    void       *m_parent{nullptr};
    std::string pick_plugin(const ConfigOptionDef &opt);
};
} // namespace
#include "slic3r/GUI/PluginsApp.inc.pick"
}} // namespace Slic3r::GUI

namespace orca { namespace webdlg {
std::unique_ptr<Slic3r::GUI::WebViewHostDialog> make_plugins_config_dialog(const std::string &type, const std::string &overrides)
{
    const Slic3r::Preset::Type t = type == "filament" ? Slic3r::Preset::TYPE_FILAMENT : type == "printer" ? Slic3r::Preset::TYPE_PRINTER : Slic3r::Preset::TYPE_PRINT;
    return std::make_unique<Slic3r::GUI::PluginsConfigDialog>(nullptr, t, overrides);
}
std::string plugins_config_result(Slic3r::GUI::WebViewHostDialog *d)
{
    auto *c = dynamic_cast<Slic3r::GUI::PluginsConfigDialog *>(d);
    return c ? c->overrides_json() : std::string();
}
}} // namespace orca::webdlg

static orca::Commands reg_settings({
    // PluginField: the capability picked for a plugin_picker option (empty when canceled)
    {"plugins.pick", [](const orca::json &a) -> orca::json {
         const Slic3r::ConfigOptionDef *def = Slic3r::print_config_def.get(a.at("key").get<std::string>());
         if (def == nullptr) throw orca::CommandError("unknown option");
         Slic3r::GUI::PickGroup g;
         return g.pick_plugin(*def);
     }},
});
