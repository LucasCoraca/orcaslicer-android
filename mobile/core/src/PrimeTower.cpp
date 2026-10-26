#include "PrimeTower.hpp"

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "Slicer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r;

namespace orca {

using arrangement::ArrangePolygon;
using arrangement::ArrangePolygons;

// PartPlate.cpp
static const float WIPE_TOWER_DEFAULT_X_POS    = 165.;
static const float WIPE_TOWER_DEFAULT_Y_POS    = 250.;   // Max y
static const float N9_WIPE_TOWER_DEFAULT_Y_POS = 160.;
static const float I3_WIPE_TOWER_DEFAULT_X_POS = 0.;
static const float I3_WIPE_TOWER_DEFAULT_Y_POS = 250.;   // Max y

#include "slic3r/GUI/PlateTower.inc"

PlateTower::PlateTower(int plate)
{
    Plater &p = Plater::get();
    m_model = &p.model();
    m_plate_index = plate;
    // PartPlateList::reset_size: the printable bounding box of the bed, as ints
    const BoundingBoxf bed = p.bed_box();
    m_width = int(bed.size().x());
    m_depth = int(bed.size().y());
    if (plate >= 0 && plate < int(p.plates.size())) {
        instances = p.plates[plate].instances;
        outside   = p.plates[plate].outside;
    }
}

bool PlateTower::contain_instance_totally(int obj_id, int instance_id) const
{
    return instances.count({obj_id, instance_id}) && !outside.count({obj_id, instance_id});
}

bool PlateTower::contain_any_instance_totally(int obj_id) const
{
    if (obj_id < 0 || obj_id >= int(m_model->objects.size())) return false;
    for (int i = 0; i < int(m_model->objects[obj_id]->instances.size()); ++i)
        if (contain_instance_totally(obj_id, i)) return true;
    return false;
}

static DynamicPrintConfig &project_config() { return Core::get().preset_bundle->project_config; }

// the plate's own filament maps (PartPlate::get_real_filament_maps), else the project's
static std::vector<int> real_filament_maps(int plate)
{
    Plater &p = Plater::get();
    if (plate < int(p.plates.size()))
        if (auto *o = p.plates[plate].config.option<ConfigOptionInts>("filament_map"); o && !o->values.empty()) return o->values;
    auto *g = project_config().option<ConfigOptionInts>("filament_map");
    return g ? g->values : std::vector<int>();
}

static void ensure_size(int n)
{
    DynamicPrintConfig &proj_cfg = project_config();
    auto *wx = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true);
    auto *wy = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true);
    if (wx->values.empty()) wx->values.push_back(WIPE_TOWER_DEFAULT_X_POS);
    if (wy->values.empty()) wy->values.push_back(WIPE_TOWER_DEFAULT_Y_POS);
    if (int(wx->values.size()) < n) wx->values.resize(n, wx->values.front());
    if (int(wy->values.size()) < n) wy->values.resize(n, wy->values.front());
}

void set_default_wipe_tower_pos(int plate_idx, bool init_pos)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    PresetBundle &bundle = *core.preset_bundle;
    DynamicPrintConfig &proj_cfg = project_config();
    ensure_size(int(p.plates.size()));
    ConfigOptionFloats *wipe_tower_x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
    ConfigOptionFloats *wipe_tower_y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");

    const DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    auto printer_structure_opt = printer.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
    // set the default position, the same with print config(left top)
    float x = WIPE_TOWER_DEFAULT_X_POS;
    float y = WIPE_TOWER_DEFAULT_Y_POS;
    if (printer_structure_opt && printer_structure_opt->value == PrinterStructure::psI3) {
        x = I3_WIPE_TOWER_DEFAULT_X_POS;
        y = I3_WIPE_TOWER_DEFAULT_Y_POS;
    }
    if (bundle.printers.get_edited_preset().get_printer_type(&bundle) == "N9") y = N9_WIPE_TOWER_DEFAULT_Y_POS;

    const Vec3d plate_origin = p.plate_origin(plate_idx);
    const BoundingBoxf3 plate_bbox = p.plate_box(plate_idx);
    BoundingBoxf plate_bbox_2d(Vec2d(plate_bbox.min(0), plate_bbox.min(1)), Vec2d(plate_bbox.max(0), plate_bbox.max(1)));
    if (auto *areas = printer.option<ConfigOptionPointsGroups>("extruder_printable_area"))
        for (const Pointfs &points : areas->values) {
            if (points.empty()) continue;
            BoundingBoxf bboxf(points);
            bboxf.translate(plate_origin.x(), plate_origin.y());
            plate_bbox_2d.min = plate_bbox_2d.min(0) >= bboxf.min(0) ? plate_bbox_2d.min : bboxf.min;
            plate_bbox_2d.max = plate_bbox_2d.max(0) <= bboxf.max(0) ? plate_bbox_2d.max : bboxf.max;
        }
    const coordf_t x_min_local = plate_bbox_2d.min(0) - plate_origin(0);
    const coordf_t x_max_local = plate_bbox_2d.max(0) - plate_origin(0);
    const coordf_t y_max_local = plate_bbox_2d.max(1) - plate_origin(1);

    std::vector<int> filament_maps = real_filament_maps(plate_idx);
    std::vector<int> f_volume_maps = bundle.get_default_nozzle_volume_types_for_filaments(filament_maps);
    DynamicPrintConfig full_config = bundle.full_config(false, filament_maps, f_volume_maps);
    PlateTower part_plate(plate_idx);
    WipeTowerFootprint footprint = part_plate.estimate_wipe_tower_footprint(full_config, init_pos ? 2 : 0);
    if (!init_pos && (is_approx(footprint.width, 0.0) || is_approx(footprint.depth, 0.0)))
        footprint = part_plate.estimate_wipe_tower_footprint(full_config, 2);
    Vec3d wipe_tower_size(footprint.width, footprint.depth, footprint.height);

    // Brim-aware margin: the brim extends outward from the tower position.
    const float margin = WIPE_TOWER_AUTO_MARGIN + float(footprint.brim_width);
    if (x + margin + wipe_tower_size(0) > x_max_local) x = x_max_local - wipe_tower_size(0) - margin;
    else if (x < margin + x_min_local) x = margin + x_min_local;
    if (y + margin + wipe_tower_size(1) > y_max_local) y = y_max_local - wipe_tower_size(1) - margin;
    else if (y < margin) y = margin;

    // pull it onto a delta / hexagonal bed's real outline
    {
        Polygons bed{Polygon::new_scale(p.bed_shape())};
        const BoundingBox tower(Point::new_scale(x, y), Point::new_scale(x + wipe_tower_size(0), y + wipe_tower_size(1)));
        const Vec2f move = WipeTower::move_box_inside_polygon(tower, bed, scaled<coord_t>(margin));
        x += move.x();
        y += move.y();
    }
    ConfigOptionFloat wt_x_opt(x), wt_y_opt(y);
    wipe_tower_x->set_at(&wt_x_opt, plate_idx, 0);
    wipe_tower_y->set_at(&wt_y_opt, plate_idx, 0);
    core.invalidate_full_config();
}

void set_default_wipe_tower_pos_all()
{
    for (int i = 0; i < int(Plater::get().plates.size()); ++i) set_default_wipe_tower_pos(i);
}

static std::vector<ColorRGBA> extruder_colors()
{
    std::vector<ColorRGBA> out;
    if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour"))
        for (const std::string &s : fc->values) { ColorRGBA c; out.push_back(decode_color(s, c) ? c : ColorRGBA(0.8f, 0.8f, 0.8f, 1.f)); }
    if (out.empty()) out.push_back(ColorRGBA(0.8f, 0.8f, 0.8f, 1.f));
    return out;
}

std::vector<TowerVolume> wipe_tower_volumes()
{
    std::vector<TowerVolume> out;
    Core &core = Core::get();
    Plater &p = Plater::get();
    const DynamicPrintConfig &config = core.full_config();
    PresetBundle &bundle = *core.preset_bundle;
    auto *fc = config.option<ConfigOptionStrings>("filament_colour");
    if (!fc) return out;
    const unsigned int filaments_count = unsigned(fc->values.size());
    const bool wt = config.opt_bool("enable_prime_tower");
    const auto *co = config.option<ConfigOptionEnum<PrintSequence>>("print_sequence");
    const DynamicPrintConfig &dconfig = bundle.prints.get_edited_preset().config;
    auto timelapse_type = dconfig.option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
    bool need_wipe_tower = timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false;
    if (dconfig.has("enable_wrapping_detection")) need_wipe_tower |= dconfig.opt_bool("enable_wrapping_detection");
    bool is_belt_printer = false;
    if (const auto *belt = bundle.printers.get_edited_preset().config.option<ConfigOptionBool>("belt_printer")) is_belt_printer = belt->value;
    if (!wt || is_belt_printer || !(need_wipe_tower || filaments_count > 1)) return out;

    ensure_size(int(p.plates.size()));
    DynamicPrintConfig &proj_cfg = project_config();
    const std::vector<ColorRGBA> ext_colors = extruder_colors();
    const float a = float(config.opt_float("wipe_tower_rotation_angle"));
    bool clamped = false;
    for (int plate_id = 0; plate_id < int(p.plates.size()); ++plate_id) {
        const Plate &plate = p.plates[plate_id];
        // print by object: only a plate with one object gets a tower
        PrintSequence seq = co ? co->value : PrintSequence::ByLayer;
        if (auto *ps = plate.config.option<ConfigOptionEnum<PrintSequence>>("print_sequence")) seq = ps->value;
        PlateTower part_plate(plate_id);
        int printable = 0;
        for (auto &[o, i] : plate.instances)
            if (part_plate.contain_instance_totally(o, i) && p.model().objects[o]->instances[i]->printable) ++printable;
        if (seq == PrintSequence::ByObject && printable != 1) continue;
        if (plate.instances.empty()) continue;

        float x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x")->get_at(plate_id);
        float y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y")->get_at(plate_id);
        const WipeTowerFootprint footprint = part_plate.estimate_wipe_tower_footprint(config);
        if (footprint.depth <= 0.) continue;
        const float brim_width = float(footprint.brim_width);
        Vec3d wipe_tower_size(footprint.width, footprint.depth, footprint.height);
        {
            Vec3d clamped_pos, clamped_size;
            part_plate.estimate_wipe_tower_polygon(config, plate_id, clamped_pos, clamped_size);
            if (std::abs(x - float(clamped_pos(0))) > EPSILON || std::abs(y - float(clamped_pos(1))) > EPSILON) {
                x = float(clamped_pos(0));
                y = float(clamped_pos(1));
                ConfigOptionFloat wt_x_opt(x), wt_y_opt(y);
                proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x")->set_at(&wt_x_opt, plate_id, 0);
                proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y")->set_at(&wt_y_opt, plate_id, 0);
                clamped = true;
            }
        }
        const Vec3d origin = p.plate_origin(plate_id);
        TowerVolume v;
        v.plate = plate_id;
        v.matrix = Geometry::translation_transform(Vec3d(x + origin.x(), y + origin.y(), 0.)) *
                   Geometry::rotation_transform(Vec3d(0., 0., (M_PI / 180.) * a));
        std::vector<int> plate_extruders = part_plate.get_extruders(true, config, proj_cfg);
        auto color_of = [&](int id) { return id >= 1 && id <= int(ext_colors.size()) ? ext_colors[id - 1] : ext_colors[0]; };

        PlateSlice *ps = Slicer::get().slice(plate_id);
        const Print *print = ps ? ps->print.get() : nullptr;
        if (print && print->is_step_done(psWipeTower) && print->wipe_tower_data().wipe_tower_mesh_data) {
            // load_real_wipe_tower_preview: the tower the slicer made, in the first filament's colour
            if (plate_extruders.empty()) continue;
            TriangleMesh mesh = print->wipe_tower_data().wipe_tower_mesh_data->real_wipe_tower_mesh;
            if (mesh.its.vertices.empty()) continue;
            mesh.merge(print->wipe_tower_data().wipe_tower_mesh_data->real_brim_mesh);
            v.parts.push_back({color_of(plate_extruders.front()), std::make_shared<const TriangleMesh>(mesh)});
            v.shell = std::make_shared<const TriangleMesh>(mesh.convex_hull_3d());
        } else {
            // load_wipe_tower_preview: one translucent slab per filament along Y, plus the brim
            float width = float(wipe_tower_size(0)), depth = float(wipe_tower_size(1)), height = float(wipe_tower_size(2));
            if (depth < 0.01f) continue;
            if (height == 0.f) height = 0.1f;
            TriangleMesh shell = make_cube(width, depth, height);
            TriangleMesh brim_slab;
            const bool show_brim = brim_width > 0.f;
            if (show_brim) {
                const DynamicPrintConfig &printer_cfg = bundle.printers.get_edited_preset().config;
                const Polygon outline = estimate_wipe_tower_first_layer_outline(dconfig, resolve_wipe_tower_type(printer_cfg), width, depth, height);
                const Polygons brim_outline = offset(outline, scaled(brim_width));
                brim_slab = WipeTower::its_make_rib_brim(brim_outline.empty() ? outline : brim_outline.front(), 0.2f);
                shell.merge(brim_slab);
            }
            std::vector<ColorRGBA> colors;
            for (int id : plate_extruders) colors.push_back(color_of(id));
            for (ColorRGBA &c : colors) c.a(0.66f);   // Orca: make it transparent
            const size_t slab_count = colors.size();
            for (size_t i = 0; i < slab_count; ++i) {
                TriangleMesh part = make_cube(width, depth / slab_count, height);
                part.translate({0.f, depth * i / slab_count, 0.f});
                v.parts.push_back({colors[i], std::make_shared<const TriangleMesh>(std::move(part))});
            }
            if (show_brim && !colors.empty()) v.parts.push_back({colors.front(), std::make_shared<const TriangleMesh>(std::move(brim_slab))});
            v.shell = std::make_shared<const TriangleMesh>(std::move(shell));
        }
        v.bbox = v.shell->transformed_bounding_box(v.matrix);
        out.push_back(std::move(v));
    }
    if (clamped) core.invalidate_full_config();
    return out;
}

// ArrangeJob::prepare_wipe_tower (+ estimate_wipe_tower_info, GLCanvas3D::get_wipe_tower_info)
void add_wipe_towers_for_arrange(ArrangePolygons &unselected, const ArrangePolygons &selected, bool only_plate, int cur_plate,
                                 const arrangement::ArrangeParams &params)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    const DynamicPrintConfig &current_config = core.preset_bundle->prints.get_edited_preset().config;
    auto op = current_config.option("enable_prime_tower");
    if (!(op && op->getBool()) || params.is_seq_print || params.is_belt) return;
    bool need_wipe_tower = false;
    if (auto sop = current_config.option("timelapse_type")) need_wipe_tower = sop->getInt() == TimelapseType::tlSmooth;
    for (const auto &item : selected) {
        std::set<int> obj_extruders(item.extrude_ids.begin(), item.extrude_ids.end());
        if (obj_extruders.size() > 1) { need_wipe_tower = true; break; }
    }
    if (params.allow_multi_materials_on_same_plate) {
        std::map<int, std::set<int>> bed_temp_to_ids;
        for (const auto &item : selected)
            for (auto id : item.extrude_ids) bed_temp_to_ids[item.bed_temp].insert(id);
        for (const auto &be : bed_temp_to_ids)
            if (be.second.size() > 1) { need_wipe_tower = true; break; }
    }

    // the towers the 3D view shows
    const std::vector<TowerVolume> towers = wipe_tower_volumes();
    const DynamicPrintConfig &full_config = core.full_config();
    DynamicPrintConfig &proj_cfg = project_config();
    std::set<int> extruder_ids;   // PartPlateList::get_extruders(true): every plate's filaments
    if (!only_plate)
        for (int i = 0; i < int(p.plates.size()); ++i)
            for (int e : PlateTower(i).get_extruders(true, full_config, proj_cfg)) extruder_ids.insert(e);

    const int plate_count = int(p.plates.size());
    int bedid_unlocked = 0;
    const int beds = only_plate ? 1 : 36;   // MAX_NUM_PLATES
    for (int bedid = 0; bedid < beds; ++bedid) {
        const int plate = only_plate ? cur_plate : std::min(bedid, plate_count - 1);
        if (!only_plate && bedid < plate_count && p.plates[plate].locked) continue;
        const TowerVolume *tv = nullptr;
        if (only_plate || bedid < plate_count)
            for (const TowerVolume &t : towers) if (t.plate == plate) tv = &t;
        ArrangePolygon ap;
        if (tv) {
            // WipeTower::get_arrange_polygon: the volume's box grown by the brim, at the stored position
            float brim = float(current_config.opt_float("prime_tower_brim_width"));
            BoundingBoxf3 bb = tv->shell->bounding_box();
            if (brim < 0) brim = WipeTower::get_auto_brim_by_height(float(bb.max.z()));
            BoundingBoxf box(to_2d(bb.min), to_2d(bb.max));
            box.offset(brim);
            box.offset(brim);
            Polygon poly({scaled(box.min), {scaled(box.max.x()), scaled(box.min.y())}, scaled(box.max), {scaled(box.min.x()), scaled(box.max.y())}});
            ap.poly.contour = std::move(poly);
            ap.translation = scaled(Vec2d(proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x")->get_at(plate),
                                          proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y")->get_at(plate)));
            ap.rotation = 0;
            ++ap.priority;
        } else if (need_wipe_tower) {
            std::set<int> ids = extruder_ids;
            if (only_plate) {
                auto pe = PlateTower(plate).get_extruders(true, full_config, proj_cfg);
                ids = std::set<int>(pe.begin(), pe.end());
            }
            Vec3d wt_size, wt_pos;
            ap = PlateTower(std::min(plate, plate_count - 1)).estimate_wipe_tower_polygon(full_config, plate, wt_pos, wt_size, int(ids.size()));
        } else {
            ++bedid_unlocked;
            continue;
        }
        ap.name = "WipeTower";
        ap.is_virt_object = true;
        ap.is_wipe_tower = true;
        ap.setter = nullptr;   // do not move the wipe tower
        ap.bed_idx = bedid_unlocked;
        unselected.emplace_back(std::move(ap));
        ++bedid_unlocked;
    }
}

// GLCanvas3D::do_move for the tower volume: its new origin goes to the project's wipe_tower_x / y of the plate
static json move(const json &a)
{
    Plater &p = Plater::get();
    const int plate = a.at("plate");
    if (plate < 0 || plate >= int(p.plates.size())) throw CommandError("bad plate");
    ensure_size(int(p.plates.size()));
    DynamicPrintConfig &proj_cfg = project_config();
    auto *wx = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
    auto *wy = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
    ConfigOptionFloat x(wx->get_at(plate) + a.at("delta")[0].get<double>()), y(wy->get_at(plate) + a.at("delta")[1].get<double>());
    wx->set_at(&x, plate, 0);
    wy->set_at(&y, plate, 0);
    Core::get().invalidate_full_config();   // the full config carries wipe_tower_x / y
    p.changed();
    return p.state();
}

static Commands reg({
    {"wipe_tower.move", move},
});

} // namespace orca
