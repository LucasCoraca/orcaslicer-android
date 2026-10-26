// Help > Migrate Filament Presets and the check at startup: GUI_App::check_legacy_user_filament_presets compiled from
// upstream on a stand-in app; its dialogs are the app's message dialogs, the side preset UI is refreshed afterwards.
#include <atomic>
#include <string>
#include <vector>

#include "Core.hpp"
#include "Settings.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"

namespace orca { namespace legacy {
using namespace Slic3r;
using namespace Slic3r::GUI;

// show_error (GUI.cpp) as the app's error message
void show_error(const void *, const wxString &message) { MessageDialog(nullptr, message, _L("Error"), wxOK | wxICON_ERROR).ShowModal(); }

struct MainFrame
{
    void update_side_preset_ui() { presets_changed(); }
};
// the settings tabs reload from the preset bundle when the app shows them
struct Tab
{
    void reload_config() {}
    void update_changed_ui() {}
};

struct LegacyApp
{
    AppConfig         *app_config{nullptr};
    PresetBundle      *preset_bundle{nullptr};
    MainFrame          main;
    MainFrame         *mainframe{&main};
    std::vector<Tab *> tabs_list;
    bool               m_legacy_filament_check_running{false};
    bool               m_legacy_filament_check_due{false};
    // presets are not pulled from a cloud yet
    bool               m_user_preset_pull_pending{false};
    std::atomic<bool>  m_sync_user_presets_now{false};

    void check_legacy_user_filament_presets(bool by_user);
};

#include "slic3r/GUI/LegacyFilamentPresets.inc"

json check(const json &a)
{
    static LegacyApp app;
    app.app_config    = Core::get().app_config.get();
    app.preset_bundle = Core::get().preset_bundle.get();
    app.check_legacy_user_filament_presets(a.value("by_user", false));
    return nullptr;
}

}} // namespace orca::legacy

namespace orca {
static Commands reg({{"presets.migrate_legacy", legacy::check}});
} // namespace orca
