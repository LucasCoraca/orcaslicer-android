// Stand-in for slic3r/GUI/MainFrame.hpp: the tab switch the print host queue asks for after an upload.
#pragma once
#include <string>
#include "wx/window.h"

#define TAB_ID_MONITOR "monitor"

namespace Slic3r { namespace GUI {
class MainFrame : public wxWindow
{
public:
    // the sidebar and the tabs follow the presets (a "presets" event)
    void update_side_preset_ui();
    void fit_tab_labels() {}
    void request_select_tab(const std::string &name);
    void load_printer_url(const std::string &url, const std::string &apikey = "");
};
}} // namespace Slic3r::GUI
