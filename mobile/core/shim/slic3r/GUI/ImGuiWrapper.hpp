// Stand-in for slic3r/GUI/ImGuiWrapper.hpp: the overlays are drawn once the 3D view exists
#pragma once

namespace Slic3r { namespace GUI {
class ImGuiWrapper
{
public:
    bool display_initialized() const { return true; }
};
}} // namespace Slic3r::GUI
