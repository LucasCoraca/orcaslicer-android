// The 3D view: plates, objects, camera, picking; the G-code preview is drawn by libvgcode (Preview.cpp).
#pragma once
#include "ImexPlate.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca {

struct Camera
{
    Slic3r::Vec3d target{100, 100, 0};
    double        distance{450};
    double        azimuth{-45};     // degrees, around Z (0 = looking along +Y); default: iso (set_default_orientation)
    double        elevation{45};    // degrees above the bed plane
    double        fov{30};
    bool          ortho{true};      // Orthographic by default on mobile (app config use_perspective_camera)
    int           width{1}, height{1};

    Eigen::Matrix4d view() const;
    Eigen::Matrix4d projection(double near_z, double far_z) const;
    Slic3r::Vec3d   eye() const;
    // ray through a pixel (top-left origin)
    void ray(double px, double py, Slic3r::Vec3d &origin, Slic3r::Vec3d &dir) const;
};

// what one frame draws, copied out of the model under the core lock
struct RenderVolume
{
    std::shared_ptr<const Slic3r::TriangleMesh> mesh;
    Slic3r::Transform3d                         matrix;     // world
    float                                       color[4];   // GLVolume::color (color_from_model_volume / extruder colour)
    int                                         obj, inst, vol;   // a prime tower: obj = 1000 + plate (GLVolume composite id)
    bool                                        wipe_tower{false};
    bool                                        selected, outside, model_part, printable;
    Slic3r::BoundingBoxf3                       bbox;       // world
    Slic3r::Vec3d                               inst_offset{0, 0, 0};   // ObjectClipper: the instance's offset
    double                                      inst_radius{0};          // and its bounding box radius
    float                                       print_volume[6];   // plate rect min x/y, max x/y, z min/max (partly outside)
};

// GL helpers shared with the gizmos (GL thread)
// resources/images/<name>.svg as a GL texture (GL thread, kept for the context)
unsigned gl_icon_texture(const std::string &name);
void gl_draw_flat(const std::vector<float> &xyz, unsigned mode, const Eigen::Matrix4f &mvp, const Slic3r::ColorRGBA &c, float width = 1.f);
void gl_draw_lit(const std::shared_ptr<const Slic3r::TriangleMesh> &mesh, const Eigen::Matrix4f &model, const float color[4],
                 const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);

class Viewport
{
public:
    static Viewport &get();
    // bumped by each new EGL context: GL names cached before it are gone
    static unsigned gl_generation();

    void gl_created();
    void resize(int w, int h);
    bool draw();      // true: keep animating

    // gestures (UI thread)
    void orbit(double dx, double dy);
    void pan(double dx, double dy);
    void zoom(double factor, double px, double py);
    nlohmann::json pick(double px, double py);
    bool drag_start(double px, double py);   // starts moving the selection when the finger is on it
    void drag_move(double px, double py);
    nlohmann::json drag_end();
    bool paint_tap(double px, double py);
    void set_view(const std::string &name);  // iso, top, bottom, front, rear, left, right
    void zoom_to(const std::string &what);   // plate, selection, all
    void set_preview(bool on);
    nlohmann::json overlay();
    void set_pressed_icon(int plate, int icon);
    bool preview() const { return m_preview; }
    // the canvas menu / View menu toggles (GLCanvas3D, Plater::priv: overhang slope, labels, wireframe, X-Ray; the rest
    // are app config keys: show_axes, show_plate_gridlines, focus_mode, ...)
    nlohmann::json view_options();
    void           set_view_option(const std::string &key, bool on);
    // the preset values the view options use (overhang angle, plate tilt), refreshed when presets change
    void           update_view_presets();
    // a touch for the 3D navigator (0 down, 1 move, 2 up): true when it takes it
    bool           navigator_touch(float x, float y, int action);
    // GLCanvas3D::CenterOfMass::on_left_down: a tap on a marker shows its details
    bool           center_of_mass_tap(double x, double y);
    // the Assembly view (Plater::select_view_3D("Assemble") / "3D"), and a reload of the scene (explosion ratio)
    void           set_assemble(bool on);
    void           reload();
    // the box of the selected volumes as drawn (Selection::get_bounding_box)
    Slic3r::BoundingBoxf3 selection_box();
    Camera camera() { std::lock_guard<std::mutex> l(m_cam_mutex); return m_cam; }
    void   set_camera(const Camera &c) { std::lock_guard<std::mutex> l(m_cam_mutex); m_cam = c; m_dirty = true; }
    void request_redraw() { m_dirty = true; }
    void request_snapshot() { m_revision = ~0u; m_dirty = true; }   // the gizmo changed: planes / boxes again
    // GLCanvas3D's section view, shared by Prepare and Preview (the canvas toolbar's button and panel)
    nlohmann::json section_view();
    void           set_section_ratio(double ratio);
    void           toggle_section_view();
    void           align_section_view();
    // the frame's cut (_get_volumes_clipping_plane: a painter's object clipper or the section view): a point p is
    // cut away when n.p > d; false when nothing is cut
    bool           clipping_plane(Slic3r::Vec3d &n, double &d);

private:
    std::atomic<bool> m_zoom_to_bed{false};   // the bed's shape changed: the next frame zooms to it
    bool              m_bed_stale{false};
    bool              m_bed_model_bbl{false};  // a Bambu bed model (bbl-3dp-*.stl): offset as Bed3D does     // the frame could not read the bed (engine busy)
    void snapshot();
    void upload();
    void draw_bed(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    void update_bed_assets();
    void update_imex();
    void draw_imex_ghosts(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    std::mutex                m_imex_mutex;
    std::vector<ImexOverlay>  m_imex;          // per plate
    std::vector<std::string>  m_imex_kind;           // the mode icon of each plate
    std::string               m_imex_key;
    std::vector<ImexMarkers>  m_imex_markers;          // per plate (under m_imex_mutex)
    std::string               m_marker_file;
    std::shared_ptr<const Slic3r::TriangleMesh> m_marker_mesh;   // the tool marker (under m_imex_mutex)
    std::shared_ptr<const Slic3r::TriangleMesh> m_box_mesh;      // the IDEX/IQEX toolhead box
    Slic3r::Vec2f             m_box_size{0, 0};
    std::atomic<bool>         m_show_toolhead_boxes{true};
    void draw_tool_markers(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    void draw_volumes(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    void draw_shadows(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    void update_clipping_plane();
    void draw_section_caps(const std::vector<RenderVolume> &vols, unsigned revision, const Slic3r::Transform3d &gizmo, const Eigen::Matrix4f &vp);
    std::mutex                m_section_mutex;
    double                    m_section_ratio{0}, m_section_last{0};
    Slic3r::Vec3d             m_section_normal{Slic3r::Vec3d::Zero()};   // zero until first aimed
    bool                      m_clp_on{false}, m_clp_section{false};       // the cut, and whether it is the section view's
    Slic3r::Vec3d             m_clp_n{0, 0, 1};
    double                    m_clp_d{0};
    std::string               m_caps_key;
    std::vector<float>        m_caps;   // the section view's cut faces (MeshClipper::render_cut), world triangles
    Slic3r::Transform3d drawn_matrix(const RenderVolume &r, unsigned revision, const Slic3r::Transform3d &gizmo) const;
    std::vector<float>        m_plate_mask;   // the shown plates' triangles at z = 0 (the plate shadow)
    Slic3r::BoundingBoxf3     m_plates_box;

    std::mutex                m_cam_mutex;
    Camera                    m_cam;
    std::mutex                m_scene_mutex;
    std::vector<RenderVolume> m_volumes;
    unsigned                  m_volumes_revision{0};
    std::atomic<unsigned>     m_revision{~0u};
    bool                      m_dirty{true};
    std::atomic<bool>         m_show_overhang{false}, m_show_labels{false}, m_show_wireframe{false}, m_show_xray{false};
    std::atomic<bool>         m_show_navigator{true}, m_show_outline{false};
    std::atomic<bool>         m_show_axes{true}, m_show_gridlines{true}, m_focus_mode{false}, m_auto_perspective{false};
    bool                      m_prevent_auto_type{false};
    Camera                    m_cam_saved;               // the other view's camera (Assembly view <-> 3D view)
    bool                      m_assemble_first{true};   // Camera::m_prevent_auto_type (under m_cam_mutex)
    void                      set_type(bool ortho);         // under m_cam_mutex
    void                      auto_type(bool ortho);
    std::atomic<float>        m_overhang_normal_z{-0.5f};
    Slic3r::BoundingBoxf3     m_current_plate_box;
    Slic3r::Vec3f             m_up_direction{0.f, 0.f, 1.f};   // build_plate_tilt_up_direction, cached by update_view_presets
    bool                      m_preview{false};
    bool                      m_gl_ready{false};
    std::string               m_bed_key;
    unsigned                  m_bed_texture{0};
    std::shared_ptr<const Slic3r::TriangleMesh> m_bed_model;
    std::vector<bool>         m_plate_locked, m_plate_has_settings;
    std::pair<int, int>       m_pressed_icon{-1, -1};
    // drag
    bool                      m_dragging{false};
    bool                      m_gizmo_drag{false};
    int                       m_ring_kind{0};   // the emboss ring dragged: 1 text, 2 SVG
    bool                      m_painting{false};
    bool                      m_tex_adjust{false};   // a drag of the texture displacement's placement handle
    bool                      m_ear_drag{false};
    int                       m_text_drag{0};   // 1: the Text gizmo's volume, 2: the SVG gizmo's
    // a finished drag stays applied until the engine's commit changes the model revision
    Slic3r::Transform3d       m_pending{Slic3r::Transform3d::Identity()};
    unsigned                  m_pending_revision{~0u};
    Slic3r::Vec3d             m_drag_start_world, m_drag_delta{0, 0, 0};
    int                       m_tower_drag{-1};        // the plate whose prime tower the finger moves
    int                       m_pending_tower{-1};
    double                    m_drag_z{0};
};

} // namespace orca
