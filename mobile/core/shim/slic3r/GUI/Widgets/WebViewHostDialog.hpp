// Stand-in for slic3r/GUI/Widgets/WebViewHostDialog.hpp: a dialog whose content is a page of resources/web, shown by
// the app as a web dialog under a name ("webdialog.open"); the page's window.wx.postMessage comes back through
// "webdialog.message", the scripts the dialog runs on it go out as "webdialog_run" events (WebHostDialog.cpp)
#pragma once
#include <exception>
#include <string>
#include <nlohmann/json.hpp>

#include "wx/chartype.h"
#include "wx/event.h"
#include "wx/gdicmn.h"
#include "wx/msgdlg.h"
#include "wx/string.h"
#include "wx/toplevel.h"

namespace Slic3r { namespace GUI {

class WebViewHostDialog : public wxWindow
{
public:
    WebViewHostDialog(wxWindow *parent, wxWindowID id = wxID_ANY, const wxString &title = wxT(""), const wxPoint &pos = wxDefaultPosition,
                      const wxSize &size = wxDefaultSize, long style = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER);
    ~WebViewHostDialog() override;

    bool create_webview(const std::string &resource_path, const wxString &title, const wxSize &dialog_size = wxSize(820, 660),
                        const wxSize &min_size = wxSize(640, 640));
    void load_url(const wxString &url);
    bool run_script(const wxString &script);
    void call_web_handler(const nlohmann::json &payload, const wxString &handler = wxT("HandleStudio"));

    static std::string document_start_injector(const std::string &markup, const char *dom_id, const char *position,
                                               const std::string &prelude = {}, const std::string &on_inject = {});
    static std::string theme_user_script();
    static std::string element_defaults_user_script();
    static std::string localized_strings_script(const nlohmann::json &strings);

    // wxDialog
    bool Show(bool show = true);
    bool IsShown() const { return m_shown; }
    void Raise() {}
    bool Close(bool force = false);
    bool Destroy() override;
    bool IsModal() const { return false; }
    void EndModal(int) { Close(); }

    // the app's side: the page and its document-start scripts, the page's messages, the dialog closed by the user
    const std::string &url() const { return m_url; }
    const std::string &title() const { return m_title; }
    std::string        start_script() const;
    void               dispatch_script_message(const nlohmann::json &payload) { on_script_message(payload); }
    void               set_name(const std::string &name) { m_name = name; }
    bool               closed() const { return m_closed; }

protected:
    void *browser() const { return nullptr; }
    wxString build_resource_url(const std::string &resource_path) const;
    bool handle_common_script_command(const nlohmann::json &payload, int close_return_code = wxID_CANCEL);

    virtual void           on_script_message(const nlohmann::json &payload) = 0;
    virtual bool           append_language_to_url() const { return true; }
    virtual void           add_user_scripts() {}
    virtual nlohmann::json localized_strings() const { return nullptr; }

private:
    std::string m_name, m_url, m_title;
    bool        m_shown{false}, m_closed{false};
};

}} // namespace Slic3r::GUI
