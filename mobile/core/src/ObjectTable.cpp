// The parameter table (ObjectTableDialog / ObjectGridTable): a row per object and per part, the columns of
// ObjectGridTable::init_cols (printable, plate, name, filament, layer height, wall loops, infill density, support, brim
// type, outer wall speed) with the process value shown where the item does not override it, a reset per override and
// the category filter (All / Quality / Strength / Support / Speed). Edits go through the objects-mode settings.
#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

namespace {

struct Col { const char *key, *label, *category; const char *type; bool only_object; };
// ObjectGridTable::init_cols
const std::vector<Col> &cols()
{
    static const std::vector<Col> c = {
        {"printable", L("Printable"), "all", "bool", true},
        {"plate_index", L("Plate"), "all", "string", true},
        {"name", L("Name"), "all", "string", false},
        {"extruder", L("Filament"), "all", "enum", false},
        {"layer_height", L("Layer height"), L("Quality"), "float", true},
        {"wall_loops", L("Wall loops"), L("Strength"), "int", false},
        {"sparse_infill_density", L("Infill density(%)"), L("Strength"), "percent", false},
        {"enable_support", L("Support"), L("Support"), "bool", true},
        {"brim_type", L("Brim type"), L("Support"), "enum", true},
        {"outer_wall_speed", L("Outer wall speed"), L("Speed"), "float", false},
    };
    return c;
}

const char *brim_keys[] = {"auto_brim", "brim_ears", "painted", "outer_only", "inner_only", "outer_and_inner", "no_brim"};
const char *brim_labels[] = {L("Auto"), L("Mouse ear"), L("Painted"), L("Outer brim only"), L("Inner brim only"), L("Outer and inner brim"), L("No-brim")};

json value_of(const DynamicPrintConfig &global, const ModelObject *mo, const ModelVolume *mv, const std::string &key, bool &overridden)
{
    const ModelConfig *local = mv ? &mv->config : &mo->config;
    overridden = local->has(key);
    const ConfigOption *opt = local->has(key) ? local->option(key) : nullptr;
    if (!opt && mv && mo->config.has(key)) opt = mo->config.option(key);   // a part shows its object's value as default
    if (!opt) opt = global.option(key);
    if (!opt) return nullptr;
    if (key == "brim_type") return opt->serialize();
    if (auto *p = dynamic_cast<const ConfigOptionPercent *>(opt)) return p->value;
    if (auto *f = dynamic_cast<const ConfigOptionFloat *>(opt)) return f->value;
    if (auto *i = dynamic_cast<const ConfigOptionInt *>(opt)) return i->value;
    if (auto *b = dynamic_cast<const ConfigOptionBool *>(opt)) return b->value;
    return opt->serialize();
}

json table_get(const json &)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    const Model &model = p.model();
    const DynamicPrintConfig &global = core.preset_bundle->prints.get_edited_preset().config;
    json rows = json::array();
    for (int oi = 0; oi < int(model.objects.size()); ++oi) {
        const ModelObject *mo = model.objects[oi];
        auto row = [&](const ModelVolume *mv, int vi) {
            json values = json::object(), overrides = json::array();
            for (const Col &c : cols()) {
                const std::string k = c.key;
                if (k == "printable" || k == "plate_index" || k == "name") continue;
                if (c.only_object && mv) continue;
                bool ov = false;
                if (k == "extruder") {
                    const ModelConfig &cfg = mv ? mv->config : mo->config;
                    values[k] = cfg.has("extruder") ? cfg.opt_int("extruder") : 0;
                    if (cfg.has("extruder") && cfg.opt_int("extruder") != 0) overrides.push_back(k);
                    continue;
                }
                values[k] = value_of(global, mo, mv, k, ov);
                if (ov) overrides.push_back(k);
            }
            const int plate = p.plate_of(oi, 0);
            rows.push_back({{"obj", oi}, {"vol", vi}, {"name", mv ? mv->name : mo->name}, {"printable", mo->instances.empty() || mo->instances[0]->printable},
                            {"plate", plate >= 0 ? std::to_string(plate + 1) : _u8L("Outside")}, {"values", values}, {"overrides", overrides},
                            {"part_type", mv ? (mv->is_model_part() ? "part" : mv->is_negative_volume() ? "negative" : "modifier") : ""}});
        };
        row(nullptr, -1);
        if (mo->volumes.size() > 1)
            for (int vi = 0; vi < int(mo->volumes.size()); ++vi) row(mo->volumes[vi], vi);
    }
    json columns = json::array();
    for (const Col &c : cols())
        columns.push_back({{"key", c.key}, {"label", _u8L(c.label)}, {"category", c.category}, {"type", c.type}, {"only_object", c.only_object}});
    json brim = json::array();
    for (int i = 0; i < 7; ++i) brim.push_back({{"value", brim_keys[i]}, {"label", _u8L(brim_labels[i])}});
    json filaments = json::array();
    for (size_t i = 0; i < core.preset_bundle->filament_presets.size(); ++i) {
        const Preset *fp = core.preset_bundle->filaments.find_preset(core.preset_bundle->filament_presets[i]);
        filaments.push_back(std::to_string(i + 1) + ": " + (fp ? fp->config.opt_string("filament_type", 0u) : std::string()));
    }
    return {{"rows", rows}, {"columns", columns}, {"brim_types", brim}, {"filaments", filaments},
            {"categories", {{{"key", "all"}, {"label", _u8L("All")}}, {{"key", "Quality"}, {"label", _u8L("Quality")}},
                            {{"key", "Strength"}, {"label", _u8L("Strength")}}, {{"key", "Support"}, {"label", _u8L("Support")}},
                            {{"key", "Speed"}, {"label", _u8L("Speed")}}}},
            {"title", _u8L("Object/Part Setting")}, {"default", _u8L("Default")}, {"reset_all", _u8L("Reset All")}};
}

// ObjectGridTable::SetValue
json table_set(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    Model &model = p.model();
    const int oi = a.at("obj"), vi = a.value("vol", -1);
    if (oi < 0 || oi >= int(model.objects.size())) throw CommandError("bad object");
    ModelObject *mo = model.objects[oi];
    const std::string key = a.at("key");
    if (key == "name") {
        p.take_snapshot("Rename");
        const std::string name = a.at("value");
        if (vi >= 0) mo->volumes.at(vi)->name = name; else mo->name = name;
        p.changed();
    } else if (key == "printable") {
        p.take_snapshot("Set Printable");
        for (ModelInstance *mi : mo->instances) mi->printable = a.at("value").get<bool>();
        p.changed();
    } else if (key == "extruder") {
        p.take_snapshot("Change Extruder");
        ModelConfig &cfg = vi >= 0 ? mo->volumes.at(vi)->config : mo->config;
        const int e = a.at("value");
        cfg.set_key_value("extruder", new ConfigOptionInt(e));
        p.changed();
    } else {
        // the settings take serialized values
        const json &v = a.at("value");
        const std::string value = v.is_string() ? v.get<std::string>() : v.is_boolean() ? std::string(v.get<bool>() ? "1" : "0")
                                : v.is_number_integer() ? std::to_string(v.get<long long>()) : float_to_string_decimal_point(v.get<double>());
        core.call("objconfig.set", {{"obj", oi}, {"vol", vi}, {"key", key}, {"value", value}});
    }
    return table_get(a);
}

// the reset icon of a cell, or "Reset All" of the category
json table_reset(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    Model &model = p.model();
    if (a.contains("category")) {
        const std::string cat = a["category"];
        p.take_snapshot("Reset Options");
        for (ModelObject *mo : model.objects)
            for (const Col &c : cols()) {
                if (cat != "all" && cat != c.category) continue;
                const std::string k = c.key;
                if (k == "printable" || k == "plate_index" || k == "name" || k == "extruder") continue;
                mo->config.erase(k);
                for (ModelVolume *mv : mo->volumes) mv->config.erase(k);
            }
        p.changed();
        return table_get(a);
    }
    const int oi = a.at("obj"), vi = a.value("vol", -1);
    const std::string key = a.at("key");
    if (key == "extruder") {
        p.take_snapshot("Reset Options");
        ModelConfig &cfg = vi >= 0 ? model.objects.at(oi)->volumes.at(vi)->config : model.objects.at(oi)->config;
        cfg.erase("extruder");
        p.changed();
    } else
        core.call("objconfig.reset", {{"obj", oi}, {"vol", vi}, {"key", key}});
    return table_get(a);
}

} // namespace

static Commands reg({
    {"table.get", table_get},
    {"table.set", table_set},
    {"table.reset", table_reset},
});

} // namespace orca
