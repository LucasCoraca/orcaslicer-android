// The printer connection dialog (Sidebar's "Connection" button: PhysicalPrinterDialog). Upstream's field logic
// (update / update_host_type / update_printhost_buttons / update_preset_input / ...) is compiled unchanged on stand-ins
// for its option group, fields and buttons that record what the dialog shows; the app draws them (ui/PrinterHostDialog.kt).
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <thread>
#include <boost/algorithm/string.hpp>
#include <boost/any.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "wx/arrstr.h"
#include "slic3r/Utils/3DPrinterOS.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/SimplyPrint.hpp"
#include "CloudHosts.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace orca {

json presets_state();
void presets_changed();

namespace pp {

// ---- stand-ins for the dialog's widgets: they keep what the app shows
struct wxWindow { virtual ~wxWindow() = default; };
struct wxTextCtrl : wxWindow
{
    wxString value;
    wxString GetValue() const { return value; }
    void     SetValue(const wxString &v) { value = v; }
};
struct TextInput : wxWindow
{
    wxTextCtrl *ctrl{nullptr};
    wxTextCtrl *GetTextCtrl() { return ctrl; }
};
struct wxStaticText
{
    wxString label;
    bool     shown{true};
    void SetLabel(const wxString &l) { label = l; }
    void Show(bool s = true) { shown = s; }
};
struct RoundedRectangle { void Refresh() {} };
struct Button
{
    wxString label;
    bool     shown{true}, enabled{true};
    void Enable(bool e = true) { enabled = e; }
    void Disable() { enabled = false; }
    void Show(bool s = true) { shown = s; }
    void SetLabel(const wxString &l) { label = l; }
};

struct Field
{
    std::string     key;
    ConfigOptionDef m_opt;
    bool            shown{true}, enabled{true};
    virtual ~Field() = default;
    void       enable() { enabled = true; }
    void       disable() { enabled = false; }
    void       field_changed() {}
    virtual wxWindow *getWindow() { return nullptr; }
    virtual json value() const = 0;
};
struct TextCtrl : Field
{
    wxTextCtrl ctrl;
    TextInput  input;
    TextCtrl() { input.ctrl = &ctrl; }
    wxTextCtrl *text_ctrl() { return &ctrl; }
    wxWindow   *getWindow() override { return &input; }
    void        propagate_value() {}
    json        value() const override { return ctrl.value; }
};
struct Choice : Field
{
    std::vector<std::string> values;
    int                      selection{-1};
    void set_values(const wxArrayString &v) { values.assign(v.begin(), v.end()); }
    void set_values(const std::vector<std::string> &v) { values = v; }
    void set_value(int i) { selection = i; }
    void set_selection() { selection = values.empty() ? -1 : 0; }
    json value() const override { return selection; }
};
struct CheckBox : Field
{
    bool       checked{false};
    void       set_value(bool v) { checked = v; }
    boost::any get_value() const { return checked; }
    json       value() const override { return checked; }
};

// ConfigOptionsGroup: the fields of the host options and the dialog's own check box
struct ConfigOptionsGroup
{
    DynamicPrintConfig                           *config{nullptr};
    std::vector<std::string>                      order;
    std::map<std::string, std::unique_ptr<Field>> fields;

    Field *add(const std::string &key, Field *f, const ConfigOptionDef *def)
    {
        f->key = key;
        if (def) f->m_opt = *def;
        fields[key].reset(f);
        order.push_back(key);
        return f;
    }
    Field *get_field(const std::string &key) { auto it = fields.find(key); return it == fields.end() ? nullptr : it->second.get(); }
    void   show_field(const std::string &key, bool show = true) { if (Field *f = get_field(key)) f->shown = show; }
    void   hide_field(const std::string &key) { show_field(key, false); }
    void   enable_field(const std::string &key, bool en = true) { if (Field *f = get_field(key)) f->enabled = en; }
    void   disable_field(const std::string &key) { enable_field(key, false); }
    void   set_value(const std::string &key, int v, bool) { if (auto *c = dynamic_cast<Choice *>(get_field(key))) c->set_value(v); }
    void   set_value(const std::string &key, const wxString &v, bool) { if (auto *t = dynamic_cast<TextCtrl *>(get_field(key))) t->ctrl.SetValue(v); }
    // OptionsGroup::reload_config: the fields show the config's values
    void reload_config()
    {
        for (auto &[key, f] : fields) {
            if (!config->has(key)) continue;
            if (auto *t = dynamic_cast<TextCtrl *>(f.get())) t->ctrl.value = config->opt_serialize(key);
            else if (auto *c = dynamic_cast<CheckBox *>(f.get())) c->checked = config->opt_bool(key);
            else if (auto *ch = dynamic_cast<Choice *>(f.get()); ch && key != "printhost_port") ch->selection = config->option(key)->getInt();
        }
    }
};

struct wxBusyCursor {};
static const wxString dots("...");
inline void show_error(void *, const wxString &msg) { MessageDialog(nullptr, msg, _L("Error"), wxOK | wxICON_ERROR).ShowModal(); }

struct PhysicalPrinterDialog
{
    enum ValidationType { Valid, NoValid, Warning };

    DynamicPrintConfig *m_config{nullptr};
    ConfigOptionsGroup  group;
    ConfigOptionsGroup *m_optgroup{&group};
    Button              browse, test, generate_creds, logout, cafile_browse, port_browse, ok;
    Button             *m_printhost_browse_btn{&browse}, *m_printhost_test_btn{&test}, *m_printhost_generate_creds_btn{&generate_creds},
                       *m_printhost_logout_btn{&logout}, *m_printhost_cafile_browse_btn{&cafile_browse},
                       *m_printhost_port_browse_btn{&port_browse}, *btnOK{&ok};
    RoundedRectangle    area;
    RoundedRectangle   *m_input_area{&area};
    wxStaticText        valid_label;
    wxStaticText       *m_valid_label{&valid_label};
    wxTextCtrl          input;
    wxTextCtrl         *m_input_ctrl{&input};
    PresetCollection   *m_presets{nullptr};
    ValidationType      m_valid_type{Valid};
    std::string         m_preset_name;

    void update(bool printer_change = false);
    void update_host_type(bool printer_change);
    void update_printers();
    void update_ports();
    void update_webui();
    void update_printhost_buttons();
    void update_preset_input();
    void check_host_key_valid();
    void SetSize(int) {}
    int  GetBestSize() { return 0; }
    void Layout() {}
};

#include "slic3r/GUI/PhysicalPrinterDialog.inc"

std::unique_ptr<PhysicalPrinterDialog> g_dialog;

// PhysicalPrinterDialog's constructor and build_printhost_settings, without the widgets
PhysicalPrinterDialog *open_dialog()
{
    Core &core = Core::get();
    auto  d    = std::make_unique<PhysicalPrinterDialog>();
    d->m_presets = &core.preset_bundle->printers;
    const Preset &sel_preset = d->m_presets->get_selected_preset();
    std::string   suffix     = _u8L_CONTEXT(L_CONTEXT("Copy", "PresetName"), "PresetName");
    d->input.value = sel_preset.is_default ? _u8L("Untitled") : sel_preset.is_system ? (boost::format(("%1% - %2%")) % sel_preset.name % suffix).str() : sel_preset.name;
    d->m_config      = &core.preset_bundle->printers.get_edited_preset().config;
    d->group.config  = d->m_config;
    d->check_host_key_valid();

    ConfigOptionsGroup &g   = d->group;
    const ConfigDef    &def = print_config_def;
    g.add("host_type", new Choice, def.get("host_type"));
    g.add("print_host", new TextCtrl, def.get("print_host"));
    g.add("print_host_webui", new TextCtrl, def.get("print_host_webui"));
    {
        ConfigOptionDef bbl;
        bbl.type    = coBool;
        bbl.label   = L("View print host webui in Device tab");
        bbl.tooltip = L("Replace the BambuLab's device tab with print host webui");
        g.add("bbl_use_print_host_webui", new CheckBox, &bbl);
    }
    g.add("printhost_authorization_type", new Choice, def.get("printhost_authorization_type"));
    g.add("printhost_apikey", new TextCtrl, def.get("printhost_apikey"));
    g.add("flashforge_serial_number", new TextCtrl, def.get("flashforge_serial_number"));
    g.add("printhost_port", new Choice, def.get("printhost_port"));
    if (Http::ca_file_supported()) g.add("printhost_cafile", new TextCtrl, def.get("printhost_cafile"));
    g.add("printhost_user", new TextCtrl, def.get("printhost_user"));
    g.add("printhost_password", new TextCtrl, def.get("printhost_password"));
    d->browse.label         = _L("Browse") + " " + dots;
    d->test.label           = _L("Test");
    d->generate_creds.label = _L("Generate API Key");
    d->logout.label         = _L("Log Out");
    d->cafile_browse.label  = _L("Browse") + " " + dots;
    d->port_browse.label    = _L("Refresh") + " " + dots;
    d->ok.label             = _L("OK");
    g.reload_config();
    {
        Choice *choice = dynamic_cast<Choice *>(g.get_field("printhost_port"));
        choice->set_values(std::vector<std::string>{d->m_config->opt_string("printhost_port")});
        choice->set_selection();
    }
    d->update();
    g_dialog = std::move(d);
    return g_dialog.get();
}

json dialog_state(PhysicalPrinterDialog &d)
{
    json fields = json::array();
    for (const std::string &key : d.group.order) {
        Field     *f = d.group.get_field(key);
        json       j = {{"key", key}, {"shown", f->shown}, {"enabled", f->enabled}, {"label", _u8L(f->m_opt.label.c_str())},
                        {"tooltip", _u8L(f->m_opt.tooltip.c_str())}, {"value", f->value()}};
        if (auto *c = dynamic_cast<Choice *>(f)) {
            json options = json::array();
            if (key == "printhost_port")
                for (auto &v : c->values) options.push_back(v);
            else
                for (auto &l : f->m_opt.enum_labels) options.push_back(_u8L(l.c_str()));
            j["kind"]    = "choice";
            j["options"] = options;
        } else
            j["kind"] = dynamic_cast<CheckBox *>(f) ? "check" : key == "printhost_password" ? "password" : "text";
        fields.push_back(j);
    }
    auto btn = [](const Button &b) { return json{{"label", b.label}, {"shown", b.shown}, {"enabled", b.enabled}}; };
    const std::string tab_title = _u8L("Printer");
    return {{"title", _u8L("Physical Printer")},
            {"name_label", (boost::format(_utf8(L("Save %s as"))) % tab_title).str()},
            {"name", d.input.value},
            {"valid", d.valid_label.shown ? d.valid_label.label : wxString()},
            {"valid_type", int(d.m_valid_type)},
            {"group", _u8L("Print Host upload")},
            {"fields", fields},
            {"ca_hint", _u8L("HTTPS CA file is optional. It is only needed if you use HTTPS with a self-signed certificate.")},
            {"buttons", {{"browse", btn(d.browse)}, {"test", btn(d.test)}, {"generate_creds", btn(d.generate_creds)},
                         {"logout", btn(d.logout)}, {"cafile_browse", btn(d.cafile_browse)}, {"port_browse", btn(d.port_browse)},
                         {"ok", btn(d.ok)}}}};
}

PhysicalPrinterDialog &dialog()
{
    if (!g_dialog) throw CommandError("the connection dialog is not open");
    return *g_dialog;
}

json open(const json &) { return dialog_state(*open_dialog()); }

// a field edited in the app: the config value, then build_printhost_settings' m_on_change
json set(const json &a)
{
    PhysicalPrinterDialog &d   = dialog();
    const std::string      key = str_arg(a, "key");
    if (key == "name") {
        d.input.value = a.at("value").get<std::string>();
        d.update();
        return dialog_state(d);
    }
    Field *f = d.group.get_field(key);
    if (!f) throw CommandError("unknown field " + key);
    if (auto *c = dynamic_cast<Choice *>(f)) {
        if (key == "printhost_port") {
            const int i = a.at("value").get<int>();
            c->selection = i;
            if (i >= 0 && i < int(c->values.size())) d.m_config->opt_string("printhost_port") = c->values[i];
        } else {
            c->selection = a.at("value").get<int>();
            d.m_config->option(key)->setInt(c->selection);
        }
    } else if (auto *cb = dynamic_cast<CheckBox *>(f)) {
        cb->checked = a.at("value").get<bool>();
        if (d.m_config->has(key)) d.m_config->set_key_value(key, new ConfigOptionBool(cb->checked));
    } else if (auto *t = dynamic_cast<TextCtrl *>(f)) {
        std::string v = a.at("value").get<std::string>();
        if (key == "print_host") boost::trim(v);   // the print_host text event trims spaces
        t->ctrl.value = v;
        d.m_config->opt_string(key) = v;
    }
    if (key == "host_type" || key == "printhost_authorization_type") d.update();
    if (key == "print_host") d.update_printhost_buttons();
    if (key == "printhost_port") d.update_ports();
    if (key == "bbl_use_print_host_webui") d.update_webui();
    presets_changed();
    return dialog_state(d);
}

// the Test button (blocking on the desktop with a wait cursor: here in a thread, the answer as an event)
json test(const json &)
{
    PhysicalPrinterDialog &d = dialog();
    std::shared_ptr<PrintHost> host(PrintHost::get_print_host(d.m_config));
    if (!host) throw CommandError(_u8L("Could not get a valid Printer Host reference"));
    std::thread([host]() {
        wxString msg;
        bool     result = false;
        try {
            result = host->test(msg);
        } catch (const std::exception &e) {
            msg = e.what();
        }
        // the cloud hosts sign in (OAuthDialog / 3DPrinterOS login / PrinterCloudAuthDialog)
        std::string api_key;
        bool        got_key = false;
        if (!result && host->is_cloud()) {
            try {
                if (const auto h = dynamic_cast<SimplyPrint *>(host.get()); h) {
                    const OAuthResult r = simplyprint_oauth(h->get_oauth_params());
                    result = r.success;
                    if (r.success) h->save_oauth_credential(r);
                    else msg = r.error_message;
                } else if (const auto h = dynamic_cast<C3DPrinterOS *>(host.get()); h) {
                    MessageDialog dlg(nullptr, _L("Valid session not detected. Proceed with login to 3DPrinterOS?"), _L("Proceed"),
                                      wxICON_INFORMATION | wxYES | wxNO);
                    if (dlg.ShowModal() == wxID_YES) result = h->login(msg);
                } else {
                    api_key = cloud_auth_api_key(host.get());
                    got_key = true;
                    result  = !api_key.empty();
                }
            } catch (const std::exception &e) {
                msg = e.what();
            }
        }
        // PrinterCloudAuthDialog's key goes into the dialog's printhost_apikey (on the engine thread)
        if (got_key)
            Core::call_after([api_key] {
                dialog().m_config->opt_string("printhost_apikey") = api_key;
                dialog().update();
                Host::event("physical_printer", dialog_state(dialog()));
            });
        Host::event("printhost_test", {{"ok", result}, {"title", result ? _u8L("Success!") : _u8L("Error")},
                                       {"message", result ? std::string(host->get_test_ok_msg()) : std::string(host->get_test_failed_msg(msg))}});
    }).detach();
    return {{"started", true}};
}

json log_out(const json &)
{
    PhysicalPrinterDialog &d = dialog();
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(d.m_config));
    if (!host) throw CommandError(_u8L("Could not get a valid Printer Host reference"));
    MessageDialog dialog(nullptr, _L("Are you sure to log out?"), "", wxICON_QUESTION | wxYES_NO);
    if (dialog.ShowModal() == wxID_YES) {
        host->log_out();
        d.update();
    }
    return dialog_state(d);
}

// Refresh printers (Repetier / Obico)
json refresh(const json &)
{
    PhysicalPrinterDialog &d = dialog();
    d.update_printers();
    return dialog_state(d);
}

// Browse: Bonjour lookup (BonjourDialog: services "octoprint" for FFF) or the Flashforge discovery
json browse(const json &)
{
    PhysicalPrinterDialog &d = dialog();
    const auto host_type = d.m_config->opt_enum<PrintHostType>("host_type");
    json found = json::array();
    if (host_type == htFlashforge) {
        std::vector<FlashforgeDiscoveredPrinter> printers;
        wxString                                 error_msg;
        if (!Flashforge::discover_printers(printers, error_msg)) throw CommandError(error_msg);
        for (const auto &printer : printers)
            found.push_back({{"columns", {(boost::format("%1% (%2%) [%3%]") % printer.name % printer.ip_address % printer.serial_number).str()}},
                             {"host", printer.ip_address}, {"serial", printer.serial_number}});
        return {{"title", _u8L("Discovered Printers")}, {"prompt", _u8L("Select a Flashforge printer")}, {"printers", found}, {"done", true}};
    }
    // BonjourDialog::show_and_lookup: the replies (FFF: not SL1) listed by address, hostname, service, version; the
    // lookup runs on Bonjour's thread and its result comes as the "printhost_browse" event
    const PrinterTechnology tech = Preset::printer_technology(*d.m_config);
    auto replies = std::make_shared<std::set<BonjourReply>>();
    auto m       = std::make_shared<std::mutex>();
    Bonjour::TxtKeys txt_keys{"version", "model"};
    auto bonjour = Bonjour("octoprint")
                       .set_txt_keys(std::move(txt_keys))
                       .set_retries(3)
                       .set_timeout(4)
                       .on_reply([replies, m, tech](BonjourReply &&reply) {
                           const auto model = reply.txt_data.find("model");
                           const bool sl1   = model != reply.txt_data.end() && model->second == "SL1";
                           if ((tech == ptFFF && sl1) || (tech == ptSLA && !sl1)) return;
                           std::lock_guard<std::mutex> l(*m);
                           replies->insert(std::move(reply));
                       })
                       .on_complete([replies, m, tech]() {
                           json found = json::array();
                           std::lock_guard<std::mutex> l(*m);
                           for (const BonjourReply &r : *replies) {
                               json item = {{"host", r.full_address}, {"columns", {r.full_address, r.hostname, r.service_name}}};
                               if (tech == ptFFF) {
                                   const auto it = r.txt_data.find("version");
                                   item["columns"].push_back(it != r.txt_data.end() ? it->second : std::string());
                               }
                               found.insert(found.begin(), item);
                           }
                           Host::event("printhost_browse", {{"printers", found}, {"done", true}});
                       })
                       .lookup();
    static std::shared_ptr<Bonjour> s_bonjour;
    s_bonjour = bonjour;
    json columns = {_u8L("Address"), _u8L("Hostname"), _u8L("Service name")};
    if (tech == ptFFF) columns.push_back(_u8L("OctoPrint version"));
    return {{"title", _u8L("Network lookup")}, {"columns", columns}, {"searching", _u8L("Searching for devices")},
            {"finished", _u8L("Searching for devices") + ": " + _u8L("Finished") + "."}};
}

json pick(const json &a)
{
    PhysicalPrinterDialog &d = dialog();
    d.group.set_value("print_host", wxString(str_arg(a, "host")), true);
    d.m_config->opt_string("print_host") = str_arg(a, "host");
    if (a.contains("serial")) {
        d.group.set_value("flashforge_serial_number", wxString(str_arg(a, "serial")), true);
        d.m_config->opt_string("flashforge_serial_number") = str_arg(a, "serial");
    }
    d.update_printhost_buttons();
    presets_changed();
    return dialog_state(d);
}

// OK: Tab::save_preset with the dialog's name
json ok(const json &)
{
    PhysicalPrinterDialog &d = dialog();
    d.update_preset_input();
    if (d.m_valid_type == PhysicalPrinterDialog::NoValid) throw CommandError(d.valid_label.label);
    const std::string name = d.m_preset_name;
    g_dialog.reset();
    return Core::get().call("presets.save", {{"type", "printer"}, {"name", name}});
}

json close(const json &)
{
    g_dialog.reset();
    return presets_state();
}

} // namespace pp

static Commands reg({
    {"printhost.dialog", pp::open},
    {"printhost.set", pp::set},
    {"printhost.test", pp::test},
    {"printhost.logout", pp::log_out},
    {"printhost.refresh", pp::refresh},
    {"printhost.browse", pp::browse},
    {"printhost.pick", pp::pick},
    {"printhost.ok", pp::ok},
    {"printhost.close", pp::close},
});

} // namespace orca
