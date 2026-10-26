// The object context menus (GUI_Factories.cpp MenuFactory: create_default_menu, create_extra_object_menu +
// object_menu(), create_bbl_part_menu + part_menu(), text / svg part menus, multi_selection_menu) as a JSON tree with
// the desktop's labels, order, separators and enable / check conditions, and the actions behind them. The app shows
// the tree as a popup (3D view long press and the object list), so menu changes upstream are a diff of this file.
//
// A node: {"label", "id", "enabled", "checked"?, "shortcut"?, "icon"?, "children"?} or {"separator": true}.
// "menu.run" {"id", ...} runs an action; actions the app has to finish (dialogs, file pickers) answer {"ui": ...}.
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Menu.hpp"
#include "Edit.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Orient.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {
bool can_reload_from_disk();
bool can_replace_with_stl();

namespace {

struct Menu
{
    json items = json::array();
    void item(const std::string &label, const std::string &id, bool enabled = true, const char *shortcut = nullptr, const char *icon = nullptr)
    {
        json n = {{"label", label}, {"id", id}, {"enabled", enabled}};
        if (shortcut) n["shortcut"] = shortcut;
        if (icon) n["icon"] = icon;
        items.push_back(n);
    }
    void check(const std::string &label, const std::string &id, bool checked, bool enabled = true, const char *shortcut = nullptr)
    {
        json n = {{"label", label}, {"id", id}, {"enabled", enabled}, {"checked", checked}};
        if (shortcut) n["shortcut"] = shortcut;
        items.push_back(n);
    }
    void sub(const std::string &label, const Menu &m, bool enabled = true, const char *icon = nullptr)
    {
        json n = {{"label", label}, {"enabled", enabled}, {"children", m.items}};
        if (icon) n["icon"] = icon;
        items.push_back(n);
    }
    void sep()
    {
        // MenuWithSeparators: no leading or doubled separators
        if (!items.empty() && !items.back().value("separator", false)) items.push_back({{"separator", true}});
    }
};

// ---------------------------------------------------------------------------------------------------------------
// the selection, as MenuFactory sees it (Selection / ObjectList)

struct Sel
{
    std::vector<int> objs;                 // distinct selected objects
    std::vector<std::pair<int, int>> vols; // (obj, vol) of selected parts
    bool full_objects{false};              // only whole objects / instances
    int  obj() const { return objs.size() == 1 ? objs[0] : -1; }
};

Sel selection()
{
    Plater &p = Plater::get();
    Sel s;
    std::set<int> o;
    bool any_vol = false, any_obj = false;
    for (const SelItem &it : p.selection) {
        o.insert(it.obj);
        if (it.vol >= 0) { s.vols.push_back({it.obj, it.vol}); any_vol = true; } else any_obj = true;
    }
    s.objs.assign(o.begin(), o.end());
    s.full_objects = any_obj && !any_vol;
    return s;
}

ModelObject *object(int i)
{
    Model &m = Plater::get().model();
    return i >= 0 && i < int(m.objects.size()) ? m.objects[i] : nullptr;
}

int filaments_count() { return int(Core::get().preset_bundle->filament_presets.size()); }

// Plater::priv::can_split / ObjectList::is_splittable
bool can_split(const Sel &s, bool to_objects)
{
    ModelObject *mo = object(s.obj());
    if (!mo) return false;
    if (to_objects) {
        if (!s.vols.empty()) return false;
        return mo->volumes.size() == 1 ? mo->volumes[0]->is_splittable() : mo->volumes.size() > 1 || mo->is_multiparts();
    }
    if (s.vols.size() == 1) return mo->volumes[s.vols[0].second]->is_splittable();
    return mo->volumes.size() == 1 && mo->volumes[0]->is_splittable();
}

bool can_drop(const Sel &s)
{
    if (s.objs.empty()) return false;
    BoundingBoxf3 b = selection_world_box();
    return b.defined && std::abs(b.min.z()) > EPSILON;
}

bool at_plate_center(const Sel &s)
{
    Plater &p = Plater::get();
    BoundingBoxf3 b = selection_world_box();
    BoundingBoxf3 pb = p.plate_box(p.current_plate);
    return b.defined && b.center().x() == pb.center().x() && b.center().y() == pb.center().y();
}

// ---------------------------------------------------------------------------------------------------------------
// menu pieces (MenuFactory::append_*)

void add_generic(Menu &m, const std::string &type)   // append_submenu_add_generic
{
    if (type != "invalid") {
        m.item(_u8L("Load..."), "add:" + type + ":load", true, nullptr, "menu_load");
        m.sep();
    }
    for (auto [label, key, icon] : std::initializer_list<std::tuple<const char *, const char *, const char *>>{
             {L("Cube"), "Cube", "menu_obj_cube"}, {L("Cylinder"), "Cylinder", "menu_obj_cylinder"}, {L("Sphere"), "Sphere", "menu_obj_sphere"},
             {L("Cone"), "Cone", "menu_obj_cone"}, {L("Disc"), "Disc", "menu_obj_disc"}, {L("Torus"), "Torus", "menu_obj_torus"}})
        m.item(_u8L(label), "add:" + type + ":" + key, true, nullptr, icon);
    // append_menu_item_add_text / add_svg: parts, negative parts, modifiers and new objects only
    if (type == "part" || type == "negative" || type == "modifier" || type == "invalid") {
        m.sep();
        m.item(_u8L("Text"), "add:" + type + ":text", true, nullptr, "menu_obj_text");
        m.sep();
        m.item(_u8L("SVG"), "add:" + type + ":svg", true, nullptr, "menu_obj_svg");
    }
}

} // namespace

// MenuFactory::handy_models (also the Speed Dial's "Add Handy models" commands)
const std::vector<HandyModel> &handy_models()
{
    static const std::vector<HandyModel> models = {
        {"orca_cube", L("Orca Cube"), {"OrcaCube_v2.drc", "OrcaPlug_v2.drc"}, true},
        {"orcasliced_combo", L("OrcaSliced Combo"), {"OrcaSliced.3mf", "OrcaCube_v2.drc", "OrcaPlug_v2.drc"}, true},
        {"orca_badge", L("Orca Badge"), {"OrcaBadge.3mf"}, false},
        {"orca_tolerance_test", L("Orca Tolerance Test"), {"OrcaToleranceTest.drc"}, false},
        {"3dbenchy", L("3DBenchy"), {"3DBenchy.drc"}, false},
        {"cali_cat", L("Cali Cat"), {"calicat.drc"}, false},
        {"autodesk_fdm_test", L("Autodesk FDM Test"), {"ksr_fdmtest_v4.drc"}, false},
        {"voron_cube", L("Voron Cube"), {"Voron_Design_Cube_v7.drc"}, false},
        {"stanford_bunny", L("Stanford Bunny"), {"Stanford_Bunny.drc"}, false},
        {"orca_string_hell", L("Orca String Hell"), {"Orca_stringhell.drc"}, false},
    };
    return models;
}

namespace {

void instance_manipulation(Menu &m, const Sel &s)   // append_menu_items_instance_manipulation
{
    ModelObject *mo = object(s.obj());
    const bool inc = mo != nullptr, dec = mo && mo->instances.size() > 1;
    m.item(_u8L("Add instance"), "instance_add", inc, "+");
    m.item(_u8L("Remove instance"), "instance_remove", dec, "-");
    m.item(_u8L("Set number of instances") + "...", "instances_set", inc);
    m.item(_u8L("Fill bed with instances") + "...", "fill_bed", inc);
}

void mirror(Menu &m, bool enabled)   // append_menu_items_mirror
{
    Menu mm;
    mm.item(_u8L("Along X Axis"), "mirror:x", true, nullptr, "menu_mirror_x");
    mm.item(_u8L("Along Y Axis"), "mirror:y", true, nullptr, "menu_mirror_y");
    mm.item(_u8L("Along Z Axis"), "mirror:z", true, nullptr, "menu_mirror_z");
    m.sub(_u8L("Mirror"), mm, enabled);
}

void split(Menu &m, const Sel &s)
{
    Menu sm;
    sm.item(_u8L("To Objects"), "split:objects", can_split(s, true), nullptr, "menu_split_objects");
    sm.item(_u8L("To Parts"), "split:parts", can_split(s, false), nullptr, "menu_split_parts");
    m.sub(_u8L("Split"), sm, can_split(s, true));
}

void align_distribute(Menu &m, const Sel &s)   // append_menu_item_align_distribute (GLGizmoAlignment)
{
    // aligning needs at least two objects (or two parts of one object); distributing needs three
    const size_t n = s.full_objects ? s.objs.size() : s.vols.size();
    Menu am;
    am.item(_u8L("Distribute left-right") + " (X)", "distribute:x", n >= 3, nullptr, "distribute_x_dark");
    am.item(_u8L("Distribute front-back") + " (Y)", "distribute:y", n >= 3, nullptr, "distribute_y_dark");
    am.item(_u8L("Distribute top-bottom") + " (Z)", "distribute:z", n >= 3, nullptr, "distribute_z_dark");
    am.sep();
    am.item(_u8L("Align left") + " (-X)", "align:x_min", n >= 2, nullptr, "align_x_min_dark");
    am.item(_u8L("Align left-right center") + " (X)", "align:x_center", n >= 2, nullptr, "align_x_center_dark");
    am.item(_u8L("Align right") + " (+X)", "align:x_max", n >= 2, nullptr, "align_x_max_dark");
    am.sep();
    am.item(_u8L("Align front") + " (-Y)", "align:y_min", n >= 2, nullptr, "align_y_min_dark");
    am.item(_u8L("Align front-back center") + " (Y)", "align:y_center", n >= 2, nullptr, "align_y_center_dark");
    am.item(_u8L("Align back") + " (+Y)", "align:y_max", n >= 2, nullptr, "align_y_max_dark");
    am.sep();
    am.item(_u8L("Align bottom") + " (-Z)", "align:z_min", n >= 2, nullptr, "align_z_min_dark");
    am.item(_u8L("Align top-bottom center") + " (Z)", "align:z_center", n >= 2, nullptr, "align_z_center_dark");
    am.item(_u8L("Align top") + " (+Z)", "align:z_max", n >= 2, nullptr, "align_z_max_dark");
    m.sub(_u8L("Align/Distribute"), am, n >= 2);
}

void per_object_process(Menu &m, const Sel &s)
{
    const bool any = !s.objs.empty();
    m.item(_u8L("Edit Process Settings"), "edit_process", any);
    const bool single = s.objs.size() == 1 && (s.vols.size() <= 1);
    m.item(_u8L("Copy Process Settings"), "copy_settings", single);
    static bool has_clipboard = false;
    (void) has_clipboard;
    m.item(_u8L("Paste Process Settings"), "paste_settings", any && !Core::get().app_config->get("mobile_settings_clipboard").empty());
}

void per_object_settings(Menu &m, const Sel &s)
{
    m.item(_u8L("Edit in Parameter Table"), "edit_table", s.objs.size() == 1 && s.vols.size() <= 1);
}

void change_filament(Menu &m, const Sel &s)   // append_menu_item_change_filament
{
    const int cnt = filaments_count();
    if (cnt <= 1 || s.objs.empty()) return;
    ModelObject *mo = object(s.objs.front());
    const bool single = s.objs.size() == 1 && s.vols.size() <= 1;
    const ModelVolume *vol = (single && s.vols.size() == 1 && mo) ? mo->volumes[s.vols[0].second] : nullptr;
    if (vol && vol->type() != ModelVolumeType::MODEL_PART && vol->type() != ModelVolumeType::PARAMETER_MODIFIER) return;
    bool has_modifier = false;
    for (auto &[o, v] : s.vols)
        if (object(o) && object(o)->volumes[v]->type() == ModelVolumeType::PARAMETER_MODIFIER) has_modifier = true;
    Menu fm;
    PresetBundle &pb = *Core::get().preset_bundle;
    auto *colours = Core::get().full_config().option<ConfigOptionStrings>("filament_colour");
    for (int i = has_modifier ? 0 : 1; i <= cnt; ++i) {
        std::string name = _u8L("Default");
        if (i > 0) {
            const Preset *preset = pb.filaments.find_preset(pb.filament_presets[i - 1]);
            name = preset ? preset->label(false) : (boost::format(_u8L("Filament %d")) % i).str();
        }
        json n = {{"label", name}, {"id", "filament:" + std::to_string(i)}, {"enabled", true}};
        if (i > 0 && colours && i - 1 < int(colours->values.size())) n["color"] = colours->values[i - 1];
        fm.items.push_back(n);
    }
    m.sub(single ? _u8L("Change Filament") : _u8L("Set Filament for selected items"), fm);
}

void convert_unit(Menu &m, const Sel &s)   // append_menu_items_convert_unit
{
    if (s.objs.empty()) return;
    auto respects = [](const ModelVolume *v, ConversionType t) {
        return (t == ConversionType::CONV_FROM_INCH && v->source.is_converted_from_inches) ||
               (t == ConversionType::CONV_TO_INCH && !v->source.is_converted_from_inches) ||
               (t == ConversionType::CONV_FROM_METER && v->source.is_converted_from_meters) ||
               (t == ConversionType::CONV_TO_METER && !v->source.is_converted_from_meters);
    };
    auto can = [&](ConversionType t) {
        for (int o : s.objs) {
            ModelObject *mo = object(o);
            if (s.vols.empty()) { for (auto *v : mo->volumes) if (respects(v, t)) return false; }
            else for (auto &[oo, vi] : s.vols) if (oo == o && respects(mo->volumes[vi], t)) return false;
        }
        return true;
    };
    const std::pair<ConversionType, const char *> items[] = {{ConversionType::CONV_FROM_INCH, L("Convert from Inches")},
                                                             {ConversionType::CONV_TO_INCH, L("Restore to Inch")},
                                                             {ConversionType::CONV_FROM_METER, L("Convert from Meters")},
                                                             {ConversionType::CONV_TO_METER, L("Restore to Meter")}};
    for (auto &[t, label] : items)
        if (can(t)) m.item(_u8L(label), "convert:" + std::to_string(int(t)));
}

void change_type(Menu &m, const Sel &s)   // append_menu_item_change_type
{
    std::set<ModelVolumeType> present;
    bool text_or_svg = false;
    for (auto &[o, v] : s.vols) {
        const ModelVolume *mv = object(o)->volumes[v];
        present.insert(mv->is_precise_seam() ? ModelVolumeType::PRECISE_SEAM_CENTER : mv->type());
        text_or_svg |= mv->is_text() || mv->is_svg();
    }
    Menu tm;
    const std::pair<ModelVolumeType, const char *> types[] = {
        {ModelVolumeType::MODEL_PART, L("Part")}, {ModelVolumeType::NEGATIVE_VOLUME, L("Negative Part")},
        {ModelVolumeType::PARAMETER_MODIFIER, L("Modifier")}, {ModelVolumeType::SUPPORT_BLOCKER, L("Support Blocker")},
        {ModelVolumeType::SUPPORT_ENFORCER, L("Support Enforcer")}, {ModelVolumeType::PRECISE_SEAM_CENTER, L("Precise Seam")}};
    for (auto &[t, label] : types) {
        const bool blocked = text_or_svg && (t == ModelVolumeType::SUPPORT_BLOCKER || t == ModelVolumeType::SUPPORT_ENFORCER || t == ModelVolumeType::PRECISE_SEAM_CENTER);
        tm.check(_u8L(label), "type:" + std::to_string(int(t)), present.count(t) > 0, !blocked);
    }
    m.sub(_u8L("Change Type"), tm);
}

void precise_seam_submenu(Menu &m, const Sel &s)   // append_menu_item_precise_seam_submenu
{
    if (s.vols.empty()) return;
    std::set<ModelVolumeType> present;
    for (auto &[o, v] : s.vols) {
        const ModelVolume *mv = object(o)->volumes[v];
        if (!mv->is_precise_seam()) return;
        present.insert(mv->type());
    }
    Menu pm;
    const std::pair<const char *, ModelVolumeType> ps[] = {
        {L("Seam Center"), ModelVolumeType::PRECISE_SEAM_CENTER}, {L("Seam Left"), ModelVolumeType::PRECISE_SEAM_LEFT},
        {L("Seam Right"), ModelVolumeType::PRECISE_SEAM_RIGHT}, {L("Seam Enforced"), ModelVolumeType::PRECISE_SEAM_ENFORCED},
        {L("Seam Blocked"), ModelVolumeType::PRECISE_SEAM_BLOCKED}, {L("Seam Neutral"), ModelVolumeType::PRECISE_SEAM_NEUTRAL}};
    for (auto &[label, t] : ps) pm.check(_u8L(label), "pstype:" + std::to_string(int(t)), present.count(t) > 0);
    m.sub(_u8L("Precise Seam Type"), pm, true, "menu_precise_seam_type");
}

void export_items(Menu &m, const Sel &s, bool multi)
{
    const bool single_ok = s.full_objects && s.objs.size() == 1, multi_ok = s.full_objects && s.objs.size() > 1;
    m.item(_u8L("Export as one STL") + "...", "export_stl", multi ? multi_ok : single_ok);
    if (multi) m.item(_u8L("Export as STLs") + "...", "export_stls", multi_ok);
    m.item(_u8L("Export as one DRC") + "...", "export_drc", multi ? multi_ok : single_ok);
    if (multi) m.item(_u8L("Export as DRCs") + "...", "export_drcs", multi_ok);
}

bool any_text(const Sel &s, bool svg)
{
    if (s.vols.size() != 1) return false;
    const ModelVolume *v = object(s.vols[0].first)->volumes[s.vols[0].second];
    return svg ? v->is_svg() : v->is_text();
}

// ---------------------------------------------------------------------------------------------------------------
// the menus

json default_menu()   // create_default_menu
{
    Menu m, prim, handy;
    add_generic(prim, "invalid");
    for (auto &h : handy_models()) handy.item(_u8L(h.label), std::string("handy:") + h.key);
    m.sub(_u8L("Add Primitive"), prim, true, "menu_add_part");
    m.sub(_u8L("Add Handy models"), handy, true, "menu_add_part");
    m.item(_u8L("Add Models"), "add_models", true, nullptr, "menu_add_part");
    m.sep();
    m.check(_u8L("Show Labels"), "show_labels", Core::get().app_config->get_bool("show_labels"));
    return m.items;
}

json object_menu(const Sel &s)   // create_extra_object_menu + object_menu()
{
    ModelObject *mo = object(s.obj());
    Menu m;
    instance_manipulation(m, s);
    m.sep();
    m.item(mo && mo->instances.size() > 1 ? _u8L("Set as Individual Objects") : _u8L("Set as An Individual Object"), "instance_to_object",
           mo && mo->instances.size() > 1);
    m.sep();
    m.item(_u8L("Clone"), "clone", true, "Ctrl+K");
    m.item(_u8L("Fix Model"), "fix", true);
    m.item(_u8L("Simplify Model"), "simplify", mo != nullptr);
    bool smooth_ok = mo && mo->get_object_stl_stats().open_edges == 0;
    m.item(_u8L("Subdivision mesh") + _u8L("(Lost color)"), "smooth", smooth_ok);
    m.sep();
    m.item(_u8L("Mesh boolean"), "mesh_boolean", mo && mo->volumes.size() > 1);
    m.item(_u8L("Center"), "center", !at_plate_center(s));
    align_distribute(m, s);
    m.item(_u8L("Drop"), "drop", can_drop(s));
    split(m, s);
    mirror(m, s.objs.size() == 1);
    m.item(_u8L("Delete"), "delete", true, "Backspace");
    m.sep();
    // append_menu_items_add_volume
    const std::pair<const char *, const char *> add[] = {{"part", L("Add Part")}, {"negative", L("Add Negative Part")}, {"modifier", L("Add Modifier")},
                                                         {"support_blocker", L("Add Support Blocker")}, {"support_enforcer", L("Add Support Enforcer")},
                                                         {"precise_seam", L("Add Precise Seam")}};
    const char *icons[] = {"menu_add_part", "menu_add_negative", "menu_add_modifier", "menu_support_blocker", "menu_support_enforcer", "menu_precise_seam_add"};
    int k = 0;
    for (auto &[type, label] : add) {
        Menu sm;
        add_generic(sm, type);
        m.sub(_u8L(label), sm, mo != nullptr, icons[k++]);
    }
    m.item(_u8L("Height Range Modifier"), "height_range", mo != nullptr, nullptr, "height_range_modifier");
    m.sep();
    bool printable = mo && !mo->instances.empty() && mo->instances[0]->printable;
    m.check(_u8L("Printable"), "printable", printable, true, "V");
    m.sep();
    m.check(_u8L("Auto Drop"), "auto_drop", mo && mo->auto_drop);
    m.sep();
    per_object_process(m, s);
    per_object_settings(m, s);
    m.sep();
    m.item(_u8L("Reload from disk"), "reload", can_reload_from_disk());
    m.item(_u8L("Replace 3D file") + "...", "replace", can_replace_with_stl());
    m.item(_u8L("Replace all with 3D files") + "...", "replace_all", true);
    export_items(m, s, false);
    // object_menu() additions
    convert_unit(m, s);
    if (mo && mo->is_cut()) m.item(_u8L("Invalidate cut info"), "invalidate_cut");
    change_filament(m, s);
    return m.items;
}

json part_menu(const Sel &s)   // create_bbl_part_menu + part_menu() (text / svg: their own menus)
{
    Menu m;
    const bool text = any_text(s, false), svg = any_text(s, true);
    if (text || svg) {
        m.item(text ? _u8L("Edit text") : _u8L("Edit SVG"), text ? "edit_text" : "edit_svg", true, nullptr, "cog");
        m.item(_u8L("Delete"), "delete", true, "Backspace");
        m.item(_u8L("Fix Model"), "fix");
        m.item(_u8L("Simplify Model"), "simplify");
        if (text) { m.item(_u8L("Center"), "center", !at_plate_center(s)); align_distribute(m, s); }
        mirror(m, true);
        m.sep();
        per_object_process(m, s);
        per_object_settings(m, s);
        change_type(m, s);
        change_filament(m, s);
        return m.items;
    }
    m.item(_u8L("Delete"), "delete", true, "Backspace");
    m.item(_u8L("Fix Model"), "fix");
    m.item(_u8L("Simplify Model"), "simplify");
    const ModelVolume *v = object(s.vols[0].first)->volumes[s.vols[0].second];
    m.item(_u8L("Subdivision mesh") + _u8L("(Lost color)"), "smooth", v->mesh().stats().open_edges == 0);
    m.item(_u8L("Center"), "center", !at_plate_center(s));
    align_distribute(m, s);
    m.item(_u8L("Drop"), "drop", can_drop(s));
    mirror(m, true);
    split(m, s);
    m.sep();
    per_object_process(m, s);
    per_object_settings(m, s);
    change_type(m, s);
    m.item(_u8L("Reload from disk"), "reload", can_reload_from_disk());
    m.item(_u8L("Replace 3D file") + "...", "replace", can_replace_with_stl());
    m.item(_u8L("Replace all with 3D files") + "...", "replace_all", true);
    // part_menu() additions
    convert_unit(m, s);
    change_filament(m, s);
    precise_seam_submenu(m, s);
    return m.items;
}

json multi_menu(const Sel &s)   // multi_selection_menu
{
    Menu m;
    if (s.full_objects) {
        m.item(_u8L("Assemble"), "assemble", s.objs.size() > 1);
        m.item(_u8L("Center"), "center", !at_plate_center(s));
        align_distribute(m, s);
        m.item(_u8L("Drop"), "drop", can_drop(s));
        m.item(_u8L("Fix Model"), "fix");
        m.item(_u8L("Delete"), "delete", true, "Backspace");
        m.sep();
        bool all_printable = true;
        for (int o : s.objs) for (auto *i : object(o)->instances) all_printable &= i->printable;
        m.item(all_printable ? _u8L("Set Unprintable") : _u8L("Set Printable"), "set_printable");
        m.sep();
        bool all_drop = true;
        for (int o : s.objs) all_drop &= object(o)->auto_drop;
        m.item(all_drop ? _u8L("Disable Auto Drop") : _u8L("Enable Auto Drop"), "set_auto_drop");
        m.sep();
        per_object_process(m, s);
        m.sep();
        convert_unit(m, s);
        m.item(_u8L("Replace all with 3D files") + "...", "replace_all", true);
        change_filament(m, s);
        m.sep();
        export_items(m, s, true);
    } else {
        m.item(_u8L("Center"), "center", !at_plate_center(s));
        align_distribute(m, s);
        m.item(_u8L("Drop"), "drop", can_drop(s));
        m.item(_u8L("Fix Model"), "fix");
        m.item(_u8L("Delete"), "delete", true, "Backspace");
        convert_unit(m, s);
        m.item(_u8L("Replace all with 3D files") + "...", "replace_all", true);
        change_filament(m, s);
        split(m, s);
        per_object_process(m, s);
        m.sep();
        change_type(m, s);
        precise_seam_submenu(m, s);
    }
    return m.items;
}

json build(const json &)
{
    Sel s = selection();
    if (s.objs.empty()) return {{"kind", "default"}, {"items", default_menu()}};
    if (s.objs.size() == 1 && s.vols.empty()) return {{"kind", "object"}, {"items", object_menu(s)}};
    if (s.objs.size() == 1 && s.vols.size() == 1) return {{"kind", "part"}, {"items", part_menu(s)}};
    return {{"kind", "multi"}, {"items", multi_menu(s)}};
}

// ---------------------------------------------------------------------------------------------------------------
// actions

ModelVolumeType volume_type(const std::string &t)
{
    if (t == "part") return ModelVolumeType::MODEL_PART;
    if (t == "negative") return ModelVolumeType::NEGATIVE_VOLUME;
    if (t == "modifier") return ModelVolumeType::PARAMETER_MODIFIER;
    if (t == "support_blocker") return ModelVolumeType::SUPPORT_BLOCKER;
    if (t == "support_enforcer") return ModelVolumeType::SUPPORT_ENFORCER;
    if (t == "precise_seam") return ModelVolumeType::PRECISE_SEAM_CENTER;
    return ModelVolumeType::INVALID;
}

// GUI_ObjectList.cpp create_mesh
TriangleMesh create_mesh(const std::string &type_name, const BoundingBoxf3 &bb)
{
    BoundingBoxf bed = Plater::get().bed_box();
    const double side = 0.1 * std::max(bed.size().x(), bed.size().y());   // get_size_proportional_to_max_bed_size(0.1)
    if (type_name == "Cube") return TriangleMesh(its_make_cube(side, side, side));
    if (type_name == "Cylinder") return TriangleMesh(its_make_cylinder(0.5 * side, side));
    if (type_name == "Sphere") return TriangleMesh(its_make_sphere(0.5 * side, PI / 90));
    if (type_name == "Slab") return TriangleMesh(its_make_cube(bb.size().x() * 1.5, bb.size().y() * 1.5, bb.size().z() * 0.5));
    if (type_name == "Cone") return TriangleMesh(its_make_cone(0.5 * side, side));
    if (type_name == "Disc") return TriangleMesh(its_make_cylinder(0.5 * side, 0.2f));
    if (type_name == "Torus") return TriangleMesh(its_make_torus(0.5 * side, 0.125 * side, PI / 60));
    return {};
}

// ObjectList::load_generic_subobject (a part / modifier / ... on the selected object)
json add_generic_volume(const std::string &type_name, ModelVolumeType type)
{
    Plater &p = Plater::get();
    Sel s = selection();
    ModelObject *mo = object(s.obj());
    if (!mo) throw CommandError("no object selected");
    int inst = 0;
    for (auto &it : p.selection) if (it.obj == s.obj() && it.inst >= 0) inst = it.inst;
    p.take_snapshot(_u8L("Add primitive"));
    BoundingBoxf3 instance_bb = mo->instance_bounding_box(inst);
    TriangleMesh mesh = create_mesh(type_name, instance_bb);
    ModelVolume *nv = mo->add_volume(std::move(mesh), type);
    const Geometry::Transformation &it = mo->instances[inst]->get_transformation();
    nv->set_transformation(it.get_matrix_no_offset().inverse());
    const BoundingBoxf3 mesh_bb = nv->mesh().bounding_box();
    Vec3d offset = type_name == "Slab" ? Vec3d(0., 0., 0.5 * mesh_bb.size().z() + instance_bb.min.z() - it.get_offset().z())
                                       : Vec3d(instance_bb.max.x(), instance_bb.min.y(), instance_bb.min.z()) + 0.5 * mesh_bb.size() - it.get_offset();
    nv->set_offset(it.get_matrix_no_offset().inverse() * offset);
    nv->name = _u8L("Generic") + "-" + _u8L(type_name.c_str());
    int extruder_id = 0;
    if (nv->type() == ModelVolumeType::MODEL_PART && mo->config.has("extruder")) extruder_id = mo->config.opt_int("extruder");
    nv->config.set_key_value("extruder", new ConfigOptionInt(extruder_id));
    nv->source.is_from_builtin_objects = true;
    // ModelObject::sort_volumes, then select the new volume (reorder_volumes_and_get_selection)
    mo->sort_volumes(true);
    int vi = int(std::find(mo->volumes.begin(), mo->volumes.end(), nv) - mo->volumes.begin());
    mo->invalidate_bounding_box();
    p.selection = {{s.obj(), -1, vi}};
    p.changed();
    json st = p.state();
    st["switch_to_objects"] = true;   // params_panel()->switch_to_object(true)
    if (Core::get().app_config->get("do_not_show_modifer_tips").empty())
        st["tip"] = {{"title", _u8L("Add Modifier")}, {"text", _u8L("Switch to per-object setting mode to edit modifier settings.")}, {"key", "do_not_show_modifer_tips"}};
    return st;
}

// ObjectList::load_shape_object / load_mesh_object (a new object)
json add_shape_object(const std::string &type_name)
{
    Plater &p = Plater::get();
    p.take_snapshot(_u8L("Add Primitive"));
    TriangleMesh mesh = create_mesh(type_name, BoundingBoxf3());
    const DynamicPrintConfig &cfg = Core::get().full_config();
    if (type_name != "Cube" && cfg.has("fan_direction") && cfg.has("auxiliary_fan"))
        orientation::orient_for_cooling(mesh, cfg.option<ConfigOptionEnum<FanDirection>>("fan_direction")->value);
    Model &model = p.model();
    BoundingBoxf3 bb = mesh.bounding_box();
    ModelObject *no = model.add_object();
    no->name = _u8L(type_name.c_str());
    no->add_instance();
    ModelVolume *nv = no->add_volume(mesh);
    nv->name = no->name;
    no->sort_volumes(true);
    no->config.set_key_value("extruder", new ConfigOptionInt(1));
    no->invalidate_bounding_box();
    no->translate(-bb.center());
    // the nearest empty cell from the bed centre (Plater::get_empty_cells)
    json r = Core::get().call("project.nearest_empty_cell", json::object());
    Vec2d cell(r.at(0).get<double>(), r.at(1).get<double>());
    no->instances[0]->set_offset(to_3d(cell, -no->origin_translation.z()));
    no->ensure_on_bed();
    Geometry::Transformation t = no->instances[0]->get_transformation();
    no->instances[0]->set_assemble_transformation(t);
    p.selection = {{int(model.objects.size()) - 1, -1, -1}};
    p.changed();
    return p.state();
}

// MenuFactory::load_handy_model
json load_handy(const std::string &key)
{
    for (auto &h : handy_models()) {
        if (key != h.key) continue;
        json paths = json::array();
        for (const char *f : h.files) paths.push_back((fs::path(resources_dir()) / "handy_models" / f).string());
        json st = Core::get().call("project.load", {{"paths", paths}, {"load_config", false}});
        if (h.arrange) st = Core::get().call("arrange", {{"plate", Plater::get().current_plate}});
        return st;
    }
    throw CommandError("unknown model");
}

// ObjectList::split_instances
json instance_to_object()
{
    Plater &p = Plater::get();
    Model &m = p.model();
    Sel s = selection();
    int obj = s.obj();
    if (!object(obj)) return nullptr;
    p.take_snapshot(_u8L("Instances to Separated Objects"));
    std::set<int> insts;
    bool whole = false;
    for (auto &it : p.selection) { if (it.obj != obj) continue; if (it.inst < 0) whole = true; else insts.insert(it.inst); }
    ModelObject *mo = m.objects[obj];
    if (whole || insts.size() == mo->instances.size()) {
        // instances_to_separated_objects: every instance but the first becomes its own object
        for (int i = int(mo->instances.size()) - 1; i > 0; --i) {
            ModelObject *copy = m.add_object(*m.objects[obj]);
            for (int k = int(copy->instances.size()) - 1; k >= 0; --k) if (k != i) copy->delete_instance(k);
            m.objects[obj]->delete_instance(i);
        }
    } else {
        ModelObject *copy = m.add_object(*mo);
        for (int k = int(copy->instances.size()) - 1; k >= 0; --k) if (!insts.count(k)) copy->delete_instance(k);
        for (auto it = insts.rbegin(); it != insts.rend(); ++it) m.objects[obj]->delete_instance(*it);
    }
    p.selection.clear();
    p.changed();
    return p.state();
}

// ObjectList::fix_through_cgal: ModelVolume::repair with progress, then a summary
json fix(const json &)
{
    Plater &p = Plater::get();
    Sel s = selection();
    std::vector<std::pair<ModelObject *, std::vector<ModelVolume *>>> work;
    if (s.vols.empty()) for (int o : s.objs) work.push_back({object(o), object(o)->volumes});
    else for (auto &[o, v] : s.vols) work.push_back({object(o), {object(o)->volumes[v]}});
    if (work.empty()) return nullptr;
    p.take_snapshot(_u8L("Repairing model object"));
    const bool keep_painting = Core::get().app_config->get_bool("keep_painting");
    std::vector<std::string> ok, unchanged;
    std::vector<std::pair<std::string, std::string>> failed;
    size_t n = 0, total = 0;
    for (auto &w : work) total += w.second.size();
    for (auto &[mo, vols] : work) {
        bool modified = false;
        std::string errors;
        for (ModelVolume *v : vols) {
            Host::event("progress", {{"text", _u8L("Repairing model object") + ": " + mo->name}, {"percent", int(100 * n / std::max<size_t>(1, total))}});
            MeshRepairStats stats;
            std::string error;
            if (!v->repair(keep_painting, &stats, &error, [](int) {})) errors += (errors.empty() ? "" : "\n") + error;
            modified |= stats.modified;
            ++n;
        }
        mo->invalidate_bounding_box();
        if (modified) mo->ensure_on_bed();
        const std::string name = s.vols.empty() ? mo->name : vols.front()->name;
        if (!errors.empty()) failed.push_back({name, errors});
        else if (modified) ok.push_back(name);
        else unchanged.push_back(name);
    }
    Host::event("progress", {{"text", ""}, {"percent", -1}});
    // the summary notification (CgalFinished)
    std::string msg;
    auto plural = [](const char *one, const char *many, size_t k) { return k == 1 ? _u8L(one) : _u8L(many); };
    if (!ok.empty()) {
        msg += plural("The following model object has been repaired", "The following model objects have been repaired", ok.size()) + ":";
        for (auto &x : ok) msg += "\n   - " + x;
    }
    if (!failed.empty()) {
        msg += (msg.empty() ? "" : "\n\n") + plural("Failed to repair the following model object", "Failed to repair the following model objects", failed.size()) + ":";
        for (auto &x : failed) msg += "\n   - " + x.first + ": " + x.second;
    }
    if (!unchanged.empty())
        msg += (msg.empty() ? "" : "\n\n") +
               (boost::format(plural("%1$d model object needed no repair.", "%1$d model objects needed no repair.", unchanged.size())) % unchanged.size()).str();
    p.changed();
    json st = p.state();
    st["notice"] = msg;
    return st;
}

// ObjectList::smooth_mesh
json smooth(const json &a)
{
    Plater &p = Plater::get();
    Sel s = selection();
    ModelObject *mo = object(s.objs.empty() ? -1 : s.objs.front());
    if (!mo) return nullptr;
    std::vector<ModelVolume *> vols;
    if (s.vols.empty()) vols = mo->volumes; else for (auto &[o, v] : s.vols) vols.push_back(object(o)->volumes[v]);
    size_t future = 0;
    for (auto *v : vols) future += v->mesh().facets_count() * 4;
    if (future > 1000000 && !a.value("confirmed", false)) {
        const std::string name = s.vols.empty() ? mo->name : vols.front()->name;
        std::string content = (boost::format(_u8L("\"%s\" will exceed 1 million faces after this subdivision, which may increase slicing time. Do you want to continue?")) % name).str();
        return {{"ui", "confirm"}, {"text", (s.vols.empty() ? _u8L("Object") : _u8L("Part")) + " " + content}, {"id", "smooth"}};
    }
    p.take_snapshot("smooth_mesh");
    const bool keep_painting = Core::get().app_config->get_bool("keep_painting");
    std::string error_part;
    for (ModelVolume *mv : vols) {
        bool ok;
        TriangleMesh result = TriangleMeshDeal::smooth_triangle_mesh(mv->mesh(), ok);
        if (!ok) { if (error_part.empty()) error_part = mv->name; continue; }
        auto saved = keep_painting ? mv->save_painting() : std::optional<TriangleSelector::SavedPainting>{};
        mv->set_mesh(result);
        mv->restore_painting(saved);
        mv->calculate_convex_hull();
        mv->invalidate_convex_hull_2d();
        mv->set_new_unique_id();
    }
    mo->invalidate_bounding_box();
    mo->ensure_on_bed();
    p.changed();
    json st = p.state();
    if (!error_part.empty()) st["notice"] = (boost::format(_u8L("\"%s\" part's mesh contains errors. Please repair it first.")) % error_part).str();
    return st;
}

// ObjectList::set_volume_type
json set_type(ModelVolumeType nt, bool preserve_ps)
{
    Plater &p = Plater::get();
    Sel s = selection();
    std::vector<std::pair<int, ModelVolume *>> vols;
    for (auto &[o, v] : s.vols) vols.push_back({o, object(o)->volumes[v]});
    if (vols.empty()) return nullptr;
    auto effective = [&](const ModelVolume *v) { return (preserve_ps && nt == ModelVolumeType::PRECISE_SEAM_CENTER && v->is_precise_seam()) ? v->type() : nt; };
    if (std::none_of(vols.begin(), vols.end(), [&](auto &x) { return x.second->type() != effective(x.second); })) return nullptr;
    if (nt != ModelVolumeType::MODEL_PART) {
        std::map<int, int> total, selected;
        for (auto &[o, v] : vols) {
            if (!total.count(o)) { int c = 0; for (auto *x : object(o)->volumes) if (x->type() == ModelVolumeType::MODEL_PART) ++c; total[o] = c; }
            if (v->type() == ModelVolumeType::MODEL_PART) ++selected[o];
        }
        for (auto &[o, c] : selected)
            if (total[o] > 0 && c == total[o]) throw CommandError(_u8L("The type of the last solid object part cannot be changed."));
    }
    p.take_snapshot(_u8L("Change part type"));
    for (auto &[o, v] : vols) {
        v->set_type(effective(v));
        object(o)->invalidate_bounding_box();
    }
    p.changed();
    return p.state();
}

// Plater::convert_unit
json convert(ConversionType t)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    Sel s = selection();
    std::vector<int> vol_idxs;
    for (auto &[o, v] : s.vols) vol_idxs.push_back(v);
    p.take_snapshot(t == ConversionType::CONV_FROM_INCH ? "Convert from imperial units" : t == ConversionType::CONV_TO_INCH ? "Revert conversion from imperial units" :
                    t == ConversionType::CONV_FROM_METER ? "Convert from meters" : "Revert conversion from meters");
    ModelObjectPtrs objects;
    std::vector<int> objs = s.objs;
    std::sort(objs.rbegin(), objs.rend());
    for (int o : objs) {
        m.objects[o]->convert_units(objects, t, vol_idxs);
        m.delete_object(size_t(o));
    }
    std::reverse(objects.begin(), objects.end());
    for (ModelObject *no : objects) m.add_object(*no);   // copies, as Plater::priv::load_model_objects does
    p.selection.clear();
    for (size_t i = 0; i < objects.size(); ++i) p.selection.insert({int(m.objects.size() - 1 - i), -1, -1});
    p.changed();
    return p.state();
}

// GLCanvas3D::mirror_selection: a world mirror around the selection centre, then do_mirror (drop to the bed)
json mirror_axis(int axis)
{
    BoundingBoxf3 b = selection_world_box();
    Vec3d sc = Vec3d::Ones();
    sc[axis] = -1;
    Transform3d T = Geometry::translation_transform(b.center()) * Geometry::scale_transform(sc) * Geometry::translation_transform(-b.center());
    return apply_world(T, "Mirror");
}

// GLCanvas3D::drop_selected / Selection::drop
json drop()
{
    BoundingBoxf3 b = selection_world_box();
    return apply_world(Geometry::translation_transform(Vec3d(0, 0, -b.min.z())), "Drop");
}

// Selection::scale_to_fit_print_volume (rectangular beds)
json scale_to_fit()
{
    Plater &p = Plater::get();
    BoundingBoxf3 b = selection_world_box();
    BoundingBoxf3 pv = p.plate_box(p.current_plate);
    pv.max.z() = p.printable_height();
    const Vec3d ps = pv.size(), bs = b.size() + 0.02 * Vec3d::Ones();
    const double s = std::min({ps.x() / bs.x(), ps.y() / bs.y(), ps.z() / bs.z()});
    if (s <= 0.0 || s == 1.0) return nullptr;
    Transform3d S = Geometry::translation_transform(b.center()) * Geometry::scale_transform(s) * Geometry::translation_transform(-b.center());
    apply_world(S, "Scale To Fit");
    BoundingBoxf3 nb = selection_world_box();
    Vec3d off = pv.center() - nb.center();
    off.z() = -nb.min.z();
    return apply_world(Geometry::translation_transform(off), "");
}

// GLGizmoAlignment::align_objects / distribute_objects (whole objects: by instance)
json align(const std::string &how)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    Sel s = selection();
    struct It { ModelObject *mo; int inst; BoundingBoxf3 b; };
    std::vector<It> items;
    for (auto &si : p.selection) {
        ModelObject *mo = object(si.obj);
        if (!mo || si.vol >= 0) continue;
        for (int k = 0; k < int(mo->instances.size()); ++k)
            if (si.inst < 0 || si.inst == k) items.push_back({mo, k, mo->instance_bounding_box(k)});
    }
    if (items.size() < 2) return nullptr;
    BoundingBoxf3 all;
    for (auto &it : items) all.merge(it.b);
    const bool distribute = how.rfind("distribute:", 0) == 0;
    const int axis = how.back() == 'x' || how.find(":x") != std::string::npos ? 0 : how.find(":y") != std::string::npos ? 1 : 2;
    p.take_snapshot(distribute ? "Distribute" : "Align");
    if (distribute) {
        // equal gaps between the boxes along the axis, the outer two stay
        std::sort(items.begin(), items.end(), [axis](const It &a, const It &b) { return a.b.center()[axis] < b.b.center()[axis]; });
        double used = 0;
        for (auto &it : items) used += it.b.size()[axis];
        const double gap = (all.size()[axis] - used) / double(items.size() - 1);
        double pos = all.min[axis];
        for (auto &it : items) {
            Vec3d d = Vec3d::Zero();
            d[axis] = pos - it.b.min[axis];
            it.mo->instances[it.inst]->set_offset(it.mo->instances[it.inst]->get_offset() + d);
            pos += it.b.size()[axis] + gap;
        }
    } else {
        const std::string mode = how.substr(how.find('_') + 1);
        for (auto &it : items) {
            Vec3d d = Vec3d::Zero();
            if (mode == "min") d[axis] = all.min[axis] - it.b.min[axis];
            else if (mode == "max") d[axis] = all.max[axis] - it.b.max[axis];
            else d[axis] = all.center()[axis] - it.b.center()[axis];
            it.mo->instances[it.inst]->set_offset(it.mo->instances[it.inst]->get_offset() + d);
        }
    }
    for (auto *mo : m.objects) { mo->invalidate_bounding_box(); if (mo->auto_drop) mo->ensure_on_bed(); }
    p.changed();
    return p.state();
}

// ObjectList::merge(true): the selected objects (every selected instance) become the parts of one "Assembly" object
json merge()
{
    Plater &p = Plater::get();
    Model &m = p.model();
    std::map<int, std::set<int>> sel;
    for (auto &si : p.selection) {
        ModelObject *mo = object(si.obj);
        if (!mo || si.vol >= 0) continue;
        for (int k = 0; k < int(mo->instances.size()); ++k) if (si.inst < 0 || si.inst == k) sel[si.obj].insert(k);
    }
    if (sel.size() < 2 && (sel.empty() || sel.begin()->second.size() < 2)) return nullptr;
    p.take_snapshot(_u8L("Assemble"));
    ModelObject *no = m.add_object();
    no->name = _u8L("Assembly");
    ModelConfig &config = no->config;
    no->add_instance();
    for (auto &[obj_idx, insts] : sel) {
        ModelObject *object = m.objects[obj_idx];
        for (int inst : insts) {
            const Transform3d &tm = object->instances[inst]->get_transformation().get_matrix();
            for (const ModelVolume *volume : object->volumes) {
                ModelVolume *nv = no->add_volume(*volume);
                nv->set_transformation(Geometry::Transformation(tm * nv->get_matrix()));
                if (object->volumes.size() > 1) nv->config.assign_config(volume->config);
                if (!nv->config.option("extruder"))
                    if (auto *opt = object->config.option("extruder")) nv->config.set_key_value("extruder", new ConfigOptionInt(opt->getInt()));
            }
            no->sort_volumes(true);
            auto new_keys = config.keys();
            for (auto &k : object->config.keys())
                if (std::find(new_keys.begin(), new_keys.end(), k) == new_keys.end()) config.set_key_value(k, object->config.option(k)->clone());
            if (!object->instances[inst]->printable) { no->printable = false; no->instances[0]->printable = false; }
            if (!object->auto_drop) no->auto_drop = false;
            for (const auto &range : object->layer_config_ranges) no->layer_config_ranges.emplace(range);
            BrimPoints bp = object->brim_points;
            for (auto &pt : bp) { pt.set_transform(tm); no->brim_points.push_back(pt); }
        }
    }
    no->ensure_on_bed();
    no->center_around_origin();
    no->translate_instances(-no->origin_translation);
    no->origin_translation = Vec3d::Zero();
    Geometry::Transformation t = no->instances[0]->get_transformation();
    no->instances[0]->set_assemble_transformation(t);
    // the sources go away (back to front keeps the indices valid)
    for (auto it = sel.rbegin(); it != sel.rend(); ++it) m.delete_object(size_t(it->first));
    p.selection = {{int(m.objects.size()) - 1, -1, -1}};
    p.changed();
    return p.state();
}

json run(const json &a)
{
    Plater &p = Plater::get();
    Core &core = Core::get();
    const std::string id = str_arg(a, "id");
    Sel s = selection();
    auto first = [&](const char *pfx) { return id.rfind(pfx, 0) == 0; };
    auto rest = [&](const char *pfx) { return id.substr(strlen(pfx)); };

    if (id == "instance_add") return core.call("objects.instances", {{"delta", 1}});
    if (id == "instance_remove") return core.call("objects.instances", {{"delta", -1}});
    if (id == "instances_set") {
        ModelObject *mo = object(s.obj());
        if (!mo) return nullptr;
        if (!a.contains("value"))   // GetNumberFromUser
            return {{"ui", "number"}, {"title", _u8L("Copies of the selected object")}, {"label", _u8L("Number of copies:")},
                    {"value", mo->instances.size()}, {"min", 0}, {"max", 1000}, {"id", id}};
        int diff = a["value"].get<int>() - int(mo->instances.size());
        return diff == 0 ? p.state() : core.call("objects.instances", {{"delta", diff}});
    }
    if (id == "instance_to_object") return instance_to_object();
    if (id == "clone") {
        if (!a.contains("value"))   // CloneDialog
            return {{"ui", "clone"}, {"title", _u8L("Clone")}, {"label", _u8L("Number of copies:")}, {"value", 1},
                    {"arrange_label", _u8L("Auto arrange plate after cloning") + ":"}, {"id", id}};
        json st = core.call("objects.clone", {{"copies", a["value"]}});
        if (a.value("arrange", true)) st = core.call("arrange", {{"plate", p.current_plate}});
        return st;
    }
    if (id == "fix") return fix(a);
    if (id == "smooth") return smooth(a);
    if (id == "center") return core.call("object.center", {{"obj", s.objs.empty() ? -1 : s.objs.front()}});
    if (id == "drop") return drop();
    if (id == "scale_to_fit") return scale_to_fit();
    if (id == "delete") return core.call("selection.delete", json::object());
    if (id == "split:objects") return core.call("object.split", {{"obj", s.obj()}});
    if (id == "split:parts") return core.call("object.split", {{"obj", s.obj()}, {"to_parts", true}, {"vol", s.vols.empty() ? 0 : s.vols[0].second}});
    if (first("mirror:")) return mirror_axis(rest("mirror:") == "x" ? 0 : rest("mirror:") == "y" ? 1 : 2);
    if (first("align:") || first("distribute:")) return align(id);
    if (first("filament:")) {
        int e = std::stoi(rest("filament:"));
        p.take_snapshot("Change Extruders");
        if (s.vols.empty()) for (int o : s.objs) object(o)->config.set("extruder", e);
        else for (auto &[o, v] : s.vols) object(o)->volumes[v]->config.set("extruder", e);
        p.changed();
        return p.state();
    }
    if (first("convert:")) return convert(ConversionType(std::stoi(rest("convert:"))));
    if (first("type:")) return set_type(ModelVolumeType(std::stoi(rest("type:"))), true);
    if (first("pstype:")) return set_type(ModelVolumeType(std::stoi(rest("pstype:"))), false);
    if (id == "printable" || id == "set_printable") {
        bool all = true;
        for (int o : s.objs) for (auto *i : object(o)->instances) all &= i->printable;
        p.take_snapshot("Set Printable");
        for (int o : s.objs) for (auto *i : object(o)->instances) i->printable = !all;
        p.changed();
        return p.state();
    }
    if (id == "auto_drop" || id == "set_auto_drop") {   // ObjectList::toggle_auto_drop
        bool all = true;
        for (int o : s.objs) all &= object(o)->auto_drop;
        p.take_snapshot("");
        for (int o : s.objs) { object(o)->auto_drop = !all; if (all == false) object(o)->ensure_on_bed(); }
        p.changed();
        return p.state();
    }
    if (id == "assemble") return merge();
    if (id == "copy_settings") {   // ObjectList::copy_settings_to_clipboard
        const ModelObject *mo = object(s.obj());
        const DynamicPrintConfig &cfg = s.vols.empty() ? mo->config.get() : mo->volumes[s.vols[0].second]->config.get();
        json j = json::object();
        for (auto &k : cfg.keys()) j[k] = cfg.opt_serialize(k);
        core.app_config->set("mobile_settings_clipboard", j.dump());
        return nullptr;
    }
    if (id == "paste_settings") {   // ObjectList::paste_settings_into_list
        json j = json::parse(core.app_config->get("mobile_settings_clipboard"), nullptr, false);
        if (!j.is_object()) return nullptr;
        p.take_snapshot("Paste settings");
        auto paste = [&](ModelConfig &cfg) {
            for (auto &[k, v] : j.items()) {
                ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
                DynamicPrintConfig tmp;
                tmp.set_deserialize(k, v.get<std::string>(), ctx);
                cfg.apply(tmp);
            }
        };
        if (s.vols.empty()) for (int o : s.objs) paste(object(o)->config);
        else for (auto &[o, v] : s.vols) paste(object(o)->volumes[v]->config);
        p.changed();
        return p.state();
    }
    if (first("add:")) {   // add:<type>:<shape>
        std::string r = rest("add:");
        std::string type = r.substr(0, r.find(':')), shape = r.substr(r.find(':') + 1);
        if (shape == "load") return {{"ui", "load_part"}, {"type", type}};
        if (shape == "text" || shape == "svg") return {{"ui", "gizmo"}, {"gizmo", shape == "text" ? "Text shape" : "SVG"}, {"type", type}};
        if (type == "invalid") return add_shape_object(shape);
        return add_generic_volume(shape, volume_type(type));
    }
    if (first("handy:")) return load_handy(rest("handy:"));
    if (id == "show_labels") {
        core.app_config->set_bool("show_labels", !core.app_config->get_bool("show_labels"));
        return {{"ui", "labels"}};
    }
    if (id == "reload") return {{"ui", "reload"}};   // the app refreshes its copies of the documents, then object.reload
    if (id == "replace") return {{"ui", "replace"}};
    if (id == "replace_all") return {{"ui", "replace_all"}, {"title", _u8L("Select folder to replace from") + ":"}};
    if (id == "fill_bed") return core.call("fill_bed", json::object());   // Plater::fill_bed_with_instances
    if (id == "height_range") {   // ObjectList::layers_editing, then the app shows the Layers item
        int obj = -1;
        for (const SelItem &it : Plater::get().selection) { obj = it.obj; break; }
        if (obj < 0) throw CommandError("no object");
        json st = core.call("layers.add", {{"obj", obj}});
        st["ui"] = "layers";
        st["obj"] = obj;
        return st;
    }
    // the rest is handled by the app: dialogs, pickers, gizmos, switching panels
    static const std::map<std::string, json> ui = {
        {"rename", {{"ui", "rename"}}}, {"add_models", {{"ui", "import"}}},
        {"simplify", {{"ui", "gizmo"}, {"gizmo", "Simplify"}}}, {"mesh_boolean", {{"ui", "gizmo"}, {"gizmo", "Mesh boolean"}}},
        {"edit_text", {{"ui", "gizmo"}, {"gizmo", "Text shape"}}}, {"edit_svg", {{"ui", "gizmo"}, {"gizmo", "SVG"}}},
        {"edit_process", {{"ui", "objects_mode"}}}, {"edit_table", {{"ui", "object_table"}}},
        {"export_stl", {{"ui", "export"}, {"what", "stl"}}}, {"export_drc", {{"ui", "export"}, {"what", "drc"}}},
        {"export_stls", {{"ui", "export"}, {"what", "stls"}}}, {"export_drcs", {{"ui", "export"}, {"what", "drcs"}}}};
    if (auto it = ui.find(id); it != ui.end()) return it->second;
    throw CommandError("not available: " + id);
}

} // namespace

static Commands reg({
    {"menu.get", build},
    {"menu.run", run},
    {"object.scale_to_fit", [](const json &) { return scale_to_fit(); }},
    {"object.add_shape", [](const json &a) { return add_shape_object(str_arg(a, "shape")); }},
});

} // namespace orca
