// Daily Tips (DailyTipsWindow / DailyTipsPanel, Help > Show Tip of the Day, and the tips in the slicing notification):
// upstream's HintDatabase reads resources/data/hints.ini and keeps the shown hints in user/hints.cereal; it is compiled
// from HintNotification.cpp with its hyperlink callbacks routed to the app as "tip_action" events.
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "wx/string.h"

// ImGui's text colour markers (imconfig.h): the app draws the marked text in the highlight colour
namespace ImGui {
const char ColorMarkerStart = 0x2;
const char ColorMarkerEnd   = 0x3;
} // namespace ImGui

#define HINTS_CEREAL_VERSION 1
namespace orca { namespace hints {
#include "slic3r/GUI/HintHelpersCereal.inc"
}} // namespace orca::hints
// version of used hints binary file
CEREAL_CLASS_VERSION(orca::hints::HintsCerealData, HINTS_CEREAL_VERSION);

namespace orca { namespace hints {
using namespace Slic3r;
using namespace Slic3r::GUI;

enum class PreferencesTab { Control };

void hint_action(const json &a) { Host::event("tip_action", a); }
ConfigOptionMode hint_mode()
{
    const std::string m = Core::get().app_config->get("user_mode");
    return m == "simple" ? comSimple : m == "expert" ? comExpert : m == "develop" ? comDevelop : comAdvanced;
}
size_t hint_extruders()
{
    const auto *nd = Core::get().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return nd ? nd->values.size() : 1;
}
struct HintTab { PresetCollection *m_presets; };
const HintTab *hint_filament_tab()
{
    static HintTab tab;
    tab.m_presets = &Core::get().preset_bundle->filaments;
    return &tab;
}

// HintNotification.cpp's markers in hints.ini texts
const std::string BOLD_MARKER_START      = "<b>";
const std::string BOLD_MARKER_END        = "</b>";
const std::string HYPERTEXT_MARKER_START = "<a>";
const std::string HYPERTEXT_MARKER_END   = "</a>";

#include "slic3r/GUI/HintTypes.inc"
#include "slic3r/GUI/HintHelpers.inc"
#include "slic3r/GUI/HintDatabase.inc"

// DailyTipsPanel::retrieve_data_from_hint_database + render_controller_buttons' page index
json tip(HintDataNavigation nav)
{
    HintData *hint_data = HintDatabase::get_instance().get_hint(nav);
    if (hint_data == nullptr) return nullptr;
    // DailyTipsDataRenderer::render_text: the first line is the headline
    std::string title, content = hint_data->text;
    if (size_t end_pos = content.find_first_of('\n'); end_pos != std::string::npos) {
        title   = content.substr(0, end_pos);
        content = content.substr(end_pos + 1);
    }
    std::string img = hint_data->image_url;
    if (img.empty()) img = "images/dailytips_placeholder.png";
    return {{"title", title},
            {"text", content},
            {"hypertext", hint_data->hypertext},
            {"follow_text", hint_data->follow_text},
            {"image", (boost::filesystem::path(resources_dir()) / img).string()},
            {"wiki", hint_data->documentation_link},
            {"wiki_line", _u8L("For more information, please check out our Wiki")},
            {"wiki_part", _u8L("Wiki")},
            {"page", HintDatabase::get_instance().get_index() + 1},
            {"pages", HintDatabase::get_instance().get_count()},
            {"has_action", bool(hint_data->callback)},
            {"strings", {{"daily_tips", _u8L("Daily Tips")}, {"collapse", _u8L("Collapse")}}}};
}

json get(const json &a)
{
    const std::string nav = a.value("nav", "curr");
    return tip(nav == "next" ? HintDataNavigation::Next : nav == "prev" ? HintDataNavigation::Prev :
               nav == "random" ? HintDataNavigation::Random : HintDataNavigation::Curr);
}

// the hypertext of the current hint
json action(const json &)
{
    HintData *hint_data = HintDatabase::get_instance().get_hint(HintDataNavigation::Curr);
    if (hint_data && hint_data->callback) hint_data->callback();
    return nullptr;
}

}} // namespace orca::hints

namespace orca {
static Commands reg({
    {"tips.get", hints::get},
    {"tips.action", hints::action},
});
} // namespace orca
