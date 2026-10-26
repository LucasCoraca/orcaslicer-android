// Stand-in for slic3r/GUI/Field.hpp (the wx editors are not used by the compiled upstream logic).
#pragma once
#include <boost/any.hpp>
#include "wx/string.h"

#include <vector>
#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI {
// Field.cpp's value formatting (extracted into PublishDialog.cpp)
wxString double_to_string(double const value, const int max_precision = 4);
wxString get_thumbnail_string(const Vec2d& value);
wxString get_thumbnails_string(const std::vector<Vec2d>& values);
}} // namespace Slic3r::GUI
