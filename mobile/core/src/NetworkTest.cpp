// Help > Network Test (NetworkTestDialog): the dialog's tests are compiled from upstream on a stand-in dialog whose
// update_status sends the result labels and log lines to the app as "network_test" events.
#include <atomic>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <sys/system_properties.h>
#include <sys/utsname.h>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/Http.hpp"
#include "wx/string.h"
#include "wx/utils.h"

// wxGetOsDescription / wxGetOsVersion of wxUniv on Linux: uname's system, release and machine
wxString wxGetOsDescription()
{
    char release[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.release", release);
    __system_property_get("ro.build.version.sdk", sdk);
    utsname u{};
    uname(&u);
    return wxString::Format("Android %s (API %s), %s %s %s ", release, sdk, u.sysname, u.release, u.machine);
}
int wxGetOsVersion(int *major, int *minor, int *micro)
{
    utsname u{};
    uname(&u);
    int a = 0, b = 0, c = 0;
    std::sscanf(u.release, "%d.%d.%d", &a, &b, &c);
    if (major) *major = a;
    if (minor) *minor = b;
    if (micro) *micro = c;
    return 0;
}

namespace orca { namespace nettest {
using namespace Slic3r;

#include "slic3r/GUI/NetworkTestTypes.inc"

static wxString NA_STR = _L("N/A");

struct NetworkTestDialog
{
    boost::thread    *test_job[TEST_JOB_MAX]{};
    boost::thread    *m_sequence_job{nullptr};
    std::atomic<bool> m_in_testing[TEST_JOB_MAX]{};
    bool              m_download_cancel = false;
    std::atomic<bool> m_closing{false};

    wxString get_studio_version();
    wxString get_os_info();
    wxString get_dns_info();
    void     start_all_job();
    void     start_all_job_sequence();
    void     start_test_bing_thread();
    void     start_test_github_thread();
    void     start_test_ping_thread();
    void     start_test_url(TestJob job, wxString name, wxString url);

    // EVT_UPDATE_RESULT's handler (init_bind): the job's result label, and the line of the log with its time
    void update_status(int job_id, wxString info)
    {
        std::time_t       t        = std::time(0);
        std::tm          *now_time = std::localtime(&t);
        std::stringstream buf;
        buf << std::put_time(now_time, "%a %b %d %H:%M:%S");
        Host::event("network_test", {{"job", job_id}, {"text", info.ToStdString()}, {"log", buf.str() + ":" + info.ToStdString() + "\n"}});
    }

    // on_close: the running tests are waited for
    void join()
    {
        for (int i = 0; i < TEST_JOB_MAX; i++)
            if (test_job[i]) {
                test_job[i]->join();
                delete test_job[i];
                test_job[i] = nullptr;
            }
        if (m_sequence_job) {
            m_sequence_job->join();
            delete m_sequence_job;
            m_sequence_job = nullptr;
        }
    }
};

#include "slic3r/GUI/NetworkTest.inc"

NetworkTestDialog &dialog()
{
    static NetworkTestDialog d;
    return d;
}

// the dialog opens: set_default and the static texts of the sizers
json open(const json &)
{
    NetworkTestDialog &d = dialog();
    d.m_closing = false;
    d.join();
    for (auto &b : d.m_in_testing) b = false;
    d.m_download_cancel = false;
    return {{"title", _u8L("Network Test")},
            {"start", _u8L("Start Test Multi-Thread")},
            {"start_sequence", _u8L("Start Test Single-Thread")},
            {"basic_info", _u8L("Basic Info")},
            {"version_title", _u8L("OrcaSlicer Version:")},
            {"version", d.get_studio_version().ToStdString()},
            {"sys_title", _u8L("System Version:")},
            {"sys", d.get_os_info().ToStdString()},
            {"tests", {{{"job", int(TEST_ORCA_JOB)}, {"button", _u8L("Test OrcaSlicer (GitHub)")}, {"title", _u8L("Test OrcaSlicer (GitHub):")}},
                       {{"job", int(TEST_BING_JOB)}, {"button", _u8L("Test bing.com")}, {"title", _u8L("Test bing.com:")}}}},
            {"na", NA_STR.ToStdString()},
            {"log_title", _u8L("Log Info")}};
}

json start(const json &a)
{
    NetworkTestDialog &d = dialog();
    const std::string what = a.value("what", "all");
    // a finished thread of the same job is let go before the next one replaces it
    auto reap = [&d](int job) {
        if (d.test_job[job] && !d.m_in_testing[job]) { d.test_job[job]->join(); delete d.test_job[job]; d.test_job[job] = nullptr; }
    };
    if (what == "all" || what == "github") reap(TEST_ORCA_JOB);
    if (what == "all" || what == "bing") reap(TEST_BING_JOB);
    if (what == "all") d.start_all_job();
    else if (what == "sequence") {
        if (d.m_sequence_job) { d.m_sequence_job->join(); delete d.m_sequence_job; }
        d.start_all_job_sequence();
    } else if (what == "github") d.start_test_github_thread();
    else if (what == "bing") d.start_test_bing_thread();
    return nullptr;
}

json close(const json &)
{
    dialog().m_download_cancel = true;
    dialog().m_closing = true;
    return nullptr;
}

}} // namespace orca::nettest

namespace orca {
static Commands reg({
    {"nettest.open", nettest::open},
    {"nettest.start", nettest::start},
    {"nettest.close", nettest::close},
});
} // namespace orca
