// Stand-in for wx/toplevel.h: the frame style flags
#pragma once
#include "wx/window.h"

enum {
    wxSYSTEM_MENU = 0x0800, wxCAPTION = 0x20000000, wxCLOSE_BOX = 0x1000, wxMAXIMIZE_BOX = 0x0200,
    wxRESIZE_BORDER = 0x0040, wxNO_BORDER = 0x00200000,
};
