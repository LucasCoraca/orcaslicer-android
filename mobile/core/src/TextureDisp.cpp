// The Texture displacement gizmo (GLGizmoTextureDisplacement): the layer stack of the selected object's textured
// volume, each layer's paint mask painted by the Painter ("texture" kind, the active layer's slot), the true preview
// (build_texture_displacement off the engine thread, drawn by the Painter in place of the volume) and Bake. The layer
// stack, the filament palette and the bake job are upstream's, compiled on a stand-in gizmo (TextureDisp.inc*); the
// panel is the app's (TextureDisplacementPanel.kt), fed by texdisp.state.
#include <GLES3/gl3.h>
#include <algorithm>
#include <cfloat>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>

#include "Core.hpp"
#include "Gizmos.hpp"
#include "Host.hpp"
#include "Painter.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TextureBake/TextureBakeDebug.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/format.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include "wx/app.h"
#include "wx/busycursor.h"
#include "wx/event.h"
#include "wx/filedlg.h"

namespace orca {
int filaments_ensure_mixed(const std::vector<unsigned int> &components, const std::vector<int> &ratios);   // Filaments.cpp
}

namespace Slic3r { namespace GUI { namespace td {

// GLGizmoTextureDisplacement.cpp's file constants
constexpr int   THUMBNAIL_MAX_PX    = 128;
constexpr int   BORDER_BAND_RINGS   = 2;
constexpr int   ADJUST_PREVIEW_MAX_PX = 384;
constexpr int   HEIGHT_TEX_MAX_PX   = 2048;
constexpr int   PALETTE_LUT_EDGE    = 24;
constexpr int   PALETTE_MAX_ENTRIES = 64;
constexpr float PREFER_PURE_DE      = 2.f;
constexpr size_t MIX_TARGET_SAMPLES = size_t(1) << 20;
constexpr size_t MIX_TARGET_BINS    = 256;
constexpr float MIN_MIX_GAIN        = 0.05f;
static constexpr float STD_REMESH_EDGE_MM     = 1.0f;
static constexpr float STD_REMESH_SHARP_DEG   = 40.f;
static constexpr float STD_SUBDIV_MAX_EDGE_MM = 20.f;
static constexpr float STD_SUBDIV_DETAIL_MM   = 0.02f;
static constexpr float STD_SUBDIV_MIN_EDGE_MM = 0.1f;
static constexpr float STD_SUBDIV_BORDER_MM   = 0.4f;
static constexpr float STD_SUBDIV_COLOR_MM    = 0.25f;

// --- the GUI the extracted code reaches -------------------------------------------------------------------------

namespace UndoRedo { enum class SnapshotType { GizmoAction }; }

inline void show_error(const void *, const std::string &message) { MessageDialog(nullptr, message, _L("Error"), wxOK | wxICON_ERROR).ShowModal(); }

// GLTexture: its pixels handed to the Painter (uploaded on the GL thread), known by an id
class GLTexture
{
public:
    GLTexture() : m_id(++s_next) {}
    ~GLTexture() { if (m_loaded) orca::Painter::get().drop_texture(m_id); }
    GLTexture(const GLTexture &) = delete;
    bool load_from_raw_data(std::vector<unsigned char> &&rgba, unsigned int w, unsigned int h, bool = false, bool = false)
    {
        m_w = int(w);
        m_h = int(h);
        orca::Painter::get().set_texture(m_id, std::make_shared<const std::vector<unsigned char>>(std::move(rgba)), m_w, m_h);
        m_loaded = true;
        return true;
    }
    unsigned int get_id() const { return unsigned(m_id); }
    int          get_width() const { return m_w; }
    int          get_height() const { return m_h; }

private:
    static inline int s_next = 0;
    int  m_id;
    int  m_w{0}, m_h{0};
    bool m_loaded{false};
};

// GLShaderProgram: the uniforms a render function sets, recorded for the Painter (GLModel::render hands them over)
class GLShaderProgram
{
public:
    std::string                                   name;
    std::map<std::string, orca::Painter::Uniform> uniforms;
    std::map<int, int>                            textures;
    int                                           wrap{0};
    void start_using() { uniforms.clear(); textures.clear(); wrap = 0; s_current = this; }
    void stop_using() { s_current = nullptr; }
    void set_uniform(const char *n, int v) { uniforms[n] = {0, {}, {v}}; }
    void set_uniform(const char *n, bool v) { uniforms[n] = {0, {}, {v ? 1 : 0}}; }
    void set_uniform(const char *n, float v) { uniforms[n] = {1, {v}, {}}; }
    void set_uniform(const char *n, double v) { uniforms[n] = {1, {float(v)}, {}}; }
    void set_uniform(const char *n, const Vec2f &v) { uniforms[n] = {2, {v.x(), v.y()}, {}}; }
    void set_uniform(const char *n, const Vec3f &v) { uniforms[n] = {3, {v.x(), v.y(), v.z()}, {}}; }
    void set_uniform(const char *n, const std::array<float, 2> &v) { uniforms[n] = {2, {v[0], v[1]}, {}}; }
    void set_uniform(const char *n, const std::array<float, 4> &v) { uniforms[n] = {4, {v[0], v[1], v[2], v[3]}, {}}; }
    void set_uniform(const char *n, const Transform3d &m)
    {
        const Eigen::Matrix4f f = m.matrix().cast<float>();
        uniforms[n] = {16, std::vector<float>(f.data(), f.data() + 16), {}};
    }
    template<class P, class = decltype(std::declval<const P &>().m.cols())>
    void set_uniform(const char *n, const P &p)
    {
        const Eigen::Matrix4f f = p.m.template cast<float>();
        uniforms[n] = {16, std::vector<float>(f.data(), f.data() + 16), {}};
    }
    void set_uniform(const char *n, const Matrix3d &m)
    {
        const Eigen::Matrix3f f = m.cast<float>();
        uniforms[n] = {9, std::vector<float>(f.data(), f.data() + 9), {}};
    }
    static inline GLShaderProgram *s_current = nullptr;
};
static GLShaderProgram *get_shader(const char *name)
{
    static std::map<std::string, std::unique_ptr<GLShaderProgram>> shaders;
    auto &s = shaders[name];
    if (!s) { s = std::make_unique<GLShaderProgram>(); s->name = name; }
    return s.get();
}
inline void rec_bind(int unit, const GLTexture *tex) { if (GLShaderProgram::s_current && tex) GLShaderProgram::s_current->textures[unit] = int(tex->get_id()); }
inline void rec_wrap(int wrap) { if (GLShaderProgram::s_current) GLShaderProgram::s_current->wrap = wrap; }
#define glsafe(cmd)
struct GLVolume { static inline const ColorRGBA NEUTRAL_COLOR = {0.8f, 0.8f, 0.8f, 1.0f}; };

// Worker / Job: the bake job runs in the command (texdisp.bake), its status as the app's progress, Cancel as texdisp.cancel
static std::atomic<bool> g_cancel{false};
class Job
{
public:
    class Ctl
    {
    public:
        void update_status(int st, const std::string &msg = "")
        {
            orca::Host::event("progress", {{"text", st >= 100 ? std::string() : msg}, {"percent", st}, {"cancel", "texdisp.cancel"}});
        }
        bool was_canceled() const { return g_cancel.load(); }
    };
    virtual ~Job() = default;
    virtual void process(Ctl &ctl) = 0;
    virtual void finalize(bool canceled, std::exception_ptr &eptr) = 0;
};
struct Worker {};
static void queue_job(Worker &, std::unique_ptr<Job> job)
{
    g_cancel = false;
    Job::Ctl           ctl;
    std::exception_ptr eptr;
    try {
        job->process(ctl);
    } catch (...) {
        eptr = std::current_exception();
    }
    job->finalize(g_cancel.load(), eptr);
    ctl.update_status(100);
    if (eptr) try { std::rethrow_exception(eptr); } catch (const std::exception &e) { show_error(nullptr, e.what()); }
}

// the 3D view's camera as GLCanvas3D's Camera
class Camera
{
public:
    enum class EType { Ortho, Perspective };
    orca::Camera c;
    Transform3d get_view_matrix() const { return Transform3d(c.view()); }
    // a perspective projection is not affine: kept as a plain 4x4 (the Painter sets its own per frame)
    struct Projection { Eigen::Matrix4d m; const Eigen::Matrix4d &matrix() const { return m; } };
    Projection get_projection_matrix() const { return {c.projection(1., 10000.)}; }
    std::array<int, 4> get_viewport() const { return {0, 0, c.width, c.height}; }
    Vec3d       get_position() const { return c.eye(); }
    EType       get_type() const { return c.ortho ? EType::Ortho : EType::Perspective; }
    Vec3d       get_dir_forward() const { return -Vec3d(c.view().block<1, 3>(2, 0).transpose()); }
};

// CameraUtils: the screen position of a point, the ray through a pixel (top-left origin, the 3D view's pixels)
namespace CameraUtils {
inline Point project(const Camera &camera, const Vec3d &world)
{
    const Eigen::Vector4d p = camera.c.projection(1., 10000.) * camera.c.view() * Eigen::Vector4d(world.x(), world.y(), world.z(), 1.);
    const double w = std::abs(p.w()) > 1e-12 ? p.w() : 1e-12;
    return Point(coord_t(std::lround((p.x() / w + 1.) * 0.5 * camera.c.width)), coord_t(std::lround((1. - p.y() / w) * 0.5 * camera.c.height)));
}
inline void ray_from_screen_pos(const Camera &camera, const Vec2d &pos, Vec3d &origin, Vec3d &dir) { camera.c.ray(pos.x(), pos.y(), origin, dir); }
} // namespace CameraUtils

#include "UVCanvas.hpp"
// pick distances in pixels are given for a mouse; a finger needs them by the display density (Gizmos' UI scale)
inline double touch_scale() { return std::max(1.f, orca::Gizmos::get().ui_scale()) * 1.5; }

#include "slic3r/GUI/TextureDisp.inc.fn"

struct Sidebar
{
    int ensure_mixed_filament(const std::vector<unsigned int> &components, const std::vector<int> &ratios)
    {
        return orca::filaments_ensure_mixed(components, ratios);
    }
};
struct ObjectList { void update_info_items(size_t) {} };

class Plater
{
public:
    struct TakeSnapshot
    {
        TakeSnapshot(Plater *, const std::string &name, UndoRedo::SnapshotType) { orca::Plater::get().take_snapshot(name); }
    };
    Model   &model() { return orca::Plater::get().model(); }
    Sidebar &sidebar() { return m_sidebar; }
    Worker  &get_ui_job_worker() { return m_worker; }
    UVEditorCanvas *get_uv_editor_canvas();
    void            show_uv_editor(bool show);
    void            undo() { orca::Plater::get().undo(); orca::Plater::get().changed(); }
    void            redo() { orca::Plater::get().redo(); orca::Plater::get().changed(); }
    const Camera &get_camera()
    {
        m_camera.c = orca::Viewport::get().camera();
        return m_camera;
    }
    // Plater::changed_object: the object's new mesh is laid on the bed and the change announced
    void changed_object(ModelObject &object)
    {
        object.invalidate_bounding_box();
        object.ensure_on_bed();
        orca::Plater::get().changed();
    }
    // Plater::get_extruders_colors: the project's filament colours
    std::vector<ColorRGBA> get_extruders_colors()
    {
        std::vector<ColorRGBA> out;
        if (auto *fc = orca::Core::get().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
            for (const std::string &s : fc->values) {
                ColorRGBA c;
                out.push_back(decode_color(s, c) ? c : ColorRGBA::BLACK());
            }
        return out;
    }

private:
    Sidebar m_sidebar;
    Worker  m_worker;
    Camera  m_camera;
};

struct App
{
    Plater               plater_;
    ObjectList           obj_list_;
    NotificationManager  notifications;
    PresetBundle        *preset_bundle{nullptr};
    AppConfig           *app_config{nullptr};
    Plater              *plater() { return &plater_; }
    Model               &model() { return orca::Plater::get().model(); }
    GLShaderProgram     *get_shader(const char *name) { return td::get_shader(name); }
    ObjectList          *obj_list() { return &obj_list_; }
    NotificationManager *notification_manager() { return &notifications; }
};
static App &wxGetApp()
{
    static App app;
    app.preset_bundle = orca::Core::get().preset_bundle.get();
    app.app_config    = orca::Core::get().app_config.get();
    return app;
}

// GLCanvas3D.cpp get_model_volume
static ModelVolume *get_model_volume(const ObjectID &volume_id, const ModelObjectPtrs &objects)
{
    for (ModelObject *mo : objects)
        for (ModelVolume *mv : mo->volumes)
            if (mv->id() == volume_id) return mv;
    return nullptr;
}

// --- the gizmo --------------------------------------------------------------------------------------------------

struct Selection { int get_instance_idx() const { return orca::Painter::get().instance(); } };
struct wxRect
{
    int     x{0}, y{0}, width{0}, height{0};
    wxPoint GetTopLeft() const { return wxPoint(x, y); }
};
// the 3D view's GL canvas: the frame's rectangle arrives in its pixels already
struct wxGLCanvas { wxPoint ScreenToClient(const wxPoint &p) const { return p; } };
// TextureProjectorFrame: the app's frame over the 3D view (its rectangle in the view's pixels, set by texdisp.projector)
class TextureProjectorFrame
{
public:
    bool   IsShown() const { return shown; }
    wxRect client_rect_on_screen() const { return rect; }
    bool   shown{false};
    wxRect rect;
};
struct Canvas
{
    Selection        selection;
    wxGLCanvas       widget;
    void             set_as_dirty() { orca::Viewport::get().request_redraw(); }
    const Selection &get_selection() const { return selection; }
    wxGLCanvas      *get_wxglcanvas() { return &widget; }
    double           get_scale() const { return 1.0; }
};
// the section view's plane (ObjectClipper)
struct ClippingPlane
{
    Vec3d  normal;
    double offset;
    bool   is_point_clipped(const Vec3d &p) const { return normal.dot(p) > offset; }
};
struct ObjectClipper
{
    ClippingPlane        plane;
    const ClippingPlane *get_clipping_plane()
    {
        return orca::Viewport::get().clipping_plane(plane.normal, plane.offset) ? &plane : nullptr;
    }
};
// MeshRaycaster over a model part (its AABB tree), the visibility test upstream's
class MeshRaycaster
{
public:
    explicit MeshRaycaster(std::shared_ptr<const TriangleMesh> mesh) : m_mesh(std::move(mesh)), m_emesh(*m_mesh, true) {}
    std::vector<unsigned> get_unobscured_idxs(const Geometry::Transformation &trafo, const Camera &camera, const std::vector<Vec3f> &points,
                                              const ClippingPlane *clipping_plane = nullptr) const;
    bool unproject_on_mesh(const Vec2d &mouse_pos, const Transform3d &trafo, const Camera &camera, Vec3f &position, Vec3f &normal,
                           const ClippingPlane *clipping_plane = nullptr, size_t *facet_idx = nullptr, bool sinking_limit = true) const;

private:
    std::shared_ptr<const TriangleMesh> m_mesh;
    AABBMesh                            m_emesh;
};
#include "slic3r/GUI/TextureDisp.inc.ray"
// the raycasters of the painted object's model parts (in the order of the Painter's parts)
struct Raycaster
{
    std::vector<std::shared_ptr<MeshRaycaster>> list;
    const std::vector<std::shared_ptr<MeshRaycaster>> &raycasters()
    {
        list.clear();
        const int obj = orca::Painter::get().object();
        Model &m = orca::Plater::get().model();
        if (obj >= 0 && obj < int(m.objects.size()))
            for (const ModelVolume *v : m.objects[size_t(obj)]->volumes)
                if (v->is_model_part()) list.push_back(std::make_shared<MeshRaycaster>(v->mesh_ptr()));
        return list;
    }
};
// a model part's selector in the Painter, edited as TriangleSelectorGUI is by select_visible_faces
struct PainterPart
{
    int              part;
    bool             cleared{false};
    std::vector<int> facets;
    explicit PainterPart(int p) : part(p) {}
    void reset() { cleared = true; facets.clear(); }
    void set_facet(int f, EnforcerBlockerType) { facets.push_back(f); }
    void request_update_render_data(bool = false)
    {
        orca::Painter::get().edit_part(part, [this](TriangleSelector &s) {
            if (cleared) s.reset();
            for (int f : facets) s.set_facet(f, EnforcerBlockerType::ENFORCER);
        });
    }
};
struct SelectionInfo
{
    ModelObject *model_object()
    {
        const int obj = orca::Painter::get().object();
        Model &m = orca::Plater::get().model();
        return obj >= 0 && obj < int(m.objects.size()) ? m.objects[size_t(obj)] : nullptr;
    }
};
struct CommonData
{
    SelectionInfo  info;
    Raycaster      ray;
    ObjectClipper  clipper;
    SelectionInfo *selection_info() { return &info; }
    Raycaster     *raycaster() { return &ray; }
    ObjectClipper *object_clipper() { return &clipper; }
};

// GLModel: line models drawn by the Painter over the textured part, by kind (Painter::set_lines); unkeyed ones draw nothing
class GLModel
{
public:
    explicit GLModel(const char *key = nullptr) : m_key(key ? key : "") {}
    struct Geometry
    {
        enum class EPrimitiveType { Lines, Triangles };
        enum class EVertexLayout { P3, P3N3T2 };
        struct Format { EPrimitiveType type; EVertexLayout layout; } format{EPrimitiveType::Lines, EVertexLayout::P3};
        std::vector<Vec3f>    vertices;
        std::vector<float>    p3n3t2;   // the triangle meshes' interleaved vertices
        std::vector<unsigned> indices;
        void reserve_vertices(size_t n) { vertices.reserve(n); }
        void reserve_indices(size_t n) { indices.reserve(n); }
        void add_vertex(const Vec3f &v) { vertices.push_back(v); }   // P3 (lines, or triangles expanded in init_from)
        void add_vertex(const Vec3f &p, const Vec3f &n, const Vec2f &t)
        {
            p3n3t2.insert(p3n3t2.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z(), t.x(), t.y()});
            vertices.push_back(p);
        }
        void add_line(unsigned a, unsigned b) { indices.push_back(a); indices.push_back(b); }
        void add_triangle(unsigned a, unsigned b, unsigned c) { indices.insert(indices.end(), {a, b, c}); }
        bool is_empty() const { return indices.empty(); }
    };
    void init_from(Geometry &&g)
    {
        m_init = true;
        if (m_key.empty()) return;
        if (g.format.type == Geometry::EPrimitiveType::Triangles) {
            if (g.format.layout == Geometry::EVertexLayout::P3)   // positions only: zero normals and uvs
                for (const Vec3f &v : g.vertices) g.p3n3t2.insert(g.p3n3t2.end(), {v.x(), v.y(), v.z(), 0.f, 0.f, 0.f, 0.f, 0.f});
            // un-indexed for the Painter
            auto data = std::make_shared<std::vector<float>>();
            data->reserve(g.indices.size() * 8);
            for (unsigned i : g.indices) data->insert(data->end(), g.p3n3t2.begin() + size_t(i) * 8, g.p3n3t2.begin() + size_t(i) * 8 + 8);
            m_triangles = std::move(data);
            return;
        }
        std::vector<Vec3f> lines;
        lines.reserve(g.indices.size());
        for (unsigned i : g.indices) lines.push_back(g.vertices[i]);
        orca::Painter::get().set_lines(m_key, std::move(lines));
    }
    void reset()
    {
        m_init = false;
        if (m_triangles) { m_triangles.reset(); orca::Painter::get().set_shader_mesh(m_key, std::nullopt); }
        else if (!m_key.empty()) orca::Painter::get().set_lines(m_key, {});
    }
    bool is_initialized() const { return m_init; }
    void set_color(const ColorRGBA &c) { m_color = c; }
    // a render function's draw: the mesh with the shader in use and its uniforms, drawn by the Painter from now on
    void render()
    {
        GLShaderProgram *s = GLShaderProgram::s_current;
        if (!m_triangles || s == nullptr) return;
        orca::Painter::ShaderMesh mesh;
        mesh.shader   = s->name;
        mesh.data     = m_triangles;
        mesh.uniforms = s->uniforms;
        mesh.uniforms["uniform_color"] = {4, {m_color.r(), m_color.g(), m_color.b(), m_color.a()}, {}};
        mesh.textures = s->textures;
        mesh.wrap     = s->wrap;
        orca::Painter::get().set_shader_mesh(m_key, std::move(mesh));
    }

private:
    std::string m_key;
    bool        m_init{false};
    ColorRGBA   m_color{0.8f, 0.8f, 0.8f, 1.f};
    std::shared_ptr<const std::vector<float>> m_triangles;
};

// the docked UV editor pane (Plater::get_uv_editor_canvas / show_uv_editor)
static UVEditorCanvas g_uv_canvas;
static bool           g_uv_shown = false;
UVEditorCanvas *Plater::get_uv_editor_canvas() { return &g_uv_canvas; }
void            Plater::show_uv_editor(bool show) { g_uv_shown = show; }

// UVEditorCanvas.cpp's file constants and helpers, its logic
const ColorRGBA UV_COLOR_DIAL = { 1.f, 0.85f, 0.2f, 0.9f };
#define SNAP_PIXELS (28.f * float(touch_scale()))
#include "slic3r/GUI/TextureDisp.inc.uvfn"
#include "slic3r/GUI/TextureDisp.inc.uv"

class GLGizmoTextureDisplacement
{
public:
    enum EState { Off, On };
    using PaletteEntry = PrintableColor;
    struct MixTarget
    {
        Vec3f lab    = Vec3f::Zero();
        float weight = 0.f;
    };
    struct LibraryTexture
    {
        std::shared_ptr<std::vector<unsigned char>> image_data;
        std::unique_ptr<GLTexture>                  thumbnail;
    };

    // upstream's, extracted
    static TextureDisplacementFacetsData facets_data_of(const ModelVolume &mv);
    static bool any_layer_colors(const ModelVolume &mv);
    TextureColorSettings color_settings_for(const ModelVolume &mv);
    const std::vector<PaletteEntry> &cached_palette();
    std::pair<ColorQuantizeFn, ColorQuantizeFn> palette_quantizers();
    static std::vector<ColorRGBA> filament_palette();
    static std::vector<MixTarget> mix_targets(const std::vector<TextureDisplacementLayer> &layers);
    static std::vector<PaletteEntry> rank_mixes(const std::vector<ColorRGBA> &filaments, const std::vector<MixTarget> &targets, int limit);
    static std::vector<PaletteEntry> pick_mixes(const std::vector<PaletteEntry> &ranking, int count, int free_slots,
                                                const std::function<bool(const PaletteEntry &)> &reusable);
    static std::vector<PaletteEntry> make_palette(const std::vector<ColorRGBA> &filaments, const std::vector<PaletteEntry> &mixes);
    static ColorQuantizeFn make_palette_quantizer(const std::vector<PaletteEntry> &palette);
    static std::vector<int> palette_filaments(const std::vector<PaletteEntry> &palette, const std::vector<uint8_t> &triangle_color,
                                              const std::function<int(const PaletteEntry &)> &slot_for_mix);
    void add_texture_layer();
    const LibraryTexture *get_library_texture(const std::string &path);
    void set_layer_texture(TextureDisplacementLayer &layer, const TextureLibraryEntry &entry);
    void import_custom_texture(TextureDisplacementLayer &layer);
    void remove_texture_layer(int slot);
    void swap_layer_slots(int slot_a, int slot_b);
    void move_texture_layer(int slot, int to_index);
    void bake(bool own_snapshot = true);
    bool apply_standard_mode_presets(ModelVolume *mv);
    void bake_standard();
    void store_panel_state();
    ModelVolume *texture_volume();
    const ModelVolume *texture_volume() const { return const_cast<GLGizmoTextureDisplacement *>(this)->texture_volume(); }
    void set_active_layer(int slot);
    void capture_view_projection(TextureDisplacementLayer &layer);
    int  select_visible_faces(const std::array<float, 12> *uv_clip = nullptr);
    int  texture_volume_raycaster_index() const;
    // Pro mode's mesh preparation
    struct LayerPaintMap
    {
        std::vector<uint8_t>              full;
        std::vector<int>                  part_start;
        std::vector<std::array<Vec3f, 3>> part;
        bool empty() const { return full.empty(); }
    };
    static TextureDisplacementPrepareResult prepare_mesh(const indexed_triangle_set &base, const TextureDisplacementFacetsData &masks,
                                                         const std::vector<TextureDisplacementLayer> &layers, const std::vector<Vec2f> &model_uv,
                                                         const TextureDisplacementPrepareParams &params, const TextureColorSettings &color_settings,
                                                         const DisplacementProgressFn &progress, BakeStageRecorder *debug = nullptr);
    static TextureDisplacementFacetsData masks_after_subdivision(const TriangleMesh &new_mesh, const std::vector<int> &source,
                                                                 const std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> &paint);
    static bool plan_remesh(const indexed_triangle_set &src, float target_edge_mm, float sharp_angle_deg, indexed_triangle_set &out);
    static bool collect_paint_region(const TriangleMesh &mesh, const TextureDisplacementFacetsData &facets, std::vector<uint8_t> &region,
                                     std::array<LayerPaintMap, TEXTURE_DISPLACEMENT_MAX_LAYERS> *paint);
    void   subdivide_model();
    void   subdivide_model_adaptive();
    void   smooth_model();
    void   remesh_model();
    void   rebuild_subdivide_preview();
    double painted_area_mm2(const ModelVolume &mv);
    size_t estimated_refined_triangles(const ModelVolume &mv, float edge_mm);
    const V2Resolution &v2_recommendation(const ModelVolume &mv);
    // the Adjust image window
    bool load_image_adjust_source(const TextureDisplacementLayer &layer);
    void open_image_adjust(const TextureDisplacementLayer &layer);
    void invalidate_adjust_textures();
    void close_image_adjust();
    void commit_image_adjust(TextureDisplacementLayer &layer);
    std::string adjust_preview_file();
    int                  m_adjust_slot = -1;
    TextureAdjustments   m_adjust;
    TextureAdjustments   m_adjust_applied;
    DecodedHeightTexture m_adjust_source;
    DecodedHeightTexture m_adjust_preview_source;
    const void          *m_adjust_source_image = nullptr;
    size_t               m_adjust_volume_id    = 0;
    std::unique_ptr<GLTexture>        m_adjust_preview_tex;
    std::future<DecodedHeightTexture> m_adjust_preview_job;
    TextureAdjustments                m_adjust_preview_shown;
    bool                              m_adjust_preview_valid = false;
    std::weak_ptr<const std::vector<unsigned char>> m_height_tex_source, m_color_tex_source;
    bool m_preview_params_dirty = false;

    // the port's
    void open();
    void close();
    void update_model_object();
    void update_from_model_object(bool first_update);
    void rebuild_preview();
    void select_whole_model(bool paint);
    bool pro_mode() const { return m_panel_mode != 0; }
    // the UV editor pane (Unwrap mapping) and seam marking on the model
    enum class UVCheckMode { None, Checker, Distortion };
    enum class UVBackground { None, Height, Checker };
    struct UVEditorState
    {
        int                                     slot       = -1;
        const void                             *image_data = nullptr;
        float                                   seam_angle = -1.f;
        float                                   padding    = -2.f;
        TriangleSelector::TriangleSplittingData facets;
        std::vector<std::pair<int, int>>        seam_edges;
        bool operator==(const UVEditorState &other) const
        {
            return slot == other.slot && image_data == other.image_data && seam_angle == other.seam_angle && padding == other.padding &&
                   facets == other.facets && seam_edges == other.seam_edges;
        }
    };
    static constexpr int UV_THUMB_PX = 64;
    void update_uv_editor();
    void push_uv_pane_state();
    void run_uv_command(int cmd, float value);
    void on_uv_command(int cmd, float value);
    void process_uv_commands();
    std::vector<Eigen::Matrix<float, 2, 3>> uv_editor_island_transforms(const TextureDisplacementLayer &layer);
    void on_island_edited(int island, const Vec2f &offset_delta, float rotation_delta, float scale_factor, bool finished);
    void on_uv_vertex_edited(const std::vector<std::pair<int, Vec2f>> &edits);
    void cut_island(TextureDisplacementLayer &layer, int chart);
    void compute_uv_editor_distortion_colors(const indexed_triangle_set &patch);
    indexed_triangle_set patch_in_world(const indexed_triangle_set &patch) const;
    std::vector<int> build_island_move_set(const TextureDisplacementLayer &layer, int primary) const;
    static int  island_group_of(const std::vector<int> &groups, int c);
    static void join_island_groups(std::vector<int> &groups, int a, int b, int chart_count);
    void apply_view_mode(int mode);
    bool m_paint_overlay_dirty = false;
    int  view_mode() const
    {
        return m_use_shaded_preview ? 1 : m_uv_check_mode == UVCheckMode::Checker ? 2 : m_uv_check_mode == UVCheckMode::Distortion ? 3 : 0;
    }
    bool on_mouse_seam(const wxMouseEvent &mouse_event);
    std::pair<int, int> seam_edge_at(const Vec2d &mouse_pos) const;
    void toggle_seam_at(const Vec2d &mouse_pos);
    int  seam_vertex_at(const Vec2d &mouse_pos) const;
    void mark_seam_path(int v_from, int v_to);
    void rebuild_seam_anchor_overlay();
    void rebuild_seam_hover_overlay();
    void rebuild_seam_overlay();
    bool         m_show_uv_editor = false;
    bool         m_uv_unwrap_pending = false;
    bool         m_uv_apply_connected_net = false;
    size_t       m_uv_overrides_sig = 0;
    UVBackground m_uv_editor_bg = UVBackground::None;
    float        m_uv_editor_bg_smoothing = -1.f;
    TextureAdjustments m_uv_editor_bg_adjust;
    const void  *m_uv_editor_bg_image = nullptr;
    std::vector<ColorRGBA> m_uv_editor_distortion_colors;
    UVEditorState m_uv_editor_state;
    Vec2f         m_uv_editor_bbox_min = Vec2f::Zero();
    Vec2f         m_uv_editor_bbox_max = Vec2f::Zero();
    PatchUnwrap   m_uv_editor_unwrap;
    GLModel          m_island_overlay_glmodel{"island"};
    const std::vector<int> *island_highlight_selection();
    void rebuild_island_overlay(const std::vector<int> &selection);
    void render_island_overlay();
    // render_painter_gizmo's poll of the pane's selection: the islands shown on the model follow it
    void follow_islands()
    {
        if (const std::vector<int> *islands = island_highlight_selection()) {
            if (*islands != m_island_overlay_selection) rebuild_island_overlay(*islands);
            render_island_overlay();
        } else if (m_island_overlay_glmodel.is_initialized()) {
            m_island_overlay_glmodel.reset();
            m_island_overlay_selection.clear();
        }
    }
    std::vector<int> m_island_overlay_selection;
    UVCheckMode   m_uv_check_mode = UVCheckMode::None;
    int           m_uv_select_mode = 0;
    std::vector<std::pair<int, float>> m_uv_command_queue;
    const void                *m_uv_thumb_source = nullptr;
    std::vector<unsigned char> m_uv_thumb_rgb;
    bool    m_seam_edit_mode   = false;
    bool    m_seam_path_mode   = false;
    GLModel m_seam_glmodel{"seams"};
    GLModel m_seam_hover_glmodel{"seam_hover"};
    GLModel m_seam_anchor_glmodel{"seam_anchor"};
    std::pair<int, int> m_seam_hover_edge{ -1, -1 };
    int     m_seam_hover_vertex = -1;
    int     m_seam_path_anchor  = -1;
    bool    m_picker_open_request    = false;
    bool    m_picker_reveal_selected = false;
    bool    m_island_drag_active = false;
    std::vector<int> m_island_move_set;
    bool    m_use_shaded_preview  = false;
    bool    m_shaded_preview_dirty = false;
    int     m_shaded_active_chart = -1;
    std::vector<uint8_t>       m_shaded_active_face;
    Eigen::Matrix<float, 2, 3> m_shaded_baked_active_xf = Eigen::Matrix<float, 2, 3>::Identity();
    Eigen::Matrix<float, 2, 3> m_shaded_island_delta    = Eigen::Matrix<float, 2, 3>::Identity();
    void refresh_wireframe() {}
    // the views drawn by upstream's shaders: Fast (shaded relief) and Checker / Distortion on the model
    void rebuild_uvcheck_mesh();
    void render_uvcheck_mesh();
    void rebuild_shaded_preview_mesh();
    void render_shaded_preview_mesh();
    void compute_shaded_active_faces(const std::vector<int> &charts, size_t patch_face_count);
    bool shaded_preview_ready() const;
    void publish_view_meshes();
    GLTexture *get_layer_height_texture(const TextureDisplacementLayer &layer);
    GLTexture *get_layer_color_texture(const TextureDisplacementLayer &layer);
    std::vector<Vec2f> model_uv_corners(const indexed_triangle_set &local_patch, const TextureDisplacementLayer &layer,
                                        const std::vector<int> *patch_source) const;
    std::vector<Vec2f> compute_layer_vertex_uvs(const indexed_triangle_set &patch, const TextureDisplacementLayer &layer,
                                                const std::vector<int> *patch_source = nullptr) const;
    int layer_projection_frame(const indexed_triangle_set &local_patch, const TextureDisplacementLayer &layer, Vec3f &center, Vec3f &axis) const;
    std::vector<Vec2f> compute_layer_corner_uvs(const indexed_triangle_set &patch, const TextureDisplacementLayer &layer,
                                                const std::vector<int> *patch_source = nullptr) const;
    struct ClippingPlaneDataWrapper { std::array<float, 4> clp_dataf; std::array<float, 2> z_range; };
    ClippingPlaneDataWrapper get_clipping_plane_data() const
    {
        Vec3d n;
        double d;
        // GLGizmoPainterBase::get_clipping_plane_data: the section view's plane (the shaders clip where dot < 0)
        if (orca::Viewport::get().clipping_plane(n, d)) return {{float(-n.x()), float(-n.y()), float(-n.z()), float(d)}, {-FLT_MAX, FLT_MAX}};
        return {{0.f, 0.f, 1.f, FLT_MAX}, {-FLT_MAX, FLT_MAX}};
    }
    GLModel     m_uvcheck_glmodel{"uvcheck"};
    bool        m_uvcheck_uses_vertex_uv  = false;
    int         m_uvcheck_projection_mode = 0;
    Vec3f       m_uvcheck_patch_center    = Vec3f::Zero();
    Vec3f       m_uvcheck_patch_axis      = Vec3f::UnitZ();
    GLModel     m_shaded_preview_glmodel{"shaded"};
    bool        m_shaded_preview_uses_vertex_uv = false;
    int         m_shaded_projection_mode = 0;
    Vec3f       m_shaded_patch_center    = Vec3f::Zero();
    Vec3f       m_shaded_patch_axis      = Vec3f::UnitZ();
    std::vector<PaletteEntry> m_shaded_preview_palette;
    std::unique_ptr<GLTexture>                      m_height_tex;
    float                                           m_height_tex_smoothing = -1.f;
    std::unique_ptr<GLTexture>                      m_color_tex;
    float                                           m_color_tex_smoothing = -1.f;
    // Adjust placement: the handle on the painted area that drags the texture's offset
    enum class AdjustHandle { None, Pan, AxisU, AxisV };
    bool  update_adjust_anchor();
    Vec3f adjust_plane_point() const;
    Vec3f adjust_handle_center(const TextureDisplacementLayer &layer) const;
    void  adjust_tangent_basis(Vec3f &u_axis, Vec3f &v_axis) const;
    bool  on_mouse_adjust_texture(const wxMouseEvent &mouse_event);
    static float layer_texture_aspect(const TextureDisplacementLayer &layer);
    const TextureDisplacementLayer *active_layer() const { return const_cast<GLGizmoTextureDisplacement *>(this)->active_layer(); }
    void  publish_adjust_overlay();
    AdjustHandle m_adjust_drag_handle = AdjustHandle::None;
    Vec2f        m_adjust_drag_start_offset = Vec2f::Zero();
    Vec2f        m_adjust_drag_start_planar = Vec2f::Zero();
    bool         m_adjust_anchor_valid = false;
    Vec3f        m_adjust_anchor_pos    = Vec3f::Zero();
    Vec3f        m_adjust_anchor_normal = Vec3f::UnitZ();
    void queue_prepare(const TextureDisplacementPrepareParams &params, const std::string &snapshot_name, bool then_bake,
                       const std::string &unchanged_msg);
    TextureDisplacementLayer *active_layer();
    nlohmann::json state();

    EState      m_state{Off};
    CommonData  m_c_data;
    CommonData *m_c{&m_c_data};
    Canvas      m_parent;
    int  m_active_layer_slot = 0;
    int  m_panel_mode        = 0;
    bool m_erase_mode        = false;
    bool m_auto_update       = true;
    bool m_bake_in_progress  = false;
    bool m_prepare_in_progress = false;
    bool m_adjust_texture_mode = false;
    bool m_project_only_visible = false;
    int  apply_projection_frame();
    TextureProjectorFrame  m_projector;
    TextureProjectorFrame *m_projector_frame = &m_projector;
    int                    m_projector_opacity = 140;
    // the Painter's selectors as GLGizmoPainterBase::m_triangle_selectors (get_facets_strict)
    struct SelectorProxy
    {
        int part = 0;
        indexed_triangle_set get_facets_strict(EnforcerBlockerType state, std::vector<int> *source = nullptr) const
        {
            return orca::Painter::get().facets_strict(part, state, source);
        }
    };
    struct Selectors
    {
        SelectorProxy  proxy;
        size_t         size() const { return size_t(orca::Painter::get().parts_count()); }
        bool           empty() const { return size() == 0; }
        SelectorProxy *operator[](size_t i) { proxy.part = int(i); return &proxy; }
    } m_triangle_selectors;
    int  m_picker_slot         = -1;
    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS>                                            m_layer_expanded{};
    std::array<std::unique_ptr<GLTexture>, TEXTURE_DISPLACEMENT_MAX_LAYERS>                       m_thumbnails;
    std::array<std::weak_ptr<const std::vector<unsigned char>>, TEXTURE_DISPLACEMENT_MAX_LAYERS> m_thumbnail_source;
    std::array<float, TEXTURE_DISPLACEMENT_MAX_LAYERS>                                            m_thumbnail_smoothing{};
    std::map<std::string, LibraryTexture> m_library_textures;
    // Standard mode's pinned recipe (apply_standard_mode_presets), Pro mode's controls
    bool  m_subdivide_adaptive      = false;
    bool  m_subdivide_feature       = false;
    float m_subdivide_target_mm     = 0.f;
    int   m_subdivide_count         = 1;
    bool  m_subdivide_editing       = false;
    int   m_subdivide_preview_tris  = -1;
    GLModel m_subdivide_preview_glmodel{"subdivide"};
    double      m_painted_area_mm2 = 0.;
    std::string m_painted_area_key;
    V2Resolution m_v2_rec;
    std::string  m_v2_rec_key;
    float m_subdivide_detail_mm     = 0.05f;
    float m_subdivide_min_edge_mm   = 0.1f;
    float m_subdivide_border_mm     = 0.4f;
    float m_subdivide_color_mm      = 0.3f;
    int   m_subdivide_budget_k      = 750;
    float m_remesh_target_edge_mm   = 0.f;
    float m_remesh_sharp_angle_deg  = 40.f;
    bool  m_remesh_keep_sharp_edges = true;
    // cached_palette
    std::vector<PaletteEntry> m_palette_cache;
    ColorQuantizeFn           m_palette_quantizer;
    ColorQuantizeFn           m_palette_pure_quantizer;
    bool                      m_palette_changed = false;
    std::vector<ColorRGBA>                                   m_palette_filaments;
    std::vector<std::shared_ptr<std::vector<unsigned char>>> m_palette_images;
    std::optional<std::vector<PaletteEntry>>                 m_mix_ranking;
    bool                                                     m_palette_mixing     = false;
    int                                                      m_palette_mix_count  = 0;
    int                                                      m_palette_free_slots = 0;
    std::vector<std::string>                                 m_palette_slots;
    // the preview job: one in flight, a newer request waits for it (queue_preview_job)
    std::shared_ptr<std::atomic<uint64_t>> m_preview_generation = std::make_shared<std::atomic<uint64_t>>(0);
    bool m_preview_job_running = false;
    bool m_preview_job_pending = false;
    void queue_preview_job();
};

#include "slic3r/GUI/TextureDisp.inc.bcls"
#include "slic3r/GUI/TextureDisp.inc.bake"
#include "slic3r/GUI/TextureDisp.inc.bfn"
#include "slic3r/GUI/TextureDisp.inc.pcls"
#include "slic3r/GUI/TextureDisp.inc.prep"
#include "slic3r/GUI/TextureDisp.inc.pfn"
#include "slic3r/GUI/TextureDisp.inc"

static GLGizmoTextureDisplacement &gizmo()
{
    static GLGizmoTextureDisplacement g;
    return g;
}

TextureDisplacementLayer *GLGizmoTextureDisplacement::active_layer()
{
    ModelVolume *mv = texture_volume();
    if (mv == nullptr) return nullptr;
    for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
        if (l.slot == m_active_layer_slot) return &l;
    return nullptr;
}

// GLGizmoTextureDisplacement::update_model_object: the selectors into the active layer's mask, the preview follows
void GLGizmoTextureDisplacement::update_model_object()
{
    store_panel_state();
    orca::Painter::get().flush();
    rebuild_preview();
}

// GLGizmoTextureDisplacement::update_from_model_object: the panel back where it was left (on open), the selectors
// reloaded from the active layer, a first layer for a never-textured volume
void GLGizmoTextureDisplacement::update_from_model_object(bool first_update)
{
    if (first_update) {
        if (const ModelVolume *mv = texture_volume(); mv != nullptr) {
            const TextureDisplacementOptions &opts = mv->texture_displacement_options;
            m_panel_mode = std::clamp(opts.panel_mode, 0, 1);
            const auto &stack = mv->texture_displacement_layers;
            if (std::any_of(stack.begin(), stack.end(), [&opts](const TextureDisplacementLayer &l) { return l.slot == opts.active_slot; }))
                m_active_layer_slot = opts.active_slot;
        }
    }
    orca::Painter::get().set_slot(m_active_layer_slot);
    if (first_update)
        if (ModelVolume *tv = texture_volume(); tv != nullptr && tv->texture_displacement_layers.empty()) {
            add_texture_layer();
            return;
        }
    rebuild_preview();
}

void GLGizmoTextureDisplacement::select_whole_model(bool paint)
{
    orca::Painter::get().fill_all(paint);   // with the snapshot of the stroke, then the preview (texture_disp_changed)
}

void GLGizmoTextureDisplacement::open()
{
    m_state = On;
    g_uv_canvas.set_status_callback([](const wxString &s) { g_uv_canvas.m_status = s; });
    update_from_model_object(true);
}

// on_shutdown: the preview dropped, any job's result discarded, the palette rebuilt next time
void GLGizmoTextureDisplacement::close()
{
    if (m_state == Off) return;
    m_state = Off;
    m_preview_generation->fetch_add(1);
    m_preview_job_pending = false;
    m_subdivide_editing      = false;
    m_subdivide_preview_tris = -1;
    m_subdivide_preview_glmodel.reset();
    close_image_adjust();
    m_adjust_texture_mode = false;
    m_adjust_drag_handle  = AdjustHandle::None;
    m_adjust_anchor_valid = false;
    publish_adjust_overlay();
    // on_shutdown: the seam marking off, the UV pane hidden
    m_seam_edit_mode   = false;
    m_seam_path_mode   = false;
    m_seam_path_anchor = -1;
    m_seam_glmodel.reset();
    m_seam_hover_glmodel.reset();
    m_seam_anchor_glmodel.reset();
    m_show_uv_editor = false;
    m_uv_editor_bg   = UVBackground::None;
    g_uv_shown       = false;
    m_uv_check_mode  = UVCheckMode::None;
    m_projector.shown = false;
    m_uvcheck_glmodel.reset();
    m_shaded_preview_glmodel.reset();
    m_island_overlay_glmodel.reset();
    m_island_overlay_selection.clear();
    orca::Painter::get().set_shader_mesh("uvcheck", std::nullopt);
    orca::Painter::get().set_shader_mesh("shaded", std::nullopt);
    orca::Painter::get().set_shader_mesh("island", std::nullopt);
    m_palette_cache.clear();
    m_palette_quantizer      = nullptr;
    m_palette_pure_quantizer = nullptr;
    m_palette_filaments.clear();
    m_palette_images.clear();
    m_mix_ranking.reset();
    m_palette_slots.clear();
    m_palette_changed = false;
}

// render_adjust_texture_gizmo's geometry: the panel (a square of panel_half around the handle) and the U / V arrows, in
// world coordinates for the Painter to draw over the model
void GLGizmoTextureDisplacement::publish_adjust_overlay()
{
    std::vector<orca::Painter::OverlayPrim> prims;
    const ModelObject              *mo    = m_c->selection_info()->model_object();
    const ModelVolume              *mv    = texture_volume();
    const TextureDisplacementLayer *layer = active_layer();
    if (m_adjust_texture_mode && m_adjust_anchor_valid && mo != nullptr && mv != nullptr && layer != nullptr) {
        const Transform3d trafo_matrix = mo->instances[m_parent.get_selection().get_instance_idx()]->get_transformation().get_matrix() * mv->get_matrix();
        const float bbox_size    = float(mv->mesh().bounding_box().size().norm());
        const float panel_half   = bbox_size * 0.04f + 1.f;
        const float arrow_length = panel_half * 2.2f;
        const Vec3f center = adjust_handle_center(*layer);
        Vec3f u_axis, v_axis;
        adjust_tangent_basis(u_axis, v_axis);
        auto world = [&](const Vec3f &local) -> Vec3f { return (trafo_matrix * local.cast<double>()).cast<float>(); };
        auto quad = [&](float sx, float sy) -> Vec3f { return world(center + u_axis * (sx * panel_half) + v_axis * (sy * panel_half)); };
        ColorRGBA panel_color = m_adjust_drag_handle == AdjustHandle::Pan ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE();
        panel_color.a(0.45f);
        prims.push_back({false, {quad(-1, -1), quad(1, -1), quad(1, 1), quad(-1, -1), quad(1, 1), quad(-1, 1)}, panel_color});
        auto arrow = [&](const Vec3f &axis, const Vec3f &other, bool active) {
            auto p = [&](float a, float b) -> Vec3f { return world(center + (axis * a + other * b) * arrow_length); };
            prims.push_back({true, {p(0, 0), p(1, 0), p(1, 0), p(0.82f, 0.08f), p(1, 0), p(0.82f, -0.08f)},
                             active ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE()});
        };
        arrow(u_axis, v_axis, m_adjust_drag_handle == AdjustHandle::AxisU);
        arrow(v_axis, u_axis, m_adjust_drag_handle == AdjustHandle::AxisV);
    }
    wx_call_after([prims = std::move(prims)]() mutable { orca::Painter::get().set_overlay(std::move(prims)); });
}

// render_painter_gizmo's draws of the shaded (Fast) mesh and the checker / distortion overlay: the render functions run
// once against the recording shader, the Painter draws the result every frame
void GLGizmoTextureDisplacement::publish_view_meshes()
{
    if (m_use_shaded_preview && m_shaded_preview_glmodel.is_initialized() && shaded_preview_ready())
        render_shaded_preview_mesh();
    else
        orca::Painter::get().set_shader_mesh("shaded", std::nullopt);
    if (m_uv_check_mode != UVCheckMode::None && m_uvcheck_glmodel.is_initialized())
        render_uvcheck_mesh();
    else
        orca::Painter::get().set_shader_mesh("uvcheck", std::nullopt);
}

// GLGizmoTextureDisplacement::rebuild_preview (the true-displacement view)
void GLGizmoTextureDisplacement::rebuild_preview()
{
    m_preview_generation->fetch_add(1);
    m_palette_changed = false;
    update_uv_editor();
    rebuild_shaded_preview_mesh();
    rebuild_uvcheck_mesh();
    rebuild_seam_overlay();
    orca::Painter::get().m_seams_on_top = m_seam_edit_mode || orca::Painter::get().wireframe();
    publish_adjust_overlay();
    publish_view_meshes();
    follow_islands();
    if (m_subdivide_editing && m_subdivide_adaptive) rebuild_subdivide_preview();
    const ModelVolume *mv = texture_volume();
    if (m_use_shaded_preview) {
        // Fast view: the shader is the preview; no CPU displacement
        m_preview_job_pending = false;
        wx_call_after([] { orca::Painter::get().set_preview(-1, nullptr, {}); });
        return;
    }
    if (mv == nullptr || !mv->is_texture_displacement_painted()) {
        m_preview_job_pending = false;
        wx_call_after([] { orca::Painter::get().set_preview(-1, nullptr, {}); });
        return;
    }
    queue_preview_job();
}

struct PreviewInput
{
    indexed_triangle_set                  base_mesh;
    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementFacetsData         facets_data;
    TextureDisplacementOptions            options;
    Transform3d                           volume_to_world = Transform3d::Identity();
    TextureColorSettings                  color;
    std::vector<Vec2f>                    model_uv;
};

// GLGizmoTextureDisplacement::queue_preview_job and TextureDisplacementPreviewJob: the displaced mesh off the engine
// thread, abandoned as soon as a newer state is asked for; the triangles grouped by palette entry for drawing
void GLGizmoTextureDisplacement::queue_preview_job()
{
    if (m_state != On) return;
    const ModelVolume *mv = texture_volume();
    if (mv == nullptr || !mv->is_texture_displacement_painted()) return;
    if (m_preview_job_running) {
        m_preview_job_pending = true;
        return;
    }
    const uint64_t generation = m_preview_generation->load();
    auto input = std::make_shared<PreviewInput>();
    input->base_mesh = mv->mesh().its;
    input->layers    = mv->texture_displacement_layers;
    input->options   = mv->texture_displacement_options;
    if (input->options.pipeline_v2) input->options.v2_max_triangles_k = 0;
    input->volume_to_world = texture_displacement_volume_to_world(*mv);
    if (mv->has_texture_uv()) input->model_uv = mv->texture_uv;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        input->facets_data[size_t(i)] = mv->texture_displacement_facet(i).get_data();
    input->color = color_settings_for(*mv);
    std::vector<ColorRGBA> entry_colors;
    for (const PrintableColor &e : input->color.palette) entry_colors.emplace_back(e.rgb.x(), e.rgb.y(), e.rgb.z(), 1.f);
    const ModelObject *mo = mv->get_object();
    const int vol = int(std::find(mo->volumes.begin(), mo->volumes.end(), mv) - mo->volumes.begin());

    m_preview_job_running = true;
    std::shared_ptr<std::atomic<uint64_t>> current = m_preview_generation;
    std::thread([input, generation, current, entry_colors, vol] {
        std::vector<uint8_t> triangle_color;
        TextureColorRequest  color_request;
        TextureColorRequest *color = nullptr;
        if (!input->color.empty()) {
            color_request.quantize = GLGizmoTextureDisplacement::make_palette_quantizer(input->color.palette);
            if (!input->color.palette_pure.empty())
                color_request.quantize_pure = GLGizmoTextureDisplacement::make_palette_quantizer(input->color.palette_pure);
            color_request.despeckle_passes = input->color.despeckle_passes;
            color_request.out_triangle     = &triangle_color;
            if (color_request.quantize) color = &color_request;
        }
        indexed_triangle_set its;
        try {
            its = build_texture_displacement(input->base_mesh, input->layers, input->facets_data, input->options,
                                             [&](int) { return current->load() == generation; }, color, input->volume_to_world, nullptr, nullptr,
                                             input->model_uv.empty() ? nullptr : &input->model_uv);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Texture displacement preview: " << e.what();
        }
        // the triangles of one colour in a row, so each is one draw (queue_preview_job's sorting)
        std::vector<orca::Painter::PreviewRun> runs;
        std::shared_ptr<const TriangleMesh> mesh;
        if (!its.indices.empty()) {
            static const ColorRGBA NEUTRAL_COLOR = {0.8f, 0.8f, 0.8f, 1.0f};
            indexed_triangle_set sorted;
            if (triangle_color.size() == its.indices.size() && !entry_colors.empty()) {
                sorted.vertices = its.vertices;
                for (int want = 0; want <= int(entry_colors.size()); ++want) {
                    const size_t first = sorted.indices.size();
                    for (size_t i = 0; i < its.indices.size(); ++i)
                        if (int(triangle_color[i]) == want) sorted.indices.push_back(its.indices[i]);
                    if (sorted.indices.size() > first)
                        runs.push_back({first, sorted.indices.size() - first, want == 0 ? NEUTRAL_COLOR : entry_colors[size_t(want - 1)]});
                }
            } else {
                sorted = std::move(its);
                runs.push_back({0, sorted.indices.size(), NEUTRAL_COLOR});
            }
            mesh = std::make_shared<const TriangleMesh>(std::move(sorted));
        }
        wx_call_after([generation, mesh, runs, vol]() mutable {
            GLGizmoTextureDisplacement &g = gizmo();
            g.m_preview_job_running = false;
            if (generation != g.m_preview_generation->load())
                g.m_preview_job_pending = true;
            else if (mesh)
                orca::Painter::get().set_preview(vol, std::move(mesh), std::move(runs));
            if (g.m_preview_job_pending) {
                g.m_preview_job_pending = false;
                std::lock_guard<std::recursive_mutex> l(orca::Core::get().mutex);
                g.queue_preview_job();
            }
        });
    }).detach();
}

// UVEditorCanvas::render's input: the view, the islands as drawn (island_uv), what is selected / hovered, the gesture's dial
nlohmann::json UVEditorCanvas::export_view()
{
    using nlohmann::json;
    if (m_needs_fit && m_size.GetWidth() > 1) fit_view_to_content();
    update_status();   // render() does it every frame
    float half_w, half_h;
    view_half_extents(half_w, half_h);
    json uvs = json::array(), tris = json::array(), vi = json::array(), edges = json::array();
    for (size_t i = 0; i < m_islands.uvs.size(); ++i) {
        const Vec2f p = island_uv(i);
        uvs.push_back(p.x());
        uvs.push_back(p.y());
    }
    for (const Vec3i32 &t : m_islands.indices) { tris.push_back(t[0]); tris.push_back(t[1]); tris.push_back(t[2]); }
    for (int v : m_islands.vertex_island) vi.push_back(v);
    for (const auto &[a, b] : m_islands.boundary_edges) { edges.push_back(a); edges.push_back(b); }
    json fills = json::array();
    for (const ColorRGBA &c : m_island_fill_colors) fills.push_back({c.r(), c.g(), c.b(), c.a()});
    json sel_edges = json::array();
    for (const auto &[a, b] : m_sel_edges) { sel_edges.push_back(a); sel_edges.push_back(b); }
    const bool rotating = (m_gesture == Gesture::RotateIsland || m_gesture == Gesture::RotateIslandModal) && m_selected_island >= 0;
    json dial = nullptr;
    if (rotating) {
        const Vec2f c = island_centroid(m_selected_island);
        dial = {{"x", c.x()}, {"y", c.y()}, {"deg", m_rot_display_deg}};
    }
    if (m_background_dirty) { m_background_dirty = false; ++m_background_revision; }
    Vec2f lo(0.f, 0.f), hi(1.f, 1.f);
    if (m_tile_enabled) framed_bounds(lo, hi);
    return {{"pan", {m_pan.x(), m_pan.y()}}, {"half", {half_w, half_h}}, {"uvs", uvs}, {"tris", tris}, {"vertex_island", vi},
            {"boundary", edges}, {"islands", m_islands.island_count}, {"fills", fills}, {"selection", m_selection},
            {"selected", m_selected_island}, {"hover_island", m_hover_island}, {"hover_vertex", m_hover_vertex},
            {"hover_edge", {m_hover_edge.first, m_hover_edge.second}}, {"mode", int(m_select_mode)}, {"sel_vertices", m_sel_vertices},
            {"sel_edges", sel_edges}, {"active_vertex", m_active_vertex}, {"active_edge", {m_active_edge.first, m_active_edge.second}},
            {"dial", dial}, {"snap", m_snap_enabled}, {"modal", modal_gesture()},
            {"background", m_background_width > 0 ? background_file() : std::string()}, {"bg_revision", m_background_revision},
            {"bg_lo", {lo.x(), lo.y()}}, {"bg_hi", {hi.x(), hi.y()}}, {"tile", m_tile_enabled}, {"mirrored", m_tile_mirrored},
            {"status", m_status.ToUTF8()}};
}

// the backdrop as a PNG (gray), rewritten when it changes
std::string UVEditorCanvas::background_file()
{
    static unsigned    written = ~0u;
    const std::string  path = (boost::filesystem::temp_directory_path() / ("orca_uvbg_" + std::to_string(m_background_revision % 2) + ".png")).string();
    if (written != m_background_revision) {
        std::vector<uint8_t> gray(size_t(m_background_width) * size_t(m_background_height));
        for (size_t i = 0; i < gray.size(); ++i) gray[i] = m_background_pixels[i * 4];
        png::write_gray_to_file(path, size_t(m_background_width), size_t(m_background_height), gray);
        written = m_background_revision;
    }
    return path;
}

// --- the panel's state ------------------------------------------------------------------------------------------

static const char *projection_name(TextureProjectionMethod m)
{
    switch (m) {
    case TextureProjectionMethod::Triplanar: return "triplanar";
    case TextureProjectionMethod::Cylindrical: return "cylindrical";
    case TextureProjectionMethod::Spherical: return "spherical";
    case TextureProjectionMethod::LSCM: return "lscm";
    case TextureProjectionMethod::ViewProjected: return "view";
    case TextureProjectionMethod::ModelUV: return "model_uv";
    }
    return "triplanar";
}

// a layer's image as a file the app can show: the library file it came from, else its bytes in the cache
static std::string layer_image_file(const TextureDisplacementLayer &layer)
{
    if (!layer.path.empty() && boost::filesystem::exists(layer.path)) return layer.path;
    if (!layer.image_data || layer.image_data->empty()) return {};
    const std::size_t h = std::hash<std::string>()(std::string(layer.image_data->begin(), layer.image_data->end()));
    const boost::filesystem::path p = boost::filesystem::temp_directory_path() / ("orca_texdisp_" + std::to_string(h) + ".png");
    if (!boost::filesystem::exists(p)) {
        FILE *f = fopen(p.string().c_str(), "wb");
        if (f) { fwrite(layer.image_data->data(), 1, layer.image_data->size(), f); fclose(f); }
    }
    return p.string();
}

// the window's image: the adjustment applied to the downscaled source (update_image_adjust_preview), as a PNG
std::string GLGizmoTextureDisplacement::adjust_preview_file()
{
    if (m_adjust_preview_source.empty()) return {};
    const DecodedHeightTexture img = apply_texture_adjustments(m_adjust_preview_source, m_adjust);
    static int n = 0;
    const std::string path = (boost::filesystem::temp_directory_path() / ("orca_texadjust_" + std::to_string(++n % 4) + ".png")).string();
    const bool ok = img.has_color() ? png::write_rgb_to_file(path, size_t(img.width), size_t(img.height), img.rgb)
                                    : png::write_gray_to_file(path, size_t(img.width), size_t(img.height), img.pixels);
    return ok ? path : std::string();
}

nlohmann::json GLGizmoTextureDisplacement::state()
{
    using nlohmann::json;
    ModelVolume *mv = texture_volume();
    json layers = json::array();
    if (mv != nullptr) {
        std::vector<const TextureDisplacementLayer *> ordered;
        for (const TextureDisplacementLayer &l : mv->texture_displacement_layers) ordered.push_back(&l);
        std::sort(ordered.begin(), ordered.end(), [](auto *a, auto *b) { return a->slot < b->slot; });
        for (const TextureDisplacementLayer *l : ordered) {
            const bool has_color = !l->empty() && height_texture_has_color(*l);
            layers.push_back({{"slot", l->slot}, {"name", l->name.empty() ? Slic3r::format(_u8L("Layer %1%"), l->slot + 1) : l->name},
                              {"image", layer_image_file(*l)}, {"painted", !mv->texture_displacement_facet(l->slot).empty()},
                              {"expanded", m_layer_expanded[size_t(l->slot)]}, {"depth", l->depth_mm}, {"tile_size", l->tiling_scale},
                              {"rotation", l->rotation_deg}, {"midlevel", l->midlevel}, {"smoothing", l->smoothing},
                              {"edge_smoothing", l->edge_smoothing}, {"edge_amount", l->edge_smoothing_amount}, {"invert", l->invert},
                              {"has_color", has_color}, {"color", l->color_enabled && has_color}, {"projection", projection_name(l->projection_method)},
                              {"tile", l->tile_enabled}, {"tile_method", int(l->tile_method)}, {"blend", int(l->blend_mode)},
                              {"projective", l->view_project_projective}});
        }
    }
    json library = json::array();
    for (const TextureLibraryEntry &e : texture_library())
        library.push_back({{"name", e.name}, {"path", e.path}, {"source", int(e.source)}, {"pack", e.pack_name}});
    json opts = json::object();
    int n_colors = 0, n_filaments = 0, max_mixes = 1;
    if (mv != nullptr) {
        const TextureDisplacementOptions &o = mv->texture_displacement_options;
        opts = {{"mix", o.color_mix_enabled}, {"mix_count", o.color_mix_count}, {"despeckle", o.color_despeckle},
                {"displace_border", o.displace_border}, {"smooth", o.smooth_enabled}, {"smooth_strength", o.smooth_strength},
                {"smooth_iterations", o.smooth_iterations}, {"smooth_skip_border", o.smooth_skip_border}, {"classic", !o.pipeline_v2},
                {"budget_k", o.v2_max_triangles_k}};
        if (any_layer_colors(*mv) && o.color_mix_enabled) {
            n_colors    = int(cached_palette().size());
            n_filaments = int(m_palette_filaments.size());
            max_mixes   = std::max(1, int(EnforcerBlockerType::ExtruderMax) - n_filaments);
        }
    }
    // on_render_input_window seeds the edge lengths from the mesh the first time Pro shows them
    json prep = json::object();
    if (mv != nullptr && pro_mode()) {
        const indexed_triangle_set &its = mv->mesh().its;
        double sum = 0.0;
        size_t cnt = 0;
        if (m_subdivide_target_mm <= 0.f || m_remesh_target_edge_mm <= 0.f)
            for (const stl_triangle_vertex_indices &tri : its.indices)
                for (int i = 0; i < 3; ++i) { sum += (its.vertices[tri[i]] - its.vertices[tri[(i + 1) % 3]]).norm(); ++cnt; }
        if (m_subdivide_target_mm <= 0.f) m_subdivide_target_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)) * 0.5f, 0.001f, 20.f) : 1.f;
        if (m_remesh_target_edge_mm <= 0.f) m_remesh_target_edge_mm = cnt > 0 ? std::clamp(float(sum / double(cnt)), 0.1f, 20.f) : 1.f;
        prep = {{"adaptive", m_subdivide_adaptive}, {"feature", m_subdivide_feature}, {"target", m_subdivide_target_mm},
                {"detail", m_subdivide_detail_mm}, {"min_edge", m_subdivide_min_edge_mm}, {"border", m_subdivide_border_mm},
                {"color", m_subdivide_color_mm}, {"steps", m_subdivide_count}, {"editing", m_subdivide_editing},
                {"preview_tris", m_subdivide_preview_tris}, {"remesh_edge", m_remesh_target_edge_mm},
                {"keep_sharp", m_remesh_keep_sharp_edges}, {"sharp_angle", m_remesh_sharp_angle_deg},
                {"any_colors", any_layer_colors(*mv)}, {"triangles", its.indices.size()}};
    }
    json adjust = nullptr;
    if (m_adjust_slot >= 0)
        adjust = {{"slot", m_adjust_slot}, {"image", adjust_preview_file()}, {"brightness", m_adjust.brightness},
                  {"contrast", m_adjust.contrast}, {"sharpness", m_adjust.sharpness}, {"blur", m_adjust.blur}, {"hue", m_adjust.hue_deg},
                  {"has_color", m_adjust_source.has_color()}, {"identity", m_adjust.identity()},
                  {"aspect", m_adjust_source.height > 0 ? double(m_adjust_source.width) / m_adjust_source.height : 1.0}};
    json uv = nullptr;
    if (g_uv_shown) {
        const UVEditorCanvas::PaneState &ps = g_uv_canvas.pane_state();
        uv = {{"has_layer", ps.has_layer}, {"layer_name", ps.layer_name.ToUTF8()}, {"tile_mm", ps.tile_mm}, {"seam_angle", ps.seam_angle_deg},
              {"connect", ps.connect_islands}, {"unwrapped", ps.unwrapped}, {"stale", ps.unwrap_stale}, {"mark_seams", ps.mark_seams},
              {"seam_path", ps.seam_path}, {"has_seams", ps.has_seams}, {"has_uv_edits", ps.has_uv_edits}, {"background", int(ps.background)},
              {"island_count", ps.island_count}, {"face_count", ps.face_count}, {"select_mode", int(g_uv_canvas.select_mode())},
              {"has_islands", g_uv_canvas.has_islands()}, {"selected", g_uv_canvas.selected_island()}, {"snap", g_uv_canvas.snap_enabled()},
              {"image", active_layer() ? layer_image_file(*active_layer()) : std::string()},
              {"stats", ps.unwrapped ? into_u8(wxString::Format(_L_PLURAL("%d island", "%d islands", ps.island_count), ps.island_count) + ", " +
                                               wxString::Format(_L_PLURAL("%s face", "%s faces", ps.face_count), wxString(std::to_string(ps.face_count))))
                                     : std::string()}};
    }
    return {{"open", m_state == On}, {"prep", prep}, {"adjust", adjust}, {"uv", uv}, {"view", view_mode()},
            {"unwrap_islands", m_uv_editor_unwrap.empty() ? -1 : m_uv_editor_unwrap.chart_count},
            {"unwrap_text", m_uv_editor_unwrap.empty() ? _u8L("Not unwrapped yet")
                                                       : Slic3r::format(into_u8(_L_PLURAL("%1% island", "%1% islands", m_uv_editor_unwrap.chart_count)),
                                                                        m_uv_editor_unwrap.chart_count)}, {"has_volume", mv != nullptr}, {"active", m_active_layer_slot}, {"pro", m_panel_mode != 0},
            {"erase", m_erase_mode}, {"auto", m_auto_update}, {"busy", m_bake_in_progress || m_prepare_in_progress},
            {"painted", mv != nullptr && mv->is_texture_displacement_painted()}, {"has_uv", mv != nullptr && mv->has_texture_uv()},
            {"only_visible", m_project_only_visible}, {"wireframe", orca::Painter::get().wireframe()},
            {"projector", {{"open", m_projector.shown}, {"opacity", m_projector_opacity}}},
            {"adjust_placement", m_adjust_texture_mode},
            {"layers", layers}, {"max_layers", TEXTURE_DISPLACEMENT_MAX_LAYERS}, {"library", library}, {"options", opts},
            {"palette_colors", n_colors}, {"palette_filaments", n_filaments}, {"max_mixes", max_mixes},
            // the panel's plurals (on_render_input_window)
            {"palette_text", Slic3r::format(into_u8(_L_PLURAL("%1% printable color %2%", "%1% printable colors %2%", n_colors)), n_colors,
                                            Slic3r::format(into_u8(_L_PLURAL("from %1% filament", "from %1% filaments", n_filaments)), n_filaments))},
            {"layers_full_text", Slic3r::format(into_u8(_L_PLURAL("All %1% layer used", "All %1% layers used", unsigned(TEXTURE_DISPLACEMENT_MAX_LAYERS))),
                                                TEXTURE_DISPLACEMENT_MAX_LAYERS)},
            {"preview_tris_text", m_subdivide_preview_tris > 0 ? Slic3r::format(into_u8(_L_PLURAL("Preview: %1% triangle", "Preview: %1% triangles",
                                                                                                  m_subdivide_preview_tris)), m_subdivide_preview_tris)
                                                               : std::string()},
            {"paint", orca::Painter::get().state()}};
}

}}} // namespace Slic3r::GUI::td

namespace orca {
using namespace Slic3r;
using Slic3r::GUI::td::GLGizmoTextureDisplacement;
using Slic3r::GUI::td::gizmo;

void texture_disp_changed() { gizmo().rebuild_preview(); }
void texture_disp_open(bool open) { if (open) gizmo().open(); else gizmo().close(); }

// GLGizmoTextureDisplacement::on_mouse in Adjust placement (UI thread): -1 when not adjusting, else whether the handle
// took it (action -1 only asks)
int texture_disp_mouse(int action, double px, double py)
{
    std::lock_guard<std::recursive_mutex> l(Core::get().mutex);
    GLGizmoTextureDisplacement &g = gizmo();
    using Slic3r::GUI::td::wxMouseEvent;
    const int type = action == 0 ? Slic3r::GUI::td::wxEVT_LEFT_DOWN : action == 1 ? Slic3r::GUI::td::wxEVT_MOTION : Slic3r::GUI::td::wxEVT_LEFT_UP;
    // on_mouse: Mark seams takes the clicks first (the brush is off meanwhile), then Adjust placement
    if (g.m_state == GLGizmoTextureDisplacement::On && g.m_seam_edit_mode) {
        if (action < 0) return 0;
        wxMouseEvent evt(type, px, py, false, false, 0, action == 1);
        g.on_mouse_seam(evt);
        g.process_uv_commands();
        if (action == 0) Host::event("gizmo_changed", json::object());
        return 1;
    }
    if (g.m_state != GLGizmoTextureDisplacement::On || !g.m_adjust_texture_mode) return -1;
    if (action < 0) return 0;
    const bool taken = g.on_mouse_adjust_texture(wxMouseEvent(type, px, py, false, false, 0, action == 1));
    if (taken && action == 1 && g.m_auto_update) g.rebuild_preview();   // m_preview_params_dirty with Auto on
    if (taken) g.publish_adjust_overlay();
    if (taken && action == 2) Plater::get().changed();
    if (taken && action != 1) Host::event("gizmo_changed", json::object());   // the panel shows what the click did
    return taken ? 1 : 0;
}

static TextureProjectionMethod projection_of(const std::string &s)
{
    return s == "cylindrical" ? TextureProjectionMethod::Cylindrical : s == "spherical" ? TextureProjectionMethod::Spherical
         : s == "lscm" ? TextureProjectionMethod::LSCM : s == "view" ? TextureProjectionMethod::ViewProjected
         : s == "model_uv" ? TextureProjectionMethod::ModelUV : TextureProjectionMethod::Triplanar;
}

// the panel's controls on the active (or given) layer: the value, the preview rebuilt (m_preview_params_dirty)
static json td_layer_set(const json &a)
{
    GLGizmoTextureDisplacement &g = gizmo();
    ModelVolume *mv = g.texture_volume();
    if (mv == nullptr) return g.state();
    const int slot = a.value("slot", g.m_active_layer_slot);
    auto it = std::find_if(mv->texture_displacement_layers.begin(), mv->texture_displacement_layers.end(),
                           [slot](const TextureDisplacementLayer &l) { return l.slot == slot; });
    if (it == mv->texture_displacement_layers.end()) return g.state();
    TextureDisplacementLayer &l = *it;
    // ImGui's sliders clamp to their ranges (float_row / typed_slider in on_render_input_window)
    if (a.contains("depth")) l.depth_mm = std::clamp(a["depth"].get<float>(), 0.01f, 10.f);
    if (a.contains("tile_size")) l.tiling_scale = std::clamp(a["tile_size"].get<float>(), 0.2f, 200.f);
    if (a.contains("rotation")) l.rotation_deg = std::clamp(a["rotation"].get<float>(), 0.f, 360.f);
    if (a.contains("midlevel")) l.midlevel = std::clamp(a["midlevel"].get<float>(), 0.f, 10.f);
    if (a.contains("smoothing")) l.smoothing = std::clamp(a["smoothing"].get<float>(), 0.f, 1.f);
    if (a.contains("edge_smoothing")) l.edge_smoothing = a["edge_smoothing"];
    if (a.contains("edge_amount")) l.edge_smoothing_amount = std::clamp(a["edge_amount"].get<float>(), 0.02f, 1.f);
    if (a.contains("invert")) l.invert = a["invert"];
    if (a.contains("color")) l.color_enabled = a["color"];
    if (a.contains("tile")) l.tile_enabled = a["tile"];
    if (a.contains("tile_method")) l.tile_method = TextureTileMethod(std::clamp(a["tile_method"].get<int>(), 0, 1));
    if (a.contains("blend")) l.blend_mode = TextureBlendMode(std::clamp(a["blend"].get<int>(), 0, 4));
    if (a.contains("projection")) {
        const TextureProjectionMethod method = projection_of(a["projection"]);
        if (method == TextureProjectionMethod::ViewProjected && l.projection_method != method) g.capture_view_projection(l);
        if (method == TextureProjectionMethod::LSCM && l.projection_method != method) g.m_show_uv_editor = true;
        l.projection_method = method;
    }
    if (a.contains("expanded")) g.m_layer_expanded[size_t(slot)] = a["expanded"];
    if (!a.contains("expanded") || a.size() > 2) {
        Plater::get().changed();
        g.rebuild_preview();
    }
    return g.state();
}

static json td_options(const json &a)
{
    GLGizmoTextureDisplacement &g = gizmo();
    ModelVolume *mv = g.texture_volume();
    if (a.contains("pro")) {
        g.m_panel_mode = a["pro"].get<bool>() ? 1 : 0;
        g.store_panel_state();
        if (!g.pro_mode()) g.apply_standard_mode_presets(mv);
    }
    if (a.contains("erase")) {
        g.m_erase_mode = a["erase"];
        Painter::get().set({{"mode", g.m_erase_mode ? 1 : 0}});
    }
    if (a.contains("wireframe")) Painter::get().set_wireframe(a["wireframe"]);
    if (a.contains("auto")) {
        g.m_auto_update = a["auto"];
        if (g.m_auto_update) g.rebuild_preview();
    }
    if (mv != nullptr) {
        TextureDisplacementOptions &o = mv->texture_displacement_options;
        bool changed = false;
        auto set = [&](const char *k, auto &field) {
            if (a.contains(k)) { field = a[k].get<std::decay_t<decltype(field)>>(); changed = true; }
        };
        set("mix", o.color_mix_enabled);
        set("mix_count", o.color_mix_count);
        set("despeckle", o.color_despeckle);
        set("displace_border", o.displace_border);
        set("smooth", o.smooth_enabled);
        set("smooth_strength", o.smooth_strength);
        set("smooth_iterations", o.smooth_iterations);
        set("smooth_skip_border", o.smooth_skip_border);
        set("budget_k", o.v2_max_triangles_k);
        if (a.contains("classic")) { o.pipeline_v2 = !a["classic"].get<bool>(); changed = true; }
        if (changed) g.rebuild_preview();
    }
    return g.state();
}

// Pro mode's Subdivision and Remeshing: their settings, Preview subdivision, Subdivide, Remesh
static json td_prepare(const json &a)
{
    GLGizmoTextureDisplacement &g = gizmo();
    bool preview_live = false;
    auto set = [&](const char *k, auto &field) {
        if (a.contains(k)) { field = a[k].get<std::decay_t<decltype(field)>>(); preview_live = true; }
    };
    set("adaptive", g.m_subdivide_adaptive);
    set("feature", g.m_subdivide_feature);
    if (a.contains("target")) { g.m_subdivide_target_mm = std::clamp(a["target"].get<float>(), 0.001f, 20.f); preview_live = true; }
    if (a.contains("detail")) { g.m_subdivide_detail_mm = std::clamp(a["detail"].get<float>(), 0.001f, 1.f); preview_live = true; }
    if (a.contains("min_edge")) { g.m_subdivide_min_edge_mm = std::clamp(a["min_edge"].get<float>(), 0.001f, 20.f); preview_live = true; }
    if (a.contains("border")) { g.m_subdivide_border_mm = std::clamp(a["border"].get<float>(), 0.f, 5.f); preview_live = true; }
    if (a.contains("color")) { g.m_subdivide_color_mm = std::clamp(a["color"].get<float>(), 0.f, 5.f); preview_live = true; }
    if (a.contains("steps")) { g.m_subdivide_count = std::clamp(a["steps"].get<int>(), 0, 5); preview_live = true; }
    if (a.contains("remesh_edge")) g.m_remesh_target_edge_mm = std::clamp(a["remesh_edge"].get<float>(), 0.1f, 20.f);
    if (a.contains("keep_sharp")) g.m_remesh_keep_sharp_edges = a["keep_sharp"];
    if (a.contains("sharp_angle")) g.m_remesh_sharp_angle_deg = std::clamp(a["sharp_angle"].get<float>(), 5.f, 90.f);
    const std::string action = a.value("action", std::string());
    if (action == "preview") {
        if (g.m_subdivide_editing) {
            g.m_subdivide_editing      = false;
            g.m_subdivide_preview_tris = -1;
            g.m_subdivide_preview_glmodel.reset();
        } else {
            g.m_subdivide_editing = true;
            g.rebuild_subdivide_preview();
        }
    } else if (action == "subdivide") {
        if (g.m_subdivide_adaptive)
            g.subdivide_model_adaptive();
        else {
            g.subdivide_model();
            g.m_subdivide_count = 0;
        }
        if (g.m_subdivide_editing) g.rebuild_subdivide_preview();
    } else if (action == "remesh")
        g.remesh_model();
    else if (action == "smooth")
        g.smooth_model();
    else if (preview_live && g.m_subdivide_editing)
        g.rebuild_subdivide_preview();
    json st = g.state();
    st["project"] = Plater::get().state();
    return st;
}

// the Adjust image window: open on a layer, the sliders (the layer's preview_adjust, the model's preview follows), Reset,
// Cancel, Save as copy
static json td_adjust(const json &a)
{
    GLGizmoTextureDisplacement &g = gizmo();
    const std::string action = a.value("action", std::string());
    ModelVolume *mv = g.texture_volume();
    auto layer_of = [&](int slot) -> TextureDisplacementLayer * {
        if (mv != nullptr)
            for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
                if (l.slot == slot) return &l;
        return nullptr;
    };
    if (action == "open") {
        if (TextureDisplacementLayer *l = layer_of(a.at("slot"))) g.open_image_adjust(*l);
    } else if (action == "cancel")
        g.close_image_adjust();
    else if (action == "save") {
        if (TextureDisplacementLayer *l = layer_of(g.m_adjust_slot)) g.commit_image_adjust(*l);
    } else if (g.m_adjust_slot >= 0) {
        if (action == "reset") g.m_adjust = TextureAdjustments{};
        if (a.contains("brightness")) g.m_adjust.brightness = std::clamp(a["brightness"].get<float>(), -1.f, 1.f);
        if (a.contains("contrast")) g.m_adjust.contrast = std::clamp(a["contrast"].get<float>(), -1.f, 1.f);
        if (a.contains("sharpness")) g.m_adjust.sharpness = std::clamp(a["sharpness"].get<float>(), 0.f, 1.f);
        if (a.contains("blur")) g.m_adjust.blur = std::clamp(a["blur"].get<float>(), 0.f, 1.f);
        if (a.contains("hue")) g.m_adjust.hue_deg = std::clamp(a["hue"].get<float>(), -180.f, 180.f);
        if (TextureDisplacementLayer *l = layer_of(g.m_adjust_slot)) {
            l->preview_adjust = g.m_adjust;
            if (g.m_adjust != g.m_adjust_applied) {
                g.m_adjust_applied = g.m_adjust;
                g.invalidate_adjust_textures();
                g.rebuild_preview();
            }
        }
    }
    return g.state();
}

// the UV editor: open it, a pane command (the toolbar), a pointer event on the canvas (pane pixels), a key, the view
static json td_uv(const json &a)
{
    using namespace Slic3r::GUI::td;
    GLGizmoTextureDisplacement &g = gizmo();
    if (a.contains("w")) g_uv_canvas.set_size(a["w"], a["h"]);
    if (a.value("open", false)) {   // Open UV editor
        g.m_show_uv_editor = true;
        g.update_uv_editor();
    }
    if (a.contains("view")) {   // the View row's Normal / Fast / Checker / Distortion
        g.apply_view_mode(a["view"]);
        g.publish_view_meshes();
        if (g.m_use_shaded_preview) wx_call_after([] { orca::Painter::get().set_preview(-1, nullptr, {}); });
    }
    if (a.contains("cmd")) g_uv_canvas.run_command(UVEditorCanvas::Command(a["cmd"].get<int>()), a.value("value", 0.f));
    if (a.contains("event")) {
        const std::string e = a["event"];
        const int type = e == "down" ? wxEVT_LEFT_DOWN : e == "up" ? wxEVT_LEFT_UP : e == "rdown" ? wxEVT_RIGHT_DOWN : e == "rup" ? wxEVT_RIGHT_UP
                       : e == "mdown" ? wxEVT_MIDDLE_DOWN : e == "mup" ? wxEVT_MIDDLE_UP : e == "wheel" ? wxEVT_MOUSEWHEEL : wxEVT_MOTION;
        wxMouseEvent evt(type, a.value("x", 0.), a.value("y", 0.), a.value("shift", false), a.value("ctrl", false), a.value("wheel", 0.),
                         a.value("held", false));
        g_uv_canvas.on_mouse(evt);
        if (g.m_use_shaded_preview) g.publish_view_meshes();   // the dragged island's delta rides the shader (m_shaded_island_delta)
    }
    g.follow_islands();
    if (a.contains("key")) {
        wxKeyEvent evt(wxEVT_KEY_DOWN, a["key"].get<int>(), 0);
        g_uv_canvas.on_key(evt);
    }
    g.process_uv_commands();
    json st = g.state();
    if (g_uv_shown) st["uv_view"] = g_uv_canvas.export_view();
    return st;
}

static Commands reg({
    {"texdisp.uv", td_uv},
    {"texdisp.adjust", td_adjust},
    {"texdisp.prepare", td_prepare},
    {"texdisp.state", [](const json &) { return gizmo().state(); }},
    {"texdisp.add_layer", [](const json &) { gizmo().add_texture_layer(); return gizmo().state(); }},
    {"texdisp.remove_layer", [](const json &a) { gizmo().remove_texture_layer(a.at("slot")); return gizmo().state(); }},
    {"texdisp.move_layer", [](const json &a) { gizmo().move_texture_layer(a.at("slot"), a.at("to")); return gizmo().state(); }},
    {"texdisp.activate", [](const json &a) { gizmo().set_active_layer(a.at("slot")); return gizmo().state(); }},
    {"texdisp.set_texture", [](const json &a) {
         GLGizmoTextureDisplacement &g = gizmo();
         ModelVolume *mv = g.texture_volume();
         if (mv != nullptr)
             for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
                 if (l.slot == a.at("slot").get<int>()) {
                     const std::string path = a.at("path");
                     for (const GUI::TextureLibraryEntry &e : GUI::texture_library())
                         if (e.path == path) { g.set_layer_texture(l, e); break; }
                 }
         return g.state();
     }},
    {"texdisp.import_texture", [](const json &a) {
         GLGizmoTextureDisplacement &g = gizmo();
         if (ModelVolume *mv = g.texture_volume())
             for (TextureDisplacementLayer &l : mv->texture_displacement_layers)
                 if (l.slot == a.at("slot").get<int>()) g.import_custom_texture(l);
         return g.state();
     }},
    // render_texture_library_popup's remove button on one of My textures, confirmed first
    {"texdisp.remove_texture", [](const json &a) {
         GLGizmoTextureDisplacement &g = gizmo();
         const std::string path = a.at("path");
         for (const GUI::TextureLibraryEntry &e : GUI::texture_library())
             if (e.path == path && e.source == GUI::TextureSource::User) {
                 GUI::MessageDialog dlg(nullptr,
                                        wxString::Format(_L("Remove \"%s\" from My textures?\n\nThe image file is deleted from your "
                                                            "texture folder. Layers already using it keep it."),
                                                         GUI::from_u8(e.name)),
                                        _L("Remove texture"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
                 if (dlg.ShowModal() == wxID_YES) {
                     std::string error;
                     if (GUI::remove_user_texture(path, error))
                         g.m_library_textures.erase(path);
                     else
                         Slic3r::GUI::td::show_error(nullptr, error);
                 }
                 break;
             }
         return g.state();
     }},
    {"texdisp.layer", td_layer_set},
    // the projection frame: shown / its opacity / its rectangle (3D view pixels), Apply, Clear (Placed by projection frame)
    {"texdisp.projector", [](const json &a) {
         GLGizmoTextureDisplacement &g = gizmo();
         if (a.contains("open")) g.m_projector.shown = a["open"];
         if (a.contains("opacity")) g.m_projector_opacity = std::clamp(a["opacity"].get<int>(), 20, 255);
         if (a.contains("rect")) {
             const json &r = a["rect"];
             g.m_projector.rect = {int(r[0].get<double>()), int(r[1].get<double>()), int(r[2].get<double>()), int(r[3].get<double>())};
         }
         const std::string action = a.value("action", std::string());
         if (action == "apply") {
             const int painted = g.apply_projection_frame();
             if (painted == 0)
                 Slic3r::GUI::td::show_error(nullptr, _u8L("Nothing of the model is inside the frame - move it over the "
                                                           "part you want to project onto."));
             else if (painted < 0)
                 Slic3r::GUI::td::show_error(nullptr, _u8L("The frame could not be applied. Make sure it overlaps the 3D view."));
             Plater::get().changed();
         } else if (action == "clear") {
             if (TextureDisplacementLayer *l = g.active_layer()) {
                 l->view_project_projective = false;
                 l->tile_enabled            = true;
                 g.rebuild_preview();
                 Plater::get().changed();
             }
         }
         return g.state();
     }},
    // Adjust placement: on (anchored on the active layer's paint, which has to exist) / off
    {"texdisp.adjust_placement", [](const json &a) {
         GLGizmoTextureDisplacement &g = gizmo();
         if (a.value("on", false) && !g.m_adjust_texture_mode) {
             g.update_model_object();
             if (g.update_adjust_anchor())
                 g.m_adjust_texture_mode = true;
             else
                 Slic3r::GUI::td::show_error(nullptr, _u8L("Paint something with this layer first."));
         } else if (!a.value("on", false)) {
             g.m_adjust_texture_mode = false;
             g.m_adjust_drag_handle  = GLGizmoTextureDisplacement::AdjustHandle::None;
         }
         g.publish_adjust_overlay();
         return g.state();
     }},
    // From view: Capture current view (the projector from the camera, the visible faces painted when asked), Project
    // only on visible
    {"texdisp.capture_view", [](const json &a) {
         GLGizmoTextureDisplacement &g = gizmo();
         if (a.contains("only_visible")) g.m_project_only_visible = a["only_visible"];
         TextureDisplacementLayer *layer = g.active_layer();
         if (layer == nullptr) return g.state();
         if (!a.contains("only_visible")) g.capture_view_projection(*layer);
         if (g.m_project_only_visible && g.select_visible_faces() == 0)
             Slic3r::GUI::td::show_error(nullptr, _u8L("Nothing is visible from this angle - turn the model to face the "
                                                       "part you want to project onto."));
         Plater::get().changed();
         g.rebuild_preview();
         return g.state();
     }},
    {"texdisp.options", td_options},
    {"texdisp.select_all", [](const json &a) { gizmo().select_whole_model(a.value("paint", true)); return gizmo().state(); }},
    {"texdisp.bake", [](const json &) {
         GLGizmoTextureDisplacement &g = gizmo();
         if (g.pro_mode()) g.bake(); else g.bake_standard();
         json st = g.state();
         st["project"] = Plater::get().state();
         return st;
     }},
});
static PureCommands pure({{"texdisp.cancel", [](const json &) { Slic3r::GUI::td::g_cancel = true; return json(); }}});

} // namespace orca
