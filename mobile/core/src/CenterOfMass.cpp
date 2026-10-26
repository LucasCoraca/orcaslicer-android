#include "CenterOfMass.hpp"

#include <GLES3/gl3.h>
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <boost/format.hpp>

#include "Core.hpp"
#include "Gizmos.hpp"
#include "ImGuiOverlay.hpp"
#include "Painter.hpp"
#include "Plater.hpp"
#include "Preview.hpp"
#include "Shaders.hpp"
#include "Slicer.hpp"
#include "Viewport.hpp"
#include "imgui_internal.h"
#include "libslic3r/ConnectedBodies.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

#define glsafe(cmd) cmd

namespace orca {
namespace com {

using GUI::wxGetApp;

enum class ECanvasType { CanvasView3D, CanvasPreview, CanvasAssembleView };

// GLModel with its Geometry, enough for the sphere octants of the markers
class GLModel
{
public:
    struct Geometry
    {
        enum class EPrimitiveType { Triangles };
        enum class EVertexLayout { P3N3 };
        struct Format { EPrimitiveType type; EVertexLayout vertex_layout; };
        Format                    format{EPrimitiveType::Triangles, EVertexLayout::P3N3};
        std::vector<float>        vertices;
        std::vector<unsigned int> indices;
        void         reserve_vertices(size_t n) { vertices.reserve(n * 6); }
        void         reserve_indices(size_t n) { indices.reserve(n); }
        void         add_vertex(const Vec3f &p, const Vec3f &n) { vertices.insert(vertices.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z()}); }
        void         add_triangle(unsigned int a, unsigned int b, unsigned int c) { indices.insert(indices.end(), {a, b, c}); }
        size_t       vertices_count() const { return vertices.size() / 6; }
        size_t       indices_count() const { return indices.size(); }
        unsigned int extract_index(size_t i) const { return indices[i]; }
        Vec3f        extract_position_3(size_t id) const { return {vertices[id * 6], vertices[id * 6 + 1], vertices[id * 6 + 2]}; }
        Vec3f        extract_normal_3(size_t id) const { return {vertices[id * 6 + 3], vertices[id * 6 + 4], vertices[id * 6 + 5]}; }
    };
    bool is_initialized() const { return m_count > 0 && m_generation == Viewport::gl_generation(); }
    void init_from(Geometry &&g)
    {
        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);
        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, g.vertices.size() * sizeof(float), g.vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &m_ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, g.indices.size() * sizeof(unsigned int), g.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
        glBindVertexArray(0);
        m_count = GLsizei(g.indices.size());
        m_generation = Viewport::gl_generation();
    }
    void set_color(const ColorRGBA &c) { m_color = c; }
    // GLModel::render: the current shader with the model's colour
    void render()
    {
        if (!is_initialized()) return;
        GLint prog = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glUniform4f(glGetUniformLocation(GLuint(prog), "uniform_color"), m_color.r(), m_color.g(), m_color.b(), m_color.a());
        glBindVertexArray(m_vao);
        glDrawElements(GL_TRIANGLES, m_count, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }
private:
    GLuint    m_vao{0}, m_vbo{0}, m_ebo{0};
    GLsizei   m_count{0};
    unsigned  m_generation{0};   // the EGL context the names belong to
    ColorRGBA m_color;
};
#include "slic3r/GUI/SmoothSphere.inc"

// GLShaderProgram on the core's program cache
struct GLShaderProgram
{
    GLuint prog;
    void start_using() const { glUseProgram(prog); }
    void stop_using() const {}
    GLint loc(const char *n) const { return glGetUniformLocation(prog, n); }
    void set_uniform(const char *n, const Transform3d &m) const { Eigen::Matrix4f f = m.matrix().cast<float>(); glUniformMatrix4fv(loc(n), 1, GL_FALSE, f.data()); }
    void set_uniform(const char *n, const Matrix3d &m) const { Eigen::Matrix3f f = m.cast<float>(); glUniformMatrix3fv(loc(n), 1, GL_FALSE, f.data()); }
    void set_uniform(const char *n, float v) const { glUniform1f(loc(n), v); }
};
GLShaderProgram *com_shader(const char *name)
{
    static GLShaderProgram s;
    s.prog = gl::orca_program(name);
    return s.prog ? &s : nullptr;
}

// Camera of the plater, on the core's camera
struct Camera
{
    orca::Camera c;
    Transform3d get_view_matrix() const { return Transform3d(c.view()); }
    Transform3d get_projection_matrix() const
    {
        return Transform3d(c.projection(std::max(1.0, c.distance * 0.01), c.distance * 4 + 2000));
    }
    double get_inv_zoom() const { return Gizmos::get().inv_zoom(c); }
    Vec3d  get_dir_right() const { const Eigen::Matrix4d v = c.view(); return Vec3d(v(0, 0), v(0, 1), v(0, 2)); }
};
Camera g_camera;
const Camera &com_camera() { return g_camera; }

namespace CameraUtils {
// CameraUtils::project: screen points, top-left origin
Points project(const Camera &camera, const std::vector<Vec3d> &points)
{
    const Eigen::Matrix4d vp = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    Points out;
    for (const Vec3d &p : points) {
        const Eigen::Vector4d c = vp * Eigen::Vector4d(p.x(), p.y(), p.z(), 1.0);
        const double w = std::abs(c.w()) < 1e-12 ? 1.0 : c.w();
        out.emplace_back(coord_t((c.x() / w * 0.5 + 0.5) * camera.c.width), coord_t((0.5 - c.y() / w * 0.5) * camera.c.height));
    }
    return out;
}
Point project(const Camera &camera, const Vec3d &point) { return project(camera, std::vector<Vec3d>{point}).front(); }
} // namespace CameraUtils

// the canvas parts CenterOfMass reads
struct Transformation
{
    Transform3d m;
    const Transform3d &get_matrix() const { return m; }
};
struct GLVolume
{
    int            o, i, v;
    bool           is_active{true}, is_wipe_tower{false};
    Transformation inst, vol;
    BoundingBoxf3  hull_box;
    int            object_idx() const { return o; }
    int            instance_idx() const { return i; }
    int            volume_idx() const { return v; }
    const Transformation &get_instance_transformation() const { return inst; }
    const Transformation &get_volume_transformation() const { return vol; }
    Transform3d    world_matrix() const { return inst.m * vol.m; }
    BoundingBoxf3  transformed_convex_hull_bounding_box() const { return hull_box; }
};
struct GLVolumeCollection { std::vector<GLVolume *> volumes; };
struct GLGizmosManager
{
    enum EType { Undefined, Move, Rotate, Scale, Flatten, Other };
    EType current{Undefined};
    EType get_current_type() const { return current; }
};
struct GCodeViewer
{
    GCodeProcessorResult::ObjectMass              plate;
    std::vector<GCodeProcessorResult::ObjectMass> objects, bodies, supports;
    std::array<size_t, 2>                         range{0, 0};
    const std::array<size_t, 2> &get_layers_z_range() const { return range; }
    const GCodeProcessorResult::ObjectMass &get_plate_mass() const { return plate; }
    const std::vector<GCodeProcessorResult::ObjectMass> &get_object_masses() const { return objects; }
    const std::vector<GCodeProcessorResult::ObjectMass> &get_body_masses() const { return bodies; }
    const std::vector<GCodeProcessorResult::ObjectMass> &get_support_masses() const { return supports; }
};
// PartPlateList::find_instance
struct PlateLookup
{
    int find_instance(int obj, int inst) const { return orca::Plater::get().plate_of(obj, inst); }
};

class GLCanvas3D
{
public:
    ECanvasType        m_canvas_type{ECanvasType::CanvasView3D};
    bool               m_render_preview{true};
    bool               m_design_canvas{false};
    float              scale{1.f};
    Model             *model{nullptr};
    GLVolumeCollection volumes;
    GLGizmosManager    gizmos;
    GCodeViewer        viewer;

    float get_scale() const { return scale; }
    const Model *get_model() const { return model; }
    const GLVolumeCollection &get_volumes() const { return volumes; }
    bool is_dragging() const { return false; }
    const GLGizmosManager &get_gizmos_manager() const { return gizmos; }
    const GCodeViewer &get_gcode_viewer() const { return viewer; }
    ECanvasType get_canvas_type() const { return m_canvas_type; }
    void _set_overlay_as_dirty() { imgui_wrapper().set_requires_extra_frame(); }

#include "slic3r/GUI/CenterOfMassDecl.inc"
};

// GLCanvas3D.cpp's helpers of the markers
static constexpr std::array<double, 4> marker_radii{9., 7., 6., 5.};
#include "slic3r/GUI/CenterOfMassHelpers.inc"
#include "slic3r/GUI/CenterOfMassFns.inc"

// ---- the canvas of a frame
struct Scene
{
    GLCanvas3D                            canvas;
    GLCanvas3D::CenterOfMass              com;
    GLCanvas3D::CenterOfMass             &center_of_mass() { return com; }
    std::vector<std::unique_ptr<GLVolume>> owned;
    std::mutex                            mutex;   // GL thread (render) and UI thread (tap)
};
Scene &scene()
{
    static Scene s;
    return s;
}

// the volumes of the model (the parts and negative volumes CenterOfMass weighs) and, in Preview, the slice's masses
void fill(Scene &s, const orca::Camera &cam, bool preview)
{
    g_camera.c            = cam;
    GLCanvas3D &c         = s.canvas;
    c.m_canvas_type       = preview ? ECanvasType::CanvasPreview : ECanvasType::CanvasView3D;
    c.scale               = Gizmos::get().ui_scale();
    const std::string gz  = Gizmos::get().active();
    c.gizmos.current      = Painter::get().active() ? GLGizmosManager::Other :
                            gz.empty()               ? GLGizmosManager::Undefined :
                            gz == "move"             ? GLGizmosManager::Move :
                            gz == "rotate"           ? GLGizmosManager::Rotate :
                            gz == "scale"            ? GLGizmosManager::Scale :
                            gz == "flatten"          ? GLGizmosManager::Flatten : GLGizmosManager::Other;
    orca::Plater &p = orca::Plater::get();
    c.model         = &p.model();
    s.owned.clear();
    c.volumes.volumes.clear();
    for (int o = 0; o < int(c.model->objects.size()); ++o) {
        const ModelObject *mo = c.model->objects[o];
        for (int i = 0; i < int(mo->instances.size()); ++i)
            for (int v = 0; v < int(mo->volumes.size()); ++v) {
                auto gv = std::make_unique<GLVolume>();
                gv->o = o; gv->i = i; gv->v = v;
                gv->inst.m   = mo->instances[i]->get_matrix();
                gv->vol.m    = mo->volumes[v]->get_matrix();
                gv->hull_box = mo->volumes[v]->mesh().transformed_bounding_box(gv->world_matrix());
                c.volumes.volumes.push_back(gv.get());
                s.owned.push_back(std::move(gv));
            }
    }
    if (preview) {
        PlateSlice *ps = Slicer::get().slice(p.current_plate);
        if (ps && ps->valid) {
            std::lock_guard<std::mutex> l(Slicer::get().result_mutex);
            c.viewer.plate    = ps->result.plate_mass;
            c.viewer.objects  = ps->result.object_masses;
            c.viewer.bodies   = ps->result.body_masses;
            c.viewer.supports = ps->result.support_masses;
        } else
            c.viewer = GCodeViewer();
        c.viewer.range = Preview::get().layers_view_range();
    }
}

} // namespace com

CenterOfMass &CenterOfMass::get()
{
    static CenterOfMass c;
    return c;
}

void CenterOfMass::render(const Camera &cam, bool preview)
{
    if (!Core::get().app_config->get_bool("show_center_of_mass")) return;
    com::Scene &s = com::scene();
    std::unique_lock<std::recursive_mutex> core_lock(Core::get().mutex, std::try_to_lock);
    if (!core_lock.owns_lock()) return;
    std::lock_guard<std::mutex> l(s.mutex);
    com::fill(s, cam, preview);
    s.center_of_mass().render(s.canvas);
}

void CenterOfMass::render_details(const Camera &cam, bool preview)
{
    com::Scene &s = com::scene();
    std::lock_guard<std::mutex> l(s.mutex);
    com::g_camera.c = cam;
    s.canvas.m_canvas_type = preview ? com::ECanvasType::CanvasPreview : com::ECanvasType::CanvasView3D;
    s.center_of_mass().render_details(s.canvas);
}

bool CenterOfMass::tap(const Camera &cam, double x, double y, bool preview)
{
    if (!Core::get().app_config->get_bool("show_center_of_mass")) return false;
    com::Scene &s = com::scene();
    std::lock_guard<std::mutex> l(s.mutex);
    com::g_camera.c = cam;
    return s.center_of_mass().on_left_down(s.canvas, Vec2d(x, y));
}

} // namespace orca
