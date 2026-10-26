// Device tab and print hosts: the printer's web UI (MainFrame::load_printer_url), the connection settings
// (PhysicalPrinterDialog), "Print" for third-party printers (Plater::send_gcode_legacy + PrintHostSendDialog) and the
// upload queue (PrintHostJobQueue with PrintHostQueueDialog's events). The print host clients are upstream's
// slic3r/Utils files compiled unchanged (CMakeLists.txt).
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include "wx/arrstr.h"
#include "wx/utils.h"
#include "slic3r/GUI/Jobs/OAuthJob.hpp"

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Slicer.hpp"
#include "SendDialogs.hpp"
#include "slic3r/Utils/ASCIIFolding.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/format.hpp"
#include "I18n.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"
#include "slic3r/Utils/3DPrinterOS.hpp"
#include "slic3r/Utils/PrintHost.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;
using namespace Slic3r::GUI;

wxEventType wxNewEventType()
{
    static std::atomic<int> next{10000};
    return ++next;
}

namespace Slic3r { namespace GUI {

const wxEventType EVT_PRINTHOST_PROGRESS = wxNewEventType();
const wxEventType EVT_PRINTHOST_ERROR    = wxNewEventType();
const wxEventType EVT_PRINTHOST_CANCEL   = wxNewEventType();
const wxEventType EVT_PRINTHOST_INFO     = wxNewEventType();

wxString GUI_App::current_language_code() const
{
    const std::string &l = orca::current_language();
    return l.empty() ? wxString("en") : wxString(l.substr(0, 2));
}

void MainFrame::request_select_tab(const std::string &name) { orca::Host::event("select_tab", {{"tab", name}}); }
void MainFrame::load_printer_url(const std::string &url, const std::string &apikey)
{
    orca::Host::event("printer_url", {{"url", url}, {"apikey", apikey}});
}

#include "slic3r/GUI/OAuthJobFns.inc"

}} // namespace Slic3r::GUI

bool wxLaunchDefaultBrowser(const wxString &url, int)
{
    orca::Host::event("open_url", {{"url", url}});
    return true;
}


namespace Slic3r { namespace GUI {

}} // namespace Slic3r::GUI

namespace orca {

void export_3mf_file(const std::string &path, SaveStrategy strategy, int export_plate_idx);

// ---- the upload queue: PrintHostJobQueue (upstream) reporting to PrintHostQueueDialog, whose job list and the
// upload notifications are the app's
struct UploadJob
{
    std::string host, file, size, status, error;
    int         progress{0};
    std::string state{"queued"};   // queued / progress / completed / error / cancelled
};
static std::mutex             g_jobs_mutex;
static std::vector<UploadJob> g_jobs;

static json jobs_json()
{
    json a = json::array();
    for (size_t i = 0; i < g_jobs.size(); ++i) {
        const UploadJob &j = g_jobs[i];
        a.push_back({{"id", i}, {"host", j.host}, {"file", j.file}, {"size", j.size}, {"status", j.status}, {"error", j.error},
                     {"progress", j.progress}, {"state", j.state}});
    }
    return a;
}

static void jobs_changed(size_t id)
{
    json e = {{"jobs", jobs_json()}, {"id", id}};
    Host::event("upload_queue", e);
}

static PrintHostQueueDialog &queue_dialog()
{
    static PrintHostQueueDialog dlg;
    return dlg;
}

static PrintHostJobQueue &job_queue()
{
    static PrintHostJobQueue queue(&queue_dialog());
    return queue;
}

} // namespace orca

namespace Slic3r { namespace GUI {

// PrintHostQueueDialog::append_job
void PrintHostQueueDialog::append_job(const PrintHostJob &job)
{
    orca::UploadJob j;
    j.host   = job.printhost->get_host();
    j.file   = job.upload_data.upload_path.string();
    j.status = _u8L("Queued");
    boost::system::error_code ec;
    boost::uintmax_t size_i = boost::filesystem::file_size(job.upload_data.source_path, ec);
    std::stringstream stream;
    if (ec)
        stream << "unknown";
    else
        stream << std::fixed << std::setprecision(2) << ((float)size_i / 1024 / 1024) << "MB";
    j.size = stream.str();
    size_t id;
    {
        std::lock_guard<std::mutex> l(orca::g_jobs_mutex);
        orca::g_jobs.push_back(j);
        id = orca::g_jobs.size() - 1;
    }
    orca::jobs_changed(id);
}

// on_progress / on_error / on_cancel (on_info is commented out upstream)
void PrintHostQueueDialog::ProcessEvent(wxEvent &event)
{
    auto &evt = static_cast<Event &>(event);
    {
        std::lock_guard<std::mutex> l(orca::g_jobs_mutex);
        if (evt.job_id >= orca::g_jobs.size()) return;
        orca::UploadJob &j = orca::g_jobs[evt.job_id];
        if (evt.GetEventType() == EVT_PRINTHOST_PROGRESS) {
            if (evt.progress < 100) {
                j.state    = "progress";
                j.status   = _u8L("Uploading");
                j.progress = evt.progress;
            } else {
                j.state    = "completed";
                j.status   = _u8L("Completed");
                j.progress = 100;
            }
        } else if (evt.GetEventType() == EVT_PRINTHOST_ERROR) {
            j.state    = "error";
            j.status   = _u8L("Error");
            j.progress = 0;
            j.error    = format_wxstr("%1%\n%2%", _L("Error uploading to print host") + ":", evt.status);
        } else if (evt.GetEventType() == EVT_PRINTHOST_CANCEL) {
            j.state    = "cancelled";
            j.status   = _u8L("Canceled");
            j.progress = 0;
        } else
            return;
    }
    orca::jobs_changed(evt.job_id);
}

}} // namespace Slic3r::GUI

namespace orca {

// MainFrame::load_printer_url: the web UI of the printer's host for the Device tab
static json device_state(const json &)
{
    Core &core = Core::get();
    PresetBundle &preset_bundle = *core.preset_bundle;
    auto cfg = preset_bundle.printers.get_edited_preset().config;
    std::string url = PrintHost::get_print_host_webui(&cfg);
    std::string apikey;
    const auto host_type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
    if (cfg.has("printhost_apikey") && host_type != htSimplyPrint)
        apikey = cfg.opt_string("printhost_apikey");

    // MainFrame::available_print_actions for third-party printers (the Bambu network actions are not part of the app)
    json actions = json::array();
    actions.push_back({{"key", "send_gcode"}, {"label", _u8L_CONTEXT("Print", "Verb")}});
    const auto *use_3mf_opt = cfg.option<ConfigOptionBool>("use_3mf");
    if (use_3mf_opt != nullptr && use_3mf_opt->value)
        actions.push_back({{"key", "export_sliced_file"}, {"label", _u8L("Export plate sliced file")}});
    actions.push_back({{"key", "export_gcode"}, {"label", _u8L("Export G-code file")}});
    std::lock_guard<std::mutex> l(g_jobs_mutex);
    return {{"url", url}, {"apikey", apikey}, {"actions", actions}, {"jobs", jobs_json()},
            {"no_url", _u8L("Please set the printer's host in the connection settings (the button next to the printer) to see its web interface here.")}};
}

// Plater::send_gcode_legacy, up to the dialog: the default name, Repetier's groups, PrusaLink's storages. The host
// queries run in a thread (the desktop blocks with a busy cursor); the dialog's data comes as "printhost_send".
static json send_prepare(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    DynamicPrintConfig *physical_printer_config = &core.preset_bundle->printers.get_edited_preset().config;
    if (p.model().objects.empty()) return nullptr;
    auto printhost = std::shared_ptr<PrintHost>(PrintHost::get_print_host(physical_printer_config));
    if (!printhost) return nullptr;

    const int  plate   = a.value("plate", p.current_plate);
    const auto *use_3mf_opt = physical_printer_config->option<ConfigOptionBool>("use_3mf");
    const bool use_3mf = use_3mf_opt != nullptr && use_3mf_opt->value;
    PlateSlice *ps = Slicer::get().slice(plate);
    if (!ps || !ps->valid || !ps->print) throw CommandError(_u8L("The plate is not sliced"));

    // output_filepath_for_project: the project name through the filename format
    fs::path default_output_file;
    try {
        const std::string project = p.project_name.empty() ? std::string("Untitled") : p.project_name;
        default_output_file = ps->print->output_filepath("", project);
    } catch (const std::exception &ex) {
        throw CommandError(ex.what());
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.filename().string()));
    if (use_3mf) default_output_file.replace_extension(".gcode.3mf");

    AppConfig *app_config = core.app_config.get();
    const std::string recent_path0 = app_config->get("recent", "printhost_path");
    const std::string recent_group = app_config->get("recent", "printhost_group");
    const std::string recent_storage = app_config->get("recent", "printhost_storage");
    const bool switch_tab = app_config->get_bool("open_device_tab_post_upload");
    const PrintHostPostUploadActions post_actions = printhost->get_post_upload_actions();
    const std::string host_name = printhost->get_name();
    const SendExtrasInput extras_in = send_extras_input(physical_printer_config, plate);

    std::thread([printhost, default_output_file, recent_path0, recent_group, recent_storage, switch_tab, post_actions, host_name, plate, extras_in]() {
        json e;
        try {
            wxArrayString groups;
            printhost->get_groups(groups);
            wxArrayString storage_paths, storage_names;
            printhost->get_storage(storage_paths, storage_names);

            // PrintHostSendDialog::init
            std::string recent_path = recent_path0;
            if (!recent_path.empty() && recent_path.back() != '/') recent_path += '/';
            recent_path += default_output_file.filename().string();
            std::string valid_suffix;
            if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
                valid_suffix = recent_path.substr(extension_start);
            e = {{"plate", plate},
                 {"title", _u8L("Send G-code to printer host")},
                 {"message", _u8L("Upload to Printer Host with the following filename:")},
                 {"filename", recent_path},
                 {"dir_hint", _u8L("Use forward slashes ( / ) as a directory separator if needed.")},
                 {"valid_suffix", valid_suffix},
                 {"suffix_warning", Slic3r::format(_u8L("Upload filename doesn't end with \"%s\". Do you wish to continue?"), valid_suffix)},
                 {"switch_label", _u8L("Switch to Device tab after upload.")},
                 {"switch", switch_tab},
                 {"upload", _u8L("Upload")},
                 {"cancel", _u8L("Cancel")}};
            if (!groups.empty()) {
                e["group_label"] = _u8L("Group");
                e["groups"]      = json(std::vector<std::string>(groups.begin(), groups.end()));
                e["group"]       = recent_group.empty() ? std::string(groups.front()) : recent_group;
            }
            if (storage_names.size() > 1) {
                e["storage_label"] = _u8L("Upload to storage") + ":";
                e["storage_names"] = json(std::vector<std::string>(storage_names.begin(), storage_names.end()));
                e["storage_paths"] = json(std::vector<std::string>(storage_paths.begin(), storage_paths.end()));
                e["storage"]       = recent_storage.empty() ? std::string(storage_names.front()) : recent_storage;
            } else if (storage_names.size() == 1) {
                e["storage_info"]   = _u8L("Upload to storage") + ": " + std::string(storage_names.front());
                e["storage_preset"] = std::string(storage_paths.front());
            }
            if (post_actions.has(PrintHostPostUploadAction::StartPrint)) e["upload_print"] = _u8L("Upload and Print");
            if (host_name == "PrusaConnect") e["confirm_print"] = _u8L("Is the printer ready? Is the print sheet in place, empty and clean?");
            // Elegoo / Creality / Flashforge: their dialog's own options
            const json extras = send_extras(extras_in, printhost.get());
            if (!extras.empty()) e["extras"] = extras;
        } catch (const std::exception &ex) {
            e = {{"error", ex.what()}};
        }
        Host::event("printhost_send", e);
    }).detach();
    return {{"started", true}};
}

// the dialog's OK (PrintHostSendDialog::EndModal + the rest of send_gcode_legacy) and
// BackgroundSlicingProcess::prepare_upload: a copy of the plate's G-code (or the plate's .gcode.3mf) is queued
static json send(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    DynamicPrintConfig *physical_printer_config = &core.preset_bundle->printers.get_edited_preset().config;
    PrintHostJob upload_job(physical_printer_config);
    if (upload_job.empty()) return nullptr;
    const auto *use_3mf_opt = physical_printer_config->option<ConfigOptionBool>("use_3mf");
    const bool  use_3mf = use_3mf_opt != nullptr && use_3mf_opt->value;
    const int   plate = a.value("plate", p.current_plate);
    PlateSlice *ps = Slicer::get().slice(plate);
    if (!ps || !ps->valid || !ps->print) throw CommandError(_u8L("The plate is not sliced"));

    const std::string filename = str_arg(a, "filename");
    // PrintHostSendDialog::EndModal: the recent folder, group and storage
    AppConfig *app_config = core.app_config.get();
    {
        std::string path = filename;
        const size_t last_slash = path.rfind('/');
        path = last_slash == std::string::npos ? std::string() : path.substr(0, last_slash + 1);
        app_config->set("recent", "printhost_path", path);
        if (a.contains("group")) app_config->set("recent", "printhost_group", a.value("group", ""));
        if (a.contains("storage_name")) app_config->set("recent", "printhost_storage", a.value("storage_name", ""));
    }
    const bool switch_tab = a.value("switch", false);
    app_config->set_bool("open_device_tab_post_upload", switch_tab);
    upload_job.switch_to_device_tab    = switch_tab;
    upload_job.upload_data.upload_path = filename;
    const std::string action = a.value("action", "upload");
    upload_job.upload_data.post_action = action == "print" ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;
    upload_job.upload_data.group       = a.value("group", "");
    upload_job.upload_data.storage     = a.value("storage", "");
    upload_job.upload_data.use_3mf     = use_3mf;
    bool start_print = upload_job.upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    send_extras_apply(a, upload_job.upload_data.extended_info, start_print);
    upload_job.upload_data.post_action = start_print ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;
    if (use_3mf) upload_job.upload_data.extended_info["plateindex"] = std::to_string(plate + 1);

    fs::path source_path = fs::path(temporary_dir()) / fs::unique_path("." SLIC3R_APP_KEY ".upload.%%%%-%%%%-%%%%-%%%%");
    if (use_3mf) {
        // Plater::send_gcode: the plate as a .gcode.3mf
        source_path.replace_extension(".3mf");
        export_3mf_file(source_path.string(), SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithGcode | SaveStrategy::SkipAuxiliary, plate);
    } else {
        std::string error_message;
        if (copy_file(ps->gcode_path, source_path.string(), error_message) != SUCCESS)
            throw CommandError(_u8L("Copying of the temporary G-code to the output G-code failed."));
        upload_job.upload_data.upload_path = ps->print->print_statistics().finalize_output_path(upload_job.upload_data.upload_path.string());
    }
    upload_job.upload_data.source_path = source_path;
    const std::string host = upload_job.printhost->get_host();
    job_queue().enqueue(std::move(upload_job));
    return {{"status", (boost::format(_utf8(L("Scheduling upload to `%1%`. See Window -> Print Host Upload Queue"))) % host).str()}};
}

static json upload_cancel(const json &a)
{
    job_queue().cancel(a.at("id").get<size_t>());
    return nullptr;
}

static json upload_jobs(const json &)
{
    std::lock_guard<std::mutex> l(g_jobs_mutex);
    return jobs_json();
}

static Commands reg({
    {"device.state", device_state},
    {"printhost.send_prepare", send_prepare},
    {"printhost.send", send},
    {"upload.cancel", upload_cancel},
    {"upload.jobs", upload_jobs},
});

} // namespace orca
