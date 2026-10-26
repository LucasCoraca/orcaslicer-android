// The prime (wipe) tower of each plate in the 3D view: where it goes (PartPlateList::set_default_wipe_tower_pos_for_plate,
// the clamp of GLCanvas3D::reload_scene), what it looks like (GLVolumeCollection::load_wipe_tower_preview /
// load_real_wipe_tower_preview), moving it by hand and keeping arrange away from it (ArrangeJob::prepare_wipe_tower).
// PartPlate's own estimate functions are compiled from upstream (tools/extract_functions.py -> PlateTower.inc).
#pragma once
#include <memory>
#include <set>
#include <vector>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/WipeTowerEstimate.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca {

// the members of PartPlate that its wipe tower functions read
struct PlateTower
{
    Slic3r::Model                      *m_model{nullptr};
    int                                 m_plate_index{0};
    int                                 m_width{0}, m_depth{0};
    std::vector<Slic3r::FilamentInfo>   slice_filaments_info;
    std::set<std::pair<int, int>>       instances, outside;

    explicit PlateTower(int plate);
    bool contain_instance_totally(int obj_id, int instance_id) const;
    bool contain_any_instance_totally(int obj_id) const;

    std::vector<int> get_extruders(bool conside_custom_gcode, const Slic3r::DynamicPrintConfig &glb_config, const Slic3r::DynamicPrintConfig &project_config,
                                   bool expand_mixed_slots = true) const;
    Slic3r::WipeTowerFootprint estimate_wipe_tower_footprint(const Slic3r::DynamicPrintConfig &config, int plate_extruder_size = 0,
                                                             bool use_global_objects = false) const;
    Slic3r::arrangement::ArrangePolygon estimate_wipe_tower_polygon(const Slic3r::DynamicPrintConfig &config, int plate_index, Slic3r::Vec3d &wt_pos,
                                                                    Slic3r::Vec3d &wt_size, int plate_extruder_size = 0, bool use_global_objects = false) const;
};

// one plate's tower as the 3D view draws it (GLWipeTowerVolume): coloured parts in tower coordinates
struct TowerVolume
{
    int                                                     plate{0};
    Slic3r::Transform3d                                     matrix;   // world
    std::vector<std::pair<Slic3r::ColorRGBA, std::shared_ptr<const Slic3r::TriangleMesh>>> parts;
    std::shared_ptr<const Slic3r::TriangleMesh>             shell;    // picking and bounding box
    Slic3r::BoundingBoxf3                                   bbox;     // world
};

// PartPlateList::set_default_wipe_tower_pos_for_plate (core lock held)
void set_default_wipe_tower_pos(int plate, bool init_pos = false);
void set_default_wipe_tower_pos_all();
// GLCanvas3D::reload_scene's wipe tower part: clamps the stored positions and builds the volumes (core lock held)
std::vector<TowerVolume> wipe_tower_volumes();
// ArrangeJob::prepare_wipe_tower: fixed items for every unlocked bed
void add_wipe_towers_for_arrange(Slic3r::arrangement::ArrangePolygons &unselected, const Slic3r::arrangement::ArrangePolygons &selected, bool only_plate,
                                 int cur_plate, const Slic3r::arrangement::ArrangeParams &params);

} // namespace orca
