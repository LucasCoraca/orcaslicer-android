// Stand-in for slic3r/GUI/DeviceManager.hpp: the Bambu device list (no Bambu network plug-in on Android: empty)
#pragma once
#include <string>

namespace Slic3r {
class MachineObject
{
public:
    std::string printer_agent_id, printer_type;
    std::string connection_type() const { return {}; }
    bool        is_online() const { return false; }
    bool        is_connected() const { return false; }
    std::string get_ota_version() const { return {}; }
};
class DeviceManager
{
public:
    MachineObject *get_selected_machine() { return nullptr; }
};
} // namespace Slic3r
