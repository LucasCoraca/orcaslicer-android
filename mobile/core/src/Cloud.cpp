// The OrcaCloud account: GUI_App::on_init_network's NetworkAgent with the OrcaCloud service agent, the login dialog
// (ZUserLogin's page commands, compiled from upstream), the loopback server of the browser sign-in (HttpServer), the
// login handling (on_user_login_handle), logout, the user preset folder and the user preset / subscribed bundle sync
// (GUI_App's functions compiled from upstream on a stand-in app, CloudApp).
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/optional/optional_io.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/thread.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "wx/msgdlg.h"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/InstanceLock.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/HttpServer.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/OrcaCloudServiceAgent.hpp"
#include "wx/utils.h"

namespace orca {
std::string preferred_language(const std::string &device);
}

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace cloud {

// ---- wx and GUI stand-ins the extracted bodies use
struct wxEvtHandler {};
struct wxCommandEvent;
// EVT_UPDATE_BUNDLE_COMPLETE to the bundle dialog: the app's page re-reads its list on its own
inline void wxQueueEvent(void *, wxCommandEvent *evt);
template<class... A> wxString format_wxstr(const wxString &f, A &&...a) { return wxString(Slic3r::format(f.ToStdString(), std::forward<A>(a)...)); }
inline void show_error(void *, const wxString &msg)
{
    orca::Host::event("info_dialog", {{"title", _u8L("Error")}, {"text", msg.ToStdString()}});
}
enum { EVT_HTTP_ERROR = 1, EVT_UPDATE_BUNDLE_COMPLETE, EVT_UPDATE_PRESET_BUNDLE };
struct wxCommandEvent
{
    explicit wxCommandEvent(int type = 0) : type(type) {}
    int      type, m_int{0};
    wxString m_str;
    int      GetInt() const { return m_int; }
    wxString GetString() const { return m_str; }
    void     SetInt(int i) { m_int = i; }
    void     SetString(const wxString &s) { m_str = s; }
};

// wxProgressDialog of a manual sync: the app's progress line
inline void wxQueueEvent(void *, wxCommandEvent *evt) { delete evt; }

struct ProgressDialog
{
    ProgressDialog(const wxString &title, const wxString &msg, int, void *, int) { Update(0, msg); }
    bool Update(int percent, const wxString &msg = wxString())
    {
        orca::Host::event("progress", {{"text", msg.ToStdString()}, {"percent", percent}});
        return true;
    }
    bool WasCanceled() const { return false; }
    void Destroy() { orca::Host::event("progress", {{"text", ""}, {"percent", 100}}); delete this; }
};
enum { wxPD_AUTO_HIDE = 1, wxPD_APP_MODAL = 2, wxPD_CAN_ABORT = 4 };
struct wxBusyCursor {};

enum class NotificationType { CustomNotification, BBLUserPresetExceedLimit };
struct NotificationManager
{
    enum class NotificationLevel { RegularNotificationLevel };
    void push_notification(NotificationType, NotificationLevel, const std::string &text) { orca::Host::event("toast", {{"text", text}}); }
    void push_notification(NotificationType type)
    {
        if (type == NotificationType::BBLUserPresetExceedLimit)
            orca::Host::event("toast", {{"text", _u8L("The number of user presets cached in the cloud has exceeded the upper limit, newly created user presets can only be used locally.")}});
    }
    void push_notification(const std::string &text) { orca::Host::event("toast", {{"text", text}}); }
    void close_notification_of_type(NotificationType) {}
    void push_hint_notification(bool) {}
    // a sync conflict: the app shows the text with Pull / Force push (Delete for a tombstone)
    template<class Pull, class Push> void push_orca_sync_conflict_notification(const std::string &text, int code, Pull pull, Push push);
};

struct Sidebar { void update_all_preset_comboboxes() {} void update_presets(Preset::Type) {} };
struct CloudPlater
{
    NotificationManager  m_notifications;
    Sidebar              m_sidebar;
    NotificationManager *get_notification_manager() { return &m_notifications; }
    Sidebar             &sidebar() { return m_sidebar; }
    void                 normalize_bed_types(bool) {}
};
struct CloudMainFrame
{
    // MainFrame::update_side_preset_ui: the sidebar and the tabs follow the presets
    void update_side_preset_ui() { orca::Host::event("presets", orca::Core::get().call("presets.state", json::object())); }
};
struct CloudTab { void reload_config() {} void update_changed_ui() {} };
struct ImGuiState { bool display_initialized() const { return true; } };

struct PluginManager
{
    static PluginManager &instance() { static PluginManager m; return m; }
    void unload_cloud_plugins() {}
    void clear_cloud_plugin_metadata() {}
    void set_cloud_user(const std::string &) {}
    void fetch_plugins_from_cloud(std::vector<std::string> * = nullptr, std::vector<std::string> * = nullptr) {}
};
// WebViewPanel: the home page gets its scripts through run_script; its OrcaCloud pane is the app's
bool g_cloud_pane_open = false;
struct WebViewPanel
{
    static WebViewPanel *if_built() { static WebViewPanel p; return &p; }
    void SetLoginPanelVisibility(bool) {}
    // an explicit sign-out ends the pane's own web session (the sign-out page, then its stored data)
    void cloud_signed_out(const wxString &url) { orca::Host::event("cloud_pane", {{"signout", url.ToStdString()}}); }
    bool cloud_mode_active() const { return g_cloud_pane_open; }
    void set_cloud_mode(bool on, const wxString &url = wxString(), const wxString & = wxString())
    {
        if (on) orca::Host::event("cloud_pane", {{"seed", url.ToStdString()}});
    }
};
struct DeviceManager { void clean_user_info(bool) {} };
struct PluginsDialog { void update_plugin_dialog_ui() {} };

enum ActionButtons { KEEP = 1, SAVE = 2 };

static std::mutex conflict_ids_mutex;
static std::mutex mutex_delete_cache_presets;

// the GUI_App the extracted bodies run on
struct CloudApp
{
    NetworkAgent                                      *m_agent{nullptr};
    PresetBundle                                      *preset_bundle{nullptr};
    AppConfig                                         *app_config{nullptr};
    CloudMainFrame                                     m_mainframe;
    CloudMainFrame                                    *mainframe{&m_mainframe};
    CloudPlater                                        m_plater;
    CloudPlater                                       *plater_{&m_plater};
    CloudPlater                                       *plater() { return plater_; }
    std::vector<CloudTab *>                            tabs_list;
    ImGuiState                                         m_imgui;
    ImGuiState                                        *imgui() { return &m_imgui; }
    void                                              *m_preset_bundle_dlg{nullptr};
    PluginsDialog                                     *m_plugins_dlg{nullptr};
    DeviceManager                                     *m_device_manager{nullptr};

    // the sync thread's state (GUI_App members)
    std::shared_ptr<int>                               m_user_sync_token;
    boost::thread                                      m_sync_update_thread;
    std::atomic<bool>                                  m_user_preset_pull_pending{false};
    std::atomic<bool>                                  m_sync_user_presets_now{false};
    bool                                               m_sync_user_preset_dlg_active{false};
    std::atomic<bool>                                  m_restart_sync_pending{false};
    std::vector<std::string>                           m_pending_conflict_setting_ids;
    std::array<bool, 6>                                m_create_preset_blocked{false, false, false, false, false, false};
    std::chrono::steady_clock::time_point              m_last_401_error_time{};
    bool                                               m_show_http_error_msgdlg{false};
    std::atomic<bool>                                  m_migration_retry_pending{false};
    bool                                               m_legacy_filament_check_due{false};
    std::map<std::string, std::string>                 need_delete_presets;
    std::map<std::string, std::map<std::string, std::set<std::string>>> need_add_vendors;
    std::map<std::string, std::string>                 need_add_filaments;
    struct { std::string version_str, description, url; bool force_upgrade{false}; } privacy_version_info;
    uint64_t                                           m_cloud_handoff_gen{0};
    std::string                                        m_cloud_handoff_state;

    bool m_is_closing{false};
    bool is_closing() const { return m_is_closing; }
    NetworkAgent *getAgent() { return m_agent; }
    void start_http_server(int port, const std::string &provider) { GUI::wxGetApp().start_http_server(port, provider); }
    void handle_script_message(std::string msg, const std::string &provider) { GUI::wxGetApp().handle_script_message(msg, provider); }
    void request_user_login(int online_login, const std::string &provider) { GUI::wxGetApp().request_user_login(online_login, provider); }
    void CallAfter(std::function<void()> fn) { orca::Core::call_after(std::move(fn)); }
    template<class... A> void Bind(A &&...) {}
    void run_script(const wxString &js) { orca::Host::event("home_script", {{"script", js.ToStdString()}}); }
    std::string get_printer_cloud_provider() const { return ORCA_CLOUD_PROVIDER; }
    void check_legacy_user_filament_presets(bool by_user) { orca::Core::get().call("presets.migrate_legacy", {{"by_user", by_user}}); }
    bool check_and_keep_current_preset_changes(const wxString &, const wxString &, int, bool *transfer = nullptr)
    {
        if (transfer) *transfer = false;
        return true;
    }
    void on_check_privacy_update(wxCommandEvent &) {}

    // compiled from upstream (GUI_App.cpp)
    void on_http_error(wxCommandEvent &evt);
    void enable_user_preset_folder(bool enable);
    bool check_privacy_update();
    bool reload_settings();
    void remove_user_presets();
    bool maybe_migrate_user_presets_on_login();
    bool check_preset_parent_available(const std::pair<std::string, std::map<std::string, std::string>> &preset_data);
    void add_pending_vendor_preset(const std::pair<std::string, std::map<std::string, std::string>> &preset_data);
    void load_pending_vendors();
    void sync_preset(Preset *preset, bool force = false);
    void update_single_bundle(wxCommandEvent &evt);
    int  sync_bundle(std::string bundle_id, std::string version);
    void check_bundle_updates();
    bool unsubscribe_bundle(const std::string &id);
    void start_sync_user_preset(bool with_progress_dlg = false);
    void stop_sync_user_preset();
    void restart_sync_user_preset();
    void force_push_conflicting_preset(const std::string &setting_id);
    std::map<std::string, std::string> &get_delete_cache_presets();
    std::map<std::string, std::string>  get_delete_cache_presets_lock();
    void process_delete_presets();
    void delete_preset_from_cloud(std::string setting_id, std::string preset_file_path);
    void preset_deleted_from_cloud(std::string setting_id);
    std::string extract_setting_id_from_info(const std::string &info_file_path);
    void scan_orphaned_info_files();
    void request_user_logout(const std::string &provider = ORCA_CLOUD_PROVIDER, bool user_initiated = false);
    void get_login_info(const std::string &provider = ORCA_CLOUD_PROVIDER);
    void post_logout_to_webview(const std::string &provider);
};

CloudApp &app()
{
    static CloudApp a;
    return a;
}
// the extracted bodies reach the app through wxGetApp() too
inline CloudApp &wxGetApp() { return app(); }

// the OrcaCloud agent behind the shared NetworkAgent (GUI_App.cpp)
static std::shared_ptr<OrcaCloudServiceAgent> get_orca_cloud_agent(NetworkAgent *agent)
{
    return agent ? std::dynamic_pointer_cast<OrcaCloudServiceAgent>(agent->get_cloud_agent()) : nullptr;
}

// GUI_App::handle_http_error: the error is handled on the UI thread
void handle_http_error(unsigned int status, std::string body, const std::string &provider)
{
    json evt_data;
    evt_data["body"]     = body;
    evt_data["provider"] = provider;
    const std::string s  = evt_data.dump();
    orca::Core::call_after([status, s] {
        wxCommandEvent evt(EVT_HTTP_ERROR);
        evt.SetInt(int(status));
        evt.SetString(wxString(s));
        app().on_http_error(evt);
    });
}

// the sync conflict notification's two actions, run when the app answers ("cloud.conflict")
std::map<int, std::pair<std::function<bool(void *)>, std::function<bool(void *)>>> &conflict_actions()
{
    static std::map<int, std::pair<std::function<bool(void *)>, std::function<bool(void *)>>> m;
    return m;
}

template<class Pull, class Push>
void NotificationManager::push_orca_sync_conflict_notification(const std::string &text, int code, Pull pull, Push push)
{
    static int next_id = 0;
    const int id = ++next_id;
    conflict_actions()[id] = {[pull](void *h) { return pull(static_cast<wxEvtHandler *>(h)); }, [push](void *h) { return push(static_cast<wxEvtHandler *>(h)); }};
    orca::Host::event("cloud_conflict", {{"id", id}, {"text", text}, {"code", code}});
}

#include "slic3r/GUI/Cloud.inc"

// ---- the login dialog (ZUserLogin) on a stand-in: its page commands, the scripts it answers with, closed or not
#include "slic3r/GUI/Cloud.inc.fn"

struct wxWebViewEvent
{
    wxString m_str;
    wxString GetString() const { return m_str; }
};

struct ZUserLogin
{
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::string                         m_AutotestToken;
    int                                 m_loopback_port{0};
    std::vector<std::string>            m_scripts;
    bool                                m_closed{false};

    void RunScript(const wxString &js) { m_scripts.push_back(js.ToStdString()); }
    void EndModal(int) { m_closed = true; }
    void Close() { m_closed = true; }
    // the dialog's own CallAfter: after the current page command (its answer goes out with this message's scripts)
    void CallAfter(std::function<void()> fn) { fn(); }
    int  ensure_loopback_port();
    void OnScriptMessage(wxWebViewEvent &evt);
};

#include "slic3r/GUI/Cloud.inc.login"

} // namespace cloud

using cloud::app;

// ---- the GUI_App members upstream code outside this file reaches (HttpServer, the shim)
static std::unique_ptr<NetworkAgent> g_agent;
static HttpServer                    g_http_server;
static std::unique_ptr<cloud::ZUserLogin> g_login;

NetworkAgent *GUI_App::getAgent() { return g_agent.get(); }
bool          GUI_App::is_user_login(const std::string &provider) { return g_agent && g_agent->is_user_login(provider); }
void          GUI_App::CallAfter(std::function<void()> fn) { orca::Core::call_after(std::move(fn)); }
void          GUI_App::ShowUserLogin(bool show, const std::string &)
{
    // ZUserLogin::EndModal from the loopback server: the app closes the login page
    if (!show) orca::Host::event("cloud_login", {{"closed", true}});
    else orca::Host::event("cloud_login", {{"open", true}});
}

// GUI_App::start_http_server
static void start_http_server(int port, const std::string &provider)
{
    auto handler = [provider](const std::string &url) { return HttpServer::auth_handle_request(url, provider); };
    g_http_server.set_request_handler(handler);
    if (port <= 0) {
        if (!g_http_server.is_started()) g_http_server.start();
        return;
    }
    if (g_http_server.is_started()) {
        if (g_http_server.get_port() == static_cast<boost::asio::ip::port_type>(port)) return;
        g_http_server.stop();
    }
    g_http_server.set_port(static_cast<boost::asio::ip::port_type>(port));
    g_http_server.start();
}

void GUI_App::start_http_server(int port, const std::string &provider) { GUI::start_http_server(port, provider); }

// GUI_App::handle_script_message: the login page's "user_login" (its session payload)
void GUI_App::handle_script_message(std::string msg, const std::string &provider)
{
    try {
        json j = json::parse(msg);
        if (j.contains("command") && j["command"] == "user_login" && g_agent) {
            g_agent->change_user(j.dump(), provider);
            if (g_agent->is_user_login(provider)) request_user_login(1, provider);
        }
    } catch (...) {
    }
}

}} // namespace Slic3r::GUI

namespace Slic3r { namespace GUI {

// GUI_App::on_user_login_handle for OrcaCloud: migrate, switch to the user's preset folder, load the user presets, ask
// whether to sync (MainFrame::on_select_default_preset), then the sync thread
static void on_user_login_handle(int online_login, const std::string &provider)
{
    cloud::CloudApp &a = app();
    if (!g_agent || a.app_config->get_stealth_mode()) return;
    a.m_last_401_error_time = std::chrono::steady_clock::now();
    g_agent->connect_server();
    if (online_login && provider == ORCA_CLOUD_PROVIDER) {
        cloud::ProgressDialog *dlg = new cloud::ProgressDialog(_L("Loading"), _L("Syncing your account…"), 100, nullptr, 0);
        dlg->Update(10, _L("Migrating presets…"));
        a.maybe_migrate_user_presets_on_login();
        a.remove_user_presets();
        a.enable_user_preset_folder(true);
        dlg->Update(70, _L("Loading user presets…"));
        a.preset_bundle->load_user_presets(g_agent->get_user_id(provider), ForwardCompatibilitySubstitutionRule::Enable);
        orca::Core::get().invalidate_full_config();
        a.mainframe->update_side_preset_ui();
        dlg->Update(100);
        dlg->Destroy();
        // MainFrame::show_sync_dialog -> on_select_default_preset
        MessageDialog dialog(nullptr,
                             _L("Do you want to synchronize your personal data from Orca Cloud?\n"
                                "It contains the following information:\n"
                                "1. The Process presets\n"
                                "2. The Filament presets\n"
                                "3. The Printer presets"),
                             _L("Synchronization"), wxCENTER | wxYES_DEFAULT | wxYES_NO | wxICON_INFORMATION);
        switch (dialog.ShowModal()) {
        case wxID_YES:
            a.app_config->set_bool("sync_user_preset", true);
            a.start_sync_user_preset(true);
            break;
        case wxID_NO:
            a.app_config->set_bool("sync_user_preset", false);
            a.stop_sync_user_preset();
            break;
        default: break;
        }
        a.app_config->save();
        a.mainframe->update_side_preset_ui();
    }
    if (a.app_config->get("sync_user_preset") == "true") a.start_sync_user_preset();
    // re-seed the open OrcaCloud pane with the new session (a new Start URL)
    ++a.m_cloud_handoff_gen;
    a.m_cloud_handoff_state.clear();
    if (cloud::g_cloud_pane_open)
        if (auto orca = cloud::get_orca_cloud_agent(g_agent.get()))
            orca::Host::event("cloud_pane", {{"seed", orca->build_web_session_start_url("")}});
    // the home page shows the account
    a.get_login_info(provider);
    orca::Host::event("cloud", orca::Core::get().call("cloud.state", json::object()));
}

// GUI_App::check_privacy_version: the privacy policy of the account's region, confirmed once per version
static void check_privacy_version(int online_login, const std::string &provider)
{
    cloud::CloudApp &a = app();
    if (a.app_config->get_stealth_mode()) {
        orca::Core::call_after([online_login, provider] { on_user_login_handle(online_login, provider); });
        return;
    }
    // the OrcaCloud account has no Bambu privacy policy to confirm: on to the login
    orca::Core::call_after([online_login, provider] { on_user_login_handle(online_login, provider); });
}

void GUI_App::request_user_login(int online_login, const std::string &provider)
{
    // GUI_App::on_user_login: check_privacy_version, check_track_enable (telemetry is off on Orca)
    orca::Core::call_after([online_login, provider] { check_privacy_version(online_login, provider); });
}

}} // namespace Slic3r::GUI

namespace orca {

using namespace Slic3r;
using namespace Slic3r::GUI;

// GUI_App::on_init_network for the OrcaCloud agent, then the startup login state (GUI_App::on_init_inner)
void cloud_init()
{
    Core &core = Core::get();
    cloud::CloudApp &a = app();
    a.preset_bundle = core.preset_bundle.get();
    a.app_config    = core.app_config.get();
    const std::string data_directory = data_dir();
    auto orca_agent = std::make_shared<OrcaCloudServiceAgent>(data_directory);
    g_agent = std::make_unique<NetworkAgent>(orca_agent, nullptr);
    orca_agent->configure_urls(a.app_config);
    a.m_agent = g_agent.get();
    g_agent->set_config_dir(data_directory);
    g_agent->init_log();
    g_agent->set_cert_file(resources_dir() + "/cert", "slicer_base64.cer");
    g_agent->set_on_server_connected_fn([](CloudEvent event, int return_code, int) {
        if (return_code == 5)
            Core::call_after([provider = event.provider] {
                app().request_user_logout(provider, /*user_initiated=*/false);
                MessageDialog msg_dlg(nullptr, _L("Login information expired. Please login again."), "", wxAPPLY | wxOK);
                msg_dlg.ShowModal();
            });
    });
    g_agent->set_on_http_error_fn([](CloudEvent event, unsigned int status, std::string body) {
        cloud::handle_http_error(status, body, event.provider);
    });
    g_agent->set_country_code(a.app_config->get_country_code());
    g_agent->start();
    if (g_agent->is_user_login()) a.enable_user_preset_folder(true);
    else a.enable_user_preset_folder(false);
    // GUI_App::on_init_inner: the sync starts after startup
    if (a.app_config->get("sync_user_preset") == "true") a.start_sync_user_preset();
}

static json cloud_state()
{
    json j = {{"logged_in", false}, {"stealth", Core::get().app_config->get_stealth_mode()}};
    if (g_agent && g_agent->is_user_login()) {
        j["logged_in"] = true;
        j["user_id"]   = g_agent->get_user_id();
        j["name"]      = g_agent->get_user_name();
        j["avatar"]    = g_agent->get_user_avatar();
        j["sync"]      = Core::get().app_config->get("sync_user_preset") == "true";
    }
    return j;
}

// the login page (ZUserLogin): its URL, then each window.wx.postMessage
json cloud_login_open()
{
    if (!g_agent) throw CommandError("the cloud agent is not available");
    g_login = std::make_unique<cloud::ZUserLogin>();
    g_login->m_cloud_agent = g_agent->get_cloud_agent(ORCA_CLOUD_PROVIDER);
    if (!g_login->m_cloud_agent) throw CommandError(_u8L("Cloud agent is not available. Please restart OrcaSlicer and try again."));
    wxString strlang = wxString(preferred_language(""));
    if (strlang.empty()) strlang = "en";
    boost::replace_all(strlang, "_", "-");
    const std::string url = g_login->m_cloud_agent->get_cloud_login_url(strlang.ToStdString());
    BOOST_LOG_TRIVIAL(info) << "login url = " << url;
    return {{"name", "login"}, {"url", url}, {"title", _u8L("Login")}, {"script", ""}, {"remote", true}};
}

json cloud_login_message(const json &a)
{
    if (!g_login) throw CommandError("the dialog is not open");
    g_login->m_scripts.clear();
    cloud::wxWebViewEvent evt{wxString(a.value("message", std::string("{}")))};
    g_login->OnScriptMessage(evt);
    json r = {{"scripts", g_login->m_scripts}, {"closed", g_login->m_closed}, {"save", nullptr}};
    if (g_login->m_closed) g_login.reset();
    return r;
}

// the Preset Bundle dialog's cloud actions: update one subscribed bundle (EVT_UPDATE_PRESET_BUNDLE), unsubscribe
void cloud_update_bundle(const std::string &id)
{
    cloud::wxCommandEvent evt(cloud::EVT_UPDATE_PRESET_BUNDLE);
    evt.SetString(wxString(id));
    app().update_single_bundle(evt);
}
bool cloud_unsubscribe_bundle(const std::string &id) { return g_agent && app().unsubscribe_bundle(id); }

// GUI_App::open_orca_cloud: the pane's Start URL (cloud_webview_handoff), or the plain cloud page without an agent
static json cloud_open_pane(const json &a)
{
    cloud::g_cloud_pane_open = true;
    const std::string next = a.value("next", std::string());
    auto orca = cloud::get_orca_cloud_agent(g_agent.get());
    std::string start = orca ? orca->build_web_session_start_url(next) : std::string();
    std::string page;
    if (orca && !next.empty()) {
        std::string base = orca->get_cloud_base_url();
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (!base.empty()) page = base + next;
    }
    if (start.empty()) start = "https://cloud.orcaslicer.com";   // kCloudUrl
    return {{"url", start}, {"page", page}};
}

// WebViewPanel::on_cloud_script_message -> GUI_App::request_cloud_web_session: only the newest request counts; signed
// out, the login page opens; signed in, the handoff token is minted off the engine thread and the pane loads the
// Complete URL (on_cloud_web_session_minted)
static json cloud_pane_message(const json &a)
{
    const json msg = json::parse(a.value("message", std::string()), nullptr, false);
    if (msg.is_discarded() || !msg.is_object() || msg.value("command", std::string()) != "request_web_session") return nullptr;
    std::string state;
    if (msg.contains("data") && msg["data"].is_object()) state = msg["data"].value("state", std::string());
    if (state.empty()) return nullptr;
    cloud::CloudApp &ca = app();
    ca.m_cloud_handoff_state = state;
    const uint64_t gen = ++ca.m_cloud_handoff_gen;
    auto orca = cloud::get_orca_cloud_agent(g_agent.get());
    if (!orca || !orca->is_user_login()) {
        Host::event("cloud_login", {{"open", true}});
        return nullptr;
    }
    boost::thread([orca, state, gen]() {
        std::string  token_hash, error_code;
        unsigned int http_code = 0;
        orca->request_web_session_handoff(token_hash, error_code, http_code);
        if (token_hash.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "cloud web session handoff not minted - http_code=" << http_code << " error_code=" << error_code;
            return;
        }
        Core::call_after([orca, state, gen, token_hash] {
            cloud::CloudApp &ca = app();
            if (gen != ca.m_cloud_handoff_gen || state != ca.m_cloud_handoff_state) return;
            Host::event("cloud_pane", {{"load", orca->build_web_session_complete_url(token_hash, state)}});
        });
    }).detach();
    return nullptr;
}

// GUI_App::delete_preset_from_cloud: queued for the sync thread
void cloud_delete_preset(const std::string &setting_id, const std::string &file) { app().delete_preset_from_cloud(setting_id, file); }

// GUI_App::get_login_info: what the home page's account row shows
std::vector<std::string> cloud_login_info_scripts()
{
    std::vector<std::string> out;
    if (!g_agent) return out;
    const std::string cmd = g_agent->is_user_login() ? g_agent->build_login_cmd(ORCA_CLOUD_PROVIDER) : g_agent->build_logout_cmd(ORCA_CLOUD_PROVIDER);
    if (!cmd.empty()) out.push_back("window.postMessage(" + cmd + ")");
    return out;
}

static Commands reg({
    {"cloud.state", [](const json &) -> json { return cloud_state(); }},
    {"cloud.open_pane", cloud_open_pane},
    {"cloud.close_pane", [](const json &) -> json { cloud::g_cloud_pane_open = false; return nullptr; }},
    {"cloud.pane_message", cloud_pane_message},
    // MainFrame "Sync Presets": pull and apply the latest presets from OrcaCloud
    {"cloud.sync", [](const json &) -> json {
         if (!g_agent || !g_agent->is_user_login()) {
             MessageDialog info_dlg(nullptr, _L("You must be logged in to sync presets from cloud."), _L("Sync Presets"), wxOK | wxICON_INFORMATION);
             info_dlg.ShowModal();
             return cloud_state();
         }
         Host::event("toast", {{"text", _u8L("Syncing presets from cloud…")}});
         app().restart_sync_user_preset();
         return cloud_state();
     }},
    {"cloud.logout", [](const json &) -> json {
         app().request_user_logout(ORCA_CLOUD_PROVIDER, /*user_initiated=*/true);
         Core::get().invalidate_full_config();
         return cloud_state();
     }},
    // the sync conflict notification's buttons: 0 pull, 1 force push
    {"cloud.conflict", [](const json &a) -> json {
         auto &m = cloud::conflict_actions();
         auto it = m.find(a.at("id").get<int>());
         if (it == m.end()) return false;
         const bool done = a.value("action", 0) == 0 ? it->second.first(nullptr) : it->second.second(nullptr);
         if (done) m.erase(it);
         return done;
     }},
    // a preset deleted while logged in leaves the cloud too (Tab::delete_preset -> delete_preset_from_cloud)
    {"cloud.delete_preset", [](const json &a) -> json {
         if (g_agent && g_agent->is_user_login()) app().delete_preset_from_cloud(str_arg(a, "setting_id"), str_arg(a, "file"));
         return nullptr;
     }},
});

} // namespace orca
