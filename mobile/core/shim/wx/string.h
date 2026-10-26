// Stand-in for wxWidgets' wxString, enough for the upstream GUI logic compiled into liborcacore (see CMakeLists.txt).
// UTF-8 std::string inside; Format() is printf with wxString arguments passed as UTF-8.
#pragma once
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <utility>
#include <boost/nowide/convert.hpp>

class wxString : public std::string
{
public:
    wxString() = default;
    wxString(const char *s) : std::string(s ? s : "") {}
    wxString(const char *s, size_t n) : std::string(s, n) {}
    wxString(const std::string &s) : std::string(s) {}
    wxString(std::string &&s) : std::string(std::move(s)) {}
    // wide strings (L"..." literals, boost::nowide::widen) as UTF-8
    wxString(const wchar_t *s) : std::string(s ? boost::nowide::narrow(s) : std::string()) {}
    wxString(const std::wstring &s) : std::string(boost::nowide::narrow(s)) {}
    wxString &operator=(const wchar_t *s) { std::string::operator=(s ? boost::nowide::narrow(s) : std::string()); return *this; }
    wxString &operator=(const std::wstring &s) { std::string::operator=(boost::nowide::narrow(s)); return *this; }
    using std::string::operator=;
    using std::string::operator+=;
    wxString &operator+=(const wchar_t *s) { append(boost::nowide::narrow(s)); return *this; }

    // positions are bytes (callers use them on ASCII text)
    wxString SubString(size_t from, size_t to) const { return from >= size() ? wxString() : wxString(substr(from, to - from + 1)); }
    wxString Mid(size_t from, size_t n = npos) const { return from >= size() ? wxString() : wxString(substr(from, n)); }
    wxString Left(size_t n) const { return wxString(substr(0, n)); }
    // wxString::append(wxChar) next to std::string's
    using std::string::append;
    wxString &append(char c) { push_back(c); return *this; }
    wxString &Truncate(size_t n) { if (size() > n) resize(n); return *this; }
    // white space off the right (or left) end
    wxString &Trim(bool fromRight = true)
    {
        const char *ws = " \t\r\n\v\f";
        if (fromRight) erase(find_last_not_of(ws) + 1);
        else erase(0, std::min(size(), find_first_not_of(ws)));
        return *this;
    }
    size_t   Len() const { return size(); }
    bool     StartsWith(const wxString &p) const { return compare(0, p.size(), p) == 0; }
    bool     EndsWith(const wxString &p) const { return size() >= p.size() && compare(size() - p.size(), p.size(), p) == 0; }
    wxString Lower() const { wxString r(*this); for (char &c : r) c = char(std::tolower((unsigned char)c)); return r; }
    wxString Upper() const { wxString r(*this); for (char &c : r) c = char(std::toupper((unsigned char)c)); return r; }
    std::wstring ToStdWstring() const { return boost::nowide::widen(*this); }
    std::wstring wc_str() const { return ToStdWstring(); }

    std::string ToStdString() const { return *this; }
    int         Find(const wxString &sub) const { const size_t p = find(sub); return p == npos ? -1 : int(p); }   // wxNOT_FOUND = -1
    std::string ToUTF8() const { return *this; }
    std::string utf8_string() const { return *this; }
    const char *utf8_str() const { return c_str(); }
    const char *mb_str() const { return c_str(); }
    bool IsEmpty() const { return empty(); }
    bool Contains(const wxString &sub) const { return find(sub) != npos; }
    wxString BeforeFirst(char c) const { const size_t p = find(c); return p == npos ? *this : wxString(substr(0, p)); }
    char Last() const { return empty() ? '\0' : back(); }
    wxString &RemoveLast(size_t n = 1) { erase(size() - std::min(n, size())); return *this; }
    // wxString(wxUniChar): the character as UTF-8
    explicit wxString(struct wxUniChar c);
    // the number parsers of wxString (C locale)
    bool ToDouble(double *v) const
    {
        const char *b = c_str(); char *e = nullptr;
        double d = std::strtod(b, &e);
        if (e == b || *e != '\0') return false;
        *v = d; return true;
    }
    bool ToULong(unsigned long *v) const
    {
        const char *b = c_str(); char *e = nullptr;
        if (empty() || *b == '-') return false;
        unsigned long d = std::strtoul(b, &e, 10);
        if (*e != '\0') return false;
        *v = d; return true;
    }
    bool ToLong(long *v) const
    {
        const char *b = c_str(); char *e = nullptr;
        long d = std::strtol(b, &e, 10);
        if (e == b || *e != '\0') return false;
        *v = d; return true;
    }
    static wxString FromDouble(double v) { char buf[64]; std::snprintf(buf, sizeof(buf), "%g", v); return wxString(buf); }
    static wxString FromUTF8(const std::string &s) { return wxString(s); }
    static wxString FromUTF8(const char *s) { return wxString(s); }

    template<class... Args> static wxString Format(const wxString &fmt, Args &&...args)
    {
        int n = std::snprintf(nullptr, 0, fmt.c_str(), arg(std::forward<Args>(args))...);
        if (n <= 0)
            return fmt;
        std::string out(size_t(n), '\0');
        std::snprintf(out.data(), out.size() + 1, fmt.c_str(), arg(std::forward<Args>(args))...);
        return wxString(std::move(out));
    }

private:
    template<class T> static T arg(T v) { return v; }
    static const char *arg(const wxString &s) { return s.c_str(); }
    static const char *arg(const std::string &s) { return s.c_str(); }
};

inline wxString operator+(const wxString &a, const wxString &b) { std::string r(a); r.append(b); return wxString(std::move(r)); }
inline wxString operator+(const char *a, const wxString &b) { std::string r(a); r.append(b); return wxString(std::move(r)); }
inline wxString operator+(const wxString &a, const char *b) { std::string r(a); r.append(b); return wxString(std::move(r)); }
inline wxString operator+(const std::string &a, const wxString &b) { std::string r(a); r.append(b); return wxString(std::move(r)); }
inline wxString operator+(const wxString &a, const std::string &b) { std::string r(a); r.append(b); return wxString(std::move(r)); }
inline wxString operator+(const wchar_t *a, const wxString &b) { return wxString(a) + b; }
inline wxString operator+(const wxString &a, const wchar_t *b) { return a + wxString(b); }
inline wxString operator+(const wxString &a, char c) { std::string r(a); r.push_back(c); return wxString(std::move(r)); }
inline wxString operator+(std::string &&a, const wxString &b) { a.append(b); return wxString(std::move(a)); }
inline bool operator==(const wxString &a, const wchar_t *b) { return a == wxString(b); }
inline bool operator!=(const wxString &a, const wchar_t *b) { return !(a == b); }

static const wxString wxEmptyString;

// wxUniChar: one Unicode code point
struct wxUniChar
{
    unsigned value;
    explicit wxUniChar(unsigned v) : value(v) {}
};
inline wxString::wxString(wxUniChar c)
{
    const unsigned v = c.value;
    if (v < 0x80) push_back(char(v));
    else if (v < 0x800) { push_back(char(0xC0 | (v >> 6))); push_back(char(0x80 | (v & 0x3F))); }
    else if (v < 0x10000) { push_back(char(0xE0 | (v >> 12))); push_back(char(0x80 | ((v >> 6) & 0x3F))); push_back(char(0x80 | (v & 0x3F))); }
    else { push_back(char(0xF0 | (v >> 18))); push_back(char(0x80 | ((v >> 12) & 0x3F))); push_back(char(0x80 | ((v >> 6) & 0x3F))); push_back(char(0x80 | (v & 0x3F))); }
}
