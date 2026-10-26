// Stand-in for wx/gdicmn.h: sizes (plugin windows and dock panes)
#pragma once

struct wxSize
{
    int x{-1}, y{-1};
    wxSize() = default;
    wxSize(int w, int h) : x(w), y(h) {}
    int GetWidth() const { return x; }
    int GetHeight() const { return y; }
};

struct wxPoint
{
    int x{-1}, y{-1};
    wxPoint() = default;
    wxPoint(int x_, int y_) : x(x_), y(y_) {}
};
inline const wxPoint wxDefaultPosition;
inline const wxSize  wxDefaultSize;
typedef int          wxWindowID;
constexpr int        wxID_ANY = -1;
