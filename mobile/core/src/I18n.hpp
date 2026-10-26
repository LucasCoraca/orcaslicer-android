#pragma once
#include <cstring>
#include <string>
#include <vector>

namespace orca {
bool               load_catalog(const std::string &po_path, const std::string &language);
const std::string &current_language();
std::string        translate(const std::string &s);
std::string        translate_ctx(const std::string &s, const std::string &ctx);
std::string        translate_plural(const std::string &s, const std::string &plural, unsigned n);
void               install_translate_callback();
} // namespace orca
