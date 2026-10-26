// Help > Troubleshoot Center (TroubleshootDialog): the system info, the profiles overview, packing the session's logs
// with the project, cleaning the system profiles cache, the stored logs and the log level. The dialog's functions are
// compiled from upstream on a stand-in dialog; the files it would save through wxFileDialog / wxDirDialog are written
// by the app into a document the user picks (troubleshoot.write), the display and GL renderer come from the app.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <boost/algorithm/string/trim_all.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <sys/system_properties.h>
#include <unistd.h>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"
#include "slic3r/GUI/BuildCommit.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "wx/string.h"

namespace orca {
void export_3mf_file(const std::string &path, Slic3r::SaveStrategy strategy, int export_plate_idx);   // ProjectIO.cpp
bool save_project_through_app();                                                                      // ProjectIO.cpp
}

namespace orca { namespace troubleshoot {
using namespace Slic3r;
using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

#define LOG_FILES_MAX_NUM 30
// the Linux branches of the upstream functions are the ones for Android
#ifndef __LINUX__
#define __LINUX__
#endif

// what the app tells about the device when the dialog opens
struct Device
{
    std::string gpu, glsl, package;
    struct Display { int width, height; double scale; };
    std::vector<Display> displays;
};
static Device g_device;

struct GLInfo
{
    std::string get_renderer() const { return g_device.gpu; }
    std::string get_glsl_version() const { return g_device.glsl; }
};
GLInfo ts_gl_info() { return {}; }

// wxDisplay of GetMONinfo
struct wxRect { int x, y, width, height; };
struct wxDisplay
{
    int         i;
    explicit    wxDisplay(int i) : i(i) {}
    static int  GetCount() { return int(g_device.displays.size()); }
    bool        IsOk() const { return i >= 0 && i < GetCount(); }
    double      GetScaleFactor() const { return g_device.displays[i].scale; }
    wxRect      GetGeometry() const { return {0, 0, g_device.displays[i].width, g_device.displays[i].height}; }
};

// Plater as PackAll / RebuildSystemProfiles see it: the project is packed as it is now (written into the cache, so
// unsaved changes are included); saving before the restart goes through the app's Save Project
struct TsPlater
{
    wxString get_project_filename(const wxString &ext)
    {
        Plater &p = Plater::get();
        if (p.model().objects.empty()) return wxString();
        fs::path dir = fs::path(temporary_dir()) / "troubleshoot";
        fs::create_directories(dir);
        fs::path path = dir / ((p.project_name.empty() ? std::string("Untitled") : p.project_name) + ext.ToStdString());
        export_3mf_file(path.string(), SaveStrategy::SplitModel | SaveStrategy::ShareMesh, -1);
        return wxString(path.string());
    }
    bool is_project_dirty() { return Plater::get().dirty; }
    void save_project() { save_project_through_app(); }
};
TsPlater *ts_plater()
{
    static TsPlater p;
    return &p;
}

struct Label
{
    std::string text;
    void SetLabel(const wxString &s) { text = s.ToStdString(); }
};

struct TroubleshootDialog
{
    Label   *m_logs_storage{&m_logs_label};
    Label    m_logs_label;
    int      m_printers__act{0};
    int      m_printers__usr{0};
    int      m_filaments_act{0};
    int      m_filaments_usr{0};
    int      m_processes_act{0};
    int      m_processes_usr{0};
    int      m_end_modal{0};
    // what ExportAsJson / ExportAsZip would save: the app asks where and writes it (troubleshoot.write)
    json     m_export;

    wxString GetTimestamp()
    {
        std::time_t t = std::time(nullptr);
        char        buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M", std::localtime(&t));
        return wxString(buf);
    }
    wxString GetSysInfoAll();
    wxString GetProfilesOverview();
    wxString GetOStype() { return "Android"; }
    wxString GetOSinfo()
    {
        char release[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "", maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "";
        __system_property_get("ro.build.version.release", release);
        __system_property_get("ro.build.version.sdk", sdk);
        __system_property_get("ro.product.manufacturer", maker);
        __system_property_get("ro.product.model", model);
        return wxString::Format("Android %s (API %s) %s %s", release, sdk, maker, model);
    }
    wxString GetPackageType() { return wxString(g_device.package); }
    wxString GetCPUinfo();
    wxString GetGPUinfo();
    wxString GetRAMinfo();
    wxString GetMONinfo();

    void     PackAll();
    void     RebuildSystemProfiles();
    // the app restarts itself; the system profiles are installed again from the resources at startup
    bool     RestartApplication()
    {
        Host::event("restart_app", json::object());
        return true;
    }
    void     ClearLogs();
    void     UpdateLogsStorage();
    void     EndModal(int id) { m_end_modal = id; }

    static std::map<std::string, std::string> parse_lscpu_etc(const std::string &name, char delimiter);

    bool ExportAsJson(const wxString &json_data, const wxString &export_name = wxEmptyString)
    {
        m_export = {{"name", (export_name.IsEmpty() ? std::string("export") : export_name.ToStdString()) + ".json"}, {"text", json_data.ToStdString()}};
        return true;
    }
    bool ExportAsZip(const std::vector<wxString> &sources, const wxString &export_name)
    {
        json src = json::array();
        for (const wxString &s : sources) src.push_back(s.ToStdString());
        m_export = {{"name", (export_name.IsEmpty() ? fs::path(sources[0].ToStdString()).filename().string() : export_name.ToStdString()) + ".zip"},
                    {"sources", src}};
        return true;
    }
};

#include "slic3r/GUI/Troubleshoot.inc"

TroubleshootDialog &dialog()
{
    static TroubleshootDialog d;
    return d;
}

// SaveAsZip / AddToZip: files and folders under their parent folder's name
bool save_as_zip(const std::vector<std::string> &sources, const std::string &zip_path)
{
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zip_path.c_str(), 0)) return false;
    bool ok = true;
    auto add_file = [&](const fs::path &file, const std::string &name) {
        ok = ok && mz_zip_writer_add_file(&zip, name.c_str(), file.string().c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION);
    };
    for (const std::string &s : sources) {
        fs::path src(s), root = src.parent_path();
        if (fs::is_directory(src)) {
            for (const auto &e : fs::recursive_directory_iterator(src))
                if (fs::is_regular_file(e.path())) add_file(e.path(), fs::relative(e.path(), root).generic_string());
        } else if (fs::is_regular_file(src))
            add_file(src, src.filename().string());
        else
            ok = false;
    }
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    if (!ok) fs::remove(zip_path);
    return ok;
}

// the dialog's texts and values (its constructor and create_item_loaded_profiles)
json state()
{
    TroubleshootDialog &d = dialog();
    PresetBundle *pb = Core::get().preset_bundle.get();
    d.GetProfilesOverview();
    auto sys_count = [](PresetCollection &col) {
        int sys = 0;
        for (auto it = col.begin(); it != col.end(); it++)
            if (it->is_system) sys++;
        return sys;
    };
    d.UpdateLogsStorage();
    const std::string level = Core::get().app_config->get("log_severity_level");
    const bool dark = Core::get().app_config->get("dark_color_mode") == "1";
    return {{"title", _u8L("Troubleshoot Center")},
            {"logo", resources_dir() + (dark ? "/images/OrcaSlicer_horizontal_dark.svg" : "/images/OrcaSlicer_horizontal_light.svg")},
            {"version", SoftFever_VERSION},
            {"build", GUI::build_commit_label},
            {"build_url", std::string("https://github.com/OrcaSlicer/OrcaSlicer/commit/") + GUI::build_commit_hash},
            {"sys_full", {d.GetOSinfo().ToStdString(), d.GetPackageType().ToStdString(), d.GetCPUinfo().ToStdString(),
                          (d.GetRAMinfo() + " RAM").ToStdString(), d.GetGPUinfo().ToStdString(), d.GetMONinfo().ToStdString()}},
            {"sys_short", {d.GetOStype().ToStdString()}},
            {"hide", _u8L("Hide")}, {"show", _u8L("Show")}, {"copy", _u8L("Copy")},
            {"hide_tip", _u8L("Show/Hide system information")}, {"copy_tip", _u8L("Copy system information to clipboard")},
            {"wiki_url", "https://www.orcaslicer.com/wiki/troubleshoot_center"},
            {"video_url", "https://www.youtube.com/watch?v=CFzt8W7OCx0"},
            {"information", _u8L("Information")},
            {"info", {_u8L("We need information for diagnosing source of the issue. Check wiki page for detailed guide."),
                      _u8L("Pack button collects project file and logs of current session onto a ZIP archive."),
                      _u8L("Any additional visual examples like images or screen recordings might be helpful while reporting the issue.")}},
            {"report", _u8L("Report issue") + " "},
            {"pack", _u8L("Pack") + "..."},
            {"profiles", _u8L("Profiles")},
            {"clean_cache", _u8L("Clean system profiles cache")},
            {"clean_cache_tip", _u8L("Cleans and rebuilds system profiles cache on next launch.")},
            {"clean", _u8L("Clean")},
            {"loaded", _u8L("Loaded profiles overview")},
            {"loaded_tip", _u8L("This section shows information for loaded profiles.")},
            {"export", _u8L("Export") + "..."},
            {"export_tip", _u8L("Exports detailed overview of loaded profiles in json format.")},
            {"columns", {_u8L("Active"), _u8L("System"), _u8L("User")}},
            {"rows", {{_u8L("Printers"), d.m_printers__act, sys_count(pb->printers), d.m_printers__usr},
                      {_u8L("Filaments"), d.m_filaments_act, sys_count(pb->filaments), d.m_filaments_usr},
                      {_u8L("Processes"), d.m_processes_act, sys_count(pb->prints), d.m_processes_usr}}},
            {"more", _u8L("More")},
            {"cfg_folder", _u8L("Configurations folder")},
            {"browse", _u8L("Browse") + "..."},
            {"browse_tip", _u8L("Opens configurations folder.")},
            {"log_level", _u8L("Log level")},
            {"levels", {_u8L("fatal"), _u8L("error"), _u8L("warning"), _u8L("info"), _u8L("debug"), _u8L("trace")}},
            {"level", level.empty() ? -1 : int(level_string_to_boost(level))},
            {"stored_logs", _u8L("Stored logs")},
            {"stored_logs_tip", _u8L("Packs all stored logs onto a ZIP archive.")},
            {"logs_storage", d.m_logs_storage->text.empty() ? std::string("-") : d.m_logs_storage->text},
            {"clear", _u8L("Clear")}};
}

json open(const json &a)
{
    g_device.gpu     = a.value("gpu", "");
    g_device.glsl    = a.value("glsl", "");
    g_device.package = a.value("package", "");
    g_device.displays.clear();
    if (a.contains("displays"))
        for (const json &m : a["displays"]) g_device.displays.push_back({m.value("width", 0), m.value("height", 0), m.value("scale", 1.0)});
    dialog().m_end_modal = 0;
    return state();
}

// the buttons: the result is the new state, what to save ("export") and whether the dialog closes
json action(const json &a)
{
    TroubleshootDialog &d = dialog();
    const std::string what = a.value("what", "");
    d.m_export = nullptr;
    if (what == "pack") d.PackAll();
    else if (what == "clean") d.RebuildSystemProfiles();
    else if (what == "export_profiles") d.ExportAsJson(d.GetProfilesOverview(), "ProfilesOverview");
    else if (what == "pack_logs") d.ExportAsZip({wxString((fs::path(data_dir()) / "log").string())}, "OrcaSlicer_Logs_" + d.GetTimestamp());
    else if (what == "clear_logs") d.ClearLogs();
    else if (what == "copy") return d.GetSysInfoAll().ToStdString();
    else if (what == "log_level") {
        // create_item_log_level_combo's selection
        auto level = get_string_logging_level(unsigned(a.value("index", 3)));
        set_logging_level(level_string_to_boost(level));
        Core::get().app_config->set("log_severity_level", level);
    } else if (what == "report") {
        // the "Report issue" link: the issue form with the version and system, on this port's tracker
        auto encodeStr = [](const std::string &text) {
            std::string out;
            for (unsigned char c : text) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
                else { char b[4]; std::snprintf(b, sizeof(b), "%%%02X", c); out += b; }
            }
            return out;
        };
        std::string url = "https://github.com/LucasCoraca/orcaslicer-android/issues/new?";
        url += "os_type=%22" + d.GetOStype().ToStdString() + "%22";
        url += "&version=" + encodeStr(SoftFever_VERSION);
        url += "&os_version=" + encodeStr(d.GetOSinfo().ToStdString());
        return {{"url", url}};
    }
    json st = state();
    st["save"]   = d.m_export;
    st["close"]  = d.m_end_modal != 0;
    return st;
}

// ExportAsJson / ExportAsZip into the file the app writes to the picked document; then "Export successful"
json write(const json &a)
{
    const json &e = a.at("export");
    const std::string path = a.at("path").get<std::string>();
    bool ok;
    if (e.contains("text")) {
        std::ofstream f(path, std::ios::binary);
        f << e["text"].get<std::string>();
        ok = f.good();
    } else
        ok = save_as_zip(e.at("sources").get<std::vector<std::string>>(), path);
    if (!ok) throw RuntimeError(_u8L("Export failed\nPlease check write permissions or file in use by another application"));
    return _u8L("Export successful");
}

}} // namespace orca::troubleshoot

namespace orca {
static Commands reg({
    {"troubleshoot.open", troubleshoot::open},
    {"troubleshoot.action", troubleshoot::action},
    {"troubleshoot.write", troubleshoot::write},
});
} // namespace orca
