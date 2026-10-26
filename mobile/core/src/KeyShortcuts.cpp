// Keyboard shortcuts (a hardware keyboard on a tablet): upstream's ShortcutRegistry and KeyChord compiled unchanged
// (Shortcuts.cpp, KeyChord.cpp), the key presses the app forwards matched per context (the main frame's and the
// canvases' key handlers), and KBShortcutsDialog's pages, editing and reset.
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Core.hpp"
#include "Host.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/KeyChord.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "wx/event.h"

namespace Slic3r { namespace GUI {

const std::string &shortkey_ctrl_prefix()
{
    static const std::string str = _u8L("Ctrl+");
    return str;
}

const std::string &shortkey_alt_prefix()
{
    static const std::string str = _u8L("Alt+");
    return str;
}

const std::string &shortkey_shift_prefix()
{
    static const std::string str = _u8L("Shift+");
    return str;
}

// GUI_App::shortcuts: the registry, loaded from the app config once
ShortcutRegistry &GUI_App::shortcuts()
{
    static ShortcutRegistry registry;
    static bool             loaded = false;
    if (!loaded && app_config) {
        registry.load(*app_config);
        loaded = true;
    }
    return registry;
}

// GUI_App::on_shortcuts_changed: the bindings are saved, the menus and tooltips relabel themselves
void GUI_App::on_shortcuts_changed()
{
    if (app_config) {
        shortcuts().save(*app_config);
        app_config->save();
    }
    orca::Host::event("shortcuts", orca::json::object());
}

namespace {

#include "slic3r/GUI/KbDialog.inc.fn"

// KBShortcutsDialog: its pages, and reset / the conflict question on its rows
class KbDialog
{
public:
    struct FixedKey
    {
        std::vector<wxString> keys;
        const char           *description;
    };
    struct MouseAction
    {
        wxString    button;
        const char *preference;
    };
    struct Row
    {
        std::variant<Shortcut, FixedKey, MouseAction> content;
        ShortcutSection                               section;
    };
    struct Page
    {
        wxString         title;
        wxString         caption;
        ShortcutContext  context;
        std::vector<Row> rows;
    };

    std::vector<Page> m_pages;

    KbDialog() { fill_pages(); }
    void fill_pages();
    void reset_shortcut(Shortcut shortcut);
    bool take_chord_from(Shortcut shortcut, const std::vector<Shortcut> &conflicts, const wxString &question);
    void apply_bindings() { wxGetApp().on_shortcuts_changed(); }
    // edit_shortcut once ShortcutCaptureDialog returned OK
    void edit_shortcut(Shortcut shortcut, const KeyChord &chord, const std::vector<Shortcut> &conflicts)
    {
        const wxString question = wxString::Format(_L("%s is assigned to %s. Reassign it to %s?"),
                                                   join_keys(to_wx(chord.display_parts())), shortcut_names(conflicts), _(shortcut_info(shortcut).name));
        if (!take_chord_from(shortcut, conflicts, question))
            return;
        wxGetApp().shortcuts().bind(shortcut, chord);
        apply_bindings();
    }
};

#include "slic3r/GUI/KbDialog.inc"

// ShortcutCaptureDialog's labels and OK button as the state its record() leaves for the app
const int ERROR_COLOUR = 1;
struct KbLabel
{
    wxString text;
    int      colour{0};
    KbLabel *GetParent() { return this; }
    void     Layout() {}
    void     Refresh() {}
    void     SetLabel(const wxString &s) { text = s; }
    void     SetForegroundColour(int c) { colour = c; }
};
struct KbButton
{
    bool enabled{false};
    void Enable(bool e) { enabled = e; }
};

class KbCapture
{
public:
    explicit KbCapture(Shortcut shortcut) : m_shortcut(shortcut)
    {
        const bool     global = (shortcut_info(shortcut).contexts & context_bit(ShortcutContext::Global)) != 0;
        const wxString advice = global_key_advice();
        const wxString typing = _L("Global shortcuts also apply while typing.");
        const wxString rule   = _L("A key that types a character cannot be a global shortcut.");
        m_hint                = global ? typing + "\n" + advice : _L("Esc cancels, Enter confirms.");
        m_rejection           = rule + "\n" + advice;
        m_status->text        = m_hint;
    }
    void record(const KeyChord &chord);
    void Layout() {}
    void Fit() {}

    Shortcut              m_shortcut;
    KeyChord              m_chord;
    std::vector<Shortcut> m_conflicts;
    KbLabel               m_chord_storage, m_status_storage;
    KbButton              m_ok_storage;
    KbLabel              *m_chord_label{&m_chord_storage};
    wxString              m_hint, m_rejection;
    KbLabel              *m_status{&m_status_storage};
    int                   m_status_colour{0};
    KbButton             *m_ok{&m_ok_storage};
};

#include "slic3r/GUI/KbDialog.inc.capture"

orca::json parts_json(const std::vector<wxString> &parts)
{
    orca::json a = orca::json::array();
    for (const wxString &p : parts) a.push_back(p.ToStdString());
    return a;
}

Shortcut shortcut_arg(const orca::json &a)
{
    const int id = a.value("shortcut", -1);
    if (id < 0 || id >= int(Shortcut::Count)) throw orca::CommandError("unknown shortcut");
    return Shortcut(id);
}

KeyChord chord_arg(const orca::json &a)
{
    if (a.contains("chord")) return KeyChord::parse(a.value("chord", std::string())).value_or(KeyChord{});
    // the app's key press: wx key code, wxMOD_* modifiers and whether it is the char event
    const wxKeyEvent evt(a.value("char", false) ? wxEVT_CHAR : wxEVT_KEY_DOWN, a.value("key", 0), a.value("mods", 0));
    return KeyChord::from_event(evt);
}

const std::vector<std::pair<std::string, ShortcutContext>> CONTEXTS = {{"global", ShortcutContext::Global},
                                                                       {"plater", ShortcutContext::Plater},
                                                                       {"preview", ShortcutContext::Preview},
                                                                       {"objectlist", ShortcutContext::ObjectList},
                                                                       {"painting", ShortcutContext::Painting}};

std::string context_name(ShortcutContext c)
{
    for (auto &[n, v] : CONTEXTS) if (v == c) return n;
    return "global";
}

orca::json capture_state(const KbCapture &c)
{
    return {{"chord", c.m_chord.valid() ? c.m_chord.to_string() : std::string()},
            {"label", c.m_chord_label->text.ToStdString()},
            {"status", c.m_status->text.ToStdString()},
            {"error", c.m_status->colour == ERROR_COLOUR},
            {"ok", c.m_ok->enabled}};
}

// the main frame's key handling: Global first, then the focused view's context
orca::json match(const orca::json &a)
{
    const KeyChord chord = chord_arg(a);
    if (!chord.valid()) return nullptr;
    const ShortcutRegistry &reg = wxGetApp().shortcuts();
    std::optional<ShortcutRegistry::Match> m = reg.match(ShortcutContext::Global, chord);
    if (!m) {
        const std::string ctx = a.value("context", std::string("plater"));
        for (auto &[n, v] : CONTEXTS)
            if (n == ctx && v != ShortcutContext::Global) m = reg.match(v, chord);
    }
    if (!m) return nullptr;
    const ShortcutInfo &info = shortcut_info(m->shortcut);
    return {{"key", info.key}, {"step_modifiers", m->step_modifiers}, {"repeatable", info.repeatable}};
}

// every chord that matches in each context, as ShortcutRegistry::match finds it: the bindings, plus Shift / Ctrl on the
// stepping ones ("key,mods" -> shortcut key, step modifiers, repeatable), so the app matches key presses as they come
orca::json table()
{
    const ShortcutRegistry &reg = wxGetApp().shortcuts();
    orca::json              out = orca::json::object();
    for (auto &[name, context] : CONTEXTS) {
        orca::json t = orca::json::object();
        for (int i = 0; i < int(Shortcut::Count); ++i) {
            const KeyChord b = reg.binding(Shortcut(i));
            if (!b.valid()) continue;
            for (int extra : {0, int(wxMOD_SHIFT), int(wxMOD_CONTROL), int(wxMOD_SHIFT | wxMOD_CONTROL)}) {
                const KeyChord chord{b.key, b.modifiers | extra};
                const std::string k = std::to_string(chord.key) + "," + std::to_string(chord.modifiers);
                if (t.contains(k)) continue;
                if (const std::optional<ShortcutRegistry::Match> m = reg.match(context, chord)) {
                    const ShortcutInfo &info = shortcut_info(m->shortcut);
                    t[k] = {{"key", info.key}, {"step", m->step_modifiers}, {"repeat", info.repeatable}};
                }
            }
        }
        out[name] = t;
    }
    return out;
}

orca::Commands reg({
    {"shortcuts.table", [](const orca::json &) -> orca::json { return table(); }},
    // the menu labels' and tooltips' keys: config key -> "Ctrl+N"
    {"shortcuts.labels", [](const orca::json &) -> orca::json {
         orca::json r = orca::json::object();
         for (int i = 0; i < int(Shortcut::Count); ++i)
             r[shortcut_info(Shortcut(i)).key] = wxGetApp().shortcuts().display(Shortcut(i));
         return r;
     }},
    // a preference a shortcut depends on (enable_speed_dial)
    {"shortcuts.app_bool", [](const orca::json &a) -> orca::json { return wxGetApp().app_config->get_bool(a.at("key").get<std::string>()); }},
    {"shortcuts.match", [](const orca::json &a) -> orca::json { return match(a); }},
    // KBShortcutsDialog: its tabs, rows with their section headings, and which rows are customized
    {"shortcuts.pages", [](const orca::json &) -> orca::json {
         KbDialog          dlg;
         orca::json        pages = orca::json::array();
         const ShortcutRegistry &shortcuts = wxGetApp().shortcuts();
         for (const KbDialog::Page &page : dlg.m_pages) {
             orca::json rows = orca::json::array();
             for (const KbDialog::Row &row : page.rows) {
                 orca::json r = {{"section", _(section_name(row.section)).ToStdString()}};
                 if (const Shortcut *s = std::get_if<Shortcut>(&row.content)) {
                     r["kind"]        = "shortcut";
                     r["shortcut"]    = int(*s);
                     r["description"] = _(shortcut_info(*s).name).ToStdString();
                     r["keys"]        = parts_json(to_wx(shortcuts.binding(*s).display_parts()));
                     r["customized"]  = shortcuts.is_customized(*s);
                 } else if (const auto *f = std::get_if<KbDialog::FixedKey>(&row.content)) {
                     r["kind"]        = "fixed";
                     r["description"] = _(f->description).ToStdString();
                     r["keys"]        = parts_json(f->keys);
                 } else if (const auto *m = std::get_if<KbDialog::MouseAction>(&row.content)) {
                     r["kind"]        = "mouse";
                     r["description"] = _(mouse_action(m->preference)).ToStdString();
                     r["keys"]        = parts_json({m->button});
                     r["preference"]  = m->preference;
                 }
                 rows.push_back(r);
             }
             pages.push_back({{"title", page.title.ToStdString()},
                              {"caption", page.caption.ToStdString()},
                              {"context", context_name(page.context)},
                              {"rows", rows}});
         }
         return {{"title", _u8L("Keyboard Shortcuts")},
                 {"pages", pages},
                 {"edit", _u8L("Edit")},
                 {"reset", _u8L("Reset")},
                 {"preferences", _u8L("Preferences")},
                 {"locked", _u8L("Not customizable")}};
     }},
    // ShortcutCaptureDialog: its texts, then each key press through record()
    {"shortcuts.capture_open", [](const orca::json &a) -> orca::json {
         const Shortcut s = shortcut_arg(a);
         KbCapture      c(s);
         c.m_chord_label->text = join_keys(to_wx(wxGetApp().shortcuts().binding(s).display_parts()));
         orca::json r          = capture_state(c);
         r["title"]            = _(shortcut_info(s).name).ToStdString();
         r["prompt"]           = wxString::Format(_L("Press the new shortcut for\n\"%s\""), _(shortcut_info(s).name)).ToStdString();
         r["unbind"]           = _u8L("Unbind");
         return r;
     }},
    {"shortcuts.capture", [](const orca::json &a) -> orca::json {
         KbCapture      c(shortcut_arg(a));
         const KeyChord chord = chord_arg(a);
         if (!chord.valid()) return nullptr;
         // on_key / on_char: punctuation typed with Shift waits for the char event that names its character
         const bool is_char = a.value("char", false);
         if (is_char ? !chord.is_punctuation() : chord.needs_char_event()) return nullptr;
         c.record(chord);
         return capture_state(c);
     }},
    // OK (or Unbind, with no chord): the conflict question, then the binding
    {"shortcuts.apply", [](const orca::json &a) -> orca::json {
         const Shortcut s     = shortcut_arg(a);
         const KeyChord chord = KeyChord::parse(a.value("chord", std::string())).value_or(KeyChord{});
         KbDialog       dlg;
         dlg.edit_shortcut(s, chord, chord.valid() ? wxGetApp().shortcuts().conflicts(s, chord) : std::vector<Shortcut>{});
         return nullptr;
     }},
    {"shortcuts.reset", [](const orca::json &a) -> orca::json {
         KbDialog dlg;
         dlg.reset_shortcut(shortcut_arg(a));
         return nullptr;
     }},
});

} // namespace
}} // namespace Slic3r::GUI
