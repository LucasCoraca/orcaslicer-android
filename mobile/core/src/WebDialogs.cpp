// The dialogs upstream builds as web pages (WebViewHostDialog: resources/web/dialog/*): the page with the host theme and
// its strings injected at document start, its window.wx.postMessage commands handled by the dialog's on_script_message,
// and the scripts the dialog runs back (call_web_handler) returned to the app. Preset Bundle (PresetBundleDialog) and
// Export Preset Bundle (ExportPresetBundleDialog) are compiled from upstream on stand-ins for their widgets.
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <miniz.h>

#include "Core.hpp"
#include "Host.hpp"
#include "SpeedDial.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/OrcaCloudServiceAgent.hpp"
#include "wx/msgdlg.h"
#include "slic3r/GUI/Widgets/WebViewHostDialog.hpp"
#include "wx/string.h"
#include "wx/utils.h"

using namespace Slic3r;

namespace orca {
void presets_changed();

json cloud_login_open();                    // Cloud.cpp
void cloud_update_bundle(const std::string &id);
bool cloud_unsubscribe_bundle(const std::string &id);
json cloud_login_message(const json &a);

namespace webdlg {
using namespace Slic3r::GUI;

// ---- the host theme (WebViewHostDialog.cpp), with the app's label / window colours of GUI_App::init_label_colours
enum { wxC2S_HTML_SYNTAX = 1 };
struct wxColour
{
    std::string hex;
    explicit wxColour(const char *h = "#000000") : hex(h) {}
    wxColour(int r, int g, int b) { char buf[8]; std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b); hex = buf; }
    wxString GetAsString(int) const { return hex; }
};
bool web_dark_mode() { return Core::get().app_config->get("dark_color_mode") == "1"; }
struct WebFont { wxString GetFaceName() const { return "HarmonyOS Sans SC"; } };
struct WebApp
{
    bool     dark{web_dark_mode()};
    wxColour get_window_default_clr() const { return dark ? wxColour(43, 43, 43) : wxColour("#FFFFFF"); }
    wxColour get_label_clr_default() const { return dark ? wxColour(250, 250, 250) : wxColour("#363636"); }
    wxColour get_label_clr_sys() const { return dark ? wxColour("#B2B3B5") : wxColour("#363636"); }
    wxColour get_highlight_default_clr() const { return dark ? wxColour("#36363B") : wxColour("#F1F1F1"); }
    WebFont  normal_font() const { return {}; }
};
// StateColor's dark variant of the Orca colour
wxColour orca_accent() { return web_dark_mode() ? wxColour("#00675b") : wxColour("#009688"); }

#include "slic3r/GUI/WebThemeFns.inc"

// WebViewHostDialog without the window: what create_webview loads and what call_web_handler runs
struct WebViewHostDialog
{
    virtual ~WebViewHostDialog() = default;
    virtual void           on_script_message(const nlohmann::json &payload) = 0;
    virtual nlohmann::json localized_strings() const { return nullptr; }
    virtual bool           append_language_to_url() const { return true; }

    std::string              m_url, m_title;
    std::vector<std::string> m_scripts;
    bool                     m_closed{false};
    json                     m_save;   // a file the dialog wrote that the app hands to the user (wxFileDialog stand-in)

    static std::string document_start_injector(const std::string &markup, const char *dom_id, const char *position,
                                               const std::string &prelude = {}, const std::string &on_inject = {});
    static std::string theme_user_script();
    static std::string element_defaults_user_script();
    static std::string localized_strings_script(const nlohmann::json &strings);

    template<class S> bool create_webview(const std::string &resource_path, const wxString &title, S, S)
    {
        m_title = title;
        m_url   = build_resource_url(resource_path);
        return true;
    }
    wxString build_resource_url(const std::string &resource_path) const
    {
        wxString target_url = file_url_from_path(boost::filesystem::path(resources_dir()) / resource_path);
        if (append_language_to_url()) {
            const wxString lang = wxGetApp().current_language_code_safe();
            if (!lang.empty()) {
                target_url += "?lang=";
                target_url += lang;
            }
        }
        return target_url;
    }
    void call_web_handler(const nlohmann::json &payload, const wxString &handler = "HandleStudio")
    {
        m_scripts.push_back(std::string(handler) + "(" + payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore) + ")");
    }
    bool handle_common_script_command(const nlohmann::json &payload, int = 0)
    {
        if (payload.value("command", "") == "close_page") {
            m_closed = true;
            return true;
        }
        return false;
    }
    void EndModal(int) { m_closed = true; }
    // register_theme_user_scripts: the document-start scripts of the page
    virtual bool        themed() const { return true; }
    virtual std::string extra_script() const { return {}; }
    std::string start_script() const
    {
        std::string s = themed() ? theme_user_script() : std::string();
        if (const nlohmann::json strings = localized_strings(); !strings.is_null()) s += "\n" + localized_strings_script(strings);
        return s + extra_script();
    }
};
#include "slic3r/GUI/WebThemeMembers.inc"

// ---- the stand-ins the dialogs' bodies use
// wxFileName for OpenFolder: the bundle's folder
struct wxFileName
{
    boost::filesystem::path p;
    explicit wxFileName(const wxString &s) : p(std::string(s)) {}
    bool     FileExists() const { return boost::filesystem::is_regular_file(p); }
    wxString GetPath() const { return p.parent_path().string(); }
    static bool DirExists(const wxString &d) { return boost::filesystem::is_directory(std::string(d)); }
};
// Android has no file browser to show a folder in: the app tells where it is
inline bool wxLaunchDefaultApplication(const wxString &target)
{
    Host::event("toast", {{"text", std::string(target)}});
    return true;
}
struct MainFrameUi { void update_side_preset_ui() { presets_changed(); } };

#include "slic3r/GUI/ExportPresetCase.inc"
struct wxSize { wxSize(int, int) {} };
// GUI_App::CallAfter of "export_page": the app opens the export page itself ("open" in the message answer)
template<class F> void web_call_after(F &&) {}
struct ExportOpener { explicit ExportOpener(void *) {} int ShowModal() { return 0; } };
// EVT_UPDATE_PRESET_BUNDLE: GUI_App::update_single_bundle pulls the OrcaCloud bundle (Cloud.cpp)
struct UpdateBundleEvent { std::string id; void SetString(const wxString &s) { id = s.ToStdString(); } };
inline void queue_update_bundle(UpdateBundleEvent *evt) { cloud_update_bundle(evt->id); delete evt; }

// ---- PresetBundleDialog (PresetBundleDialog.cpp)
struct PresetBundleDialog : WebViewHostDialog
{
    std::unordered_map<std::string, BundleMetadata> bundle_copy;
    std::atomic<bool> m_check_update_pending{false};
    std::atomic<bool> m_update_bundles{false};
    AppConfig        *app_config{nullptr};
    MainFrameUi       mainframe;

    PresetBundleDialog()
    {
        create();
        RefreshBundleMap();
    }
    void           create();
    bool           DeleteBundleById(const wxString &id);
    bool           UnsubscribeBundleById(const std::string &id) { return cloud_unsubscribe_bundle(id); }   // GUI_App::unsubscribe_bundle
    void           ListBundles();
    void           OpenFolder(const std::string &id);
    void           DeleteBundle(const std::string &id);
    void           UnsubscribeBundle(const std::string &id);
    void           OpenBundleOnCloud(const std::string &id);
    bool           CheckUpdateCloud();
    static bool    CompareVer(const std::string &a, const std::string &b);
    void           on_script_message(const nlohmann::json &payload) override;
    nlohmann::json localized_strings() const override;
    void           RefreshBundleMap();
    // OnFSWatch + the worker's m_update_bundles: the user preset folders changed (Android has no file watcher here)
    void fs_changed()
    {
        // the dialog worker's pass: the subscribed bundles' versions on OrcaCloud ("refresh_bundles")
        if (m_check_update_pending.exchange(false)) CheckUpdateCloud();
        wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem);
        mainframe.update_side_preset_ui();
        RefreshBundleMap();
        ListBundles();
    }
};
#include "slic3r/GUI/PresetBundleDialogFns.inc"

// ---- ExportPresetBundleDialog (ExportPresetBundleDialog.cpp)
#include "slic3r/GUI/ExportPresetBundleStatics.inc"
struct NetworkAgent { std::string get_version() { return {}; } std::string get_user_id() { return {}; } };
// wxFileDialog of "export_local": the bundle is written to the cache and handed to the app's save picker
struct wxFileDialog
{
    wxString m_path;
    wxFileDialog(void *, const wxString &, const wxString &, const wxString &file, const wxString &, long)
        : m_path((boost::filesystem::path(temporary_dir()) / std::string(file)).string()) {}
    int      ShowModal() { return wxID_OK; }
    wxString GetPath() const { return m_path; }
};
enum { wxFD_SAVE = 1, wxFD_OVERWRITE_PROMPT = 2 };
struct wxFileName2
{
    boost::filesystem::path p;
    explicit wxFileName2(const wxString &s) : p(std::string(s)) {}
    wxString GetName() const { return p.stem().string(); }
    wxString GetExt() const { std::string e = p.extension().string(); return e.empty() ? e : e.substr(1); }
    void     SetExt(const std::string &e) { p.replace_extension("." + e); }
    wxString GetFullPath() const { return p.string(); }
};

struct ExportPresetBundleDialog : WebViewHostDialog
{
    ExportPresetBundleDialog() { Init(); }
    ~ExportPresetBundleDialog()
    {
        for (auto &p : m_printer_presets) delete p.second;
    }
    void show_export_result(const ExportPresetCase &e);
    void Init();
    void InitExportData();
    void on_script_message(const nlohmann::json &payload) override;
    nlohmann::json localized_strings() const override;
    void OnRequestPresets();
    void OnExportData(const wxString &path, const wxString &name, nlohmann::json data);

    std::unordered_map<std::string, Preset *>                                       m_printer_presets;
    std::unordered_map<std::string, std::vector<const Preset *>>                    m_filament_presets;
    std::unordered_map<std::string, std::vector<const Preset *>>                    m_process_presets;
    std::unordered_map<std::string, std::vector<std::pair<std::string, Preset *>>> m_filament_name_to_presets;
};
#include "slic3r/GUI/ExportPresetBundleFns.inc"

// ---- SpeedDialWebDialog (SpeedDialDialog.cpp): the page's commands on the registry of SpeedDial.cpp
namespace sdui {
using namespace orca::sd;
enum class NotificationType { CustomNotification };
struct NotificationManager
{
    enum class NotificationLevel { RegularNotificationLevel, ErrorNotificationLevel };
    void push_notification(NotificationType, NotificationLevel, const std::string &text) { Host::event("toast", {{"text", text}}); }
};
struct SdPlater
{
    NotificationManager  nm;
    NotificationManager *get_notification_manager() { return &nm; }
};
struct SdUiApp
{
    void            *mainframe{nullptr};
    SdPlater         plater_;
    ActionRegistry  &action_registry() { return orca::sd::registry(); }
    template<class F> void CallAfter(F &&f) { f(); }   // the dialog's page commands run in order already
    bool             is_closing() const { return false; }
    ConfigOptionMode get_mode() const { return orca::sd::get_mode(); }
    void             save_mode(ConfigOptionMode m) { orca::sd::save_mode(m); }
    void             enable_developer_mode() { orca::sd::enable_developer_mode(); }
    SdPlater        *plater() { return &plater_; }
};
SdUiApp &wxGetApp()
{
    static SdUiApp app;
    return app;
}
namespace GUI {
// the shortcut prefixes of a hardware keyboard on Android (Shortcuts.cpp off macOS)
std::string shortkey_alt_prefix() { return "Alt+"; }
std::string shortkey_ctrl_prefix() { return "Ctrl+"; }
} // namespace GUI
#include "slic3r/GUI/SpeedDialDialog.inc.fns"

struct SpeedDialWebDialog : WebViewHostDialog
{
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    bool m_page_ready{false};
    int  m_height{0};   // resize_to_content: the dialog follows its page

    SpeedDialWebDialog() { create_webview("web/dialog/SpeedDial/index.html", wxEmptyString, 540, 282); }
    nlohmann::json localized_strings() const override;
    void           on_script_message(const nlohmann::json &payload) override;
    void           handle_web_command(const nlohmann::json &payload);
    void           search_tabs();
    void           resize_to_content(int height) { m_height = height; }
    void           run_action(const std::string &id, const std::string &title, const std::string &param);
    void           open_wiki(const std::string &id);
    void           send_actions();
    bool           IsModal() const { return false; }
    void           Hide() { m_closed = true; }
};
#include "slic3r/GUI/SpeedDialDialog.inc"
} // namespace sdui
using sdui::SpeedDialWebDialog;

// ---- the dialogs compiled from upstream on the Slic3r::GUI stand-in (Widgets/WebViewHostDialog.hpp shim): Plugins
struct GuiDialogAdapter : WebViewHostDialog
{
    std::unique_ptr<Slic3r::GUI::WebViewHostDialog> dlg;
    explicit GuiDialogAdapter(std::unique_ptr<Slic3r::GUI::WebViewHostDialog> d, const std::string &name) : dlg(std::move(d))
    {
        dlg->set_name(name);
        m_url   = dlg->url();
        m_title = dlg->title();
    }
    void on_script_message(const nlohmann::json &payload) override
    {
        dlg->dispatch_script_message(payload);
        m_closed = dlg->closed();
    }
};
std::unique_ptr<Slic3r::GUI::WebViewHostDialog> make_plugins_dialog();   // Plugins.cpp
std::unique_ptr<Slic3r::GUI::WebViewHostDialog> make_plugins_config_dialog(const std::string &type, const std::string &overrides);
std::string plugins_config_result(Slic3r::GUI::WebViewHostDialog *d);
std::unique_ptr<Slic3r::GUI::WebViewHostDialog> make_terminal_dialog();

// ---- WipingDialog: resources/web/flush/WipingDialog.html on its own wxWebView, messages through window.wipingDialog
} // namespace webdlg
json flushing_init();   // Filaments.cpp
json flushing_update(int extruder_id);
void flushing_store(const json &j);
namespace webdlg {
struct FlushingWebDialog : WebViewHostDialog
{
    FlushingWebDialog()
    {
        m_title = _u8L("Flushing volumes for filament change");
        m_url   = file_url_from_path(boost::filesystem::path(resources_dir()) / "web/flush/WipingDialog.html").ToStdString();
    }
    bool themed() const override { return false; }
    // the page's strings (T() ids), as WipingDialog registers them
    // TRN Corner header of the flushing volume table: rows are the filament flushed from, columns the filament changed to.
    nlohmann::json localized_strings() const override { return {{"from_to", _u8L("from/to")}}; }
    std::string extra_script() const override
    {
        return "\nwindow.wipingDialog={postMessage:function(d){window.wx.postMessage(typeof d==='string'?d:JSON.stringify(d));}};";
    }
    void on_script_message(const nlohmann::json &j) override
    {
        const std::string msg = j.value("msg", "");
        if (msg == "init") {
            const json r = flushing_init();
            m_scripts.push_back("buildTable(" + r["table"].dump() + ")");
            m_scripts.push_back("buildText(" + r["text"].dump() + ")");
        } else if (msg == "updateMatrix") {
            const int extruder_id = j["extruder_id"].get<int>();
            m_scripts.push_back("updateTable(" + flushing_update(extruder_id).dump() + "," + std::to_string(extruder_id) + ")");
        } else if (msg == "storeData") {
            flushing_store(j);
            m_closed = true;
        } else if (msg == "quit")
            m_closed = true;
    }
};

// ---- the open dialogs, by name
std::map<std::string, std::unique_ptr<WebViewHostDialog>> &dialogs()
{
    static std::map<std::string, std::unique_ptr<WebViewHostDialog>> d;
    return d;
}

// an open dialog compiled on the GUI stand-in (GUI_App::m_plugins_dlg), or null
Slic3r::GUI::WebViewHostDialog *open_gui_dialog(const std::string &name)
{
    auto it = dialogs().find(name);
    auto *g = it == dialogs().end() ? nullptr : dynamic_cast<GuiDialogAdapter *>(it->second.get());
    return g ? g->dlg.get() : nullptr;
}

json open(const json &a)
{
    const std::string name = str_arg(a, "name");
    if (name == "login") return cloud_login_open();   // ZUserLogin (Cloud.cpp)
    std::unique_ptr<WebViewHostDialog> d;
    if (name == "preset_bundle") d = std::make_unique<PresetBundleDialog>();
    else if (name == "export_preset_bundle") d = std::make_unique<ExportPresetBundleDialog>();
    else if (name == "speed_dial") d = std::make_unique<SpeedDialWebDialog>();
    else if (name == "flushing") d = std::make_unique<FlushingWebDialog>();
    else if (name == "plugins" || name == "plugins_config" || name == "terminal") {
        // PluginConfigField: the preset type and the overrides of the option
        auto g = std::make_unique<GuiDialogAdapter>(name == "plugins"    ? make_plugins_dialog()
                                                    : name == "terminal" ? make_terminal_dialog()
                                                                         : make_plugins_config_dialog(a.value("type", std::string("print")), a.value("overrides", std::string())),
                                                    name);
        json r = {{"name", name}, {"url", g->m_url}, {"title", g->m_title}, {"script", g->dlg->start_script()}};
        dialogs()[name] = std::move(g);
        return r;
    }
    else throw CommandError("unknown web dialog " + name);
    json r = {{"name", name}, {"url", d->m_url}, {"title", d->m_title}, {"script", d->start_script()}};
    dialogs()[name] = std::move(d);
    return r;
}

// window.wx.postMessage from the page: the scripts to run back, whether the dialog closed, a file to save
json message(const json &a)
{
    const std::string name = str_arg(a, "name");
    if (name == "login") return cloud_login_message(a);
    auto it = dialogs().find(name);
    if (it == dialogs().end()) throw CommandError("the dialog is not open");
    WebViewHostDialog &d = *it->second;
    d.m_scripts.clear();
    d.m_save = nullptr;
    json msg = json::parse(a.value("message", std::string("{}")), nullptr, false);
    if (!msg.is_discarded()) d.on_script_message(msg);
    if (name == "preset_bundle" && msg.is_object()) {
        const std::string cmd = msg.value("command", "");
        if (cmd == "top_row_menu_action" || cmd == "refresh_bundles") static_cast<PresetBundleDialog &>(d).fs_changed();
    }
    json r = {{"scripts", d.m_scripts}, {"closed", d.m_closed}, {"save", d.m_save}};
    if (auto *sd = dynamic_cast<SpeedDialWebDialog *>(&d)) {
        r["height"] = sd->m_height;
        Core::get().app_config->save();   // favourites, run stats (the desktop saves the config when idle)
    }
    // "Export Presets" in the bundle list opens the export dialog over it
    if (name == "preset_bundle" && msg.is_object() && msg.value("command", "") == "export_page") r["open"] = "export_preset_bundle";
    if (auto *g = dynamic_cast<GuiDialogAdapter *>(&d); g && name == "plugins_config") r["overrides"] = plugins_config_result(g->dlg.get());
    if (d.m_closed) dialogs().erase(it);
    return r;
}

// the dialog closed by the app: what PluginConfigField reads from it after ShowModal (overrides_json)
json close(const json &a)
{
    const std::string name = str_arg(a, "name");
    json r = nullptr;
    auto it = dialogs().find(name);
    if (it != dialogs().end())
        if (auto *g = dynamic_cast<GuiDialogAdapter *>(it->second.get()); g && name == "plugins_config")
            r = {{"overrides", plugins_config_result(g->dlg.get())}};
    dialogs().erase(name);
    return r;
}

} // namespace webdlg

} // namespace orca

// ---- Slic3r::GUI::WebViewHostDialog (the shim): the theme scripts and the page of the webdlg host, the scripts the
// dialog runs sent to the app as they come ("webdialog_run": its work finishes after the message that started it)
namespace Slic3r { namespace GUI {
WebViewHostDialog::WebViewHostDialog(wxWindow *, wxWindowID, const wxString &title, const wxPoint &, const wxSize &, long) : m_title(title) {}
WebViewHostDialog::~WebViewHostDialog() = default;
bool WebViewHostDialog::create_webview(const std::string &resource_path, const wxString &title, const wxSize &, const wxSize &)
{
    m_title = title;
    m_url   = build_resource_url(resource_path);
    return true;
}
wxString WebViewHostDialog::build_resource_url(const std::string &resource_path) const
{
    wxString target_url = file_url_from_path(boost::filesystem::path(resources_dir()) / resource_path);
    if (append_language_to_url()) {
        const wxString lang = wxGetApp().current_language_code_safe();
        if (!lang.empty()) target_url += "?lang=" + lang;
    }
    return target_url;
}
void WebViewHostDialog::load_url(const wxString &url) { orca::Host::event("webdialog_run", {{"name", m_name}, {"url", url.ToStdString()}}); }
bool WebViewHostDialog::run_script(const wxString &script)
{
    orca::Host::event("webdialog_run", {{"name", m_name}, {"script", script.ToStdString()}});
    return true;
}
void WebViewHostDialog::call_web_handler(const nlohmann::json &payload, const wxString &handler)
{
    run_script(std::string(handler) + "(" + payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore) + ")");
}
std::string WebViewHostDialog::document_start_injector(const std::string &markup, const char *dom_id, const char *position,
                                                       const std::string &prelude, const std::string &on_inject)
{
    return orca::webdlg::WebViewHostDialog::document_start_injector(markup, dom_id, position, prelude, on_inject);
}
std::string WebViewHostDialog::theme_user_script() { return orca::webdlg::WebViewHostDialog::theme_user_script(); }
std::string WebViewHostDialog::element_defaults_user_script() { return orca::webdlg::WebViewHostDialog::element_defaults_user_script(); }
std::string WebViewHostDialog::localized_strings_script(const nlohmann::json &strings)
{
    return orca::webdlg::WebViewHostDialog::localized_strings_script(strings);
}
std::string WebViewHostDialog::start_script() const
{
    std::string s = theme_user_script();
    if (const nlohmann::json strings = localized_strings(); !strings.is_null()) s += "\n" + localized_strings_script(strings);
    return s;
}
bool WebViewHostDialog::Show(bool show)
{
    m_shown = show;
    return true;
}
bool WebViewHostDialog::Close(bool)
{
    m_closed = true;
    orca::Host::event("webdialog_run", {{"name", m_name}, {"close", true}});
    return true;
}
bool WebViewHostDialog::Destroy() { return Close(); }
bool WebViewHostDialog::handle_common_script_command(const nlohmann::json &payload, int)
{
    if (payload.value("command", "") == "close_page") {
        m_closed = true;
        return true;
    }
    return false;
}
}} // namespace Slic3r::GUI

namespace orca {

static Commands reg({
    {"webdialog.open", webdlg::open},
    {"webdialog.message", webdlg::message},
    {"webdialog.close", webdlg::close},
});

} // namespace orca
