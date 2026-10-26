// Stand-in for slic3r/GUI/PluginProgressDialog.hpp: a plugin's progress dialog, shown by the app (Plugins.cpp)
#pragma once
#include <functional>
#include <string>
#include "wx/progdlg.h"
#include "wx/string.h"
#include "wx/window.h"

namespace Slic3r { namespace GUI {

class PluginProgressDialog : public wxWindow
{
public:
    using CloseHandler = std::function<void()>;

    PluginProgressDialog(wxWindow *parent, const wxString &title, const wxString &message, int maximum, int style,
                         CloseHandler on_destroyed = nullptr);
    ~PluginProgressDialog() override;

    static PluginProgressDialog *create_dialog(wxWindow *parent, const wxString &title, const wxString &message, int maximum, int style,
                                               CloseHandler on_destroyed);
    static bool pulse(PluginProgressDialog *dialog, const wxString &message = wxString());
    static bool update(PluginProgressDialog *dialog, int value, const wxString &message = wxString());
    static void start_pulse(PluginProgressDialog *dialog, int interval_ms, const wxString &message = wxString());
    static void stop_pulse(PluginProgressDialog *dialog);
    static void request_close(PluginProgressDialog *dialog);

    bool pulse(const wxString &message = wxString());
    bool update(int value, const wxString &message = wxString());
    void start_pulse(int interval_ms, const wxString &message = wxString());
    void stop_pulse();
    void close();
    bool is_open() const { return m_open; }

    int  id() const { return m_id; }
    bool m_canceled{false};   // the app's Cancel

private:
    void send(const char *what);

    int          m_id;
    bool         m_open{true};
    std::string  m_title, m_message;
    int          m_maximum, m_style, m_value{0};
    bool         m_pulsing{false};
    CloseHandler m_on_destroyed;
};

}} // namespace Slic3r::GUI
