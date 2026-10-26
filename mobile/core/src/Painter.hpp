// The painting gizmos (GLGizmoPainterBase and its FdmSupports / Seam / MmuSegmentation / FuzzySkin flavours): one
// TriangleSelector (libslic3r) per model part of the selected object, painted with the brush / fill tools by touch,
// drawn in the state colours and written back to the ModelVolume facets when a stroke ends.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "SelectorPatch.hpp"

namespace orca {

struct Camera;

class Painter
{
public:
    static Painter &get();

    // "support", "seam", "mmu", "fuzzy", "texture" (GLGizmoTextureDisplacement) or "" (off); starts on the selected object
    void           set_kind(const std::string &kind);
    std::string    kind();
    bool           active();
    int            object();   // the painted object (its volumes are drawn by the painter)
    int            instance() { std::lock_guard<std::mutex> l(m_mutex); return m_inst; }

    // the panel's settings: tool (circle / sphere / fill / gap_fill / smart_fill / height_range / pointer), size, ...
    void           set(const nlohmann::json &a);
    nlohmann::json state();

    // a stroke from the 3D view (UI thread): begin returns false when the finger is not on the object
    bool           stroke_begin(const Camera &cam, double px, double py);
    void           stroke_move(const Camera &cam, double px, double py);
    void           stroke_end();   // writes the facets back to the model (with an undo snapshot)
    void           erase_all();
    void           gap_fill();   // GLGizmoMmuSegmentation's "Apply" of the gap fill tool

    // texture displacement: the layer slot painted (its mask is reloaded), the selectors written back to the model
    // without a snapshot (update_model_object), every face of every part set or cleared
    void           set_slot(int slot);
    void           flush();
    void           fill_all(bool paint);
    // a part's selector edited directly (GLGizmoTextureDisplacement::select_visible_faces), its drawing rebuilt
    int            parts_count() { std::lock_guard<std::mutex> l(m_mutex); return int(m_parts.size()); }
    void           edit_part(int part, const std::function<void(Slic3r::TriangleSelector &)> &fn);
    // texture displacement: the displaced preview of a model part drawn instead of its selector (null: none), with
    // its triangles' colours as runs (first index, count, colour); `drawing` is while a stroke is on
    struct PreviewRun { size_t first, count; Slic3r::ColorRGBA color; };
    void           set_preview(int vol, std::shared_ptr<const Slic3r::TriangleMesh> mesh, std::vector<PreviewRun> runs);
    bool           stroking() { std::lock_guard<std::mutex> l(m_mutex); return m_stroking; }
    // the mesh edges over the painted parts (GLGizmoTextureDisplacement's Wireframe, GLGizmoPainterBase's Alt+Shift+Enter)
    void           set_wireframe(bool on);
    // lines over the textured part, in its mesh coordinates, by kind: "subdivide" (the subdivision preview's wireframe),
    // "seams" (marked texture seams), "seam_hover", "seam_anchor"; empty: none
    void           set_lines(const std::string &key, std::vector<Slic3r::Vec3f> lines);
    // handles over everything, in world coordinates (the texture displacement's placement handle): triangles or lines
    struct OverlayPrim { bool lines; std::vector<Slic3r::Vec3f> points; Slic3r::ColorRGBA color; };
    void           set_overlay(std::vector<OverlayPrim> prims);
    // meshes drawn with one of upstream's shaders and the uniforms its render function set (GLGizmoTextureDisplacement's
    // Fast view "shaded", which replaces the textured part, and the "uvcheck" overlay); the camera's uniforms are set
    // per frame. Vertices: P3N3T2 interleaved. Textures by id, uploaded on the GL thread.
    struct Uniform { int type; std::vector<float> f; std::vector<int> i; };   // type: 0 ints, 1..4 floats, 9 mat3, 16 mat4
    struct ShaderMesh
    {
        std::string                    shader;
        std::shared_ptr<const std::vector<float>> data;
        std::map<std::string, Uniform> uniforms;
        std::map<int, int>             textures;   // unit -> texture id
        int                            wrap{0};    // GL wrap mode for the textures (0: default)
    };
    void           set_shader_mesh(const std::string &key, std::optional<ShaderMesh> mesh);
    void           set_texture(int id, std::shared_ptr<const std::vector<unsigned char>> rgba, int w, int h);
    void           drop_texture(int id);
    // the part's selector facets of a state (TriangleSelector::get_facets_strict on the Painter's selector)
    indexed_triangle_set facets_strict(int part, Slic3r::EnforcerBlockerType state, std::vector<int> *source);
    bool           wireframe() { std::lock_guard<std::mutex> l(m_mutex); return m_wireframe; }

    // GL thread: the painted object's model parts in their state colours (instead of the normal volume)
    void           render(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);
    void           gl_created() { std::lock_guard<std::mutex> l(m_mutex); m_gpu.clear(); for (auto &[k, ls] : m_lines) { ls.vao = 0; ls.dirty = true; } m_overlay_vao = 0; m_shader_gpu.clear(); for (auto &[id, t] : m_textures) t.gl = 0; m_dead_textures.clear(); }   // a new EGL context

private:
    struct Part
    {
        int                                     vol;
        std::shared_ptr<const Slic3r::TriangleMesh> mesh;      // the volume mesh (TriangleSelector keeps a pointer)
        std::unique_ptr<SelectorPatch>              selector;
        Slic3r::Transform3d                     matrix;         // world = instance * volume
        std::vector<std::pair<Slic3r::ColorRGBA, std::shared_ptr<const Slic3r::TriangleMesh>>> drawn;   // per state
        std::shared_ptr<const Slic3r::TriangleMesh> others;   // texture: the other layers' paint (muted grey)
    };
    Slic3r::FacetsAnnotation &facets(Slic3r::ModelVolume &mv) const;
    void   load();                       // selectors from the model (core lock held)
    void   rebuild_drawn(Part &p);
    bool   raycast(const Camera &cam, double px, double py, int &part, Slic3r::Vec3f &hit, int &facet);
    void   apply(const Camera &cam, double px, double py, bool first);
    Slic3r::EnforcerBlockerType new_state() const;
    std::vector<Slic3r::ColorRGBA> state_colors() const;

    std::mutex          m_mutex;
    std::string         m_kind;
    int                 m_obj{-1}, m_inst{0};
    std::vector<Part>   m_parts;
    // the panel (GLGizmoPainterBase members)
    std::string         m_tool{"circle"};
    float               m_cursor_radius{1.f}, m_smart_fill_angle{30.f}, m_cursor_height{0.2f};
    int                 m_mode{0};        // touch: 0 = left button (enforce / paint), 1 = right button (block), 2 = shift (erase)
    int                 m_extruder{0};    // MMU: the selected filament (0 based)
    bool                m_overhangs_only{false};
    float               m_highlight_angle{0.f};
    bool                m_vertical_only{false}, m_horizontal_only{false};
    bool                m_stroking{false};
    Slic3r::Vec3f       m_up{0, 0, 1};    // build_plate_tilt_up_direction
    struct Gpu { std::weak_ptr<const Slic3r::TriangleMesh> owner; unsigned vao{0}, vbo{0}; int count{0}; };
    std::map<const Slic3r::TriangleMesh *, Gpu> m_gpu;   // GL thread
    Slic3r::Vec2d       m_last{0, 0};
    bool                m_has_last{false};
    Slic3r::Vec2d       m_first{0, 0};
    int                 m_slot{0};
    bool                m_wireframe{false};
    struct LineSet { std::vector<Slic3r::Vec3f> lines; unsigned vao{0}, vbo{0}; bool dirty{true}; };
    std::map<std::string, LineSet> m_lines;
public:
    bool                m_seams_on_top{false};   // render_seam_overlay: over the model while marking or with the wireframe
private:
    std::vector<OverlayPrim> m_overlay;
    struct GpuMesh { std::shared_ptr<const std::vector<float>> data; unsigned vao{0}, vbo{0}; };
    std::map<std::string, ShaderMesh> m_shader_meshes;
    std::map<std::string, GpuMesh>    m_shader_gpu;
    struct Tex { std::shared_ptr<const std::vector<unsigned char>> rgba; int w{0}, h{0}; unsigned gl{0}; };
    std::map<int, Tex>                m_textures;
    std::vector<unsigned>             m_dead_textures;
    void draw_shader_mesh(const std::string &key, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj, const Slic3r::Transform3d &part);
    unsigned            m_overlay_vao{0}, m_overlay_vbo{0};        // texture: the active layer's slot
    int                 m_preview_vol{-1};
    std::shared_ptr<const Slic3r::TriangleMesh> m_preview;
    std::vector<PreviewRun> m_preview_runs;
};

} // namespace orca
