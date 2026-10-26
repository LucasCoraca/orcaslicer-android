// Stand-in for wx/arrstr.h: wxArrayString as a vector of the wxString stand-in.
#pragma once
#include <vector>
#include "wx/string.h"

class wxArrayString : public std::vector<wxString>
{
public:
    void     Add(const wxString &s) { push_back(s); }
    size_t   GetCount() const { return size(); }
    bool     IsEmpty() const { return empty(); }
    void     Clear() { clear(); }
    wxString &Item(size_t i) { return at(i); }
    const wxString &Item(size_t i) const { return at(i); }
    wxString &Last() { return back(); }
    int      Index(const wxString &s) const
    {
        for (size_t i = 0; i < size(); ++i)
            if ((*this)[i] == s) return int(i);
        return -1;
    }
};
