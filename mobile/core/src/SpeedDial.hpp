// Speed Dial (MainFrame's "Open Speed Dial"): upstream's ActionRegistry and its built-in actions, compiled from
// ActionRegistry.cpp / NativeCommands.cpp (SpeedDial.cpp); the page is SpeedDialWebDialog (WebDialogs.cpp).
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

#include "libslic3r/Config.hpp"
#include "wx/string.h"

namespace orca { namespace sd {
using Slic3r::ConfigOptionMode;
using Slic3r::comSimple;
using Slic3r::comAdvanced;
using Slic3r::comExpert;
using Slic3r::comDevelop;

#include "slic3r/GUI/SpeedDialTypes.inc"

inline constexpr const char *kOrcaSourceKey  = "orca";
inline constexpr const char *kOrcaSourceName = "OrcaSlicer";

std::vector<std::string> cap_favourites(const std::vector<std::string> &ids, size_t limit);
nlohmann::json           action_json(const AppAction &a);

// the app's single registry (GUI_App::action_registry), built on first use
ActionRegistry &registry();
// GUI_App::get_mode / save_mode / enable_developer_mode on the app config
ConfigOptionMode get_mode();
void             save_mode(ConfigOptionMode mode);
void             enable_developer_mode();

}} // namespace orca::sd
