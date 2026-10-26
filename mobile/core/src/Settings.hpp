#pragma once
#include <nlohmann/json.hpp>

namespace orca {
nlohmann::json presets_state();
void           presets_changed();
std::string    next_filament_color();   // Plater::get_next_color_for_filament (Presets.cpp)
} // namespace orca
