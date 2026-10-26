// Stand-in for slic3r/GUI/Tab.hpp: the settings tabs follow a preset reload (the app reloads its pages from the
// "presets" event)
#pragma once

namespace Slic3r { namespace GUI {
class Tab
{
public:
    void reload_config() {}
    void update_changed_ui() {}
};
}} // namespace Slic3r::GUI
