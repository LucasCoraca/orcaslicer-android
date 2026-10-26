// The cloud print hosts' sign-in (PhysicalPrinterDialog's Test on a cloud host): SimplyPrint's OAuthDialog (the job's
// local callback server and token request compiled from upstream, the system browser, "Authorizing..." with Cancel),
// Obico's PrinterCloudAuthDialog (its login page in a web dialog that posts the token) and 3DPrinterOS, whose client
// is compiled from upstream here with its token login wait and upload options dialog as the app's.
#include "CloudHosts.hpp"

#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Jobs/OAuthJob.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/3DPrinterOS.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/SimplyPrint.hpp"
#include "wx/arrstr.h"
#ifndef wxNOT_FOUND
#define wxNOT_FOUND (-1)
#endif
#include "wx/msgdlg.h"
#include "wx/utils.h"

namespace pt = boost::property_tree;

namespace orca {

// the "Authorizing..." dialog with Cancel (OAuthDialog / TokenAuthDialog): open while the wait runs
static std::atomic<bool> g_auth_canceled{false};

struct AuthWait
{
    explicit AuthWait(const std::string &title)
    {
        g_auth_canceled = false;
        Host::event("authorizing", {{"open", true}, {"title", title}, {"text", _u8L("Authorizing...")}});
    }
    ~AuthWait() { Host::event("authorizing", {{"open", false}}); }
    bool canceled() const { return g_auth_canceled.load(); }
};

// OAuthDialog::Show: the job's callback server, the login page in the browser, then the result (or "User canceled.")
Slic3r::GUI::OAuthResult simplyprint_oauth(const Slic3r::GUI::OAuthParams &params)
{
    using namespace Slic3r::GUI;
    AuthWait wait(_u8L("Login"));
    auto result = std::make_shared<OAuthResult>();
    OAuthJob job(OAuthData{params, result});
    wxLaunchDefaultBrowser(params.login_url);
    OAuthJob::Ctl ctl{[&wait] { return wait.canceled(); }};
    job.process(ctl);
    job.finalize();
    return *result;
}

// PrinterCloudAuthDialog: the host's login page; its "login_token" message carries the API key
std::string cloud_auth_api_key(Slic3r::PrintHost *host)
{
    wxString url;
    host->get_login_url(url);
    BOOST_LOG_TRIVIAL(info) << "login url = " << url.ToStdString();
    const json r = Host::ask({{"kind", "cloud_auth"}, {"url", url.ToStdString()}, {"title", "Login"}});
    return r.value("token", std::string());
}

static Commands reg({
    {"printhost.auth_cancel", [](const json &) -> json { g_auth_canceled = true; return nullptr; }},
});

} // namespace orca

// ---- 3DPrinterOS (3DPrinterOS.cpp) with its dialogs as the app's
namespace {

using namespace Slic3r;

// UploadOptionsDialog: single file or a cloud project, and the cloud printer type (the preset's model preselected)
class UploadOptionsDialog
{
public:
    UploadOptionsDialog(void *, const wxArrayString &cloud_projects, const wxArrayString &cloud_printer_types, const wxString preset_name)
    {
        orca::json projects = orca::json::array(), types = orca::json::array();
        for (size_t i = 0; i < cloud_projects.size(); ++i) projects.push_back(cloud_projects[i].ToStdString());
        int selected = -1;
        for (size_t i = 0; i < cloud_printer_types.size(); ++i) {
            types.push_back(cloud_printer_types[i].ToStdString());
            if (selected < 0 && cloud_printer_types[i].Find(preset_name) != wxNOT_FOUND) selected = int(i);
        }
        if (selected < 0 && cloud_printer_types.size() == 1) selected = 0;
        m_request = {{"kind", "3dprinteros_upload"},
                     {"title", _u8L("3DPrinterOS Cloud upload options")},
                     {"projects", projects},
                     {"printer_types", types},
                     {"selected", selected},
                     // shown when the preset's printer has no single match
                     {"warning", cloud_printer_types.size() > 1 ? _u8L("Printer type not found, please select manually.") : std::string()},
                     {"single", _u8L("Single file")},
                     {"project_file", _u8L("Project File")},
                     {"project_label", _u8L("Project:")},
                     {"printer_label", _u8L("Printer type:")}};
    }
    int ShowModal()
    {
        const orca::json r = orca::Host::ask(m_request);
        if (!r.value("ok", false)) return wxID_CANCEL;
        m_project = r.value("project", std::string());
        m_printer_type = r.value("printer_type", std::string());
        return wxID_OK;
    }
    void GetValues(std::string &project, std::string &printer_type)
    {
        project = m_project;
        printer_type = m_printer_type;
    }

private:
    orca::json  m_request;
    std::string m_project, m_printer_type;
};

// TokenAuthDialog: the login_with_token poll (up to 10 tries, 500 ms apart) while "Authorizing..." shows, with Cancel
class TokenAuthDialog
{
public:
    TokenAuthDialog(void *, const std::string &url, const std::string &token, const std::string &cafile, pt::ptree &resp)
        : m_url(url), m_token(token), m_cafile(cafile), m_resp(resp) {}
    int ShowModal()
    {
        orca::AuthWait wait("3DPrinterOS");
        for (int attempt = 1; attempt <= m_max_retries; ++attempt) {
            if (wait.canceled()) return wxID_CANCEL;
            bool        done = false, retry = false;
            std::string postBody = "token=" + m_token;
            auto        http = Slic3r::Http::post(m_url);
            http.timeout_max(60);
            if (!m_cafile.empty()) http.ca_file(m_cafile);
            http.header("Content-Length", std::to_string(postBody.size()));
            http.set_post_body(postBody);
            http.on_error([&](std::string, std::string error, unsigned status) {
                    m_resp.put("result", false);
                    // TRN %1% is an HTTP status code.
                    m_resp.put("message", (status != 200) ? Slic3r::GUI::format(_u8L("HTTP error: %1%"), status) : error);
                    done = true;
                })
                .on_complete([&](std::string body, unsigned status) {
                    if (status != 200) {
                        m_resp.put("result", false);
                        m_resp.put("message", Slic3r::GUI::format(_u8L("HTTP error: %1%"), status));
                        done = true;
                        return;
                    }
                    try {
                        std::stringstream ss(body);
                        pt::read_json(ss, m_resp);
                    } catch (...) {
                        m_resp.put("result", false);
                        m_resp.put("message", _u8L("Could not parse server response."));
                    }
                    if (m_resp.get<bool>("result", false) && m_resp.get_optional<std::string>("message.session").has_value()) done = true;
                    else if (m_resp.get<bool>("result", false)) retry = true;
                    else done = true;
                })
                .perform_sync();
            if (done) return wxID_OK;
            if (!retry) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(m_retry_delay_ms));
        }
        m_resp.put("result", false);
        m_resp.put("message", _u8L("Maximum login retries exceeded"));
        return wxID_ABORT;
    }

private:
    std::string m_url, m_token, m_cafile;
    pt::ptree  &m_resp;
    const int   m_max_retries{10};
    const int   m_retry_delay_ms{500};
};

} // namespace

namespace Slic3r {

static const std::string API_CREDENTIALS_PATH = "3dprinteros_api_cred.json";

#include "slic3r/GUI/C3DPrinterOS.inc"

} // namespace Slic3r
