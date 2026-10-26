// Stand-in for wx/numformatter.h: wxNumberFormatter::ToString with a dot separator (double_to_string)
#pragma once
#include <cstdio>
#include "wx/string.h"

class wxNumberFormatter
{
public:
    enum Style { Style_None = 0, Style_WithThousandsSep = 1, Style_NoTrailingZeroes = 2 };
    static wxString ToString(double val, int precision, int = Style_None)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", precision, val);
        return wxString(buf);
    }
};
