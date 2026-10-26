// The desktop GUI the Python plugin system (slic3r/plugin, compiled from upstream) reaches, as the app's: a plugin's
// HTML windows and docked panels are web views of the app ("plugin_window" / "plugin_dock" events, their page messages
// back through plugins.window_message / plugins.dock_message), its progress dialogs and notifications likewise; the
// "UI thread" is the engine thread (wxIsMainThread, GUI_App::CallAfter = Core::call_after), and a modal window runs
// the queued calls while it is open. GUI_App / Plater / MainFrame give the plugin host the core's project and presets.
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Thread.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DockPanel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PluginProgressDialog.hpp"
#include "slic3r/GUI/WebDialog.hpp"
#include "slic3r/GUI/Widgets/WebHosting.hpp"
#include "slic3r/GUI/Widgets/WebViewHostDialog.hpp"
#include "wx/app.h"
#include "wx/msgdlg.h"
#include "wx/thread.h"
#include "wx/timer.h"
#include "wx/evtloop.h"
#include "wx/filedlg.h"

#include <GLES3/gl3.h>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace orca {
void write_3mf(const std::string &path, Slic3r::SaveStrategy strategy, int export_plate_idx);   // ProjectIO.cpp
}

// ---- the UI thread and its timers
bool wxIsMainThread() { return Slic3r::is_main_thread_active(); }

wxApp *wxApp::GetInstance() { return &Slic3r::GUI::wxGetApp(); }

struct wxTimer::State
{
    std::atomic<bool> running{false};
    std::atomic<int>  generation{0};
};

wxTimer::~wxTimer() { Stop(); }

// a thread that queues Notify() on the engine thread every interval, while this generation of the timer runs
bool wxTimer::Start(int milliseconds, bool one_shot)
{
    if (!m_state) m_state = std::make_shared<State>();
    const int gen = ++m_state->generation;
    m_state->running = true;
    const int interval = milliseconds > 0 ? milliseconds : 1;
    std::weak_ptr<State> weak = m_state;
    wxTimer *self = this;
    std::thread([weak, gen, interval, one_shot, self] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            auto st = weak.lock();
            if (!st || !st->running || st->generation != gen) return;
            orca::Core::call_after([weak, gen, self] {
                auto s = weak.lock();
                if (s && s->running && s->generation == gen) self->fire();
            });
            if (one_shot) { st->running = false; return; }
        }
    }).detach();
    return true;
}

void wxTimer::Stop()
{
    if (m_state) { m_state->running = false; ++m_state->generation; }
}

bool wxTimer::IsRunning() const { return m_state && m_state->running; }

namespace Slic3r { namespace GUI {

// ---- GUI_App / MainFrame / Plater: the core's project and presets
bool GUI_App::is_closing() const { return false; }

ImGuiWrapper *GUI_App::imgui()
{
    static ImGuiWrapper imgui;
    return &imgui;
}

DeviceManager *GUI_App::getDeviceManager()
{
    static DeviceManager devices;
    return &devices;
}

const boost::optional<Semver> &GUI_App::last_config_version() const
{
    static boost::optional<Semver> version;
    return version;
}

void MainFrame::update_side_preset_ui() { orca::Host::event("presets", orca::Core::get().call("presets.state", nlohmann::json::object())); }

void Sidebar::update_all_preset_comboboxes() { orca::Host::event("presets", orca::Core::get().call("presets.state", nlohmann::json::object())); }

Model &Plater::model() { return orca::Plater::get().model(); }

bool Plater::is_project_dirty() const { return orca::Plater::get().dirty; }

bool Plater::is_presets_dirty() const
{
    const PresetBundle &b = *orca::Core::get().preset_bundle;
    return b.prints.current_is_dirty() || b.filaments.current_is_dirty() || b.printers.current_is_dirty();
}

bool Plater::inside_snapshot_capture() { return false; }

wxString Plater::get_project_filename(const wxString &) const { return orca::Plater::get().project_path; }

// Plater::export_3mf_copy: the project as it is, its file name and saved state untouched
bool Plater::export_3mf_copy(const boost::filesystem::path &path)
{
    try {
        orca::write_3mf(path.string(), SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::Silence, -1);
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "export_3mf_copy: " << e.what();
        return false;
    }
}

NotificationManager *Plater::get_notification_manager()
{
    static NotificationManager notifications;
    return &notifications;
}

Sidebar &Plater::sidebar()
{
    static Sidebar sidebar;
    return sidebar;
}

// Plater::revalidate_current_plate_if_plugins_missing: the plate's validation runs again with the next slice
void Plater::revalidate_current_plate_if_plugins_missing() { orca::Host::event("project", orca::Plater::get().state()); }

// ---- notifications: the app shows them; a hyperlink click runs the callback (plugins.notification_click)
namespace {
std::mutex                                               g_notify_mutex;
std::map<int, std::function<bool(wxEvtHandler *)>>       g_notify_callbacks;
int                                                      g_notify_next{1};

const char *level_name(NotificationManager::NotificationLevel l)
{
    switch (l) {
    case NotificationManager::NotificationLevel::ErrorNotificationLevel: return "error";
    case NotificationManager::NotificationLevel::SeriousWarningNotificationLevel:
    case NotificationManager::NotificationLevel::WarningNotificationLevel: return "warning";
    case NotificationManager::NotificationLevel::ImportantNotificationLevel: return "important";
    default: return "regular";
    }
}
} // namespace

void NotificationManager::push_notification(NotificationType, NotificationLevel level, const std::string &text, const std::string &hypertext,
                                            std::function<bool(wxEvtHandler *)> callback, int)
{
    int id;
    {
        std::lock_guard<std::mutex> l(g_notify_mutex);
        id = g_notify_next++;
        if (callback) g_notify_callbacks[id] = std::move(callback);
    }
    orca::Host::event("plugin_notification", {{"id", id}, {"level", level_name(level)}, {"text", text}, {"hypertext", hypertext}});
}

// ---- OpenGL info of the 3D view (Viewport::gl_created)
static OpenGLManager::GLInfo g_gl_info;

const OpenGLManager::GLInfo &OpenGLManager::get_gl_info() { return g_gl_info; }

// ---- plugin HTML windows (WebDialog)
namespace {
std::mutex                    g_ui_mutex;
std::map<int, WebDialog *>    g_windows;
std::map<int, DockPanel *>    g_docks;
std::map<int, PluginProgressDialog *> g_progress;
int                           g_ui_next{1};

int next_ui_id()
{
    std::lock_guard<std::mutex> l(g_ui_mutex);
    return g_ui_next++;
}

// what a plugin page runs at document start (WebDialog::add_user_scripts, after the host theme of create_webview):
// the theme variables, the element defaults and the window.orca bridge (WebHosting.cpp)
std::string bridge_script()
{
    return WebViewHostDialog::theme_user_script() + "\n" + WebViewHostDialog::element_defaults_user_script() + "\n" + web_hosting::orca_bridge_script();
}
} // namespace

WebDialog::WebDialog(wxWindow *, const wxString &title, const std::string &html, const wxSize &size, MessageHandler on_message,
                     SubmitHandler on_submit, CloseHandler on_close, CloseHandler on_destroyed, long)
    : m_id(next_ui_id())
    , m_title(title)
    , m_html(html)
    , m_width(size.GetWidth())
    , m_height(size.GetHeight())
    , m_on_message(std::move(on_message))
    , m_on_submit(std::move(on_submit))
    , m_on_close(std::move(on_close))
    , m_on_destroyed(std::move(on_destroyed))
{
    std::lock_guard<std::mutex> l(g_ui_mutex);
    g_windows[m_id] = this;
}

WebDialog::~WebDialog()
{
    {
        std::lock_guard<std::mutex> l(g_ui_mutex);
        g_windows.erase(m_id);
    }
    if (m_shown) orca::Host::event("plugin_window", {{"id", m_id}, {"open", false}});
    if (m_on_destroyed) m_on_destroyed();
}

bool WebDialog::Show(bool show)
{
    if (!show) return Close();
    m_shown = true;
    orca::Host::event("plugin_window", {{"id", m_id},
                                        {"open", true},
                                        {"title", m_title},
                                        {"html", m_html},
                                        {"base_url", web_hosting::content_base_url().ToStdString()},
                                        {"script", bridge_script()},
                                        {"width", m_width},
                                        {"height", m_height},
                                        {"modal", m_modal}});
    return true;
}

// a modal loop on the engine thread: the page's messages and the plugin's marshaled calls arrive as queued calls
int WebDialog::ShowModal()
{
    m_modal        = true;
    m_modal_result = -1;
    Show();
    while (m_modal_result < 0) orca::Core::pump_call_after(100);
    m_modal = false;
    return m_modal_result;
}

void WebDialog::EndModal(int code)
{
    if (m_modal) m_modal_result = code;
}

bool WebDialog::Close(bool)
{
    on_close_window();
    return true;
}

bool WebDialog::Destroy()
{
    // a modal window is destroyed by the code that showed it, once ShowModal returned
    if (m_modal) {
        if (m_modal_result < 0) m_modal_result = wxID_CANCEL;
        return true;
    }
    orca::Core::call_after([this] { delete this; });
    return true;
}

void WebDialog::post_message(WebDialog *dialog, const nlohmann::json &data)
{
    if (dialog != nullptr && dialog->is_open()) dialog->push_message(data);
}

void WebDialog::request_close(WebDialog *dialog)
{
    if (dialog != nullptr) dialog->Close();
}

void WebDialog::destroy_silently(WebDialog *dialog)
{
    if (dialog == nullptr) return;
    if (dialog->IsModal()) {
        dialog->m_open = false;
        dialog->EndModal(wxID_CANCEL);
    }
    dialog->Destroy();
}

// WebDialog::on_script_message
void WebDialog::on_script_message(const nlohmann::json &payload)
{
    if (payload.value("channel", std::string()) == "orca") {
        const std::string    kind = payload.value("kind", std::string());
        const nlohmann::json data = payload.contains("data") ? payload["data"] : nlohmann::json();
        if (kind == "message") {
            if (m_on_message) m_on_message(data);
        } else if (kind == "submit") {
            finish(true, data);
        } else if (kind == "close") {
            finish(false, nlohmann::json());
        }
        return;
    }
    if (payload.value("command", std::string()) == "close_page") on_close_window();
}

// WebDialog::push_message: window.__orcaDispatch({data})
void WebDialog::push_message(const nlohmann::json &data)
{
    if (!m_open) return;
    nlohmann::json envelope;
    envelope["data"] = data;
    orca::Host::event("plugin_window", {{"id", m_id}, {"script", "__orcaDispatch(" + envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ")"}});
}

void WebDialog::finish(bool submitted, const nlohmann::json &data)
{
    if (!m_open) return;
    m_open = false;
    if (submitted) {
        m_result = data;
        fire_submit(data);
    } else {
        m_result.reset();
        fire_close();
    }
    if (IsModal())
        EndModal(submitted ? wxID_OK : wxID_CANCEL);
    else
        Close();
}

// WebDialog::on_close_window
void WebDialog::on_close_window()
{
    if (!m_open) {
        if (!IsModal()) Destroy();
        return;
    }
    m_open = false;
    m_result.reset();
    fire_close();
    if (IsModal()) {
        EndModal(wxID_CANCEL);
        return;
    }
    Destroy();
}

void WebDialog::fire_submit(const nlohmann::json &data)
{
    if (m_on_submit) {
        SubmitHandler cb = std::move(m_on_submit);
        cb(data);
    }
}

void WebDialog::fire_close()
{
    if (m_close_fired) return;
    m_close_fired = true;
    if (m_on_close) {
        CloseHandler cb = m_on_close;
        m_on_close      = nullptr;
        cb();
    }
}

// ---- docked panels (DockPanel, Plater::add_dock_pane)
std::string plugin_pane_name(const std::string &plugin_key, const std::string &title)
{
    std::string name = "plugin:" + plugin_key + ":" + title;
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '|' || c == ';' || c == '=' || c == '\\'; }, '_');
    return name;
}

DockPanel::DockPanel(wxWindow *, const std::string &html, MessageHandler on_message, CloseHandler on_close, CloseHandler on_destroyed)
    : m_id(next_ui_id()), m_html(html), m_on_message(std::move(on_message)), m_on_close(std::move(on_close)), m_on_destroyed(std::move(on_destroyed))
{
    std::lock_guard<std::mutex> l(g_ui_mutex);
    g_docks[m_id] = this;
}

DockPanel::~DockPanel()
{
    {
        std::lock_guard<std::mutex> l(g_ui_mutex);
        g_docks.erase(m_id);
    }
    if (m_on_destroyed) m_on_destroyed();
}

bool DockPanel::on_page_message(const std::string &kind, const nlohmann::json &data)
{
    if (kind == "message") {
        if (m_on_message) m_on_message(data);
        return true;
    }
    if (kind == "close") {
        request_close();
        return true;
    }
    return false;
}

void DockPanel::push_message(const nlohmann::json &data)
{
    if (m_closing) return;
    nlohmann::json envelope;
    envelope["data"] = data;
    orca::Host::event("plugin_dock", {{"id", m_id}, {"script", "__orcaDispatch(" + envelope.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ")"}});
}

void DockPanel::fire_close()
{
    if (m_closing) return;
    m_closing = true;
    if (m_on_close) {
        CloseHandler on_close = std::move(m_on_close);
        m_on_close            = nullptr;
        on_close();
    }
}

void DockPanel::request_close()
{
    if (m_closing) return;
    fire_close();
    const int id = m_id;
    orca::Core::call_after([id] {
        DockPanel *self = nullptr;
        {
            std::lock_guard<std::mutex> l(g_ui_mutex);
            auto it = g_docks.find(id);
            if (it != g_docks.end()) self = it->second;
        }
        if (self) self->remove_pane();
    });
}

void DockPanel::destroy_silently()
{
    m_closing  = true;
    m_on_close = nullptr;
    remove_pane();
}

void DockPanel::remove_pane()
{
    orca::Host::event("plugin_dock", {{"id", m_id}, {"open", false}});
    delete this;
}

void Plater::add_dock_pane(DockPanel *panel, const std::string &name, const wxString &title, const std::string &dock, const wxSize &size,
                           std::function<void()>)
{
    orca::Host::event("plugin_dock", {{"id", panel->id()},
                                      {"open", true},
                                      {"name", name},
                                      {"title", title.ToStdString()},
                                      {"html", panel->html()},
                                      {"base_url", web_hosting::content_base_url().ToStdString()},
                                      {"script", bridge_script()},
                                      {"dock", dock},
                                      {"width", size.GetWidth()},
                                      {"height", size.GetHeight()}});
}

void Plater::show_dock_pane(DockPanel *panel, bool show) { orca::Host::event("plugin_dock", {{"id", panel->id()}, {"shown", show}}); }

// ---- progress dialogs (PluginProgressDialog on ProgressDialog)
PluginProgressDialog::PluginProgressDialog(wxWindow *, const wxString &title, const wxString &message, int maximum, int style,
                                           CloseHandler on_destroyed)
    : m_id(next_ui_id()), m_title(title), m_message(message), m_maximum(maximum), m_style(style), m_on_destroyed(std::move(on_destroyed))
{
    {
        std::lock_guard<std::mutex> l(g_ui_mutex);
        g_progress[m_id] = this;
    }
    send("open");
}

PluginProgressDialog::~PluginProgressDialog()
{
    {
        std::lock_guard<std::mutex> l(g_ui_mutex);
        g_progress.erase(m_id);
    }
    orca::Host::event("plugin_progress", {{"id", m_id}, {"open", false}});
    if (m_on_destroyed) m_on_destroyed();
}

void PluginProgressDialog::send(const char *)
{
    orca::Host::event("plugin_progress", {{"id", m_id},
                                          {"open", true},
                                          {"title", m_title},
                                          {"message", m_message},
                                          {"value", m_value},
                                          {"maximum", m_maximum},
                                          {"pulsing", m_pulsing},
                                          {"can_abort", (m_style & wxPD_CAN_ABORT) != 0}});
}

PluginProgressDialog *PluginProgressDialog::create_dialog(wxWindow *parent, const wxString &title, const wxString &message, int maximum,
                                                          int style, CloseHandler on_destroyed)
{
    return new PluginProgressDialog(parent, title, message, maximum, style, std::move(on_destroyed));
}

bool PluginProgressDialog::pulse(PluginProgressDialog *dialog, const wxString &message) { return dialog != nullptr ? dialog->pulse(message) : false; }
bool PluginProgressDialog::update(PluginProgressDialog *dialog, int value, const wxString &message)
{
    return dialog != nullptr ? dialog->update(value, message) : false;
}
void PluginProgressDialog::start_pulse(PluginProgressDialog *dialog, int interval_ms, const wxString &message)
{
    if (dialog != nullptr) dialog->start_pulse(interval_ms, message);
}
void PluginProgressDialog::stop_pulse(PluginProgressDialog *dialog)
{
    if (dialog != nullptr) dialog->stop_pulse();
}
void PluginProgressDialog::request_close(PluginProgressDialog *dialog)
{
    if (dialog != nullptr) dialog->close();
}

// wxProgressDialog::Pulse / Update: false once the user canceled
bool PluginProgressDialog::pulse(const wxString &message)
{
    if (!m_open) return false;
    if (!message.empty()) m_message = message;
    m_pulsing = true;
    send("pulse");
    return !m_canceled;
}

bool PluginProgressDialog::update(int value, const wxString &message)
{
    if (!m_open) return false;
    if (!message.empty()) m_message = message;
    m_pulsing = false;
    m_value   = value;
    send("update");
    // wxPD_AUTO_HIDE: the dialog goes away at its maximum
    if (value >= m_maximum && (m_style & wxPD_AUTO_HIDE) != 0) orca::Host::event("plugin_progress", {{"id", m_id}, {"open", false}});
    return !m_canceled;
}

// the app animates an indeterminate bar while pulsing
void PluginProgressDialog::start_pulse(int, const wxString &message) { pulse(message); }

void PluginProgressDialog::stop_pulse()
{
    if (!m_pulsing) return;
    m_pulsing = false;
    if (m_open) send("stop");
}

void PluginProgressDialog::close()
{
    if (!m_open) return;
    m_open = false;
    delete this;
}

}} // namespace Slic3r::GUI

namespace orca {

// Viewport::gl_created: what host.gl_info() reports
void plugins_gl_created()
{
    auto str = [](GLenum e) { const GLubyte *s = glGetString(e); return s ? std::string(reinterpret_cast<const char *>(s)) : std::string(); };
    g_gl_info.m_vendor   = str(GL_VENDOR);
    g_gl_info.m_renderer = str(GL_RENDERER);
    g_gl_info.m_version  = str(GL_VERSION);
    g_gl_info.m_glsl     = str(GL_SHADING_LANGUAGE_VERSION);
}

// the app's side of the plugin windows: page messages and closes are queued for the engine thread (a modal window's
// loop runs them), so the app sends them from outside the engine's command queue
static Commands reg({
    {"plugins.window_message", [](const json &a) -> json {
         const int id = a.at("id");
         json payload = a.at("message").is_string() ? json::parse(a["message"].get<std::string>(), nullptr, false) : a["message"];
         Core::call_after([id, payload] {
             WebDialog *w = nullptr;
             {
                 std::lock_guard<std::mutex> l(g_ui_mutex);
                 auto it = g_windows.find(id);
                 if (it != g_windows.end()) w = it->second;
             }
             if (w) w->on_script_message(payload);
         });
         return nullptr;
     }},
    {"plugins.window_close", [](const json &a) -> json {
         const int id = a.at("id");
         Core::call_after([id] {
             WebDialog *w = nullptr;
             {
                 std::lock_guard<std::mutex> l(g_ui_mutex);
                 auto it = g_windows.find(id);
                 if (it != g_windows.end()) w = it->second;
             }
             if (w) w->on_close_window();
         });
         return nullptr;
     }},
    {"plugins.dock_message", [](const json &a) -> json {
         const int id = a.at("id");
         json payload = a.at("message").is_string() ? json::parse(a["message"].get<std::string>(), nullptr, false) : a["message"];
         Core::call_after([id, payload] {
             DockPanel *d = nullptr;
             {
                 std::lock_guard<std::mutex> l(g_ui_mutex);
                 auto it = g_docks.find(id);
                 if (it != g_docks.end()) d = it->second;
             }
             // WebPanel: the bridge's {channel: "orca", kind, data}
             if (d && payload.value("channel", std::string()) == "orca")
                 d->on_page_message(payload.value("kind", std::string()), payload.contains("data") ? payload["data"] : json());
         });
         return nullptr;
     }},
    {"plugins.dock_close", [](const json &a) -> json {
         const int id = a.at("id");
         Core::call_after([id] {
             DockPanel *d = nullptr;
             {
                 std::lock_guard<std::mutex> l(g_ui_mutex);
                 auto it = g_docks.find(id);
                 if (it != g_docks.end()) d = it->second;
             }
             if (d) d->request_close();
         });
         return nullptr;
     }},
    {"plugins.progress_cancel", [](const json &a) -> json {
         std::lock_guard<std::mutex> l(g_ui_mutex);
         auto it = g_progress.find(a.at("id").get<int>());
         if (it != g_progress.end()) it->second->m_canceled = true;
         return nullptr;
     }},
    {"plugins.notification_click", [](const json &a) -> json {
         std::function<bool(wxEvtHandler *)> cb;
         {
             std::lock_guard<std::mutex> l(g_notify_mutex);
             auto it = g_notify_callbacks.find(a.at("id").get<int>());
             if (it != g_notify_callbacks.end()) cb = it->second;
         }
         // the callback's True closes the notification
         const bool close = cb ? cb(nullptr) : true;
         if (close) {
             std::lock_guard<std::mutex> l(g_notify_mutex);
             g_notify_callbacks.erase(a.at("id").get<int>());
         }
         return close;
     }},
});

} // namespace orca

// ---- wx stand-ins of the Plugins dialog: the app object, nested loops, progress dialogs, the file picker
wxApp *wxTheApp = wxApp::GetInstance();

void wx_call_after(std::function<void()> fn) { orca::Core::call_after(std::move(fn)); }

// wxEventLoop::Run on the engine thread: the queued calls run until one of them ends the loop
int wxEventLoop::Run()
{
    m_running = true;
    while (m_running) orca::Core::pump_call_after(100);
    return 0;
}

static std::atomic<int> g_progress_next{100000};

wxProgressDialog::wxProgressDialog(const wxString &title, const wxString &message, int maximum, wxWindow *, int style)
    : m_id(g_progress_next++), m_title(title), m_message(message), m_maximum(maximum), m_style(style)
{
    orca::Host::event("plugin_progress", {{"id", m_id}, {"open", true}, {"title", m_title}, {"message", m_message}, {"value", 0},
                                          {"maximum", m_maximum}, {"pulsing", true}, {"can_abort", false}});
}

wxProgressDialog::~wxProgressDialog() { orca::Host::event("plugin_progress", {{"id", m_id}, {"open", false}}); }

bool wxProgressDialog::Pulse(const wxString &message, bool *)
{
    if (!message.empty()) m_message = message;
    return true;   // the pulse animates on its own in the app
}

bool wxProgressDialog::Update(int value, const wxString &message, bool *)
{
    if (!message.empty()) m_message = message;
    orca::Host::event("plugin_progress", {{"id", m_id}, {"open", true}, {"title", m_title}, {"message", m_message}, {"value", value},
                                          {"maximum", m_maximum}, {"pulsing", false}, {"can_abort", false}});
    return true;
}

// the system picker; the document is copied to the cache and its path answered
int wxFileDialog::ShowModal()
{
    const nlohmann::json r = orca::Host::ask({{"kind", "open_file"}, {"title", m_message.ToStdString()}, {"wildcard", m_wildcard.ToStdString()}});
    m_path = r.value("path", std::string());
    return m_path.empty() ? wxID_CANCEL : wxID_OK;
}
