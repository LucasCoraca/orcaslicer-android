// Arrange and auto-orient: ports of the desktop ArrangeJob / OrientJob (slic3r/GUI/Jobs) and the PartPlateList
// pre/post-processing they use. They run on the engine thread, reporting progress events.
#include <numeric>

#include <boost/format.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "PrimeTower.hpp"
#include "Slicer.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Orient.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace orca {

static constexpr int MAX_PLATES = 36;   // PartPlateList::MAX_PLATES_COUNT

static void notify(const std::string &text) { Host::event("toast", {{"text", text}}); }

// GLCanvas3D::ArrangeSettings, kept in the app config ("arrange" section, as the desktop's arrange dialog does)
static ArrangeParams init_arrange_params(const DynamicPrintConfig &full, bool only_plate, int plate)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    ArrangeParams params;
    PrintConfig pc;
    pc.apply(full, true);
    auto get = [&](const char *k, const char *def) { std::string v = core.app_config->get("arrange", k); return v.empty() ? std::string(def) : v; };
    // Print::object_skirt_offset of the plate's print (zero until it has been applied once)
    double object_skirt_offset = 0;
    if (PlateSlice *ps = Slicer::get().slice(only_plate ? plate : p.current_plate); ps && ps->print && !ps->print->objects().empty())
        object_skirt_offset = std::get<0>(ps->print->object_skirt_offset());
    params.clearance_height_to_rod = pc.extruder_clearance_height_to_rod.value;
    params.clearance_height_to_lid = pc.extruder_clearance_height_to_lid.value;
    params.clearance_radius = pc.extruder_clearance_radius.value + object_skirt_offset * 2;
    params.object_skirt_offset = object_skirt_offset;
    params.printable_height = pc.printable_height.value;
    params.allow_rotations = get("enable_rotation", "0") == "1";
    params.nozzle_height = pc.nozzle_height.value;
    params.align_center = pc.best_object_pos.value;
    params.allow_multi_materials_on_same_plate = get("allow_multi_materials_on_same_plate", "1") == "1";
    params.avoid_extrusion_cali_region = get("avoid_extrusion_cali_region", "1") == "1";
    params.is_seq_print = pc.print_sequence.value == PrintSequence::ByObject;
    params.min_obj_distance = scaled(std::stod(get("min_object_distance", "0")));
    params.align_to_y_axis = get("align_to_y_axis", "0") == "1";
    if (only_plate) {
        const Plate &pl = p.plates[plate];
        if (pl.config.has("print_sequence")) {
            bool by_object = pl.config.opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject;
            if (by_object != params.is_seq_print) params.min_obj_distance = 0;
            params.is_seq_print = by_object;
        }
    }
    if (params.is_seq_print) { params.bed_shrink_x = BED_SHRINK_SEQ_PRINT; params.bed_shrink_y = BED_SHRINK_SEQ_PRINT; }
    return params;
}

// PartPlateList::preprocess_exclude_areas: the excluded bed areas as fixed items on every plate
static void exclude_areas(ArrangePolygons &unselected, int num_plates, float inflation = 0)
{
    auto *ex = Core::get().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("bed_exclude_area");
    if (!ex || ex->values.size() < 3) return;
    BoundingBoxf bb(ex->values);
    Polygon ap({{scaled(bb.min.x()), scaled(bb.min.y())}, {scaled(bb.max.x()), scaled(bb.min.y())},
                {scaled(bb.max.x()), scaled(bb.max.y())}, {scaled(bb.min.x()), scaled(bb.max.y())}});
    for (int j = 0; j < num_plates; j++) {
        ArrangePolygon ret;
        ret.poly.contour = ap;
        ret.translation = Vec2crd(0, 0);
        ret.rotation = 0.f;
        ret.is_virt_object = true;
        ret.bed_idx = j;
        ret.height = 1;
        ret.name = "ExcludedRegion0";
        ret.inflation = inflation;
        unselected.emplace_back(std::move(ret));
    }
}

// PartPlateList::preprocess_arrange_polygon: an item's plate as the arranger's bed index, in plate-local coordinates
static void to_plate_local(Plater &p, ArrangePolygon &ap, int plate)
{
    const int cols = p.plate_cols();
    ap.bed_idx = plate;
    ap.row = plate / cols;
    ap.col = plate % cols;
    ap.translation(X) -= scaled<double>(p.plate_stride_x() * ap.col);
    ap.translation(Y) += scaled<double>(p.plate_stride_y() * ap.row);
}

static json arrange(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    Model &model = p.model();
    const bool only_plate = a.contains("plate");
    const int cur = a.value("plate", p.current_plate);
    p.update_plates();
    const DynamicPrintConfig &config = core.full_config();
    ArrangePolygons selected, unselected, unprintable, locked_items;
    std::vector<ModelInstance *> sel_inst, unsel_inst, unprint_inst, locked_inst;

    if (only_plate && p.plates[cur].locked) {
        notify(_u8L("This plate is locked.\nCannot auto-arrange on this plate."));
        return p.state();
    }
    p.take_snapshot("Arrange");
    Model::setExtruderParams(config, int(core.preset_bundle->filament_presets.size()));
    { PrintConfig pc; pc.apply(config, true); Model::setPrintSpeedTable(config, pc); }
    ArrangeParams params = init_arrange_params(config, only_plate, cur);

    int lockplate_cnt = 0;
    for (int oi = 0; oi < int(model.objects.size()); ++oi) {
        ModelObject *mo = model.objects[oi];
        for (int ii = 0; ii < int(mo->instances.size()); ++ii) {
            ModelInstance *mi = mo->instances[ii];
            ArrangePolygon ap = get_instance_arrange_poly(mi, config);
            int plate = p.plate_of(oi, ii);
            if (only_plate) {
                // ArrangeJob::prepare_partplate + preprocess_arrange_polygon_other_locked
                bool in_plate = plate == cur;
                if (in_plate || !mi->printable) {
                    auto &cont = mi->printable ? selected : unprintable;
                    (mi->printable ? sel_inst : unprint_inst).push_back(mi);
                    ap.itemid = int(cont.size());
                    cont.emplace_back(std::move(ap));
                } else {
                    if (plate >= 0) to_plate_local(p, ap, plate); else ap.bed_idx = MAX_PLATES;
                    ap.itemid = int(locked_items.size());
                    locked_items.emplace_back(std::move(ap));
                    locked_inst.push_back(mi);
                }
            } else {
                // ArrangeJob::prepare_all + preprocess_arrange_polygon
                bool locked = plate >= 0 && p.plates[plate].locked;
                if (locked) {
                    to_plate_local(p, ap, plate);
                    ap.itemid = int(locked_items.size());
                    locked_items.emplace_back(std::move(ap));
                    locked_inst.push_back(mi);
                } else {
                    auto &cont = mi->printable ? selected : unprintable;
                    (mi->printable ? sel_inst : unprint_inst).push_back(mi);
                    ap.itemid = int(cont.size());
                    cont.emplace_back(std::move(ap));
                }
            }
        }
    }
    (void) lockplate_cnt;
    if (selected.empty()) {
        notify(_u8L("No arrangeable objects are selected."));
        return p.state();
    }
    exclude_areas(unselected, only_plate ? 1 : MAX_PLATES);
    add_wipe_towers_for_arrange(unselected, selected, only_plate, cur, params);   // ArrangeJob::prepare_wipe_tower

    // ArrangeJob::check_unprintable
    for (size_t k = 0; k < selected.size();) {
        if (selected[k].poly.area() < 0.001 || selected[k].height > params.printable_height) {
            if (selected[k].poly.area() < 0.001) notify((boost::format(_u8L("Object %s has zero size and can't be arranged.")) % selected[k].name).str());
            unprintable.push_back(selected[k]);
            unprint_inst.push_back(sel_inst[k]);
            selected.erase(selected.begin() + k);
            sel_inst.erase(sel_inst.begin() + k);
        } else
            ++k;
    }
    for (size_t k = 0; k < selected.size(); ++k) selected[k].itemid = int(k);
    // ArrangeJob::process
    const DynamicPrintConfig *cfg = &config;
    update_arrange_params(params, cfg, selected);
    update_selected_items_inflation(selected, cfg, params);
    update_unselected_items_inflation(unselected, cfg, params);
    update_selected_items_axis_align(selected, cfg, params);
    Points bedpts = get_shrink_bedpts(cfg, params);
    const size_t total = selected.size();
    params.progressind = [total](unsigned n, std::string name) {
        Host::event("progress", {{"text", GUI::format(_u8L("Arranging %1%"), name)}, {"percent", total ? int(n * 100 / total) : 100}});
    };
    arrangement::arrange(selected, unselected, bedpts, params);
    std::vector<std::pair<ArrangePolygon, ModelInstance *>> sel;
    for (auto &ap : selected) sel.push_back({ap, sel_inst[ap.itemid]});
    std::sort(sel.begin(), sel.end(), [](auto &x, auto &y) { return x.first.itemid < y.first.itemid; });

    // ArrangeJob::finalize: bed index -> plate (skipping locked plates, creating plates as needed), then plate offsets
    int beds = 0;
    bool unpackable = false;
    std::vector<int> plate_of_bed;
    for (auto &[ap, mi] : sel) {
        if (ap.bed_idx < 0) { unpackable = true; continue; }
        if (only_plate) {
            // postprocess_bed_index_for_current_plate
            ap.bed_idx = ap.bed_idx == 0 ? cur : int(p.plates.size());
        } else {
            // postprocess_bed_index_for_selected
            for (size_t i = 0; i < p.plates.size(); ++i) {
                if (p.plates[i].locked) ap.bed_idx += 1;
                else if (ap.bed_idx <= int(i)) break;
            }
        }
        beds = std::max(beds, ap.bed_idx);
    }
    // create plates for the beds beyond the last one (PartPlateList::create_plate keeps existing objects in place)
    if (beds >= int(p.plates.size())) p.resize_plates(std::min(beds + 1, MAX_PLATES));
    const int cols = p.plate_cols();
    auto post = [&](ArrangePolygon &ap) {   // PartPlateList::postprocess_arrange_polygon
        if (ap.bed_idx == -1) {
            ap.bed_idx = int(p.plates.size());
            BoundingBox apbox = get_extents(ap.transformed_poly());
            ap.translation(X) = 0.5 * apbox.size()[0];
            ap.translation(Y) = scaled<double>(p.bed_box().size().y()) - 0.5 * apbox.size()[1];
        }
        ap.row = ap.bed_idx / cols;
        ap.col = ap.bed_idx % cols;
        ap.translation(X) += scaled<double>(p.plate_stride_x() * ap.col);
        ap.translation(Y) -= scaled<double>(p.plate_stride_y() * ap.row);
    };
    for (auto &[ap, mi] : sel) {
        post(ap);
        int bed = ap.bed_idx;
        ap.bed_idx = 0;   // the translation already holds the plate offset
        mi->apply_arrange_result(ap.translation.cast<double>(), ap.rotation);   // scaled, it unscales
        mi->arrange_order = ap.itemid;
        (void) bed;
    }
    // unprintable items go past the last plate
    for (size_t k = 0; k < unprintable.size(); ++k) {
        ArrangePolygon ap = unprintable[k];
        ap.bed_idx = beds + 1;
        post(ap);
        unprint_inst[k]->apply_arrange_result(ap.translation.cast<double>(), ap.rotation);
    }
    for (ModelObject *mo : model.objects) { mo->invalidate_bounding_box(); mo->ensure_on_bed(); }
    // rebuild_plates_after_arrangement: drop trailing empty, unlocked plates
    p.update_plates();
    if (!only_plate)
        while (p.plates.size() > 1 && p.plates.back().instances.empty() && !p.plates.back().locked) p.resize_plates(p.plates.size() - 1);
    Host::event("progress", {{"text", ""}, {"percent", 100}});
    if (unpackable) notify(_u8L("Arranging complete, but some items were not able to be arranged. Reduce spacing and try again."));
    p.changed();
    return p.state();
}

// OrientJob: selected objects (or the current plate's)
static json orient(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    Model &model = p.model();
    const DynamicPrintConfig &config = core.full_config();
    std::set<int> objs;
    if (a.contains("plate")) {
        for (auto &[o, i] : p.plates[a["plate"].get<int>()].instances) objs.insert(o);
    } else
        for (auto &s : p.selection) objs.insert(s.obj);
    if (objs.empty()) for (int o = 0; o < int(model.objects.size()); ++o) objs.insert(o);
    p.take_snapshot("Orient");
    orientation::OrientMeshs selected, unselected;
    for (int oi : objs) {
        ModelObject *obj = model.objects[oi];
        for (int ii = 0; ii < int(obj->instances.size()); ++ii) {
            int plate = p.plate_of(oi, ii);
            if (plate >= 0 && p.plates[plate].locked) continue;
            ModelInstance *instance = obj->instances[ii];
            orientation::OrientMesh om;
            om.name = obj->name;
            om.mesh = obj->mesh();
            om.overhang_angle = obj->config.has("support_threshold_angle") ? obj->config.opt_int("support_threshold_angle") : config.opt_int("support_threshold_angle");
            if (config.has("fan_direction") && config.has("auxiliary_fan")) {
                FanDirection dir = config.option<ConfigOptionEnum<FanDirection>>("fan_direction")->value;
                if (dir == FanDirection::fdUndefine || !config.opt_bool("auxiliary_fan")) om.cooling_direction = {0, 0, 0};
                else if (dir == FanDirection::fdRight) { om.cooling_direction = {1, 0, 0}; om.has_cooling_fan = true; }
                else { om.cooling_direction = {-1, 0, 0}; om.has_cooling_fan = true; }
            }
            om.setter = [instance](const orientation::OrientMesh &r) {
                instance->rotate(r.rotation_matrix);
                instance->rotate(r.rotation_matrix_vertical);
                instance->get_object()->invalidate_bounding_box();
                instance->get_object()->ensure_on_bed();
            };
            (obj->printable ? selected : unselected).emplace_back(std::move(om));
        }
    }
    orientation::OrientParams params;
    params.min_volume = core.app_config->get("orient", "min_area") != "1";
    const size_t count = selected.size();
    params.progressind = [count](unsigned st, std::string name) {
        Host::event("progress", {{"text", GUI::format(_u8L("Orienting %1%"), name)}, {"percent", count ? int(st * 100 / count) : 100}});
    };
    orientation::orient(selected, unselected, params);
    for (auto &m : selected) m.apply();
    Host::event("progress", {{"text", ""}, {"percent", 100}});
    p.changed();
    return p.state();
}

// Plater::center_selection / "Center" in the object menu: the object to its plate's centre
static json center(const json &a)
{
    Plater &p = Plater::get();
    ModelObject *mo = p.model().objects.at(a.at("obj").get<int>());
    p.take_snapshot("Center");
    for (int ii = 0; ii < int(mo->instances.size()); ++ii) {
        int plate = std::max(0, p.plate_of(a["obj"].get<int>(), ii));
        BoundingBoxf3 pb = p.plate_box(plate), bb = mo->instance_bounding_box(ii);
        Vec3d d = pb.center() - bb.center();
        mo->instances[ii]->set_offset(mo->instances[ii]->get_offset() + Vec3d(d.x(), d.y(), 0));
    }
    mo->invalidate_bounding_box();
    p.changed();
    return p.state();
}

// FillBedJob (instances): copies of the selected object fill the free area of its plate, then the plate is arranged
static json fill_bed(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    Model &model = p.model();
    int obj_idx = -1, sel_id = 0;
    for (const SelItem &s : p.selection) { obj_idx = s.obj; sel_id = std::max(0, s.inst); break; }
    if (obj_idx < 0 || obj_idx >= int(model.objects.size())) return p.state();
    ModelObject *model_object = model.objects[obj_idx];
    if (model_object->instances.empty()) return p.state();
    p.update_plates();
    const int cur_plate = std::max(0, p.plate_of(obj_idx, sel_id));
    p.current_plate = cur_plate;   // select_plate_by_obj
    const DynamicPrintConfig &global_config = core.full_config();
    Model::setExtruderParams(global_config, int(core.preset_bundle->filament_presets.size()));
    ArrangeParams params = init_arrange_params(global_config, true, cur_plate);
    const BoundingBoxf3 pb3 = p.plate_box(cur_plate);
    const BoundingBox plate_bb(Point(scaled(pb3.min.x()), scaled(pb3.min.y())), Point(scaled(pb3.max.x()), scaled(pb3.max.y())));

    ArrangePolygons m_selected, m_unselected, m_locked;
    std::vector<ModelInstance *> sel_inst;
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx) {
        ModelObject *mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx) {
            const bool selected = int(oidx) == obj_idx;
            ArrangePolygon ap = get_instance_arrange_poly(mo->instances[inst_idx], global_config);
            const BoundingBox ap_bb = ap.transformed_poly().contour.bounding_box();
            ap.name = mo->name;
            if (selected && mo->instances[inst_idx]->printable) {
                ++ap.priority;
                ap.itemid = int(m_selected.size());
                m_selected.emplace_back(ap);
                sel_inst.push_back(mo->instances[inst_idx]);
            } else if (plate_bb.contains(ap_bb)) {
                to_plate_local(p, ap, cur_plate);
                ap.bed_idx = 0;
                ap.itemid = int(m_unselected.size());
                m_unselected.emplace_back(ap);
            } else {
                ap.bed_idx = MAX_PLATES;
                ap.itemid = int(m_locked.size());
                m_locked.emplace_back(ap);
            }
        }
    }
    if (m_selected.empty()) return p.state();
    exclude_areas(params.excluded_regions, 1, 0);
    exclude_areas(m_unselected, 1);
    add_wipe_towers_for_arrange(m_unselected, m_selected, true, cur_plate, params);
    Points bedpts = get_bed_shape(global_config);

    const double sc = scaled<double>(1.) * scaled(1.);
    auto polys = offset_ex(m_selected.front().poly, params.min_obj_distance / 2);
    ExPolygon poly = polys.empty() ? m_selected.front().poly : polys.front();
    const double poly_area = poly.area() / sc;
    const double unsel_area = std::accumulate(m_unselected.begin(), m_unselected.end(), 0.,
                                              [](double s, const auto &ap) { return s + (ap.bed_idx == 0) * ap.poly.area(); }) / sc;
    const double fixed_area = unsel_area + m_selected.size() * poly_area;
    const double bed_area = Slic3r::Polygon{bedpts}.area() / sc;
    const int needed_items = poly_area > 0 ? int((bed_area - fixed_area) / poly_area) : 0;
    ArrangePolygon template_ap = get_instance_arrange_poly(model_object->instances[sel_id], global_config);
    for (int i = 0; i < needed_items; ++i) {
        ArrangePolygon ap = template_ap;
        ap.poly = m_selected.front().poly;
        ap.bed_idx = MAX_PLATES;
        ap.itemid = -1;
        m_selected.emplace_back(ap);
    }
    p.take_snapshot("Fill bed");

    // process
    update_arrange_params(params, &global_config, m_selected);
    bedpts = get_shrink_bedpts(&global_config, params);
    update_selected_items_inflation(m_selected, &global_config, params);
    update_unselected_items_inflation(m_unselected, &global_config, params);
    bool do_stop = false;
    params.stopcondition = [&do_stop]() { return do_stop; };
    const size_t status_range = m_selected.size();
    params.progressind = [status_range](unsigned st, std::string str) {
        if (st > 0) Host::event("progress", {{"text", GUI::format(_u8L("Filling %1%"), str)}, {"percent", int(st * 100 / status_range)}});
    };
    params.on_packed = [&do_stop](const ArrangePolygon &ap) { do_stop = ap.bed_idx > 0 && ap.priority == 0; };
    params.do_final_align = !core.preset_bundle->is_bbl_vendor();
    arrangement::arrange(m_selected, m_unselected, bedpts, params);

    // finalize: the copies that found room become instances
    const int added_cnt = std::accumulate(m_selected.begin(), m_selected.end(), 0, [](int s, auto &ap) { return s + int(ap.priority == 0 && ap.bed_idx == 0); });
    if (added_cnt > 0) {
        const int cols = p.plate_cols();
        const double offset_base = 0.05 * std::max(p.bed_box().size().x(), p.bed_box().size().y());   // get_size_proportional_to_max_bed_size
        double offset = offset_base;
        for (ArrangePolygon &ap : m_selected) {
            if (ap.bed_idx != 0) continue;
            ap.bed_idx = cur_plate;
            ap.row = ap.bed_idx / cols;
            ap.col = ap.bed_idx % cols;
            ap.translation(X) += scaled<double>(p.plate_stride_x() * ap.col);
            ap.translation(Y) -= scaled<double>(p.plate_stride_y() * ap.row);
            if (ap.itemid >= 0 && ap.priority > 0) {
                sel_inst[ap.itemid]->apply_arrange_result(ap.translation.cast<double>(), ap.rotation);
            } else {
                ModelInstance *model_instance = model_object->instances.back();
                const Vec3d offset_vec = model_instance->get_offset() + Vec3d(offset, offset, 0.0);
                ModelInstance *ni = model_object->add_instance(offset_vec, model_instance->get_scaling_factor(), model_instance->get_rotation(),
                                                               model_instance->get_mirror());
                ni->apply_arrange_result(ap.translation.cast<double>(), ap.rotation);
                offset += offset_base;
            }
        }
        model_object->invalidate_bounding_box();
        model_object->ensure_on_bed();
        p.changed();
        Host::event("progress", {{"text", ""}, {"percent", 100}});
        return arrange({{"plate", cur_plate}});   // m_plater->arrange() with PREPARE_STATE_MENU
    }
    Host::event("progress", {{"text", ""}, {"percent", 100}});
    p.changed();
    return p.state();
}

static Commands reg({
    {"arrange", arrange},
    {"fill_bed", fill_bed},
    {"orient", orient},
    {"object.center", center},
});

} // namespace orca
