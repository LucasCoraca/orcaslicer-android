// Stand-in for slic3r/GUI/format.hpp: boost::format based, returning wxString / std::string.
#pragma once
#include <string>
#include "libslic3r/format.hpp"
#include "wx/string.h"

namespace Slic3r { namespace GUI {
template<typename... TArgs> inline wxString format_wxstr(const std::string &fmt, TArgs &&...args)
{
    return wxString(Slic3r::format(fmt, std::forward<TArgs>(args)...));
}
template<typename... TArgs> inline std::string format(const std::string &fmt, TArgs &&...args)
{
    return Slic3r::format(fmt, std::forward<TArgs>(args)...);
}
}} // namespace Slic3r::GUI
