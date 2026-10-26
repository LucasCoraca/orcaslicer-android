// The Home page (WebViewPanel with resources/web/homepage): GUI_App::handle_web_request for its commands and
// MainFrame's recent projects list (add_to_recent_projects / get_recent_projects / remove_recent_project).
#include <ctime>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

// the home page's account scripts (GUI_App::get_login_info: build_login_cmd / build_logout_cmd), Cloud.cpp
std::vector<std::string> cloud_login_info_scripts();

namespace {

#include "slic3r/GUI/HomeStrings.inc"

std::vector<std::string> recent() { return Core::get().app_config->get_recent_projects(); }

std::string base64(const std::string &s)
{
    std::string out(boost::beast::detail::base64::encoded_size(s.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), s.data(), s.size()));
    return out;
}

// MainFrame::get_recent_projects, with the 3MF thumbnails (FileHistory::GetThumbnailUrl)
json recent_list()
{
    json list = json::array();
    for (const std::string &proj : recent()) {
        json item = {{"project_name", fs::path(proj).filename().string()}, {"path", proj}, {"published", bbs_3mf_is_published(proj) ? "1" : "0"}};
        boost::system::error_code ec;
        const std::time_t t = fs::last_write_time(proj, ec);
        if (!ec) {
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            item["time"] = buf;
            const std::string thumbnail = bbs_3mf_get_thumbnail(proj.c_str());
            if (!thumbnail.empty()) item["image"] = "data:image/png;base64," + base64(thumbnail);
        } else
            item["time"] = _u8L("File is missing");
        list.push_back(item);
    }
    return list;
}

std::string post(const json &msg) { return "window.postMessage(" + msg.dump() + ")"; }

std::string send_recent_list() { return post({{"sequence_id", ""}, {"command", "get_recent_projects"}, {"response", recent_list()}}); }

void set_recent(std::vector<std::string> list)
{
    Core::get().app_config->set_recent_projects(list);
    Core::get().app_config->save();
}

// GUI_App::handle_web_request for the home page
json home_message(const json &a)
{
    json msg = json::parse(a.at("message").get<std::string>(), nullptr, false);
    if (msg.is_discarded()) return {{"scripts", json::array()}};
    const std::string cmd = msg.value("command", std::string());
    json scripts = json::array(), out = json::object();
    const std::string path = msg.contains("data") ? msg["data"].value("path", std::string()) : std::string();
    if (cmd == "get_recent_projects") scripts.push_back(send_recent_list());
    else if (cmd == "homepage_newproject") out["action"] = "new";
    else if (cmd == "homepage_openproject") out["action"] = "open";
    else if (cmd == "homepage_open_recentfile") {
        // MainFrame::open_recent_project: a file that is gone leaves the list
        if (fs::exists(path)) { out["action"] = "open_recent"; out["path"] = path; }
        else {
            out["notice"] = _u8L("The project is no longer available.");
            std::vector<std::string> l = recent();
            l.erase(std::remove(l.begin(), l.end(), path), l.end());
            set_recent(l);
            scripts.push_back(send_recent_list());
        }
    } else if (cmd == "homepage_delete_recentfile") {
        std::vector<std::string> l = recent();
        l.erase(std::remove(l.begin(), l.end(), path), l.end());
        set_recent(l);
        scripts.push_back(send_recent_list());
    } else if (cmd == "homepage_delete_all_recentfile") {
        set_recent({});
        scripts.push_back(send_recent_list());
    } else if (cmd == "userguide_wiki_open") {
        const std::string page = msg.contains("data") ? msg["data"].value("guide_page", std::string()) : std::string();
        out["url"] = "https://www.orcaslicer.com/wiki/" + page;
    }
    // the OrcaCloud account row (GUI_App::handle_web_request): the login page, sign out, the account info
    else if (cmd == "homepage_login_or_register" || cmd == "homepage_orca_login_or_register") {
        if (Core::get().app_config->get_stealth_mode()) out["notice"] = _u8L("Stealth mode is enabled. Login is disabled.");
        else out["action"] = "login";
    } else if (cmd == "homepage_logout" || cmd == "homepage_orca_logout") {
        Core::get().call("cloud.logout", json::object());
    } else if (cmd == "get_login_info" || cmd == "get_orca_login_info") {
        // WebViewPanel::OnDocumentLoaded: the providers first (SendCloudProvidersInfo), then the account
        json providers = json::array();
        if (!Core::get().app_config->get_hide_login_side_panel())
            for (const std::string &p : Core::get().app_config->get_cloud_providers())
                if (p == "orca") providers.push_back(p);   // Bambu Cloud needs Bambu's network plug-in: not on Android
        scripts.push_back(post({{"command", "cloud_providers_info"}, {"data", {{"providers", providers}}}}));
        for (const std::string &s : cloud_login_info_scripts()) scripts.push_back(s);
    }
    // the OrcaCloud pane beside the page (GUI_App::open_orca_cloud / WebViewPanel::set_cloud_mode)
    else if (cmd == "homepage_open_cloud") out["action"] = "open_cloud";
    else if (cmd == "homepage_close_cloud") out["action"] = "close_cloud";
    // model mall, plug-in download: not on Android
    out["scripts"] = scripts;
    return out;
}

} // namespace

// MainFrame::add_to_recent_projects (Plater::load_project / save_project)
void add_to_recent_projects(const std::string &filename)
{
    if (!fs::exists(filename)) return;
    std::vector<std::string> l = recent();
    l.erase(std::remove(l.begin(), l.end(), filename), l.end());
    l.insert(l.begin(), filename);
    int max_count = 18;
    try { max_count = std::stoi(Core::get().app_config->get("max_recent_count")); } catch (...) {}
    if (int(l.size()) > max_count) l.resize(std::max(0, max_count));
    set_recent(l);
}

static Commands reg({
    // WebViewPanel: the page with its strings (AddUserScript) and the language
    {"home.open", [](const json &) -> json {
        std::string url = "file://" + (fs::path(resources_dir()) / "web/homepage/index.html").string();
        const std::string lang = Core::get().app_config->get("language");
        if (!lang.empty()) url += "?lang=" + lang;
        return {{"url", url}, {"script", "window.ORCA_UI_STRINGS = " + home_page_strings().dump(-1, ' ', false, json::error_handler_t::ignore) + ";"}};
    }},
    {"home.message", home_message},
    {"home.recent", [](const json &) -> json { return recent_list(); }},
});

} // namespace orca
