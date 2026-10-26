// Stand-in for wx/uri.h: percent-decoding (WebHosting.cpp's is_content_url)
#pragma once
#include <cstdlib>
#include "wx/string.h"

class wxURI
{
public:
    static wxString Unescape(const wxString &uri)
    {
        std::string out;
        for (size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size()) {
                out.push_back(char(std::strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            } else
                out.push_back(uri[i]);
        }
        return out;
    }
};
