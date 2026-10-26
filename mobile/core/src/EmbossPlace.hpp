// What GLGizmoEmboss and GLGizmoSVG do the same way with their volume (EmbossJob.cpp start_* / GLGizmoEmboss.cpp
// helpers / SurfaceDrag): create it on the object's surface or as a new object, re-mesh it, slide it over the surface,
// turn it around its Z, move it from the surface, face it to the camera, change its operation.
#pragma once
#include <optional>

#include "EmbossCommon.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"

namespace orca::emboss {

struct Created { int obj, inst, vol; };
// start_create_volume (at a screen point) / start_create_volume_without_position: on the selected object's surface, or
// a new object on the bed when nothing is selected; throws JobException
Created create(DataBase &base, Slic3r::ModelVolumeType type, std::optional<Slic3r::Vec2d> screen, std::optional<float> angle,
               std::optional<float> distance, const std::string &object_snapshot, const std::string &volume_snapshot);

// start_update_volume + UpdateJob / UpdateSurfaceVolumeJob; throws JobException
bool update(Slic3r::ModelVolume &volume, int instance, DataBase &base, const std::string &snapshot /* empty: none */);

// do_local_z_rotate (the instance of an embossed object)
void local_z_rotate(Slic3r::ModelVolume &volume, int instance, double angle);
// GLGizmoRotate (Z, set_force_local_coordinate) of the volume: the volume's world rotation with the centre of its box as
// translation, and the ring radius (Offset + the radius of the box in the volume's axes)
void rotate_ring(const Slic3r::ModelVolume &volume, int instance, Slic3r::Transform3d &frame, double &radius);
// do_local_z_move
void local_z_move(Slic3r::ModelVolume &volume, double distance);
// face_selected_volume_to_camera
void face_camera(Slic3r::ModelVolume &volume, int instance, const Camera &camera, bool keep_up);
// draw_model_type: true when the volume moved in or out of the object (it must be re-meshed)
bool change_type(Slic3r::ModelVolume &volume, int instance, Slic3r::ModelVolumeType type, const std::string &snapshot);
// the volume's index triple in the model
SelItem index_of(const Slic3r::ModelVolume &volume, int instance);

// SurfaceDrag: on_mouse_surface_drag for a finger
class SurfaceDrag
{
public:
    // the finger must be on the volume; not for an embossed object
    bool begin(Slic3r::ModelVolume &volume, int instance, const Camera &cam, double px, double py, bool keep_up);
    void move(Slic3r::ModelVolume &volume, int instance, const Camera &cam, double px, double py, bool keep_up);
    // one undo step "Move over surface"
    void end(Slic3r::ModelVolume &volume);
    bool active() const { return m_drag.has_value(); }
    void reset() { m_drag.reset(); }

private:
    struct Drag { Slic3r::Transform3d world, instance_inv; std::optional<float> start_angle; Slic3r::Vec2d offset; Slic3r::Transform3d before; };
    std::optional<Drag> m_drag;
};

} // namespace orca::emboss
