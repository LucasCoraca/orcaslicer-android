// Translations: Orca's gettext catalogs (localization/i18n/<lang>/OrcaSlicer_<lang>.po, shipped as they are) read at
// run time, for libslic3r's translate callback, the upstream GUI logic (shim I18N.hpp) and the app (i18n.* calls).
#include "I18n.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "libslic3r/I18N.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace orca {

namespace {

std::unordered_map<std::string, std::vector<std::string>> g_catalog;   // key: [ctx \x04] msgid
std::string                                               g_plural_expr = "n != 1";
std::string                                               g_language;

// Tiny evaluator for the C expression of the Plural-Forms header (n, integers, ?:, ||, &&, comparisons, %, parens).
struct PluralExpr
{
    const char *p;
    long        n;
    void ws() { while (*p && std::isspace((unsigned char)*p)) ++p; }
    bool eat(const char *t)
    {
        ws();
        size_t l = std::strlen(t);
        if (std::strncmp(p, t, l) == 0) { p += l; return true; }
        return false;
    }
    long primary()
    {
        ws();
        if (eat("(")) { long v = ternary(); eat(")"); return v; }
        if (eat("!")) return !primary();
        if (*p == 'n') { ++p; return n; }
        char *end; long v = std::strtol(p, &end, 10); p = end; return v;
    }
    long mul()
    {
        long v = primary();
        for (;;) {
            if (eat("%")) { long r = primary(); v = r ? v % r : 0; }
            else if (eat("*")) v *= primary();
            else if (eat("/")) { long r = primary(); v = r ? v / r : 0; }
            else return v;
        }
    }
    long add()
    {
        long v = mul();
        for (;;) {
            if (eat("+")) v += mul(); else if (eat("-")) v -= mul(); else return v;
        }
    }
    long rel()
    {
        long v = add();
        for (;;) {
            if (eat("<=")) v = v <= add(); else if (eat(">=")) v = v >= add();
            else if (eat("<")) v = v < add(); else if (eat(">")) v = v > add();
            else return v;
        }
    }
    long eq()
    {
        long v = rel();
        for (;;) {
            if (eat("==")) v = v == rel(); else if (eat("!=")) v = v != rel(); else return v;
        }
    }
    long land() { long v = eq(); while (eat("&&")) { long r = eq(); v = v && r; } return v; }
    long lor() { long v = land(); while (eat("||")) { long r = land(); v = v || r; } return v; }
    long ternary()
    {
        long c = lor();
        if (eat("?")) { long a = ternary(); eat(":"); long b = ternary(); return c ? a : b; }
        return c;
    }
};

std::string unescape(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
        } else
            out += s[i];
    }
    return out;
}

// the quoted string(s) starting at line[pos]
std::string po_quoted(const std::string &line)
{
    size_t a = line.find('"'), b = line.rfind('"');
    return (a == std::string::npos || b <= a) ? std::string() : unescape(line.substr(a + 1, b - a - 1));
}

std::string lookup(const std::string &key, size_t form)
{
    auto it = g_catalog.find(key);
    if (it == g_catalog.end() || form >= it->second.size() || it->second[form].empty())
        return {};
    return it->second[form];
}

std::string libslic3r_translate(const char *s) { return translate(s); }

} // namespace

bool load_catalog(const std::string &po_path, const std::string &language)
{
    g_catalog.clear();
    g_plural_expr = "n != 1";
    g_language    = language;
    std::ifstream in(po_path);
    if (!in)
        return false;
    std::string ctx, id, plural, *cur = nullptr, line;
    std::vector<std::string> strs;
    bool fuzzy = false, next_fuzzy = false;
    auto flush = [&]() {
        if (!id.empty() && !fuzzy && !strs.empty())
            g_catalog[ctx.empty() ? id : ctx + '\x04' + id] = strs;
        else if (id.empty() && !strs.empty()) {   // header
            size_t p = strs[0].find("plural=");
            if (p != std::string::npos) {
                std::string e = strs[0].substr(p + 7);
                g_plural_expr = e.substr(0, e.find(';'));
            }
        }
        ctx.clear(); id.clear(); plural.clear(); strs.clear(); cur = nullptr;
        fuzzy = next_fuzzy; next_fuzzy = false;
    };
    bool in_entry = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("#,", 0) == 0) { if (line.find("fuzzy") != std::string::npos) next_fuzzy = true; continue; }
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("msgctxt", 0) == 0) { if (in_entry) flush(); else { fuzzy = next_fuzzy; next_fuzzy = false; } in_entry = true; ctx = po_quoted(line); cur = &ctx; }
        else if (line.rfind("msgid_plural", 0) == 0) { plural = po_quoted(line); cur = &plural; }
        else if (line.rfind("msgid", 0) == 0) {
            if (in_entry && (!id.empty() || !strs.empty())) flush();
            else if (!in_entry) { fuzzy = next_fuzzy; next_fuzzy = false; }
            in_entry = true; id = po_quoted(line); cur = &id;
        } else if (line.rfind("msgstr", 0) == 0) {
            size_t idx = 0;
            if (line.size() > 6 && line[6] == '[') idx = std::strtoul(line.c_str() + 7, nullptr, 10);
            if (strs.size() <= idx) strs.resize(idx + 1);
            strs[idx] = po_quoted(line); cur = &strs[idx];
        } else if (line[0] == '"' && cur) *cur += po_quoted(line);
    }
    if (in_entry) flush();
    return true;
}

const std::string &current_language() { return g_language; }

std::string translate(const std::string &s)
{
    std::string t = lookup(s, 0);
    return t.empty() ? s : t;
}

std::string translate_ctx(const std::string &s, const std::string &ctx)
{
    std::string t = lookup(ctx + '\x04' + s, 0);
    return t.empty() ? translate(s) : t;
}

std::string translate_plural(const std::string &s, const std::string &plural, unsigned n)
{
    PluralExpr e{g_plural_expr.c_str(), long(n)};
    std::string t = lookup(s, size_t(e.ternary()));
    return t.empty() ? (n == 1 ? s : plural) : t;
}

void install_translate_callback() { Slic3r::I18N::set_translate_callback(&libslic3r_translate); }

} // namespace orca

namespace Slic3r { namespace GUI { namespace I18N {
std::string translate_utf8(const std::string &s) { return orca::translate(s); }
std::string translate_utf8(const std::string &s, const std::string &context) { return orca::translate_ctx(s, context); }
std::string translate_utf8(const std::string &s, const std::string &plural, unsigned int n) { return orca::translate_plural(s, plural, n); }
}}} // namespace Slic3r::GUI::I18N
