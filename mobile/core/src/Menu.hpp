// The 3D view's context menus (Menu.cpp, MenuFactory): what other parts of the core list from them.
#pragma once
#include <vector>

namespace orca {

// MenuFactory::HandyModel: Add > Add Handy models
struct HandyModel { const char *key; const char *label; std::vector<const char *> files; bool arrange; };
const std::vector<HandyModel> &handy_models();

} // namespace orca
