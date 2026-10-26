#include "Plater.hpp"
#include "Published.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <cmath>

#include "Core.hpp"
#include "Host.hpp"
#include "PrimeTower.hpp"
#include "I18n.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/ClipperUtils.hpp"

using namespace Slic3r;

namespace orca {

// PartPlate.cpp
static const double LOGICAL_PART_PLATE_GAP = 1. / 5.;
static const size_t MAX_UNDO = 50;

// PartPlate.hpp compute_colum_count
static int compute_colum_count(int count)
{
    float value = std::sqrt((float) count);
    float round_value = std::round(value);
    return value > round_value ? int(round_value) + 1 : int(round_value);
}

Plater &Plater::get()
{
    static Plater p;
    if (p.plates.empty()) p.plates.emplace_back();
    return p;
}

Model &Plater::model() { return Core::get().model; }

static const DynamicPrintConfig &printer_config() { return Core::get().preset_bundle->printers.get_edited_preset().config; }

Pointfs Plater::bed_shape() const
{
    if (auto *o = printer_config().option<ConfigOptionPoints>("printable_area"); o && o->values.size() >= 3)
        return o->values;
    return {Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200)};
}

BoundingBoxf Plater::bed_box() const { return BoundingBoxf(bed_shape()); }

double Plater::printable_height() const
{
    auto *o = printer_config().option<ConfigOptionFloat>("printable_height");
    return o ? o->value : 250.;
}

int    Plater::plate_cols() const { return compute_colum_count(int(plates.size())); }
double Plater::plate_stride_x() const { return bed_box().size().x() * (1. + LOGICAL_PART_PLATE_GAP); }
double Plater::plate_stride_y() const { return bed_box().size().y() * (1. + LOGICAL_PART_PLATE_GAP); }

// PartPlateList::compute_shape_position
Vec3d Plater::plate_origin(int i) const
{
    const int cols = plate_cols();
    return Vec3d((i % cols) * plate_stride_x(), -(i / cols) * plate_stride_y(), 0.);
}

BoundingBoxf3 Plater::plate_box(int i) const
{
    BoundingBoxf b = bed_box();
    Vec3d        o = plate_origin(i);
    return BoundingBoxf3(Vec3d(b.min.x() + o.x(), b.min.y() + o.y(), 0.), Vec3d(b.max.x() + o.x(), b.max.y() + o.y(), printable_height()));
}

int Plater::plate_of(int obj, int inst) const
{
    for (size_t i = 0; i < plates.size(); ++i)
        if (plates[i].instances.count({obj, inst})) return int(i);
    return -1;
}

// PartPlateList::notify_instance_update / PartPlate::check_outside: an instance stays on its plate while it touches
// it, otherwise it moves to the first plate it touches; "outside" when it is not fully inside the printable volume
// or it touches an excluded area.
void Plater::update_plates()
{
    Model &m = model();
    std::vector<std::set<std::pair<int, int>>> old(plates.size());
    for (size_t i = 0; i < plates.size(); ++i) old[i] = plates[i].instances, plates[i].instances.clear(), plates[i].outside.clear();
    Polygons exclude;
    if (auto *ex = printer_config().option<ConfigOptionPoints>("bed_exclude_area"); ex && ex->values.size() >= 3) {
        Polygon p;
        for (auto &v : ex->values) p.points.push_back(Point::new_scale(v.x(), v.y()));
        exclude.push_back(p);
    }
    for (int oi = 0; oi < int(m.objects.size()); ++oi)
        for (int ii = 0; ii < int(m.objects[oi]->instances.size()); ++ii) {
            BoundingBoxf3 box = m.objects[oi]->instance_convex_hull_bounding_box(ii);
            int found = -1;
            for (size_t p = 0; p < plates.size() && found < 0; ++p)
                if (old[p].count({oi, ii}) && plate_box(int(p)).intersects(box)) found = int(p);
            for (size_t p = 0; p < plates.size() && found < 0; ++p)
                if (plate_box(int(p)).intersects(box)) found = int(p);
            if (found < 0) continue;
            Plate &plate = plates[found];
            plate.instances.insert({oi, ii});
            BoundingBoxf3 pb = plate_box(found);
            if (box.min.z() < 0) pb.min.z() = box.min.z();   // sinking objects: the part below the bed is not "outside"
            bool outside = !pb.contains(box);
            if (!outside && !exclude.empty()) {
                Vec3d o = plate_origin(found);
                Polygon hull = m.objects[oi]->instances[ii]->convex_hull_2d();
                hull.translate(Point::new_scale(-o.x(), -o.y()));
                outside = !intersection(exclude, Polygons{hull}).empty();
            }
            if (outside) plate.outside.insert({oi, ii});
        }
}

void Plater::take_snapshot(const std::string &name)
{
    m_undo.push_back({name, Model::make_copy(model()), plates, current_plate, selection});
    if (m_undo.size() > MAX_UNDO) m_undo.erase(m_undo.begin());
    m_redo.clear();
}

bool Plater::undo()
{
    if (m_undo.empty()) return false;
    Snapshot s = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back({s.name, Model::make_copy(model()), plates, current_plate, selection});
    model().assign_copy(std::move(s.model));
    plates = std::move(s.plates); current_plate = s.current_plate; selection = std::move(s.selection);
    changed();
    return true;
}

bool Plater::redo()
{
    if (m_redo.empty()) return false;
    Snapshot s = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back({s.name, Model::make_copy(model()), plates, current_plate, selection});
    model().assign_copy(std::move(s.model));
    plates = std::move(s.plates); current_plate = s.current_plate; selection = std::move(s.selection);
    changed();
    return true;
}

void Plater::resize_plates(size_t n)
{
    n = std::max<size_t>(1, n);
    update_plates();
    std::vector<Vec3d> old_origins;
    for (size_t i = 0; i < plates.size(); ++i) old_origins.push_back(plate_origin(int(i)));
    std::vector<std::set<std::pair<int, int>>> members;
    for (auto &pl : plates) members.push_back(pl.instances);
    plates.resize(n);
    Model &m = model();
    for (size_t i = 0; i < std::min(n, old_origins.size()); ++i) {
        Vec3d d = plate_origin(int(i)) - old_origins[i];
        if (d.norm() < EPSILON) continue;
        for (auto &[o, inst] : members[i]) m.objects[o]->instances[inst]->set_offset(m.objects[o]->instances[inst]->get_offset() + d);
        for (auto &[o, inst] : members[i]) m.objects[o]->invalidate_bounding_box();
    }
    if (current_plate >= int(n)) current_plate = int(n) - 1;
    update_plates();
    // PartPlateList::create_plate: a new plate's prime tower starts at the default place
    for (size_t i = old_origins.size(); i < n; ++i) set_default_wipe_tower_pos(int(i), true);
}

void Plater::reset_project()
{
    model().clear_objects();
    model().clear_materials();
    plates.assign(1, Plate());
    current_plate = 0;
    only_gcode = false;
    selection.clear();
    set_default_wipe_tower_pos(0, true);   // PartPlateList::reinit
    m_undo.clear();
    m_redo.clear();
    project_name.clear();
    project_path.clear();
    pending_published() = {};
    changed();
    dirty = false;   // a new project has nothing to save (up_to_date)
    Host::event("project", state());
}

bool ensure_belt_purge_tower();   // BeltPurge.cpp

void Plater::changed(bool)
{
    // drop selection entries that no longer exist
    Model &m = model();
    for (auto it = selection.begin(); it != selection.end();) {
        bool ok = it->obj >= 0 && it->obj < int(m.objects.size()) && it->inst < int(m.objects[it->obj]->instances.size()) &&
                  it->vol < int(m.objects[it->obj]->volumes.size());
        it = ok ? std::next(it) : selection.erase(it);
    }
    if (current_plate >= int(plates.size())) current_plate = int(plates.size()) - 1;
    update_plates();
    ensure_belt_purge_tower();   // a belt printer's purge prism follows the plates' contents
    for (auto &p : plates) p.slice_valid = false;
    dirty = true;
    ++revision;
    Host::event("project", state());
}

static const char *volume_type(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    default: return "unknown";
    }
}

// GLCanvas3D object info: the volume of the first instance (model parts only)
static double object_volume(const ModelObject &o)
{
    if (o.instances.empty()) return 0.;
    const double inst = std::abs(o.instances[0]->get_matrix().matrix().block<3, 3>(0, 0).determinant());
    double v = 0;
    for (const ModelVolume *mv : o.volumes)
        if (mv->is_model_part()) v += std::abs(its_volume(mv->mesh().its)) * std::abs(mv->get_matrix().matrix().block<3, 3>(0, 0).determinant());
    return v * inst;
}

// the categories of the overridden settings (ObjectDataViewModel settings digest), "extruder" not counted
static json settings_categories(const DynamicPrintConfig &cfg)
{
    std::vector<std::string> cats;
    for (const std::string &k : cfg.keys()) {
        if (k == "extruder") continue;
        const ConfigOptionDef *def = print_config_def.get(k);
        std::string c = def ? def->category : std::string();
        if (!c.empty() && std::find(cats.begin(), cats.end(), c) == cats.end()) cats.push_back(c);
    }
    json out = json::array();
    for (auto &c : cats) out.push_back(_u8L(c.c_str()));
    return out;
}

Transform3d Plater::shown_instance_matrix(const ModelInstance &mi) const
{
    return assemble_view ? mi.get_assemble_transformation().get_matrix() : mi.get_matrix();
}

// Plater::show_object_info: the selected single instance's (or part's) name, size, volume, triangles and the
// ObjectList::get_mesh_errors_info sidebar line, in upstream's words; null when the box is hidden
static json object_info(Plater &p)
{
    Model &m = p.model();
    if (p.selection.size() != 1) return nullptr;
    const SelItem s = *p.selection.begin();
    if (s.obj < 0 || s.obj >= int(m.objects.size()) || m.objects[s.obj]->volumes.empty()) return nullptr;
    const ModelObject *model_object = m.objects[s.obj];
    // a whole object of several instances is not a single full instance
    const int inst_idx = s.inst >= 0 ? s.inst : (model_object->instances.size() == 1 ? 0 : -1);
    if (inst_idx < 0 || inst_idx >= int(model_object->instances.size())) return nullptr;
    const AppConfig *cfg = Core::get().app_config.get();
    const bool   imperial_units = cfg != nullptr && cfg->get("use_inches") == "1";
    const double koef = imperial_units ? 0.0393700787 : 1.0;   // GizmoObjectManipulation::mm_to_in
    const ModelVolume *vol = nullptr;
    Transform3d t;
    int   face_count;
    Vec3d size;
    std::string info_text;
    if (s.vol >= 0 && s.vol < int(model_object->volumes.size())) {
        vol = model_object->volumes[s.vol];
        t = model_object->instances[inst_idx]->get_matrix() * vol->get_matrix();
        info_text += (boost::format(_u8L("Part name: %1%\n")) % vol->name).str();
        face_count = static_cast<int>(vol->mesh().facets_count());
        size = vol->get_convex_hull().transformed_bounding_box(t).size();
    } else {
        info_text += (boost::format(_u8L("Object name: %1%\n")) % model_object->name).str();
        face_count = static_cast<int>(model_object->facets_count());
        size = model_object->instance_convex_hull_bounding_box(inst_idx).size();
    }
    if (imperial_units)
        info_text += (boost::format(_u8L("Size: %1% x %2% x %3% in\n")) % (size(0) * koef) % (size(1) * koef) % (size(2) * koef)).str();
    else
        info_text += (boost::format(_u8L("Size: %1% x %2% x %3% mm\n")) % size(0) % size(1) % size(2)).str();
    const TriangleMeshStats &stats = vol ? vol->mesh().stats() : model_object->get_object_stl_stats();
    double volume_val = stats.volume;
    if (vol) volume_val *= std::fabs(t.matrix().block(0, 0, 3, 3).determinant());
    volume_val = volume_val * std::pow(koef, 3);
    if (imperial_units)
        info_text += (boost::format(_u8L("Volume: %1% in³\n")) % volume_val).str();
    else
        info_text += (boost::format(_u8L("Volume: %1% mm³\n")) % volume_val).str();
    info_text += (boost::format(_u8L("Triangles: %1%\n")) % face_count).str();
    // ObjectList::get_mesh_errors_info(sidebar_info)
    std::string info_manifold;
    if (stats.repaired() || !stats.manifold()) {
        std::string auto_repaired_info, remaining_info;
        if (stats.repaired()) {
            const int errors = model_object->get_repaired_errors_count(s.vol);
            auto_repaired_info = (boost::format(_L_PLURAL("%1$d error repaired", "%1$d errors repaired", errors)) % errors).str();
        }
        if (!stats.manifold())
            remaining_info = (boost::format(_L_PLURAL("Error: %1$d non-manifold edge.", "Error: %1$d non-manifold edges.", stats.open_edges)) % stats.open_edges).str();
        info_manifold = stats.manifold() ? auto_repaired_info : (remaining_info + (stats.repaired() ? ("\n" + auto_repaired_info) : ""));
        if (stats.open_edges > 0) info_manifold += "\n" + _u8L("Tips:") + "\n" + _u8L("Use \"Fix Model\" to repair the mesh.");
    }
    return {{"text", info_text}, {"errors", info_manifold}};
}

json Plater::state()
{
    Model &m = model();
    json objects = json::array();
    for (int oi = 0; oi < int(m.objects.size()); ++oi) {
        const ModelObject *o = m.objects[oi];
        json vols = json::array(), insts = json::array();
        for (const ModelVolume *v : o->volumes)
            vols.push_back({{"name", v->name}, {"type", volume_type(v->type())}, {"extruder", v->extruder_id()},
                            {"settings", v->config.keys().size()}, {"categories", settings_categories(v->config.get())},
                            {"text", v->is_text()}, {"svg", v->is_svg()},
                            {"errors", v->get_repaired_errors_count()}, {"open_edges", v->mesh().stats().open_edges}});
        for (int ii = 0; ii < int(o->instances.size()); ++ii) {
            const ModelInstance *in = o->instances[ii];
            int p = plate_of(oi, ii);
            Vec3d off = in->get_offset();
            insts.push_back({{"plate", p}, {"outside", p >= 0 && plates[p].outside.count({oi, ii}) > 0}, {"printable", in->printable},
                             {"offset", {off.x(), off.y(), off.z()}}});
        }
        BoundingBoxf3 bb = o->instance_bounding_box(0);
        json layers = json::array();
        for (const auto &[range, cfg] : o->layer_config_ranges) layers.push_back({range.first, range.second, cfg.has("extruder") ? cfg.opt_int("extruder") : 0});
        objects.push_back({{"name", o->name},
                           {"volumes", vols},
                           {"instances", insts},
                           {"printable", o->printable},
                           {"settings", o->config.keys().size()},
                           {"extruder", o->config.has("extruder") ? o->config.opt_int("extruder") : 0},
                           {"layer_ranges", layers},
                           {"variable_layer_height", o->layer_height_profile.get().size() > 4},
                           {"supports_painted", o->is_fdm_support_painted()},
                           {"seam_painted", o->is_seam_painted()},
                           {"mm_painted", o->is_mm_painted()},
                           {"fuzzy_painted", o->is_fuzzy_skin_painted()},
                           {"sinking", !o->instances.empty() && o->get_instance_min_z(0) < SINKING_Z_THRESHOLD},
                           {"cut", o->is_cut()},
                           {"categories", settings_categories(o->config.get())},
                           {"open_edges", o->get_object_stl_stats().open_edges},
                           {"repaired_errors", o->get_repaired_errors_count()},
                           {"size", {bb.size().x(), bb.size().y(), bb.size().z()}},
                           {"facets", o->facets_count()},
                           {"volume", object_volume(*o)}});
    }
    json pl = json::array();
    for (size_t i = 0; i < plates.size(); ++i)
        pl.push_back({{"name", plates[i].name}, {"locked", plates[i].locked}, {"count", plates[i].instances.size()},
                      {"outside", !plates[i].outside.empty()}, {"sliced", plates[i].slice_valid}});
    json sel = json::array();
    for (auto &s : selection) sel.push_back({{"obj", s.obj}, {"inst", s.inst}, {"vol", s.vol}});
    return {{"objects", objects},
            {"object_info", object_info(*this)},
            {"plates", pl},
            {"current_plate", current_plate},
            {"selection", sel},
            {"can_undo", !m_undo.empty()},
            {"can_redo", !m_redo.empty()},
            {"undo_name", m_undo.empty() ? "" : m_undo.back().name},
            {"redo_name", m_redo.empty() ? "" : m_redo.back().name},
            {"project", project_name},
            {"dirty", dirty}, {"only_gcode", only_gcode}};
}

} // namespace orca
