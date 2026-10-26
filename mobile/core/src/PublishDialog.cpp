// File > Publish 3MF (MainFrame::publish_project): PublishSettingsDialog's option model (the printer's extruder
// retraction / Z-hop rows, one section per used filament slot with Enable / Full Publish and the colour / type
// requirements, the process pages), its dirty pre-check, the remembered selection, All / None, the filter and its menu,
// the tab indicators and its result, compiled from upstream on stand-in widgets that keep their state for the app's
// dialog; the settings pages are the app's (Tab.cpp's layout). Then Plater::export_published_3mf.
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Published.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/ConfigValueFormatter.hpp"
#include "slic3r/GUI/Field.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "wx/numformatter.h"

namespace Slic3r { namespace GUI {
#include "slic3r/GUI/PublishDialog.inc.field"
}} // namespace Slic3r::GUI

namespace orca {
void write_3mf(const std::string &path, Slic3r::SaveStrategy strategy, int export_plate_idx);   // ProjectIO.cpp
void add_to_recent_projects(const std::string &filename);
} // namespace orca

namespace orca { namespace pub {

using namespace Slic3r;
using namespace Slic3r::GUI;

// ---- stand-in widgets: the state the dialog's code sets, read back by the app's dialog
struct W
{
    bool                  shown{true}, enabled{true}, value{false}, bold{false};
    wxString              label;
    std::vector<wxString> options;   // MultiSwitchButton
    int                   selection{-1};
    std::vector<bool>     indicators;   // TabCtrl::SetItemIndicator
    void Show(bool s = true) { shown = s; }
    void Hide() { shown = false; }
    bool IsShown() const { return shown; }
    void Enable(bool e = true) { enabled = e; }
    bool IsEnabled() const { return enabled; }
    bool GetValue() const { return value; }
    void SetValue(bool v) { value = v; }
    void SetLabel(const wxString &l) { label = l; }
    void Layout() {}
    void FitInside() {}
    void Refresh() {}
    void Scroll(int, int) {}
    void GetViewStart(int *x, int *y) const { *x = 0; *y = 0; }
    W   *GetSizer() { return this; }
    void SelectItem(int i) { selection = i; }
    void Unselect() { selection = -1; }
    void SetSelection(int i) { selection = i; }
    void SetItemIndicator(unsigned i, bool on)
    {
        if (indicators.size() <= i) indicators.resize(i + 1, false);
        indicators[i] = on;
    }
    wxString GetOptionText(unsigned i) const { return i < options.size() ? options[i] : wxString(); }
    void     SetOptionText(unsigned i, const wxString &t) { if (i < options.size()) options[i] = t; }
    template<class... A> void Bind(A &&...) {}
};
struct TextW
{
    wxString text;
    wxString GetValue() const { return text; }
    void     ChangeValue(const wxString &t) { text = t; }
};
// the check boxes' toggle event (bound, never sent: the app's actions call the handlers)
struct wxCommandEvent
{
    void Skip(bool = true) {}
};
const int wxEVT_TOGGLEBUTTON = 0;
using CheckBox     = W;
using wxStaticText = W;

// ---- the settings tabs: the pages of the app's settings editor (generated from Tab.cpp), with Tab::translate_category
struct OptGroup
{
    std::string                                           title;
    wxString                                              icon;
    std::map<std::string, std::pair<std::string, int>>    m_opt_map;   // t_opt_map: opt_id -> (pure key, index)
    const std::map<std::string, std::pair<std::string, int>> &opt_map() const { return m_opt_map; }
};
using ConfigOptionsGroupShp = std::shared_ptr<OptGroup>;
struct Page
{
    std::string                        m_title;
    std::vector<ConfigOptionsGroupShp> m_optgroups;
    wxString                           title() const { return m_title; }
};
using PageShp = std::shared_ptr<Page>;
struct Tab
{
    virtual ~Tab() = default;
    Preset::Type         m_type{Preset::TYPE_PRINT};
    std::vector<PageShp> m_pages;
    static wxString      translate_category(const wxString &title, Preset::Type preset_type, bool translate = true);
};
struct TabPrinter : Tab
{
    size_t m_extruders_count{1};
    Page  *extruder_page() const
    {
        for (const PageShp &page : m_pages)
            if (page->title() == "Extruder") return page.get();
        return nullptr;
    }
    // TabPrinter::extruder_variant_index: the variant column of an extruder's values
    int extruder_variant_index(int extruder)
    {
        const DynamicPrintConfig &cfg       = Core::get().preset_bundle->printers.get_edited_preset().config;
        const auto               *extruders = cfg.option<ConfigOptionEnumsGeneric>("extruder_type");
        if (extruders == nullptr || extruder >= int(extruders->size())) return extruder;
        NozzleVolumeType nvt = nvtStandard;
        if (auto *v = Core::get().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
            if (extruder < int(v->size())) nvt = NozzleVolumeType(v->values[extruder]);
        const int index = cfg.get_index_for_extruder(extruder + 1, "printer_extruder_id", ExtruderType(extruders->values[extruder]), nvt,
                                                      "printer_extruder_variant");
        return index < 0 ? extruder : index;
    }
};

struct PubPlater
{
    Model &model() { return Plater::get().model(); }
};
struct PubApp
{
    PresetBundle      *preset_bundle{nullptr};
    std::vector<Tab *> tabs_list;
    PubPlater          plater_;
    PubPlater         *plater() { return &plater_; }
};
PubApp &wxGetApp()
{
    static PubApp app;
    app.preset_bundle = Core::get().preset_bundle.get();
    return app;
}

#include "slic3r/GUI/PublishDialog.inc.tab"
#include "slic3r/GUI/PublishDialog.inc.types"
#include "slic3r/GUI/PublishDialog.inc.fn"

// PublishSettingsDialog with its widgets as the stand-ins above
class PubDialog
{
public:
    enum class Section { Print, Printer, Material };
    enum class RowKind { Setting, Color, Type };
    struct Row
    {
        std::string key;
        wxString    category, subcategory, label, value, unit, section_title;
        Section     section{Section::Print};
        RowKind     kind{RowKind::Setting};
        size_t      outer_index{0}, inner_index{0};
        bool        dirty{false}, matches_filter{false};
        CheckBox   *check{nullptr};
        W          *check_label{nullptr};
        W          *item{nullptr};
    };
    struct Subcategory
    {
        wxString            title, icon;
        W                  *header{nullptr};
        W                  *item{nullptr};
        std::vector<size_t> rows;
    };
    struct Category
    {
        wxString            title;
        Section             section{Section::Print};
        size_t              group{0}, source_index{0};
        W                  *page{nullptr}, *scroll{nullptr}, *list_sizer{nullptr}, *info{nullptr};
        struct { int x{0}, y{0}; } scroll_pos;
        W                  *enable_check{nullptr}, *enable_label{nullptr}, *full_check{nullptr}, *full_label{nullptr};
        W                  *full_line_item{nullptr};
        bool                is_mixed{false};
        std::string         filament_type, filament_vendor, filament_id;
        size_t              filament_slot{0};
        std::vector<Subcategory> subs;
        std::vector<size_t> rows;
    };
    // PublishSettingsDialog::MixedVisualSpec, with the colours as hex strings for the app
    struct MixedVisualSpec
    {
        bool valid{false};
        bool is_gradient{false};
        std::vector<std::string> component_colours;
        std::vector<double> ratios, tri_weights;
        std::vector<std::pair<double, double>> gradient_samples, gradient_anchors;
    };
    std::map<size_t, MixedVisualSpec> m_mixed_visuals;   // by category
    struct SectionGroup
    {
        wxString            title;
        Section             kind{Section::Print};
        std::string         icon_name;
        W                  *page{nullptr}, *tabs{nullptr}, *variant_switch{nullptr}, *mixed_tabs{nullptr}, *page_host_sizer{nullptr};
        int                 selected_inner{-1}, selected_mixed{-1};
        std::vector<size_t> categories, mixed_categories;
    };
    enum class FilterMode { Text, SelectedOnly, UnselectedOnly };

    std::vector<Row>          m_rows;
    std::vector<Category>     m_categories;
    std::vector<SectionGroup> m_sections;
    W                        *m_outer_tabs{nullptr}, *m_outer_host_sizer{nullptr}, *m_fb_sizer{nullptr}, *m_pseudo_chip{nullptr};
    TextW                     m_filter_text;
    TextW                    *m_filter_ctrl{&m_filter_text};
    int                       m_selected_outer{-1};
    FilterMode                m_filter_mode{FilterMode::Text};
    wxString                  m_info_nonsel, m_info_allsel, m_info_empty, m_info_mix;

    PubDialog(const std::vector<std::string> *published_keys, const std::vector<PublishedMaterialEntry> *material_keys)
    {
        m_outer_tabs = w(); m_outer_host_sizer = w(); m_fb_sizer = w(); m_pseudo_chip = w(false);
        build_option_model();
        if (published_keys != nullptr || material_keys != nullptr)
            apply_selection(published_keys != nullptr ? *published_keys : std::vector<std::string>(),
                            material_keys != nullptr ? *material_keys : std::vector<PublishedMaterialEntry>());
    }

    void build_option_model();
    void apply_selection(const std::vector<std::string> &published_keys, const std::vector<PublishedMaterialEntry> &material_keys);
    std::vector<std::string>            GetPublishedKeys() const;
    std::vector<PublishedMaterialEntry> GetPublishedMaterialKeys() const;
    std::vector<MixedDependencyIssue>   unpublished_mixed_components() const;
    bool category_has_selection(const Category &cat) const;
    void refresh_tab_indicators();
    std::vector<std::string> full_keys_for_slot() const;
    void on_full_toggle(size_t category_index);
    void on_enable_toggle(size_t category_index);
    void select_all(bool value);
    void select_visible(bool value);
    void apply_filter(const wxString &filter_text);
    void apply_pseudo_filter(bool selected_only);
    void refresh_filter(const wxString &filter);
    void apply_visibility();
    bool row_is_visible(const Row &row) const;
    void save_scroll_position(Category &category);
    void show_outer_page(size_t section_index);
    void show_inner_page(size_t section_index, int inner_index);
    void show_mixed_page(size_t section_index, int mixed_index);

    // the widget-building parts, as data
    size_t section_group_for(Section kind)
    {
        for (size_t i = 0; i < m_sections.size(); ++i)
            if (m_sections[i].kind == kind) return i;
        SectionGroup section;
        section.kind = kind;
        switch (kind) {
        case Section::Printer: section.title = _L("Printer"); section.icon_name = "printer"; break;
        case Section::Material: section.title = _L("Filament"); section.icon_name = "filament"; break;
        case Section::Print: section.title = _L("Process"); section.icon_name = "process"; break;
        }
        section.page = w(false); section.tabs = w(); section.page_host_sizer = w();
        if (kind == Section::Material) section.mixed_tabs = w(false);
        m_sections.push_back(std::move(section));
        return m_sections.size() - 1;
    }
    size_t category_index_for(const wxString &title, Section section, size_t group, size_t source_index,
                              const PublishMaterialIdentity &identity = PublishMaterialIdentity(), bool is_mixed = false)
    {
        auto match = [&](const Category &existing) {
            return existing.title == title && existing.section == section && existing.source_index == source_index &&
                   existing.filament_id == identity.id && existing.filament_type == identity.type && existing.filament_vendor == identity.vendor;
        };
        for (size_t i : m_sections[group].categories)
            if (match(m_categories[i])) return i;
        for (size_t i : m_sections[group].mixed_categories)
            if (match(m_categories[i])) return i;
        Category category;
        category.title = title; category.section = section; category.group = group; category.source_index = source_index;
        category.filament_type = identity.type; category.filament_vendor = identity.vendor; category.filament_id = identity.id;
        category.filament_slot = source_index; category.is_mixed = is_mixed;
        category.page = w(false);
        if (section == Section::Material) {
            category.enable_check = w(); category.enable_label = w();
            if (!is_mixed) { category.full_check = w(); category.full_label = w(); category.full_line_item = w(); }
        }
        category.scroll = w(section != Section::Material); category.list_sizer = w();
        category.info = w(false);
        category.info->label = is_mixed ? m_info_mix : m_info_empty;
        const size_t index = m_categories.size();
        m_categories.push_back(std::move(category));
        if (is_mixed) {
            m_sections[group].mixed_categories.push_back(index);
            if (m_sections[group].selected_mixed < 0) m_sections[group].selected_mixed = 0;
            m_sections[group].mixed_tabs->Show();
        } else {
            m_sections[group].categories.push_back(index);
            if (m_sections[group].selected_inner < 0) m_sections[group].selected_inner = 0;
        }
        return index;
    }
    size_t subcategory_index_for(size_t category_index, const wxString &title, const wxString &icon)
    {
        Category &category = m_categories[category_index];
        for (size_t i = 0; i < category.subs.size(); ++i)
            if (category.subs[i].title == title) return i;
        Subcategory sub;
        sub.title = title; sub.icon = icon;
        if (!title.IsEmpty()) { sub.header = w(); sub.item = w(); }
        category.subs.push_back(std::move(sub));
        return category.subs.size() - 1;
    }
    void add_row_ui(const std::string &key, const wxString &label, const wxString &value, const wxString &unit, size_t category_index,
                    size_t subcategory_index, RowKind kind = RowKind::Setting)
    {
        Category &category = m_categories[category_index];
        Row row;
        row.key = key; row.label = label; row.value = value; row.unit = unit; row.kind = kind;
        row.category = category.title; row.subcategory = category.subs[subcategory_index].title; row.section = category.section;
        row.section_title = m_sections[category.group].title; row.outer_index = category.group; row.inner_index = category_index;
        row.check = w(); row.check_label = w(); row.item = w();
        m_rows.push_back(std::move(row));
        category.rows.push_back(m_rows.size() - 1);
        category.subs[subcategory_index].rows.push_back(m_rows.size() - 1);
    }
    // PublishSettingsDialog::make_mixed_visual_spec: the components' colours, the sublayer shares (barycentric for three) or
    // the gradient curve sampled with its anchors
    static MixedVisualSpec make_mixed_visual_spec(const DynamicPrintConfig &full, size_t slot)
    {
        MixedVisualSpec spec;
        const std::vector<unsigned int> comps = mixed_slot_components(full, slot);
        if (comps.empty()) return spec;
        const auto *grad_opt = full.opt<ConfigOptionBools>("filament_mixed_gradient");
        spec.is_gradient = grad_opt != nullptr && slot < grad_opt->size() && grad_opt->values[slot];
        const auto *colours = full.opt<ConfigOptionStrings>("filament_colour");
        for (unsigned int cid : comps)
            spec.component_colours.push_back(colours != nullptr && cid >= 1 && cid <= colours->size() ? colours->values[cid - 1] : "#D9D9D9");
        if (!spec.is_gradient) {
            spec.ratios.assign(comps.size(), 1.0 / comps.size());
            if (const auto *ratios_opt = full.opt<ConfigOptionStrings>("filament_mixed_sublayer_ratios"))
                if (slot < ratios_opt->size()) {
                    const std::vector<double> rs = parse_mixed_ratios(ratios_opt->values[slot], comps.size());
                    if (rs.size() == comps.size()) spec.ratios = rs;
                }
            if (comps.size() == 3) spec.tri_weights = spec.ratios;
        } else {
            // mixed_gradient_curve (FilamentBitmapUtils): the custom curve, else the gradient range, else 0.1 -> 0.9
            GradientCurve curve;
            const auto *curve_opt = full.opt<ConfigOptionStrings>("filament_mixed_gradient_curve");
            if (curve_opt && slot < curve_opt->size() && !curve_opt->values[slot].empty()) curve = parse_gradient_curve(curve_opt->values[slot]);
            if (curve.points.size() < 2) {
                double start = kGradientMinRatio, end = kGradientMaxRatio;
                const auto *range_opt = full.opt<ConfigOptionStrings>("filament_mixed_gradient_range");
                if (range_opt && slot < range_opt->size() && !range_opt->values[slot].empty()) {
                    CNumericLocalesSetter c_locale_setter;
                    float v0 = 0, v1 = 0;
                    if (std::sscanf(range_opt->values[slot].c_str(), "%f,%f", &v0, &v1) == 2 && v0 > 0 && v0 < 1 && v1 > 0 && v1 < 1) {
                        start = v0;
                        end   = v1;
                    }
                }
                curve.points = {{0.0, start, NAN, NAN}, {1.0, end, NAN, NAN}};
            }
            constexpr int kSamples = 256;
            for (int i = 0; i <= kSamples; ++i) spec.gradient_samples.emplace_back(double(i) / kSamples, sample_gradient_curve(curve, double(i) / kSamples));
            for (const GradientAnchor &anchor : curve.points) spec.gradient_anchors.emplace_back(anchor.x, anchor.y);
        }
        spec.valid = true;
        return spec;
    }
    void add_mixed_visual(size_t category_index, const MixedVisualSpec &spec)
    {
        if (category_index < m_categories.size() && spec.valid && !spec.component_colours.empty()) m_mixed_visuals[category_index] = spec;
    }
    void set_row_bold(Row &row, bool bold) { row.check_label->bold = bold; }
    void setup_variant_switch(size_t section_index)
    {
        SectionGroup &section = m_sections[section_index];
        section.variant_switch = w();
        for (size_t category : section.categories) section.variant_switch->options.push_back(m_categories[category].title);
        section.variant_switch->selection = section.selected_inner;
        section.tabs->Hide();
    }
    void bind_tab_events() {}
    void Freeze() {}
    void Thaw() {}
    void Layout() {}

    json state() const;

private:
    std::deque<W> m_widgets;
    W *w(bool shown = true)
    {
        m_widgets.emplace_back();
        m_widgets.back().shown = shown;
        return &m_widgets.back();
    }
};

#include "slic3r/GUI/PublishDialog.inc"

json PubDialog::state() const
{
    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
    json sections = json::array();
    for (size_t s = 0; s < m_sections.size(); ++s) {
        const SectionGroup &sec = m_sections[s];
        auto cat_json = [&](size_t ci) {
            const Category &cat = m_categories[ci];
            json c = {{"index", ci}, {"title", cat.title}, {"mixed", cat.is_mixed}, {"shown_rows", cat.scroll->IsShown()},
                      {"info", cat.info->IsShown() ? cat.info->label.ToStdString() : std::string()}};
            if (auto v = m_mixed_visuals.find(ci); v != m_mixed_visuals.end()) {
                json samples = json::array(), anchors = json::array();
                for (auto &[t, r] : v->second.gradient_samples) samples.push_back({t, r});
                for (auto &[t, r] : v->second.gradient_anchors) anchors.push_back({t, r});
                c["visual"] = {{"gradient", v->second.is_gradient}, {"colors", v->second.component_colours}, {"ratios", v->second.ratios},
                               {"tri", v->second.tri_weights}, {"samples", samples}, {"anchors", anchors}};
            }
            if (cat.section == Section::Material) {
                c["slot"]   = cat.filament_slot;
                c["color"]  = filament_color_hex(full, cat.filament_slot);
                c["enable"] = cat.enable_check != nullptr && cat.enable_check->GetValue();
                if (cat.full_check != nullptr) c["full"] = cat.full_check->GetValue();
            }
            json subs = json::array();
            for (const Subcategory &sub : cat.subs) {
                json rows = json::array();
                for (size_t r : sub.rows) {
                    const Row &row = m_rows[r];
                    if (!row.item->IsShown()) continue;
                    rows.push_back({{"index", r}, {"label", row.label}, {"value", row.value}, {"unit", row.unit},
                                    {"kind", row.kind == RowKind::Color ? "color" : row.kind == RowKind::Type ? "type" : "setting"},
                                    {"checked", row.check->GetValue()}, {"enabled", row.check->IsEnabled()}, {"bold", row.check_label->bold}});
                }
                if (sub.item != nullptr && !sub.item->IsShown()) continue;
                if (rows.empty() && sub.item == nullptr) continue;
                subs.push_back({{"title", sub.title}, {"icon", sub.icon}, {"rows", rows}});
            }
            c["subs"] = subs;
            return c;
        };
        json cats = json::array(), mixed = json::array();
        for (size_t i = 0; i < sec.categories.size(); ++i) {
            json c = cat_json(sec.categories[i]);
            c["indicator"] = i < sec.tabs->indicators.size() && sec.tabs->indicators[i];
            cats.push_back(c);
        }
        for (size_t i = 0; i < sec.mixed_categories.size(); ++i) {
            json c = cat_json(sec.mixed_categories[i]);
            c["indicator"] = sec.mixed_tabs && i < sec.mixed_tabs->indicators.size() && sec.mixed_tabs->indicators[i];
            mixed.push_back(c);
        }
        json js = {{"title", sec.title}, {"icon", sec.icon_name}, {"categories", cats}, {"mixed", mixed},
                   {"selected_inner", sec.selected_inner}, {"selected_mixed", sec.selected_mixed},
                   {"indicator", s < m_outer_tabs->indicators.size() && m_outer_tabs->indicators[s]}};
        if (sec.variant_switch != nullptr) {
            json opts = json::array();
            for (const wxString &o : sec.variant_switch->options) opts.push_back(o.ToStdString());
            js["switch"] = opts;
        }
        sections.push_back(js);
    }
    return {{"sections", sections},
            {"selected", m_selected_outer},
            {"filter", m_filter_text.text},
            {"pseudo", m_filter_mode == FilterMode::Text ? std::string() : m_pseudo_chip->label.ToStdString()},
            {"filter_mode", int(m_filter_mode)}};
}

std::unique_ptr<PubDialog>             g_dlg;
std::vector<std::unique_ptr<Tab>>      g_tabs;

// the app's settings pages: [{title, groups: [{title, icon, options: [[opt_id, pure key, index]]}]}]
void load_tabs(const json &a)
{
    g_tabs.clear();
    PubApp &app = wxGetApp();
    app.tabs_list.clear();
    auto pages = [](Tab &tab, const json &list) {
        for (const json &p : list) {
            auto page     = std::make_shared<Page>();
            page->m_title = p.value("title", std::string());
            for (const json &g : p.value("groups", json::array())) {
                auto group   = std::make_shared<OptGroup>();
                group->title = g.value("title", std::string());
                group->icon  = g.value("icon", std::string());
                for (const json &o : g.value("options", json::array()))
                    group->m_opt_map[o[0].get<std::string>()] = {o[1].get<std::string>(), o[2].get<int>()};
                page->m_optgroups.push_back(group);
            }
            tab.m_pages.push_back(page);
        }
    };
    auto printer             = std::make_unique<TabPrinter>();
    printer->m_type          = Preset::TYPE_PRINTER;
    printer->m_extruders_count = Core::get().preset_bundle->get_printer_extruder_count();
    pages(*printer, a.value("printer", json::array()));
    auto filament    = std::make_unique<Tab>();
    filament->m_type = Preset::TYPE_FILAMENT;
    pages(*filament, a.value("filament", json::array()));
    auto print    = std::make_unique<Tab>();
    print->m_type = Preset::TYPE_PRINT;
    pages(*print, a.value("print", json::array()));
    // GUI_App::tabs_list: print, filament, printer
    for (auto *t : {print.get(), filament.get(), static_cast<Tab *>(printer.get())}) app.tabs_list.push_back(t);
    g_tabs.push_back(std::move(print));
    g_tabs.push_back(std::move(filament));
    g_tabs.push_back(std::move(printer));
}

PubDialog &dlg()
{
    if (!g_dlg) throw CommandError("the publish dialog is not open");
    return *g_dlg;
}

// MixedFilamentWarningDialog: the unmet dependencies, "Publish anyway" or back to the dialog
bool confirm_mixed(const std::vector<MixedDependencyIssue> &issues)
{
    if (issues.empty()) return true;
    std::string text = _u8L("Some mixed filaments rely on filaments that will not be published:") + "\n";
    for (const MixedDependencyIssue &issue : issues) {
        const std::string mix  = wxString::Format(_L("Filament %d (mixed)"), int(issue.mixed_slot) + 1).ToStdString();
        const std::string comp = wxString::Format(_L("Filament %d"), int(issue.component_slot) + 1).ToStdString();
        text += "\n" + Slic3r::GUI::format(issue.reason == MixedDependencyIssue::Reason::Disabled ? _u8L("%1% needs %2%, which is not enabled.")
                                                                                              : _u8L("%1% needs %2%, whose material will not be published."),
                                        mix, comp);
    }
    text += "\n\n" + _u8L("To publish a mixed filament, enable every filament it uses and choose Full Publish or check its Type requirement.");
    MessageDialog warn(nullptr, text, _u8L("Warning"), wxOK | wxCANCEL | wxICON_WARNING);
    warn.SetButtonLabel(wxID_OK, _u8L("Publish anyway"));
    return warn.ShowModal() == wxID_OK;
}

// Plater::export_published_3mf: the selection and the filtered config as the archive's metadata, the project config
// and the embedded presets left out (SaveStrategy::MinimalPublished); the model keeps its own metadata
json export_published(const std::string &path, const std::vector<std::string> &published_keys,
                      const std::vector<PublishedMaterialEntry> &material_keys)
{
    json j = json::array();
    for (const std::string &key : published_keys) j.push_back(key);
    json jm = json::array();
    for (const PublishedMaterialEntry &e : material_keys)
        jm.push_back({{"material",
                       {{"filament_type", e.filament_type},
                        {"filament_vendor", e.filament_vendor},
                        {"filament_id", e.filament_id},
                        {"setting_id", e.setting_id},
                        {"name", e.preset_name}}},
                      {"slot", e.slot},
                      {"keys", e.keys},
                      {"full", e.full},
                      {"full_keys", e.full_keys},
                      {"publish_type", e.publish_type},
                      {"type", e.publish_type_value},
                      {"publish_color", e.publish_color},
                      {"color", e.color}});

    Model &model = Plater::get().model();
    const std::shared_ptr<ModelInfo> prev_info = model.model_info;
    model.model_info = prev_info ? std::make_shared<ModelInfo>(*prev_info) : std::make_shared<ModelInfo>();
    model.model_info->metadata_items[ORCA_PUBLISHED_TAG]          = "1";
    model.model_info->metadata_items[ORCA_PUBLISHED_KEYS_TAG]     = j.dump();
    model.model_info->metadata_items[ORCA_PUBLISHED_MATERIAL_TAG] = jm.dump();
    bool ok = false;
    try {
        DynamicPrintConfig full_cfg     = Core::get().preset_bundle->full_config_secure();
        DynamicPrintConfig filtered_cfg = filter_published_config(full_cfg, published_keys, material_keys);
        std::string payload;
        for (const std::string &key : filtered_cfg.keys()) {
            std::string value = filtered_cfg.opt_serialize(key);
            if (value.find('\n') != std::string::npos) {
                BOOST_LOG_TRIVIAL(warning) << "publish: dropping key \"" << key << "\" from the published payload (value contains a newline)";
                continue;
            }
            payload += key + " = " + value + "\n";
        }
        model.model_info->metadata_items[ORCA_PUBLISHED_CONFIG_TAG] = std::move(payload);
        SaveStrategy strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::Silence | SaveStrategy::MinimalPublished;
        if (Core::get().app_config->get_bool("export_sources_full_pathnames")) strategy = strategy | SaveStrategy::FullPathSources;
        write_3mf(path, strategy, -1);
        ok = true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "publish: " << e.what();
    }
    model.model_info = prev_info;
    if (!ok) {
        MessageDialog(nullptr, _u8L("Failed to export the published 3MF file.\nPlease check whether the folder exists online or if other programs have the file open."),
                      _u8L("Publish"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    }
    return true;
}

Commands reg({
    // MainFrame::publish_project: the dialog seeded from the session's selection
    {"publish.open", [](const json &a) -> json {
         load_tabs(a);
         const PendingPublished &pending = pending_published();
         g_dlg = std::make_unique<PubDialog>(pending.has ? &pending.keys : nullptr, pending.has ? &pending.material_keys : nullptr);
         json st = g_dlg->state();
         st["strings"] = {{"title", _u8L("Publish 3MF...")},
                          {"message", _u8L("Select which settings to be published in the 3MF file")},
                          {"hint", _u8L("Type to filter...")},
                          {"all", _u8L("All")},
                          {"none", _u8L("None")},
                          {"select_all", _u8L("Select All")},
                          {"deselect_all", _u8L("Deselect All")},
                          {"select_visible", _u8L("Select visible")},
                          {"deselect_visible", _u8L("Deselect visible")},
                          {"filter_selected", _u8L("Filter selected")},
                          {"filter_nonselected", _u8L("Filter non-selected")},
                          {"enable", _u8L("Enable")},
                          {"full", _u8L("Full Publish")},
                          {"wiki", _u8L("Wiki Guide")},
                          {"video", _u8L("Video Guide")}};
         const std::string name = Plater::get().project_name.empty() ? _u8L("Untitled") : Plater::get().project_name;
         st["file_name"] = name + ".published.3mf";   // Plater::priv::get_export_file(FT_3MF, published)
         return st;
     }},
    {"publish.action", [](const json &a) -> json {
         PubDialog &d = dlg();
         const std::string what = a.value("what", std::string());
         if (what == "check") {
             const size_t r = a.at("row");
             if (r < d.m_rows.size() && d.m_rows[r].check->IsEnabled()) { d.m_rows[r].check->SetValue(a.value("value", false)); d.refresh_tab_indicators(); }
         } else if (what == "enable") {
             const size_t c = a.at("category");
             if (c < d.m_categories.size() && d.m_categories[c].enable_check) { d.m_categories[c].enable_check->SetValue(a.value("value", false)); d.on_enable_toggle(c); }
         } else if (what == "full") {
             const size_t c = a.at("category");
             if (c < d.m_categories.size() && d.m_categories[c].full_check) { d.m_categories[c].full_check->SetValue(a.value("value", false)); d.on_full_toggle(c); }
         } else if (what == "select_all") d.select_all(a.value("value", true));
         else if (what == "select_visible") d.select_visible(a.value("value", true));
         else if (what == "filter") { d.m_filter_ctrl->ChangeValue(a.value("text", std::string())); d.apply_filter(d.m_filter_ctrl->GetValue()); }
         else if (what == "pseudo") {
             // the menu's check items: the active one again returns to the text filter
             const bool selected = a.value("selected", true);
             if (d.m_filter_mode == (selected ? PubDialog::FilterMode::SelectedOnly : PubDialog::FilterMode::UnselectedOnly)) d.apply_filter(d.m_filter_ctrl->GetValue());
             else d.apply_pseudo_filter(selected);
         } else if (what == "text_mode") d.apply_filter(d.m_filter_ctrl->GetValue());
         else if (what == "outer") d.show_outer_page(a.value("index", 0));
         else if (what == "inner") { d.m_sections[a.value("section", 0)].tabs->SelectItem(a.value("index", 0)); d.show_inner_page(a.value("section", 0), a.value("index", 0)); }
         else if (what == "mixed") d.show_mixed_page(a.value("section", 0), a.value("index", 0));
         return d.state();
     }},
    // OK: the mixed filament warning, then the session's selection; the app asks where to save
    {"publish.ok", [](const json &) -> json {
         PubDialog &d = dlg();
         if (!confirm_mixed(d.unpublished_mixed_components())) return {{"ok", false}};
         pending_published() = {true, d.GetPublishedKeys(), d.GetPublishedMaterialKeys()};
         g_dlg.reset();
         return {{"ok", true}};
     }},
    {"publish.cancel", [](const json &) -> json { g_dlg.reset(); return nullptr; }},
    {"publish.export", [](const json &a) -> json {
         const PendingPublished &p = pending_published();
         if (!export_published(a.at("path"), p.keys, p.material_keys)) return {{"ok", false}};
         if (a.contains("recent")) add_to_recent_projects(a["recent"].get<std::string>());
         return {{"ok", true}};
     }},
});

}} // namespace orca::pub
