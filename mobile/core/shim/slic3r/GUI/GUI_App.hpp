// Stand-in for slic3r/GUI/GUI_App.hpp: the app state upstream GUI logic reaches through wxGetApp(), backed by the core.
#pragma once
#include "libslic3r/AppConfig.hpp"
#include "wx/app.h"
#include "wx/string.h"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <functional>
#include <string>

#include <vector>
#include <boost/optional.hpp>
#include "libslic3r/Semver.hpp"

class wxWindow;

namespace Slic3r {
class NetworkAgent;
class DeviceManager;
namespace GUI {

class MainFrame;
class Tab;
class ImGuiWrapper;
class ShortcutRegistry;

class GUI_App : public wxApp
{
public:
    PresetBundle *preset_bundle{nullptr};
    AppConfig    *app_config{nullptr};
    Plater       *plater_{nullptr};
    Model        *model_{nullptr};

    MainFrame    *mainframe{nullptr};

    Plater *plater() { return plater_; }
    // the UI language ("en", "de", ...)
    wxString current_language_code() const;
    wxString current_language_code_safe() const { return current_language_code(); }
    Model  &model() { return *model_; }
    void   *GetTopWindow() { return nullptr; }   // a dialog's parent

    // the OrcaCloud account (Cloud.cpp): the network agent, a finished login, the login dialog
    NetworkAgent *getAgent();
    void          request_user_login(int online_login, const std::string &provider = "orca");
    void          ShowUserLogin(bool show, const std::string &provider = "orca");
    bool          is_user_login(const std::string &provider = "orca");
    void          start_http_server(int port, const std::string &provider);
    void          handle_script_message(std::string msg, const std::string &provider = "orca");
    // runs on the engine thread soon after (Core::call_after)
    void          CallAfter(std::function<void()> fn);
    // the keyboard shortcuts (KeyShortcuts.cpp): the bindings, saved and announced after a change
    ShortcutRegistry &shortcuts();
    void              on_shortcuts_changed();
    bool              is_editor() const { return true; }

    // the plugin system (Plugins.cpp)
    std::vector<Tab *>               tabs_list;
    bool                             is_closing() const;
    ImGuiWrapper                    *imgui();
    void                             UpdateDarkUI(wxWindow *, bool = false, bool = false) {}
    DeviceManager                   *getDeviceManager();
    const boost::optional<Semver>   &last_config_version() const;
    void                             open_terminal_dialog();
    void                             refresh_plugins();
    void                             install_local_plugin();
};

GUI_App &wxGetApp();
inline AppConfig *get_app_config() { return wxGetApp().app_config; }

} // namespace GUI
} // namespace Slic3r
