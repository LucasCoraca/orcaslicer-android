// The project backup (MainFrame's set_backup_interval / set_backup_callback, Plater's EVT_RESTORE_PROJECT): the
// unsaved project is written to the model's backup folder every backup_interval seconds; at start the previous
// session's backup is offered for restoring. Android may end the app in the background, so the app also asks for a
// backup when it goes to the background.
#include <thread>

#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

void export_3mf_file(const std::string &path, SaveStrategy strategy, int export_plate_idx);

namespace {

std::string g_last_backup;   // the previous session's backup folder (app config last_backup_path at start)
unsigned g_backup_revision = ~0u;

// the callback's action 1: Plater::export_3mf(backup_path + "/.3mf", SaveStrategy::Backup) when not up to date
void backup_now()
{
    std::lock_guard<std::recursive_mutex> l(Core::get().mutex);
    Plater &p = Plater::get();
    if (!p.dirty || p.revision == g_backup_revision) return;
    try {
        export_3mf_file(p.model().get_backup_path() + "/.3mf", SaveStrategy::Backup, -1);
        g_backup_revision = p.revision;
        BOOST_LOG_TRIVIAL(info) << "project backed up to " << p.model().get_backup_path();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "backup failed: " << e.what();
    }
}

} // namespace

// Core::init: the interval and the callback, and the backup folder of this session
void backup_init()
{
    AppConfig &cfg = *Core::get().app_config;
    g_last_backup = cfg.get_last_backup_dir();
    if (cfg.get("backup_switch").empty()) cfg.set("backup_switch", "true");
    if (cfg.get("backup_switch") == "true") {
        std::string backup_interval;
        if (!cfg.get("app", "backup_interval", backup_interval) || backup_interval.empty()) backup_interval = "10";
        set_backup_interval(std::stol(backup_interval));
    } else
        set_backup_interval(0);
    set_backup_callback([](int action) {
        if (action == 0) {
            // EVT_BACKUP_POST: the backup thread calls this holding its lock; the UI tasks run after it, as the
            // desktop's posted event does
            std::thread([] {
                std::lock_guard<std::recursive_mutex> l(Core::get().mutex);
                run_backup_ui_tasks();
            }).detach();
        } else if (action == 1)
            backup_now();
    });
    cfg.update_last_backup_dir(Plater::get().model().get_backup_path());
    Plater::get().model().set_need_backup();
}

static Commands reg({
    // EVT_RESTORE_PROJECT: is there an unsaved project of an earlier session?
    {"project.restore_check", [](const json &) -> json {
        std::string last = g_last_backup, origin;
        if (has_restore_data(last, origin))
            return {{"available", true}, {"title", std::string(SLIC3R_APP_FULL_NAME) + " - " + _u8L("Restore")},
                    {"message", _u8L("Previously unsaved items have been detected. Do you want to restore them?")}};
        try {
            if (origin != "<lock>" && !g_last_backup.empty()) fs::remove_all(g_last_backup);   // see bbs_3mf.cpp for lock detail
        } catch (...) {}
        return {{"available", false}};
    }},
    // load_project(last_backup, originfile) or drop the backup
    {"project.restore", [](const json &a) -> json {
        std::string last = g_last_backup, origin;
        const bool ok = has_restore_data(last, origin);
        if (!ok || !a.value("restore", false)) {
            try { if (origin != "<lock>" && !g_last_backup.empty()) fs::remove_all(g_last_backup); } catch (...) {}
            return Plater::get().state();
        }
        json st = Core::get().call("project.load", {{"paths", {last}}, {"restore_origin", origin}});
        backup_soon();
        return st;
    }},
    {"project.backup_now", [](const json &) -> json { backup_now(); return nullptr; }},
});

} // namespace orca
