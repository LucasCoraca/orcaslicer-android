// Stand-in for slic3r/GUI/DeviceCore/DevConfigUtil.h: the printer capability lookups upstream GUI logic uses
// (resources/printers/<type>.json, section "00.00.00.00"), read the same way as upstream.
#pragma once
#include <string>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include "libslic3r/Utils.hpp"

#ifndef MAIN_EXTRUDER_ID
#define MAIN_EXTRUDER_ID   0
#define DEPUTY_EXTRUDER_ID 1
#endif

namespace Slic3r {
/// Toolhead component type (extruder / nozzle / hotend)
enum class ToolHeadComponent { Extruder, Nozzle, Hotend };
/// Name case style for toolhead display names
enum class ToolHeadNameCase { TitleCase = 0, SentenceCase = 1, LowerCase = 2 };

class DevPrinterConfigUtil
{
public:
    // the display name of a toolhead part (DevConfigUtil.cpp, compiled from upstream: DevConfigUtil.inc)
    static std::string get_toolhead_display_name(const std::string &type_str, int ext_id, ToolHeadComponent component,
                                                 ToolHeadNameCase name_case = ToolHeadNameCase::TitleCase, bool short_name = false);
    template<typename T> static T get_value_from_config(const std::string &type_str, const std::string &item) { return value<T>(type_str, item); }
    static bool support_wrapping_detection(const std::string &type_str) { return value<bool>(type_str, "support_wrapping_detection"); }
    static bool support_safety_options(const std::string &type_str) { return value<bool>(type_str, "support_safety_options"); }

    template<typename T> static T value(const std::string &type_str, const std::string &item)
    {
        try {
            boost::nowide::ifstream f(resources_dir() + "/printers/" + type_str + ".json");
            if (f.is_open()) {
                nlohmann::json jj;
                f >> jj;
                if (jj.contains("00.00.00.00") && jj["00.00.00.00"].contains(item))
                    return jj["00.00.00.00"][item].get<T>();
            }
        } catch (...) {}
        return T();
    }
};
} // namespace Slic3r
