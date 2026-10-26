// Stand-in for wx/stdpaths.h: the user data directory is the core's data dir (app-private storage).
#pragma once
#include "wx/string.h"

class wxStandardPaths
{
public:
    static wxStandardPaths &Get() { static wxStandardPaths p; return p; }
    wxString GetUserDataDir() const;   // Host.cpp
};
