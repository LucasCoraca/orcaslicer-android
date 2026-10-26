// Stand-in for wx/filedlg.h: the app's document picker (Host::ask "open_file", PluginsUi.cpp)
#pragma once
#include "wx/msgdlg.h"
#include "wx/string.h"

enum { wxFD_OPEN = 0x0001, wxFD_SAVE = 0x0002, wxFD_OVERWRITE_PROMPT = 0x0004, wxFD_FILE_MUST_EXIST = 0x0010, wxFD_MULTIPLE = 0x0020 };

class wxFileDialog
{
public:
    wxFileDialog(const void *, const wxString &message, const wxString &default_dir = wxString(), const wxString &default_file = wxString(),
                 const wxString &wildcard = wxString(), long style = wxFD_OPEN)
        : m_message(message), m_wildcard(wildcard), m_style(style) {}
    int      ShowModal();
    wxString GetPath() const { return m_path; }

private:
    wxString m_message, m_wildcard, m_path;
    [[maybe_unused]] long m_style;
};
