// Help > Check for Updates and the check at startup: GUI_App::check_new_version_sf compiled from upstream, asking this
// port's GitHub releases about the app's own version; EVT_SLIC3R_VERSION_ONLINE's handler (skip version, the
// UpdateVersionDialog with the release notes as HTML) and no_new_version are the app's dialogs.
#include <regex>
#include <sstream>
#include <string>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <md4c-html.h>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/Http.hpp"
#include "wx/string.h"

namespace orca { namespace update {
using namespace Slic3r;

static std::string g_version;   // the app's version name
std::string port_version() { return g_version; }
std::string port_releases_url() { return "https://api.github.com/repos/LucasCoraca/orcaslicer-android/releases"; }

// the update server's query (installation id, signature) is Orca's own server's: GitHub is asked without it
struct UpdaterQuery { std::string iid, version, os, arch, os_info; };
template<class... A> std::string detect_updater_iid(A &&...) { return {}; }
std::string detect_updater_version() { return {}; }
std::string platform_os_type() { return {}; }
std::string platform_architecture() { return {}; }
std::string detect_updater_os_info() { return {}; }
std::string build_updater_query(const UpdaterQuery &) { return {}; }
void        maybe_attach_updater_signature(Http &, const std::string &, const std::string &) {}

Semver get_version(const std::string &str, const std::regex &regexp)
{
    std::smatch match;
    if (std::regex_match(str, match, regexp)) {
        std::string version_cleaned = match[0];
        const boost::optional<Semver> version = Semver::parse(version_cleaned);
        if (version.has_value()) return *version;
    }
    return Semver::invalid();
}

const int EVT_SLIC3R_VERSION_ONLINE = 1;
struct wxCommandEvent
{
    int         type;
    std::string str;
    int         i{0};
    explicit    wxCommandEvent(int type) : type(type) {}
    void        SetString(const std::string &s) { str = s; }
    void        SetInt(int v) { i = v; }
    int         GetInt() const { return i; }
};

struct VersionInfo
{
    std::string version_str, version_name, description, url;
    bool        force_upgrade{false};
};

struct UpdApp
{
    AppConfig  *app_config{nullptr};
    VersionInfo version_info;

    void check_new_version_sf(bool show_tips = false, int by_user = 0);

    // EVT_SHOW_NO_NEW_VERSION
    void no_new_version() { Host::event("info_dialog", {{"title", _u8L("Info")}, {"text", _u8L("This is the newest version.")}}); }

    // EVT_SLIC3R_VERSION_ONLINE: unless the version is skipped, UpdateVersionDialog with update_version_info's page
    void QueueEvent(wxCommandEvent *evt)
    {
        std::unique_ptr<wxCommandEvent> e(evt);
        std::string skip_version_str = app_config->get("app", "skip_version");
        bool skip_this_version = false;
        if (!skip_version_str.empty()) {
            BOOST_LOG_TRIVIAL(info) << "new version = " << version_info.version_str << ", skip version = " << skip_version_str;
            if (version_info.version_str <= skip_version_str) {
                skip_this_version = true;
            } else {
                app_config->set("skip_version", "");
                skip_this_version = false;
            }
        }
        if (skip_this_version && e->GetInt() == 0) return;
        const bool dark = app_config->get("dark_color_mode") == "1";
        const std::string bg_color = dark ? "#2D2D31" : "#FFFFFF", fg_color = dark ? "#EFEFF0" : "#262E30";
        std::string style = "body {color:" + fg_color + "; background-color:" + bg_color + "; font-family:sans-serif}"
                          + "a    {color: #009688}"
                          + "img  {max-width:100%; height:auto}"
                          + "ul   {padding-inline-start: 20px}";
        std::string html_source = (boost::format("<html><head><style>%1%</style></head><body>") % style).str();
        md_html(version_info.description.data(), MD_SIZE(version_info.description.size()), [](const MD_CHAR *text, MD_SIZE size, void *userdata) {
            ((std::string *) userdata)->append(text, size);
        }, (void *) &html_source, MD_DIALECT_GITHUB | MD_FLAG_STRIKETHROUGH | MD_FLAG_WIKILINKS, 0);
        html_source.append("</body></html>");
        Host::event("update_available", {{"title", _u8L("New version of Orca Slicer")},
                                         {"info", (boost::format(_u8L("Click to download new version in default browser: %s")) % version_info.version_str).str()},
                                         {"link", _u8L("Check on GitHub")},
                                         {"html", html_source},
                                         {"url", version_info.url},
                                         {"version", version_info.version_str},
                                         {"download", _u8L("Download")},
                                         {"skip", e->GetInt() == 0 ? _u8L("Skip this Version") : std::string()},
                                         {"cancel", _u8L("Cancel")},
                                         {"stable_only_label", _u8L("Check for stable updates only")},
                                         {"stable_only", app_config->get_bool("check_stable_update_only")}});
    }
};

namespace GUI {
UpdApp &wxGetApp()
{
    static UpdApp app;
    app.app_config = Core::get().app_config.get();
    return app;
}
} // namespace GUI
using GUI::wxGetApp;

#include "slic3r/GUI/CheckNewVersion.inc"

json check(const json &a)
{
    g_version = a.value("version", "");
    wxGetApp().check_new_version_sf(true, a.value("by_user", 0));
    return nullptr;
}

// the dialog's buttons: Skip this Version (GUI_App::set_skip_version) and the stable-only check box
json set(const json &a)
{
    AppConfig *cfg = Core::get().app_config.get();
    if (a.value("skip", false)) cfg->set("skip_version", wxGetApp().version_info.version_str);
    if (a.contains("stable_only")) cfg->set_bool("check_stable_update_only", a["stable_only"].get<bool>());
    cfg->save();
    return nullptr;
}

}} // namespace orca::update

namespace orca {
static Commands reg({
    {"update.check", update::check},
    {"update.set", update::set},
});
} // namespace orca
