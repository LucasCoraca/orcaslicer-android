// EditGCodeDialog (the custom G-code editors of the settings): the built-in placeholder tree and the description of a
// placeholder, compiled from upstream on a stand-in dialog whose list records its groups / params as JSON. The
// "Presets" subgroups are the app's settings pages, which upstream reads from its Tabs.
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Core.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "wx/string.h"

namespace orca { namespace gcodeph {
using namespace Slic3r;
using namespace Slic3r::GUI;

#include "slic3r/GUI/EditGCodeTypes.inc"
#include "slic3r/GUI/EditGCode.inc.fns"
#include "slic3r/GUI/EditGCode.inc.wrap"

struct wxDataViewItem
{
    int  id{-1};
    bool IsOk() const { return id >= 0; }
};
struct wxDataViewEvent {};

// ParamsViewCtrl / ParamsModel: the nodes in order, a param's text as ParamsNode makes it
struct ParamsList
{
    struct Node
    {
        std::string      text, icon, key;
        int              parent{-1};
        bool             group{false}, expand{false};
        std::vector<int> children;
    };
    std::vector<Node> nodes;
    std::vector<int>  roots;
    std::string       sel_key, sel_category;

    wxDataViewItem add(Node n)
    {
        const int id = int(nodes.size());
        if (n.parent < 0) roots.push_back(id); else nodes[n.parent].children.push_back(id);
        nodes.push_back(std::move(n));
        return {id};
    }
    wxDataViewItem AppendGroup(const wxString &group_name, const std::string &icon_name) { return add({group_name.ToStdString(), icon_name, {}, -1, true}); }
    wxDataViewItem AppendSubGroup(wxDataViewItem parent, const wxString &sub_group_name, const std::string &icon_name)
    {
        if (!parent.IsOk()) return {};
        return add({sub_group_name.ToStdString(), icon_name, {}, parent.id, true});
    }
    wxDataViewItem AppendParam(wxDataViewItem parent, ParamType param_type, const std::string &param_key)
    {
        if (!parent.IsOk()) return {};
        std::string text = param_key;
        if (param_type == ParamType::Vector) text += "[]";
        else if (param_type == ParamType::FilamentVector) text += "[current_extruder]";
        const char *icon = param_type == ParamType::Vector ? "custom-gcode_vector" :
                           param_type == ParamType::FilamentVector ? "custom-gcode_vector-index" : "custom-gcode_single";
        return add({text, icon, param_key, parent.id});
    }
    void Expand(wxDataViewItem item) { if (item.IsOk()) nodes[item.id].expand = true; }
    std::string GetSelectedParamKey() const { return sel_key; }
    std::string GetSelectedTopLevelCategory() const { return sel_category; }

    json to_json(int id) const
    {
        const Node &n = nodes[id];
        json j = {{"text", n.text}, {"icon", n.icon}};
        if (n.group) {
            j["expand"] = n.expand;
            json c = json::array();
            for (int k : n.children) c.push_back(to_json(k));
            j["children"] = std::move(c);
        } else
            j["key"] = n.key;
        return j;
    }
    json tree() const
    {
        json out = json::array();
        for (int r : roots) out.push_back(to_json(r));
        return out;
    }
};

// Tab / Page / ConfigOptionsGroup of add_presets_placeholders: the app's settings pages
struct OptGroup
{
    std::map<std::string, int> keys;
    const std::map<std::string, int> &opt_map() const { return keys; }
};
struct Page
{
    wxString                               m_title;
    int                                    m_icon{0};
    std::vector<std::shared_ptr<OptGroup>> m_optgroups;
    const wxString &title() const { return m_title; }
    int             iconID() const { return m_icon; }
};
struct Tab
{
    Preset::Type                       m_type;
    std::vector<std::shared_ptr<Page>> m_pages;
    std::map<std::string, int>         m_icon_index;
};
struct GPlater { PrinterTechnology printer_technology() const { return ptFFF; } };
struct GApp
{
    PresetBundle      *preset_bundle{nullptr};
    std::vector<Tab *> tabs_list;
    GPlater            plater_;
    GPlater           *plater() { return &plater_; }
};
static GApp g_app;
GApp &wxGetApp() { return g_app; }

struct Label { wxString text; void SetLabel(const wxString &s) { text = s; } };

struct EditGCodeDialog
{
    std::unique_ptr<ParamsList> list = std::make_unique<ParamsList>();
    ParamsList *m_params_list{list.get()};
    Label       label, description;
    Label      *m_param_label{&label};
    Label      *m_param_description{&description};

    ReadOnlySlicingStatesConfigDef  cgp_ro_slicing_states_config_def;
    ReadWriteSlicingStatesConfigDef cgp_rw_slicing_states_config_def;
    OtherSlicingStatesConfigDef     cgp_other_slicing_states_config_def;
    PrintStatisticsConfigDef        cgp_print_statistics_config_def;
    ObjectsInfoConfigDef            cgp_objects_info_config_def;
    DimensionsConfigDef             cgp_dimensions_config_def;
    TemperaturesConfigDef           cgp_temperatures_config_def;
    TimestampsConfigDef             cgp_timestamps_config_def;
    OtherPresetsConfigDef           cgp_other_presets_config_def;

    void           init_params_list(const std::string &custom_gcode_name);
    wxDataViewItem add_presets_placeholders();
    void           selection_changed(wxDataViewEvent &evt);
    void           Layout() {}
};

#include "slic3r/GUI/EditGCode.inc"

// the app's pages: [{type, pages: [{title, icon, keys}]}]
std::vector<std::unique_ptr<Tab>> make_tabs(const json &a)
{
    std::vector<std::unique_ptr<Tab>> tabs;
    if (!a.contains("tabs")) return tabs;
    for (const json &t : a["tabs"]) {
        auto tab = std::make_unique<Tab>();
        const std::string type = t.value("type", "print");
        tab->m_type = type == "filament" ? Preset::TYPE_FILAMENT : type == "printer" ? Preset::TYPE_PRINTER : Preset::TYPE_PRINT;
        for (const json &p : t.value("pages", json::array())) {
            auto page = std::make_shared<Page>();
            page->m_title = p.value("title", "");
            const std::string icon = p.value("icon", "");
            if (!tab->m_icon_index.count(icon)) tab->m_icon_index.emplace(icon, int(tab->m_icon_index.size()) + 1);
            page->m_icon = tab->m_icon_index[icon];
            auto group = std::make_shared<OptGroup>();
            for (const json &k : p.value("keys", json::array())) group->keys.emplace(k.get<std::string>(), 0);
            page->m_optgroups.push_back(group);
            tab->m_pages.push_back(page);
        }
        tabs.push_back(std::move(tab));
    }
    return tabs;
}

json placeholders(const json &a)
{
    const std::string key = a.value("key", "");
    auto tabs = make_tabs(a);
    g_app.preset_bundle = Core::get().preset_bundle.get();
    g_app.tabs_list.clear();
    for (auto &t : tabs) g_app.tabs_list.push_back(t.get());
    EditGCodeDialog d;
    d.init_params_list(key);
    g_app.tabs_list.clear();
    return {{"tree", d.m_params_list->tree()},
            {"title", Slic3r::GUI::format(_L("Edit Custom G-code (%1%)"), key)},
            {"label_top", _u8L("Built-in placeholders (Double click item to add to G-code)") + ":"},
            {"search", _u8L("Search G-code placeholders")},
            {"select", _u8L("Select placeholder")},
            {"add_tip", _u8L("Add selected placeholder to G-code")},
            {"presets", _u8L("Presets")}};
}

// selection_changed: the label and the description of a placeholder
json describe(const json &a)
{
    g_app.preset_bundle = Core::get().preset_bundle.get();
    EditGCodeDialog d;
    d.m_params_list->sel_key = a.value("key", "");
    d.m_params_list->sel_category = a.value("category", "");
    wxDataViewEvent e;
    d.selection_changed(e);
    return {{"label", d.label.text.ToStdString()}, {"description", d.description.text.ToStdString()}};
}

}} // namespace orca::gcodeph

namespace orca {
static Commands reg({
    {"gcode.placeholders", gcodeph::placeholders},
    {"gcode.describe", gcodeph::describe},
});
} // namespace orca
