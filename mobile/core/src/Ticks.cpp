// The custom G-code of the preview's layer slider (IMSlider: pause, custom G-code, custom template, filament change)
// with upstream's TickCodeInfo (slic3r/GUI/TickCode.cpp, compiled unchanged), stored per plate in
// Model::plates_custom_gcodes like Plater's EVT_CUSTOMEVT_TICKSCHANGED handler does.
#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Preview.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/TickCode.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::CustomGCode;

namespace orca {

namespace {

// IMSlider's static gcode(type)
std::string gcode(Type type)
{
    const DynamicPrintConfig &config = Core::get().full_config();
    switch (type) {
    case Template: return config.opt_string("template_custom_gcode");
    case PausePrint: return config.opt_string("machine_pause_gcode");
    default: return "";
    }
}

struct Ctx
{
    int                      plate;
    std::vector<double>      zs;       // IMSlider::m_values: the layers' print_z
    std::vector<std::string> colors;   // m_extruder_colors
    TickCodeInfo             ticks;
};

// IMSlider::SetTicksValues / SetModeAndOnlyExtruder for the plate's sliced layers
void load(Ctx &c, const json &a)
{
    Plater &p = Plater::get();
    c.plate = a.value("plate", p.current_plate);
    for (auto &z : a.at("layers")) c.zs.push_back(z.get<double>());
    if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour")) c.colors = fc->values;
    c.ticks.set_extruder_colors(&c.colors);
    c.ticks.set_pause_print_msg(_u8L("Pause:"));
    Info &info = p.model().plates_custom_gcodes[c.plate];
    c.ticks.mode = info.mode != Undef ? info.mode : (c.colors.size() > 1 ? MultiAsSingle : SingleExtruder);
    auto tick_of = [&](double z) {   // get_tick_from_value(z, true)
        auto it = std::lower_bound(c.zs.begin(), c.zs.end(), z - EPSILON);
        return it == c.zs.end() ? -1 : int(it - c.zs.begin());
    };
    for (const Item &h : info.gcodes)
        if (int t = tick_of(h.print_z); t >= 0) c.ticks.ticks.emplace(TickCode{t, h.type, h.extruder, h.color, h.extra});
}

// GetTicksValues + the ticks changed handler: store, invalidate the plate's slice
json store(Ctx &c, Type changed)
{
    Plater &p = Plater::get();
    Info info;
    info.mode = c.ticks.mode;
    for (const TickCode &t : c.ticks.ticks)
        if (t.tick < int(c.zs.size())) info.gcodes.push_back(Item{c.zs[t.tick], t.type, t.extruder, t.color, t.extra});
    p.take_snapshot("Custom G-code");
    p.model().plates_custom_gcodes[c.plate] = info;
    if (c.plate < int(p.plates.size())) p.plates[c.plate].slice_valid = false;
    p.dirty = true;
    json st = p.state();
    Host::event("project", st);
    return {{"changed", true}, {"type", int(changed)}};
}

json state(const json &a)
{
    Ctx c;
    load(c, a);
    json ticks = json::array();
    for (const TickCode &t : c.ticks.ticks)
        ticks.push_back({{"layer", t.tick}, {"z", t.tick < int(c.zs.size()) ? c.zs[t.tick] : 0.}, {"type", int(t.type)},
                         {"extruder", t.extruder}, {"color", t.color}, {"extra", t.extra}});
    // IMSlider::render_add_menu / render_edit_menu for the given layer
    json menu = json::array();
    const int layer = a.value("layer", -1);
    auto it = c.ticks.ticks.find(TickCode{layer});
    const bool sequential = Core::get().full_config().opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject;
    if (layer >= 0 && it == c.ticks.ticks.end()) {
        menu.push_back({{"label", _u8L("Add Pause")}, {"id", "pause"}, {"enabled", !sequential}, {"tooltip", _u8L("Insert a pause command at the beginning of this layer.")}});
        menu.push_back({{"label", _u8L("Add Custom G-code")}, {"id", "custom"}, {"enabled", !sequential}, {"tooltip", _u8L("Insert custom G-code at the beginning of this layer.")}});
        if (!gcode(Template).empty())
            menu.push_back({{"label", _u8L("Add Custom Template")}, {"id", "template"}, {"enabled", !sequential},
                            {"tooltip", _u8L("Insert template custom G-code at the beginning of this layer.")}});
        menu.push_back({{"label", _u8L("Jump to layer")}, {"id", "jump"}, {"enabled", true}});
        if (c.colors.size() > 1) {
            json sub = json::array();
            for (size_t i = 0; i < c.colors.size(); ++i)
                sub.push_back({{"label", (boost::format(_u8L("Filament %d")) % (i + 1)).str()}, {"id", "tool:" + std::to_string(i + 1)}, {"enabled", true},
                               {"color", c.colors[i]}, {"tooltip", _u8L("Change filament at the beginning of this layer.")}});
            menu.push_back({{"label", _u8L("Change Filament")}, {"enabled", !Core::get().full_config().opt_bool("spiral_mode")}, {"children", sub}});
        }
    } else if (it != c.ticks.ticks.end()) {
        switch (it->type) {
        case PausePrint: menu.push_back({{"label", _u8L("Delete Pause")}, {"id", "delete"}, {"enabled", true}}); break;
        case Template:
            if (!gcode(Template).empty()) menu.push_back({{"label", _u8L("Delete Custom Template")}, {"id", "delete"}, {"enabled", true}});
            break;
        case Custom:
            menu.push_back({{"label", _u8L("Edit Custom G-code")}, {"id", "edit"}, {"enabled", true}});
            menu.push_back({{"label", _u8L("Delete Custom G-code")}, {"id", "delete"}, {"enabled", true}});
            break;
        case ToolChange:
        case ColorChange: menu.push_back({{"label", _u8L("Delete Filament Change")}, {"id", "delete"}, {"enabled", true}}); break;
        default: break;
        }
    }
    return {{"ticks", ticks}, {"menu", menu}, {"layer", layer}, {"custom", it != c.ticks.ticks.end() && it->type == Custom ? it->extra : ""},
            {"labels", {{"custom_title", _u8L("Custom G-code")}, {"custom_prompt", _u8L("Enter Custom G-code used on current layer:")},
                        {"jump_title", _u8L("Jump to layer")}, {"jump_prompt", _u8L("Please enter the layer number.")}}}};
}

// IMSlider::add_code_as_tick / add_custom_gcode / delete_tick / edit
json change(const json &a)
{
    Ctx c;
    load(c, a);
    const int layer = a.at("layer");
    const std::string what = str_arg(a, "what");
    auto it = c.ticks.ticks.find(TickCode{layer});
    if (what == "delete") {
        if (it == c.ticks.ticks.end()) return nullptr;
        Type t = it->type;
        c.ticks.ticks.erase(it);
        return store(c, t);
    }
    if (what == "custom" || what == "edit") {
        if (it != c.ticks.ticks.end()) c.ticks.ticks.erase(it);
        c.ticks.ticks.emplace(TickCode{layer, Custom, 1, "", a.value("gcode", std::string())});
        return store(c, Custom);
    }
    Type type = what == "pause" ? PausePrint : what == "template" ? Template : what.rfind("tool:", 0) == 0 ? ToolChange : ColorChange;
    const int extruder = what.rfind("tool:", 0) == 0 ? std::stoi(what.substr(5)) : 1;
    if (it == c.ticks.ticks.end()) {
        if (!c.ticks.add_tick(layer, type, extruder, layer < int(c.zs.size()) ? c.zs[layer] : 0.)) return nullptr;
    } else if (type == ToolChange || type == ColorChange) {
        if (!c.ticks.switch_code_for_tick(it, type, extruder)) return nullptr;
    } else
        return nullptr;
    return store(c, type);
}

} // namespace

static Commands reg({
    {"ticks.get", state},
    {"ticks.change", change},
});

} // namespace orca
