// Stand-in for wx/msgdlg.h: the style and answer ids used by upstream GUI logic. Dialogs: slic3r/GUI/MsgDialog.hpp.
#pragma once
#include "wx/string.h"

enum {
    wxOK = 0x00000004, wxYES = 0x00000002, wxNO = 0x00000008, wxCANCEL = 0x00000010,
    wxICON_EXCLAMATION = 0x00000100, wxICON_WARNING = wxICON_EXCLAMATION, wxICON_HAND = 0x00000200,
    wxICON_ERROR = wxICON_HAND, wxICON_QUESTION = 0x00000400, wxICON_INFORMATION = 0x00000800,
    wxYES_NO = wxYES | wxNO, wxYES_DEFAULT = 0, wxNO_DEFAULT = 0x00000080, wxCENTRE = 0x0001, wxCENTER = wxCENTRE,
    wxAPPLY = 0x00000020, wxCLOSE = 0x00001000, wxCANCEL_DEFAULT = 0x80000000,
};
enum { wxID_OK = 5100, wxID_CANCEL = 5101, wxID_YES = 5103, wxID_NO = 5104, wxID_CLOSE = 5111, wxID_REMOVE = 5138, wxID_ABORT = 5115 };

class wxWindow;

// wxMessageDialog: asks the app like MessageDialog (MsgDialog.hpp's host_message_dialog)
namespace Slic3r { namespace GUI {
int host_message_dialog(const std::string &message, const std::string &caption, long style, const std::string &yes_label,
                        const std::string &no_label, const std::string &ok_label, bool dsa_button, bool *dsa_checked,
                        const std::string &dsa_label);
}}
class wxMessageDialog
{
public:
    wxMessageDialog(const void *, const wxString &message, const wxString &caption = wxString(), long style = wxOK)
        : m_message(message), m_caption(caption), m_style(style) {}
    void SetOKCancelLabels(const wxString &ok, const wxString &) { m_ok = ok; }
    void SetYesNoLabels(const wxString &yes, const wxString &no) { m_yes = yes; m_no = no; }
    int  ShowModal()
    {
        bool dsa = false;
        return Slic3r::GUI::host_message_dialog(m_message, m_caption, m_style, m_yes, m_no, m_ok, false, &dsa, {});
    }

private:
    std::string m_message, m_caption, m_ok, m_yes, m_no;
    long        m_style;
};

// wxMessageBox answers wxYES / wxNO / wxOK / wxCANCEL, not the button ids
inline int wxMessageBox(const wxString &message, const wxString &caption = "Message", long style = wxOK | wxCENTRE, const void * = nullptr)
{
    const int r = wxMessageDialog(nullptr, message, caption, style).ShowModal();
    return r == wxID_YES ? wxYES : r == wxID_NO ? wxNO : r == wxID_OK ? wxOK : wxCANCEL;
}
