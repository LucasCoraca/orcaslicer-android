// Height range modifiers (ObjectList::layers_editing / add_layer_range_after_current / del_layer_range /
// edit_layer_range and the ObjectLayers panel): an object's layer_config_ranges, each with its own layer height,
// filament and region settings.
#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

namespace {

ModelObject *object_of(const json &a)
{
    Model &m = Plater::get().model();
    const int o = a.at("obj");
    if (o < 0 || o >= int(m.objects.size())) throw CommandError("bad object");
    return m.objects[o];
}

t_layer_height_range range_of(const json &r) { return {r[0].get<double>(), r[1].get<double>()}; }

const DynamicPrintConfig &print_config() { return Core::get().preset_bundle->prints.get_edited_preset().config; }
const DynamicPrintConfig &printer_config() { return Core::get().preset_bundle->printers.get_edited_preset().config; }

// ObjectList::get_default_layer_config
DynamicPrintConfig default_layer_config(const ModelObject *mo)
{
    DynamicPrintConfig config;
    const coordf_t layer_height = mo->config.has("layer_height") ? mo->config.opt_float("layer_height") : print_config().opt_float("layer_height");
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    config.set_key_value("extruder", new ConfigOptionInt(0));
    return config;
}

double get_min_layer_height(const int extruder_idx) { return printer_config().opt_float("min_layer_height", std::max(0, extruder_idx - 1)); }

double get_max_layer_height(const int extruder_idx)
{
    const int extruder_idx_zero_based = std::max(0, extruder_idx - 1);
    double max_layer_height = printer_config().opt_float("max_layer_height", extruder_idx_zero_based);
    // In case max_layer_height is set to zero, it should default to 75 % of nozzle diameter:
    if (max_layer_height < EPSILON) max_layer_height = 0.75 * printer_config().opt_float("nozzle_diameter", extruder_idx_zero_based);
    return max_layer_height;
}

// the ObjectLayers panel: one row per range
json layers_state(const json &a)
{
    ModelObject *mo = object_of(a);
    json ranges = json::array();
    for (const auto &[range, cfg] : mo->layer_config_ranges)
        ranges.push_back({{"min", range.first}, {"max", range.second},
                          {"layer_height", cfg.has("layer_height") ? cfg.opt_float("layer_height") : print_config().opt_float("layer_height")},
                          {"extruder", cfg.has("extruder") ? cfg.opt_int("extruder") : 0}});
    json st = Plater::get().state();
    st["layers"] = {{"obj", a.at("obj")}, {"ranges", ranges},
                    {"labels", {{"title", _u8L("Height ranges")}, {"start", _u8L("Start height")}, {"end", _u8L("End height")},
                                {"layer_height", _u8L("Layer height")}, {"add", _u8L("Add height range")}, {"delete", _u8L("Remove height range")},
                                {"layers", _u8L("Layers")}}}};
    return st;
}

// ObjectList::layers_editing: a first range 0-2 mm when there is none
json layers_add(const json &a)
{
    ModelObject *mo = object_of(a);
    Plater &p = Plater::get();
    if (print_config().opt_bool("enable_mixed_color_sublayer") && Core::get().app_config->get("no_warn_mixed_sublayer_variable_layer") != "1") {
        json st = layers_state(a);
        st["notice"] = _u8L("Using variable layer height together with mixed color sublayer may result in poor color mixing quality.");
        if (mo->layer_config_ranges.empty()) {
            p.take_snapshot(_u8L("Add layers"));
            mo->layer_config_ranges[{0.0f, 2.0f}].assign_config(default_layer_config(mo));
            p.changed();
            st = layers_state(a);
        }
        return st;
    }
    if (mo->layer_config_ranges.empty()) {
        p.take_snapshot(_u8L("Add layers"));
        mo->layer_config_ranges[{0.0f, 2.0f}].assign_config(default_layer_config(mo));
        p.changed();
    }
    return layers_state(a);
}

// ObjectList::add_layer_range_after_current
json layers_add_after(const json &a)
{
    ModelObject *mo = object_of(a);
    Plater &p = Plater::get();
    const t_layer_height_range current_range = range_of(a.at("range"));
    auto &ranges = mo->layer_config_ranges;
    auto it_range = ranges.find(current_range);
    if (it_range == ranges.end()) return layers_state(a);
    auto it_next_range = it_range;
    bool changed = false;
    if (++it_next_range == ranges.end()) {
        // Adding a new layer height range after the last one.
        p.take_snapshot("Add Height Range");
        changed = true;
        const t_layer_height_range new_range = {current_range.second, current_range.second + 2.};
        ranges[new_range].assign_config(default_layer_config(mo));
    } else if (const std::pair<coordf_t, coordf_t> next_range = it_next_range->first; current_range.second <= next_range.first) {
        if (current_range.second == next_range.first) {
            // Splitting the next layer height range to two.
            const auto old_config = ranges.at(next_range);
            const coordf_t delta = next_range.second - next_range.first;
            const coordf_t old_min_layer_height = get_min_layer_height(old_config.opt_int("extruder"));
            const coordf_t new_min_layer_height = get_min_layer_height(0);
            if (delta >= old_min_layer_height + new_min_layer_height - EPSILON) {
                const coordf_t middle_layer_z = (new_min_layer_height > 0.5 * delta) ? next_range.second - new_min_layer_height
                                                                                     : next_range.first + std::max(old_min_layer_height, 0.5 * delta);
                p.take_snapshot("Add Height Range");
                changed = true;
                ranges.erase(next_range);
                ranges[{middle_layer_z, next_range.second}] = old_config;
                ranges[{current_range.second, middle_layer_z}].assign_config(default_layer_config(mo));
            }
        } else if (next_range.first - current_range.second >= get_min_layer_height(0) - EPSILON) {
            // Filling in a gap between the current and a new layer height range with a new one.
            p.take_snapshot("Add Height Range");
            changed = true;
            ranges[{current_range.second, next_range.first}].assign_config(default_layer_config(mo));
        }
    }
    if (changed) p.changed();
    return layers_state(a);
}

// ObjectList::del_layer_range
json layers_delete(const json &a)
{
    ModelObject *mo = object_of(a);
    Plater &p = Plater::get();
    p.take_snapshot("Delete Height Range");
    mo->layer_config_ranges.erase(range_of(a.at("range")));
    p.changed();
    return layers_state(a);
}

// ObjectLayers::create_layer / LayerRangeEditor: a new start / end (edit_layer_range(range, new_range)) or layer height
json layers_edit(const json &a)
{
    ModelObject *mo = object_of(a);
    Plater &p = Plater::get();
    const t_layer_height_range range = range_of(a.at("range"));
    auto &ranges = mo->layer_config_ranges;
    auto it = ranges.find(range);
    if (it == ranges.end()) return layers_state(a);
    if (a.contains("layer_height")) {
        const double layer_height = a["layer_height"];
        ModelConfig &config = it->second;
        const int extruder_idx = config.has("extruder") ? config.opt_int("extruder") : 0;
        if (std::abs(layer_height - config.opt_float("layer_height")) >= EPSILON) {
            if (layer_height >= get_min_layer_height(extruder_idx) && layer_height <= get_max_layer_height(extruder_idx)) {
                p.take_snapshot("Edit Height Range");
                config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
                p.changed();
            } else {
                json st = layers_state(a);
                st["notice"] = _u8L("Layer height is out of the range allowed by the printer.");
                return st;
            }
        }
    }
    if (a.contains("min") || a.contains("max")) {
        // LayerRangeEditor: the start must stay below the end, and ranges must not overlap
        t_layer_height_range new_range = {a.value("min", range.first), a.value("max", range.second)};
        if (new_range.first >= new_range.second) return layers_state(a);
        for (const auto &[r, cfg] : ranges)
            if (r != range && new_range.first < r.second && r.first < new_range.second) {
                json st = layers_state(a);
                st["notice"] = _u8L("Height ranges must not overlap.");
                return st;
            }
        p.take_snapshot("Edit Height Range");
        ModelConfig config = std::move(it->second);
        ranges.erase(it);
        ranges[new_range] = std::move(config);
        p.changed();
    }
    if (a.contains("extruder")) {
        p.take_snapshot("Change Extruder");
        it = ranges.find(range);
        if (it != ranges.end()) it->second.set_key_value("extruder", new ConfigOptionInt(a["extruder"].get<int>()));
        p.changed();
    }
    return layers_state(a);
}

} // namespace

static Commands reg({
    {"layers.state", layers_state},
    {"layers.add", layers_add},
    {"layers.add_after", layers_add_after},
    {"layers.delete", layers_delete},
    {"layers.edit", layers_edit},
});

} // namespace orca
