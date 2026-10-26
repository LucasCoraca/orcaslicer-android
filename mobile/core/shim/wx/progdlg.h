// Stand-in for wx/progdlg.h: included by upstream files compiled into liborcacore that use none of it.
#pragma once
#include "wx/string.h"

// wxProgressDialog's styles (PluginProgressDialog)
constexpr int wxPD_CAN_ABORT = 0x0001, wxPD_APP_MODAL = 0x0002, wxPD_AUTO_HIDE = 0x0004, wxPD_ELAPSED_TIME = 0x0008,
              wxPD_ESTIMATED_TIME = 0x0010, wxPD_SMOOTH = 0x0020, wxPD_REMAINING_TIME = 0x0040, wxPD_CAN_SKIP = 0x0080;

// wxProgressDialog: the app's progress dialog (PluginsUi.cpp)
#include <functional>
class wxWindow;
class wxProgressDialog
{
public:
    wxProgressDialog(const wxString &title, const wxString &message, int maximum = 100, wxWindow *parent = nullptr, int style = wxPD_APP_MODAL | wxPD_AUTO_HIDE);
    ~wxProgressDialog();
    bool Pulse(const wxString &message = wxString(), bool *skip = nullptr);
    bool Update(int value, const wxString &message = wxString(), bool *skip = nullptr);
    bool Destroy() { delete this; return true; }

private:
    int      m_id;
    wxString m_title, m_message;
    int      m_maximum;
    [[maybe_unused]] int m_style;
};
