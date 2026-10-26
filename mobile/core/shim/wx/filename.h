// Stand-in for wx/filename.h: the path joins and directory checks upstream networking code makes (boost::filesystem).
#pragma once
#include <boost/filesystem.hpp>
#include "wx/string.h"

enum { wxS_DIR_DEFAULT = 0777 };
enum { wxPATH_MKDIR_FULL = 1 };

class wxFileName
{
public:
    wxFileName() = default;
    explicit wxFileName(const wxString &full) : m_path(full.ToStdString()) {}
    wxFileName(const wxString &dir, const wxString &name) : m_path(boost::filesystem::path(dir.ToStdString()) / name.ToStdString()) {}
    void     MakeAbsolute() { m_path = boost::filesystem::absolute(m_path); }
    wxString GetFullPath() const { return wxString(m_path.string()); }
    wxString GetPath() const { return wxString(m_path.parent_path().string()); }
    wxString GetFullName() const { return wxString(m_path.filename().string()); }
    static bool DirExists(const wxString &dir) { return boost::filesystem::is_directory(dir.ToStdString()); }
    static bool Mkdir(const wxString &dir, int = wxS_DIR_DEFAULT, int = 0)
    {
        boost::system::error_code ec;
        boost::filesystem::create_directories(dir.ToStdString(), ec);
        return !ec;
    }

private:
    boost::filesystem::path m_path;
};
