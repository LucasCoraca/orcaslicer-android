// The transform gizmos of the 3D view (GLGizmosManager): Move, Rotate, Scale and Lay on face, drawn and dragged
// natively like GLGizmoMove3D / GLGizmoRotate3D / GLGizmoScale3D / GLGizmoFlatten. Their input panels
// (GizmoObjectManipulation) are Compose; the commands they use are registered in GizmoCommands.cpp.
#pragma once
#include <mutex>
#include <string>
#include <vector>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <memory>

namespace orca {

struct Camera;
struct RenderVolume;

// GLGizmoCut3D::get_connector_mesh
indexed_triangle_set cut_connector_mesh(const Slic3r::CutConnectorAttributes &a, float space, float bulge);

class Gizmos
{
public:
    static Gizmos &get();

    void               set_active(const std::string &name);   // "", "move", "rotate", "scale", "flatten"
    std::string        active();
    void               set_ui_scale(float s) { m_ui_scale = s; }
    float              ui_scale() const { return m_ui_scale; }
    double inv_zoom(const Camera &cam) const;   // GLGizmoBase::INV_ZOOM: world units per pixel at the target
    float  grabber_px() const { return 16.f * m_ui_scale; }   // Grabber::FixedGrabberSize (dp)
    float  touch_px() const { return 28.f * m_ui_scale; }     // how far from a grabber a finger still takes it

    // GLGizmoCut3D: the cut plane of the selected instance (world centre + rotation), set up by update_planes()
    struct CutPlane
    {
        bool                  valid{false};
        int                   obj{-1}, inst{0};
        Slic3r::Vec3d         center{0, 0, 0}, bb_center{0, 0, 0};
        Slic3r::Transform3d   rotation{Slic3r::Transform3d::Identity()};
        Slic3r::BoundingBoxf3 box;
        double                radius{0};
        // dovetail mode: the groove plane mesh (GroovePlane), grey part colours, red when the groove is invalid
        bool                  groove{false}, valid_groove{true};
        std::shared_ptr<const Slic3r::TriangleMesh> groove_mesh;
    };
    void               set_cut_groove(bool groove, std::shared_ptr<const Slic3r::TriangleMesh> mesh, bool valid);
    // the cut connectors being edited (ModelObject::cut_connectors, copied for drawing) and the cross section
    struct CutEdit
    {
        bool                                editing{false};
        std::vector<Slic3r::CutConnector>   connectors;   // pos relative to the instance offset, like the desktop
        std::vector<bool>                   selected;
        Slic3r::ExPolygons                  section;      // in the plane frame (scaled), for is_projection_inside_cut
        Slic3r::Transform3d                 to_plane{Slic3r::Transform3d::Identity()};
        Slic3r::Vec3d                       offset{0, 0, 0};   // instance offset
        float                               snap_space{0.3f}, snap_bulge{0.15f};
    };
    void               set_cut_edit(CutEdit e);
    // GLGizmoCut3D::PartSelection: the parts of the cut, drawn in the upper / lower colours in place of the object
    struct CutPart { std::shared_ptr<const Slic3r::TriangleMesh> mesh; Slic3r::Transform3d matrix; bool upper{true}, modifier{false}; };
    void               set_cut_parts(int obj, std::vector<CutPart> parts);
    bool               cut_parts_hide(int obj);
    CutEdit            cut_edit();
    CutPlane           cut_plane();
    void               set_cut_plane(const Slic3r::Vec3d &center, const Slic3r::Transform3d &rotation);
    void               reset_cut_plane();
    // GLGizmoCut3D::process_cut_line (Shift + drag on the desktop): a drag that does not take a grabber draws the line
    void               set_cut_line_mode(bool on) { std::lock_guard<std::mutex> l(m_mutex); m_cut_line_mode = on; }
    bool               cut_line_mode() { std::lock_guard<std::mutex> l(m_mutex); return m_cut_line_mode; }
    // the colour clip plane of the gouraud shader while cutting: normal (pointing down) + offset, for the cut object
    bool               color_clip(int obj, float plane[4]);
    // the clipping plane (hides what is above it) of the cut object while connectors are edited
    bool               clip(int obj, float plane[4]);

    // GLGizmoEmboss / GLGizmoSVG m_rotate_gizmo: the ring around the text / SVG volume's own Z axis (GLGizmoRotate in
    // local coordinates, grabber upward). frame: the volume's world rotation with the ring centre as translation.
    void               set_emboss_ring(bool on, const Slic3r::Transform3d &frame, double radius);
    void               set_emboss_ring_hidden(bool hidden) { std::lock_guard<std::mutex> l(m_mutex); m_ring.hidden = hidden; }
    bool               emboss_ring_drag_start(const Camera &cam, double px, double py);
    bool               emboss_ring_dragging() { std::lock_guard<std::mutex> l(m_mutex); return m_ring.dragging; }

    // GL thread, after the volumes, on top of them (the gizmos clear the depth buffer like on_render())
    void render(const std::vector<RenderVolume> &vols, unsigned revision, const Camera &cam, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    // the flatten planes of the selected instance, under the core lock (Viewport::snapshot)
    void update_planes();

    // UI thread
    bool           drag_start(const std::vector<RenderVolume> &vols, unsigned revision, const Camera &cam, double px, double py);
    void           drag_move(const Camera &cam, double px, double py);
    nlohmann::json drag_end();
    bool           dragging() { std::lock_guard<std::mutex> l(m_mutex); return m_drag.grabber >= 0; }
    // applied (premultiplied, world) to the selected volumes while a grabber is dragged
    Slic3r::Transform3d drag_transform();
    // Lay on face: the plane under the finger, {obj, inst, normal} or null
    nlohmann::json pick_plane(const Camera &cam, double px, double py);
    void           set_pressed_plane(int i) { std::lock_guard<std::mutex> l(m_mutex); m_pressed_plane = i; }

private:
    struct SelBox
    {
        bool                   valid{false};
        Slic3r::BoundingBoxf3  box;          // world
        Slic3r::Vec3d          center;
        double                 sphere{0};    // Selection::get_bounding_sphere radius
    };
    SelBox selection_box(const std::vector<RenderVolume> &vols, unsigned revision);
    Slic3r::Transform3d transform_locked() const;

    struct Drag
    {
        int           grabber{-1};   // move: axis; rotate: axis; scale: 0..9 as GLGizmoScale3D
        SelBox        start;
        Slic3r::Vec3d start_pos{0, 0, 0};      // the dragged point at start (m_starting_drag_position)
        Slic3r::Vec3d plane_normal{0, 0, 1};
        double        start_angle{0};
        double        value{0};               // displacement, angle or ratio
        double        inv_zoom{1};
        Slic3r::Transform3d cut_rotation{Slic3r::Transform3d::Identity()};
    };

    struct EmbossRing
    {
        bool                on{false}, hidden{false}, dragging{false};
        Slic3r::Transform3d frame{Slic3r::Transform3d::Identity()};
        double              radius{0};
        double              value{0};   // the angle dragged, from the upward grabber
    };
    EmbossRing   m_ring;

    std::mutex   m_mutex;
    std::string  m_active;
    float        m_ui_scale{1};
    Drag         m_drag;
    SelBox       m_box;          // cached for m_box_revision
    unsigned     m_box_revision{~0u};

    struct Plane
    {
        Slic3r::Vec3d              normal;    // object coordinates
        std::vector<Slic3r::Vec3d> outline;   // mesh coordinates, convex, smoothed (update_planes)
    };
    CutPlane             m_cut;
    bool                 m_cut_line_mode{false};
    Slic3r::Vec3d        m_line_beg{0, 0, 0}, m_line_end{0, 0, 0}, m_line_dir{0, 0, 1};
    CutEdit              m_cut_edit;
    std::vector<CutPart> m_cut_parts;
    int                  m_cut_parts_obj{-1};
    int                  m_planes_obj{-1}, m_planes_inst{-1};
    Slic3r::Transform3d  m_planes_matrix{Slic3r::Transform3d::Identity()};   // instance matrix
    std::vector<Plane>   m_planes;
    int                  m_pressed_plane{-1};
};

} // namespace orca
