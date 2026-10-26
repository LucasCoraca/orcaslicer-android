// Stand-in for wx/accel.h: the accelerator flags and entry KeyChord builds (menus are the app's).
#pragma once
#include "wx/defs.h"

enum wxAcceleratorEntryFlags
{
    wxACCEL_NORMAL   = 0x0000,
    wxACCEL_ALT      = 0x0001,
    wxACCEL_CTRL     = 0x0002,
    wxACCEL_SHIFT    = 0x0004,
    wxACCEL_RAW_CTRL = wxACCEL_CTRL,
    wxACCEL_CMD      = wxACCEL_CTRL
};

class wxAcceleratorEntry
{
public:
    wxAcceleratorEntry(int flags = 0, int keyCode = 0, int cmd = 0) : m_flags(flags), m_keyCode(keyCode), m_command(cmd) {}
    int GetFlags() const { return m_flags; }
    int GetKeyCode() const { return m_keyCode; }
    int GetCommand() const { return m_command; }

private:
    int m_flags, m_keyCode, m_command;
};
