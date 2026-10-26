// Stand-in for wx/utils.h: opening a URL is the app's (core host event "open_url").
#pragma once
#include "wx/string.h"

enum { wxBROWSER_NEW_WINDOW = 0x0001, wxBROWSER_NOBUSYCURSOR = 0x0002 };
bool wxLaunchDefaultBrowser(const wxString &url, int flags = 0);

// the system's name and kernel version (NetworkTestDialog::get_os_info)
wxString wxGetOsDescription();
int      wxGetOsVersion(int *major = nullptr, int *minor = nullptr, int *micro = nullptr);

// the device's name and the user's id (OrcaCloudServiceAgent's machine-bound token key)
wxString wxGetHostName();
wxString wxGetUserId();
