// Stand-in for slic3r/GUI/MsgDialog.hpp: ShowModal() asks the app (Compose dialog) through the core's host
// interface and blocks the calling engine thread until the user answers.
#pragma once
#include <string>
#include "wx/msgdlg.h"

namespace Slic3r { namespace GUI {

// Implemented in mobile/core/src/Host.cpp. Returns wxID_OK / wxID_YES / wxID_NO / wxID_CANCEL.
int host_message_dialog(const std::string &message, const std::string &caption, long style,
                        const std::string &yes_label, const std::string &no_label, const std::string &ok_label,
                        bool dsa_button, bool *dsa_checked, const std::string &dsa_label = {});

class MessageDialog
{
public:
    MessageDialog(const void *, const wxString &message, const wxString &caption = wxString(), long style = wxOK)
        : m_message(message), m_caption(caption), m_style(style) {}
    void SetButtonLabel(int id, const wxString &label, bool = false)
    {
        if (id == wxID_YES) m_yes = label; else if (id == wxID_NO) m_no = label; else if (id == wxID_OK) m_ok = label;
    }
    void show_dsa_button(const wxString &label = wxString()) { m_dsa = true; m_dsa_label = label; }
    bool get_checkbox_state() const { return m_dsa_checked; }
    // wxRichMessageDialog's check box
    void ShowCheckBox(const wxString &label, bool checked = false) { m_dsa = true; m_dsa_label = label; m_dsa_checked = checked; }
    bool IsCheckBoxChecked() const { return m_dsa_checked; }
    void CenterOnParent() {}
    int  ShowModal() { return host_message_dialog(m_message, m_caption, m_style, m_yes, m_no, m_ok, m_dsa, &m_dsa_checked, m_dsa_label); }

private:
    std::string m_message, m_caption, m_yes, m_no, m_ok, m_dsa_label;
    long        m_style;
    bool        m_dsa{false}, m_dsa_checked{false};
};
using RichMessageDialog = MessageDialog;
using WarningDialog     = MessageDialog;

}} // namespace Slic3r::GUI
