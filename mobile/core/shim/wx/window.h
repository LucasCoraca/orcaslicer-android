// Stand-in for wx/window.h: the windows the plugin system keeps (WebDialog, DockPanel, PluginProgressDialog stand-ins)
#pragma once
#include "wx/string.h"

class wxWindow
{
public:
    virtual ~wxWindow() = default;
    virtual bool Destroy() { delete this; return true; }
    bool         IsShownOnScreen() const { return true; }
};
