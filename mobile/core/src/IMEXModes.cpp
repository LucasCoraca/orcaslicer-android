// The IDEX/IQEX modes editor of the printer settings (TabPrinter's "Modes" line, IMEXModesCtrl): the whole control is
// compiled from upstream on stand-in widgets that only keep their values (a tile's role and label, a name, a G-code),
// so its role cycling, naming rules, reset to the saved preset and serialization are upstream's. The app draws the
// rows from imex.state and sends the user's actions; each change writes the three option vectors like the Tab does.
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <boost/algorithm/string.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "wx/string.h"

// ---- the widgets the control's rows hold (global, as upstream's header forward-declares them)
class wxTextCtrl
{
public:
    wxString value;
    wxString GetValue() const { return value; }
    void     ChangeValue(const wxString &v) { value = v; }
    void     SetValue(const wxString &v) { value = v; }
};
class ComboBox
{
public:
    wxTextCtrl  text;
    wxTextCtrl *GetTextCtrl() { return &text; }
    const wxTextCtrl *GetTextCtrl() const { return &text; }
};
class ScalableButton
{
public:
    bool        enabled{true};
    std::string bitmap{"dot"};
    bool        IsEnabled() const { return enabled; }
    void        SetBitmap_(const std::string &name) { bitmap = name; }
};

namespace orca { namespace imex {
using namespace Slic3r;
using namespace Slic3r::GUI;

struct wxColour
{
    int r{0}, g{0}, b{0};
    wxColour() = default;
    wxColour(int r, int g, int b) : r(r), g(g), b(b) {}
};
static const wxColour s_white(255, 255, 255);
#define wxWHITE (&s_white)

class wxWindow {};
class wxPanel : public wxWindow
{
public:
    wxPanel() = default;
    wxPanel(wxWindow *, int) {}
    virtual ~wxPanel() = default;
    virtual bool Show(bool show = true) { m_shown = show; return true; }
    void         Destroy() { delete this; }
    void         Layout() {}
    bool         m_shown{true};
};
class wxButton
{
public:
    wxString label;
    wxColour bg, fg;
    void SetLabel(const wxString &l) { label = l; }
    void SetBackgroundColour(const wxColour &c) { bg = c; }
    void SetForegroundColour(const wxColour &c) { fg = c; }
    void Refresh() {}
};
class wxBoxSizer { public: void Detach(wxPanel *) {} };
struct WxApp { template<class F> void CallAfter(F &&f) { f(); } };
static WxApp s_app;
static WxApp *wxTheApp = &s_app;
enum { wxID_ANY = -1 };

// the rows' members are reached from the commands below
#define private public
#include "slic3r/GUI/IMEXModesTypes.inc"
#undef private

// IMEXModesCtrl's constructor without its sizers: the grid shape, no rows yet
IMEXModesCtrl::IMEXModesCtrl(wxWindow *parent, int n_cols, int n_rows, int layout)
    : wxPanel(parent, wxID_ANY), m_n_cols(std::max(1, n_cols)), m_n_rows(std::max(1, n_rows)), m_layout(layout)
{
    m_outer      = new wxBoxSizer();
    m_rows_sizer = new wxBoxSizer();
}
// the instructions, legend and headers are the app's (state())
void IMEXModesCtrl::rebuild_info_and_header() {}

// IMEXModesCtrl::add_row without the controls' layout: the name (or Primary), the tiles in the grid's on-screen order
// (front row at the bottom, mirrored by the T0 corner), the G-code and the reset button
void IMEXModesCtrl::add_row(const std::string &name, const std::string &active_tools, const std::string &gcode, bool is_primary)
{
    Row r;
    r.is_primary = is_primary;
    r.panel = new wxPanel(this, wxID_ANY);
    if (is_primary) {
        r.orig_name = kImexPrimaryMode;
        r.name = nullptr;
    } else {
        const std::string nm = name.empty() ? unique_mode_name({}) : name;
        r.orig_name = nm;
        r.name = new ::ComboBox();
        r.name->GetTextCtrl()->ChangeValue(from_u8(nm));
    }
    auto tool_roles = roles_for_mode(active_tools);
    if (tool_roles.empty())
        tool_roles[0] = ImexRole::Primary; // default T0 → Primary when no assignment is stored
    r.all_tool_roles = tool_roles;
    bool flip_x = (m_layout == 1 || m_layout == 3);
    bool flip_y = (m_layout == 2 || m_layout == 3);
    for (int row = m_n_rows - 1; row >= 0; --row) {
        for (int col = 0; col < m_n_cols; ++col) {
            int raw_row = flip_y ? (m_n_rows - 1 - row) : row;
            int raw_col = flip_x ? (m_n_cols - 1 - col) : col;
            int tool_idx = raw_row * m_n_cols + raw_col;
            std::optional<ImexRole> role;
            auto it = tool_roles.find(tool_idx);
            if (it != tool_roles.end()) role = it->second;
            auto *btn = new wxButton();
            apply_btn(btn, tool_idx, role);
            r.btns.push_back(btn);
            r.btn_roles.push_back(role);
            r.btn_tool_idx.push_back(tool_idx);
        }
    }
    r.gcode = new wxTextCtrl();
    r.gcode->ChangeValue(from_u8(gcode));
    if (row_has_parent_counterpart(static_cast<int>(m_rows.size())))
        r.reset_btn = new ScalableButton();
    m_rows.push_back(std::move(r));
}

// the tile button's handler in add_row: the next role this tile may take
static void click_tile(IMEXModesCtrl &c, int row, int btn_pos)
{
    auto &row_ref = c.m_rows[row];
    if (row_ref.is_primary || btn_pos >= int(row_ref.btn_roles.size()) || row_ref.btn_roles[btn_pos] == ImexRole::Primary) return;
    std::optional<ImexRole> &tile = row_ref.btn_roles[btn_pos];
    const int tidx = row_ref.btn_tool_idx[btn_pos];
    bool other_primary = false;
    int  primary_gantry = -1;
    for (int j = 0; j < (int)row_ref.btn_roles.size(); ++j) {
        if (row_ref.btn_roles[j] == ImexRole::Primary) {
            if (j != btn_pos) other_primary = true;
            primary_gantry = row_ref.btn_tool_idx[j] / c.m_n_cols;
        }
    }
    tile = c.next_tile_role(tile, [&](ImexRole cand) { return c.role_allowed_on_tile(cand, tidx, other_primary, primary_gantry); });
    if (tile)
        row_ref.all_tool_roles[tidx] = *tile;
    else
        row_ref.all_tool_roles.erase(tidx);
    c.apply_btn(row_ref.btns[btn_pos], row_ref.btn_tool_idx[btn_pos], tile);
    c.notify();
}

#include "slic3r/GUI/IMEXModes.inc"

static std::unique_ptr<IMEXModesCtrl> g_ctrl;
static std::vector<std::string>       g_removed;   // on_mode_removed's names, for the plates

DynamicPrintConfig &printer_config() { return Core::get().preset_bundle->printers.get_edited_preset().config; }

// imex_grid_shape (Tab.cpp)
static void grid_shape(int &n_cols, int &n_rows, int &layout)
{
    const DynamicPrintConfig &cfg = printer_config();
    n_cols = std::max(1, imex_cfg_int(cfg, "imex_tools_per_gantry"));
    n_rows = std::max(1, imex_cfg_int(cfg, "imex_gantry_count"));
    layout = IMEXModesCtrl::parse_layout(imex_cfg_enum<ImexToolLayout>(cfg, "imex_tool_layout"));
}

// the control for the edited printer, rebuilt when the preset changed under it (TabPrinter::update_fff)
static IMEXModesCtrl &ctrl()
{
    int n_cols = 0, n_rows = 0, layout = 0;
    grid_shape(n_cols, n_rows, layout);
    bool fresh = false;
    if (!g_ctrl) {
        fresh = true;
        g_ctrl = std::make_unique<IMEXModesCtrl>(nullptr, n_cols, n_rows, layout);
        g_ctrl->set_parent_config_lookup([]() -> const DynamicPrintConfig * {
            return &Core::get().preset_bundle->printers.get_selected_preset().config;
        });
        g_ctrl->on_mode_removed = [](const std::vector<std::string> &removed) { g_removed = removed; };
    }
    g_ctrl->set_grid_size(n_cols, n_rows, layout);
    // the Primary row is always there: a control without rows "matches" a printer without modes
    if (fresh || g_ctrl->m_rows.empty() || !g_ctrl->matches_config(printer_config())) g_ctrl->load_from_config(printer_config());
    return *g_ctrl;
}

static std::string hex(const wxColour &c)
{
    char b[8];
    std::snprintf(b, sizeof b, "#%02X%02X%02X", c.r, c.g, c.b);
    return b;
}

json state()
{
    IMEXModesCtrl &c = ctrl();
    c.refresh_reset_buttons();
    json legend = json::array();
    for (const ImexRoleDesc &d : kImexRoleTable) {
        if (!c.role_offered(d.role)) continue;
        const auto st = IMEXModesCtrl::role_style(d.role);
        legend.push_back({{"label", st.label.ToStdString()}, {"color", hex(st.bg)}, {"hint", IMEXModesCtrl::role_hint(d.role).ToStdString()}});
    }
    json rows = json::array();
    for (size_t i = 0; i < c.m_rows.size(); ++i) {
        const auto &r = c.m_rows[i];
        json tiles = json::array();
        for (size_t k = 0; k < r.btns.size(); ++k)
            tiles.push_back({{"label", r.btns[k]->label.ToStdString()}, {"bg", hex(r.btns[k]->bg)}, {"fg", hex(r.btns[k]->fg)},
                             {"enabled", !(r.is_primary || r.btn_roles[k] == ImexRole::Primary)}});
        json sugg = json::array();
        for (const wxString &sname : c.suggested_mode_names(r.name)) sugg.push_back(sname.ToStdString());
        rows.push_back({{"primary", r.is_primary},
                        {"name", r.is_primary ? _u8L("Primary") : r.name->GetTextCtrl()->GetValue().ToStdString()},
                        {"gcode", r.gcode->GetValue().ToStdString()},
                        {"tiles", tiles},
                        {"suggestions", sugg},
                        {"reset", r.reset_btn ? r.reset_btn->bitmap : std::string()}});
    }
    return {{"cols", c.m_n_cols},
            {"rows", rows},
            {"legend", legend},
            {"strings",
             {{"add", _u8L("Add Mode")},
              {"help", _u8L("Each mode names the tool heads that take part and the role each one plays. "
                            "Click a tool button to cycle its role — hover a color below for what each role does.")},
              {"mode", _u8L("Mode")}, {"tools", _u8L("Tools")}, {"gcode", _u8L("G-code")},
              {"mode_tip", _u8L("How the mode is labeled in the plate's IDEX/IQEX mode selector. Required — a mode with no name cannot be selected.")},
              {"tools_tip", _u8L("Which tool heads take part in the mode and what role each one plays. Click a tile to cycle its role.")},
              {"gcode_tip", _u8L("G-code run at print start to put the printer into this mode (for example a Klipper SET_PRINT_MODE call). "
                                 "The slicer only emits the Primary tool's paths; the firmware drives the others.")},
              {"name_hint", _u8L("Mode name (required)")},
              {"edit_tip", _u8L("Edit G-code / browse placeholders")},
              {"remove_tip", _u8L("Remove mode")},
              {"reset_tip", _u8L("Discard in-session edits to this mode (snap back to saved value)")}}}};
}

// on_change of TabPrinter::build_fff: the three option vectors, through the settings' own value path; a deleted mode
// resets the plates using it to Primary (on_mode_removed)
static json write_back()
{
    IMEXModesCtrl &c = *g_ctrl;
    auto [names, tools, gcodes] = c.get_mode_data();
    Core &core = Core::get();
    core.call("config.set", {{"type", "printer"}, {"key", "imex_mode_names"}, {"value", names}});
    core.call("config.set", {{"type", "printer"}, {"key", "imex_mode_active_tools"}, {"value", tools}});
    json st = core.call("config.set", {{"type", "printer"}, {"key", "imex_mode_gcodes"}, {"value", gcodes}});
    if (!g_removed.empty()) {
        Plater &p = Plater::get();
        std::vector<int> affected;
        for (int i = 0; i < int(p.plates.size()); ++i) {
            auto *opt = p.plates[i].config.option<ConfigOptionString>("imex_parallel_mode");
            if (opt && opt->value != kImexPrimaryMode && std::find(g_removed.begin(), g_removed.end(), opt->value) != g_removed.end())
                affected.push_back(i);
        }
        if (!affected.empty()) {
            p.take_snapshot("reset imex mode");
            for (int i : affected) p.plates[i].config.erase("imex_parallel_mode");
            p.changed();
        }
        g_removed.clear();
    }
    return st;
}

// the user's actions: add, remove, tile, name, name_commit (the focus leaving the field), gcode, reset
json edit(const json &a)
{
    IMEXModesCtrl &c = ctrl();
    const std::string what = a.value("what", "");
    const int row = a.value("row", -1);
    const bool has_row = row >= 0 && row < int(c.m_rows.size());
    if (what == "add") {
        c.add_row();
        c.notify();
    } else if (has_row && what == "remove") {
        const std::vector<std::string> removed = c.remove_row(c.m_rows[row].panel);
        c.notify();
        if (c.on_mode_removed && !removed.empty()) c.on_mode_removed(removed);
    } else if (has_row && what == "tile") {
        click_tile(c, row, a.value("tile", -1));
    } else if (has_row && what == "name" && c.m_rows[row].name) {
        c.m_rows[row].name->GetTextCtrl()->ChangeValue(from_u8(a.value("value", "")));
        c.notify();
    } else if (has_row && what == "name_commit") {
        if (c.fix_row_name(c.m_rows[row])) c.notify();
    } else if (has_row && what == "gcode") {
        c.m_rows[row].gcode->ChangeValue(from_u8(a.value("value", "")));
        c.notify();
    } else if (has_row && what == "reset") {
        c.reset_row_to_parent(c.m_rows[row].panel);
    }
    json config = write_back();   // first: state() reloads the rows when they differ from the config
    json out = state();
    out["config"] = std::move(config);
    return out;
}

}} // namespace orca::imex

namespace orca {
static Commands reg({
    {"imex.state", [](const json &) { return imex::state(); }},
    {"imex.edit", imex::edit},
});
} // namespace orca
