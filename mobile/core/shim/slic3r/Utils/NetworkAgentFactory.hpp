// Stand-in for slic3r/Utils/NetworkAgentFactory.hpp: PluginHostApp.cpp includes it without using it; the Python printer
// agents need the Bambu device stack, which is not on Android
#pragma once

namespace Slic3r {
static constexpr char BBL_PRINTER_AGENT_ID[] = "bbl";
}
