// Stand-in for wx/filefn.h: file existence and removal (boost::filesystem).
#pragma once
#include <boost/filesystem.hpp>
#include "wx/string.h"

inline bool wxFileExists(const wxString &path) { return boost::filesystem::is_regular_file(path.ToStdString()); }
inline bool wxRemoveFile(const wxString &path)
{
    boost::system::error_code ec;
    return boost::filesystem::remove(path.ToStdString(), ec) && !ec;
}
