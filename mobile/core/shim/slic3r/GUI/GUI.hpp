// Stand-in for slic3r/GUI/GUI.hpp: string conversions.
#pragma once
#include <string>
#include <boost/filesystem/path.hpp>
#include "wx/string.h"

namespace Slic3r { namespace GUI {
inline wxString    from_u8(const std::string &s) { return wxString(s); }
inline std::string into_u8(const wxString &s) { return s; }
inline boost::filesystem::path into_path(const wxString &s) { return boost::filesystem::path(std::string(s)); }
// "Ctrl+" / "Alt+" in the UI language (KeyChord, Shortcuts.cpp)
const std::string &shortkey_ctrl_prefix();
const std::string &shortkey_alt_prefix();
const std::string &shortkey_shift_prefix();
// Android has no file browser to show a folder in: the app tells where it is (Plugins.cpp)
void desktop_open_any_folder(const std::string &path);
inline wxString    file_url_from_path(const boost::filesystem::path &path) { return wxString("file://" + path.generic_string()); }
}} // namespace Slic3r::GUI
