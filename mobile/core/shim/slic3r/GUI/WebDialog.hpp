// Stand-in for slic3r/GUI/WebDialog.hpp: a plugin's HTML window, shown by the app as a web dialog with the
// window.orca bridge (Plugins.cpp); its messages come back through "plugins.window_message"
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include "wx/string.h"
#include "wx/toplevel.h"

#include "wx/gdicmn.h"

namespace Slic3r { namespace GUI {

class WebDialog : public wxWindow
{
public:
    using MessageHandler = std::function<void(const nlohmann::json &data)>;
    using SubmitHandler  = std::function<void(const nlohmann::json &data)>;
    using CloseHandler   = std::function<void()>;

    WebDialog(wxWindow *parent, const wxString &title, const std::string &html, const wxSize &size, MessageHandler on_message,
              SubmitHandler on_submit, CloseHandler on_close, CloseHandler on_destroyed,
              long wx_style = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER);
    ~WebDialog() override;

    static void post_message(WebDialog *dialog, const nlohmann::json &data);
    static void request_close(WebDialog *dialog);
    static void destroy_silently(WebDialog *dialog);

    void push_message(const nlohmann::json &data);
    bool is_open() const { return m_open; }
    void use_normal_window_type() {}
    const std::optional<nlohmann::json> &result() const { return m_result; }

    // wxDialog
    int  ShowModal();
    void EndModal(int code);
    bool IsModal() const { return m_modal; }
    bool Show(bool show = true);
    bool Close(bool force = false);
    bool Destroy() override;

    // the page's window.wx.postMessage (WebViewHostDialog::OnScriptMessage)
    void on_script_message(const nlohmann::json &payload);
    void on_close_window();
    int  id() const { return m_id; }

private:
    void finish(bool submitted, const nlohmann::json &data);
    void fire_submit(const nlohmann::json &data);
    void fire_close();

    int                           m_id;
    std::string                   m_title, m_html;
    int                           m_width, m_height;
    bool                          m_open{true}, m_close_fired{false}, m_modal{false}, m_shown{false};
    int                           m_modal_result{-1};
    std::optional<nlohmann::json> m_result;
    MessageHandler                m_on_message;
    SubmitHandler                 m_on_submit;
    CloseHandler                  m_on_close;
    CloseHandler                  m_on_destroyed;
};

}} // namespace Slic3r::GUI
