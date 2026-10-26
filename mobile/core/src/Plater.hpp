// The project being edited: the model, its plates (desktop PartPlateList), the selection and the undo history.
#pragma once
#include <atomic>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace orca {

using json = nlohmann::json;

struct Plate
{
    std::string                  name;
    bool                         locked{false};
    Slic3r::DynamicPrintConfig   config;            // plate settings (bed type, print sequence, spiral mode ...)
    std::set<std::pair<int, int>> instances;        // (object, instance) on this plate
    std::set<std::pair<int, int>> outside;          // ... partly outside the printable area
    bool                         slice_valid{false};
};

struct SelItem
{
    int obj{-1};
    int inst{-1};   // -1: every instance of the object
    int vol{-1};    // -1: the whole object / instance
    bool operator<(const SelItem &o) const { return std::tie(obj, inst, vol) < std::tie(o.obj, o.inst, o.vol); }
    bool operator==(const SelItem &o) const { return obj == o.obj && inst == o.inst && vol == o.vol; }
};

class Plater
{
public:
    static Plater &get();

    Slic3r::Model &model();

    // printer geometry, from the edited printer preset (Bed3D / BuildVolume on the desktop)
    Slic3r::Pointfs           bed_shape() const;
    Slic3r::BoundingBoxf      bed_box() const;     // the printable area of plate 0
    double                    printable_height() const;
    Slic3r::Vec3d             plate_origin(int i) const;   // PartPlateList::compute_origin
    Slic3r::BoundingBoxf3     plate_box(int i) const;
    int                       plate_cols() const;
    double                    plate_stride_x() const;
    double                    plate_stride_y() const;

    std::vector<Plate>        plates;
    int                       current_plate{0};
    std::set<SelItem>         selection;
    std::string               project_name;
    std::string               project_path;        // the 3MF this project was opened from / saved to
    bool                      dirty{false};
    bool                      only_gcode{false};
    // the Assembly view (AssembleView canvas): instances at their assemble transformations, the parts exploded by the ratio
    std::atomic<bool>         assemble_view{false};
    std::atomic<bool>         assemble_part_mode{false};   // Selection::Volume in the assembly control's selection mode
    std::atomic<float>        explosion_ratio{1.f};
    // the instance matrix the canvas shows: its assemble transformation in the Assembly view
    Slic3r::Transform3d       shown_instance_matrix(const Slic3r::ModelInstance &mi) const;   // Plater::m_only_gcode: a G-code file is previewed, nothing to edit
    std::atomic<unsigned>     revision{0};         // bumped on every change: the 3D view re-uploads meshes

    // PartPlateList::reload_all_objects / notify_instance_update: which plate each instance is on
    void update_plates();
    int  plate_of(int obj, int inst) const;

    // undo / redo (desktop: Plater::take_snapshot, UndoRedo::Stack)
    void take_snapshot(const std::string &name);
    bool undo();
    bool redo();

    void changed(bool snapshot_taken = true);   // after any edit: plates, dirty flag, event to the UI
    json state();                               // what the object list / plate bar show

    void reset_project();
    // PartPlateList::create_plate / delete_plate: change the plate count, keeping every object where it is on its
    // plate (plate origins move when the number of columns changes)
    void resize_plates(size_t n);

private:
    struct Snapshot
    {
        std::string             name;
        Slic3r::Model           model;
        std::vector<Plate>      plates;
        int                     current_plate;
        std::set<SelItem>       selection;
    };
    std::vector<Snapshot> m_undo, m_redo;
};

} // namespace orca
