// Stand-in for slic3r/GUI/I18N.hpp: same macros, translated by the core's catalog (mobile/core/src/I18n.cpp).
#pragma once
#include <string>
#include "wx/string.h"

#define _(s)                 Slic3r::GUI::I18N::translate((s))
#define _L(s)                Slic3r::GUI::I18N::translate((s))
#define _devL(s)             wxString((s))
#define _omitL(s)            ("")
#define _utf8(s)             Slic3r::GUI::I18N::translate_utf8((s))
#define _u8L(s)              Slic3r::GUI::I18N::translate_utf8((s))
#define _L_CONTEXT(s, ctx)   Slic3r::GUI::I18N::translate((s), (ctx))
#define _u8L_CONTEXT(s, ctx) Slic3r::GUI::I18N::translate_utf8((s), (ctx))
#define _L_PLURAL(s, p, n)   Slic3r::GUI::I18N::translate((s), (p), (n))
#ifndef L
#define L(s) s
#endif
#ifndef L_CONTEXT
#define L_CONTEXT(s, context) s
#endif

namespace Slic3r { namespace GUI { namespace I18N {
std::string translate_utf8(const std::string &s);
std::string translate_utf8(const std::string &s, const std::string &context);
std::string translate_utf8(const std::string &s, const std::string &plural, unsigned int n);
inline wxString translate(const std::string &s) { return translate_utf8(s); }
inline wxString translate(const std::string &s, const std::string &context) { return translate_utf8(s, context); }
inline wxString translate(const std::string &s, const std::string &plural, unsigned int n) { return translate_utf8(s, plural, n); }
}}} // namespace Slic3r::GUI::I18N
