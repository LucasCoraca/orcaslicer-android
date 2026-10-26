// Stand-in for slic3r/plugin/host/PluginPages.hpp: a plugin's pages are tabs of the app's main tab bar
// (PluginPagesApp.cpp), not wx notebook pages
#pragma once
#include <string>

#include "slic3r/plugin/PythonPluginInterface.hpp"

namespace Slic3r {

class PluginPages
{
public:
    void on_cap_register(const PluginCapabilityId &id);
    void on_cap_deregister(const PluginCapabilityId &id);
    void on_plugin_register(const std::string &plugin_key);
    void on_plugin_deregister(const std::string &plugin_key);
    void set_visible_page_count(int count);
    void select_page(const PluginCapabilityId &id);
};

} // namespace Slic3r
