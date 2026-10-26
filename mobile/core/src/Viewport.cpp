#include "Viewport.hpp"
#include <atomic>
#include "ImexPlate.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "Benchmark.hpp"
#include "TextGizmo.hpp"
#include "SvgGizmo.hpp"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <cfloat>
#include <cmath>
#include <map>

#include "Core.hpp"
#include "Shaders.hpp"
#include "Gizmos.hpp"
#include "Painter.hpp"
#include "PrimeTower.hpp"
#include "Simplify.hpp"
#include "BrimEars.hpp"
#include "MeasureGizmo.hpp"
#include "LayersEditing.hpp"
#include "Plater.hpp"
#include "Preview.hpp"
#include "ImGuiOverlay.hpp"
#include "CenterOfMass.hpp"
#include "Realistic.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <fstream>

using namespace Slic3r;

namespace orca {
bool navigator_content(Camera &cam, bool dark, float scale, bool &ortho_request, bool &persp_request);
bool navigator_hit(float x, float y, int canvas_h, float scale);
void render_assemble_overlay(const Camera &cam, bool dark);

#define GLLOG(...) __android_log_print(ANDROID_LOG_WARN, "orcacore-gl", __VA_ARGS__)

// -------------------------------------------------------------------------------------------------------------------
// camera

static double rad(double d) { return d * M_PI / 180.; }

Vec3d Camera::eye() const
{
    double ce = std::cos(rad(elevation)), se = std::sin(rad(elevation));
    double ca = std::cos(rad(azimuth)), sa = std::sin(rad(azimuth));
    return target + distance * Vec3d(sa * ce, -ca * ce, se);
}

Eigen::Matrix4d Camera::view() const
{
    Vec3d e = eye(), f = (target - e).normalized();
    Vec3d up(0, 0, 1);
    if (std::abs(f.dot(up)) > 0.999) up = Vec3d(std::sin(rad(azimuth)), -std::cos(rad(azimuth)), 0) * (elevation > 0 ? 1 : -1) * -1;
    Vec3d s = f.cross(up).normalized(), u = s.cross(f);
    Eigen::Matrix4d m = Eigen::Matrix4d::Identity();
    m.block<1, 3>(0, 0) = s.transpose();
    m.block<1, 3>(1, 0) = u.transpose();
    m.block<1, 3>(2, 0) = -f.transpose();
    m(0, 3) = -s.dot(e); m(1, 3) = -u.dot(e); m(2, 3) = f.dot(e);
    return m;
}

Eigen::Matrix4d Camera::projection(double n, double f) const
{
    Eigen::Matrix4d p = Eigen::Matrix4d::Zero();
    double aspect = double(width) / std::max(1, height);
    if (ortho) {
        double h = distance * std::tan(rad(fov) / 2), w = h * aspect;
        p(0, 0) = 1 / w; p(1, 1) = 1 / h; p(2, 2) = -2 / (f - n); p(2, 3) = -(f + n) / (f - n); p(3, 3) = 1;
    } else {
        double t = 1. / std::tan(rad(fov) / 2);
        p(0, 0) = t / aspect; p(1, 1) = t; p(2, 2) = -(f + n) / (f - n); p(2, 3) = -2 * f * n / (f - n); p(3, 2) = -1;
    }
    return p;
}

void Camera::ray(double px, double py, Vec3d &origin, Vec3d &dir) const
{
    double aspect = double(width) / std::max(1, height);
    double nx = 2 * px / width - 1, ny = 1 - 2 * py / height;
    Eigen::Matrix4d vinv = view().inverse();
    if (ortho) {
        double h = distance * std::tan(rad(fov) / 2), w = h * aspect;
        origin = (vinv * Eigen::Vector4d(nx * w, ny * h, 0, 1)).head<3>();
        dir = (vinv.block<3, 3>(0, 0) * Vec3d(0, 0, -1)).normalized();
    } else {
        double t = std::tan(rad(fov) / 2);
        origin = eye();
        dir = (vinv.block<3, 3>(0, 0) * Vec3d(nx * t * aspect, ny * t, -1)).normalized();
    }
}

// -------------------------------------------------------------------------------------------------------------------
// GL helpers

namespace {

using namespace gl;

struct GpuMesh
{
    std::weak_ptr<const TriangleMesh> owner;
    GLuint vao{0}, vbo{0};
    GLsizei count{0};
};

GLuint g_lit = 0, g_flat = 0, g_tex = 0;
GLuint g_lines_vao = 0, g_lines_vbo = 0;
std::map<const TriangleMesh *, GpuMesh> g_meshes;
std::map<std::string, GLuint> g_icon_textures;

void upload_mesh(const std::shared_ptr<const TriangleMesh> &mesh)
{
    auto it = g_meshes.find(mesh.get());
    if (it != g_meshes.end() && !it->second.owner.expired()) return;
    if (it != g_meshes.end()) { glDeleteBuffers(1, &it->second.vbo); glDeleteVertexArrays(1, &it->second.vao); g_meshes.erase(it); }
    const indexed_triangle_set &its = mesh->its;
    std::vector<float> data;
    data.reserve(its.indices.size() * 18);
    for (const Vec3i32 &f : its.indices) {
        const Vec3f &a = its.vertices[f[0]], &b = its.vertices[f[1]], &c = its.vertices[f[2]];
        Vec3f n = (b - a).cross(c - a);
        float l = n.norm();
        if (l > 0) n /= l;
        for (const Vec3f *v : {&a, &b, &c}) { data.insert(data.end(), {v->x(), v->y(), v->z(), n.x(), n.y(), n.z()}); }
    }
    GpuMesh g;
    g.owner = mesh;
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);
    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
    glBindVertexArray(0);
    g.count = GLsizei(data.size() / 6);
    g_meshes[mesh.get()] = g;
}

void purge_meshes()
{
    for (auto it = g_meshes.begin(); it != g_meshes.end();) {
        if (it->second.owner.expired()) {
            glDeleteBuffers(1, &it->second.vbo);
            glDeleteVertexArrays(1, &it->second.vao);
            it = g_meshes.erase(it);
        } else
            ++it;
    }
}

// lines / triangles in world space with the flat shader
void draw_flat(const std::vector<float> &xyz, GLenum mode, const Eigen::Matrix4f &mvp, const ColorRGBA &c, float width = 1.f)
{
    if (xyz.empty()) return;
    glUseProgram(g_flat);
    glUniformMatrix4fv(glGetUniformLocation(g_flat, "mvp"), 1, GL_FALSE, mvp.data());
    glUniform4f(glGetUniformLocation(g_flat, "uniform_color"), c.r(), c.g(), c.b(), c.a());
    glBindVertexArray(g_lines_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_lines_vbo);
    glBufferData(GL_ARRAY_BUFFER, xyz.size() * sizeof(float), xyz.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *) 0);
    glLineWidth(width);
    glDrawArrays(mode, 0, GLsizei(xyz.size() / 3));
    glBindVertexArray(0);
}

} // namespace

void gl_draw_flat(const std::vector<float> &xyz, unsigned mode, const Eigen::Matrix4f &mvp, const ColorRGBA &c, float width)
{
    draw_flat(xyz, GLenum(mode), mvp, c, width);
}

namespace {

bool dark_mode() { return Core::get().app_config && Core::get().app_config->get("dark_color_mode") == "1"; }

} // namespace

// -------------------------------------------------------------------------------------------------------------------

Viewport &Viewport::get()
{
    static Viewport v;
    return v;
}

void plugins_gl_created();   // PluginsUi.cpp: host.gl_info()

static std::atomic<unsigned> g_gl_generation{0};
unsigned Viewport::gl_generation() { return g_gl_generation.load(); }

void Viewport::gl_created()
{
    ++g_gl_generation;
    plugins_gl_created();
    // a new EGL context: every GL name is gone
    g_meshes.clear();
    Painter::get().gl_created();
    LayersEditing::get().gl_created();
    Realistic::get().gl_created();
    forget_programs();
    g_lit = program(LIT_VS, LIT_FS);
    g_flat = program(FLAT_VS, FLAT_FS);
    g_tex = program(TEX_VS, TEX_FS);
    m_bed_key.clear();
    m_bed_texture = 0;
    g_icon_textures.clear();
    glGenVertexArrays(1, &g_lines_vao);
    glGenBuffers(1, &g_lines_vbo);
    m_gl_ready = true;
    m_dirty = true;
    if (Core::get().app_config) { std::lock_guard<std::mutex> l(m_cam_mutex); m_cam.ortho = !Core::get().app_config->get_bool("use_perspective_camera"); }
    Preview::get().gl_created();
}

void Viewport::resize(int w, int h)
{
    std::lock_guard<std::mutex> l(m_cam_mutex);
    m_cam.width = std::max(1, w);
    m_cam.height = std::max(1, h);
    glViewport(0, 0, w, h);
    m_dirty = true;
}

static ColorRGBA color_from_model_volume(const ModelVolume &v);
void render_mesh_boolean(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);   // MeshBooleanGizmo.cpp

// copy what the frame needs out of the model, under the core lock (skipped while the engine is busy)
void Viewport::snapshot()
{
    Core &core = Core::get();
    std::unique_lock<std::recursive_mutex> lock(core.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    Plater &p = Plater::get();
    {   // the Text / SVG gizmo's rotation ring follows its volume (also when the gizmo opens on an unchanged model)
        Transform3d frame;
        double radius = 0;
        const bool ring = text_gizmo_ring(frame, radius) || svg_gizmo_ring(frame, radius);
        Gizmos::get().set_emboss_ring(ring, frame, radius);
    }
    if (p.revision == m_revision) return;
    m_revision = p.revision.load();
    const Model &m = p.model();
    std::vector<ColorRGBA> colors;
    if (auto *fc = core.full_config().option<ConfigOptionStrings>("filament_colour"))
        for (auto &s : fc->values) { ColorRGBA c; colors.push_back(decode_color(s, c) ? c : ColorRGBA(0.8f, 0.8f, 0.8f, 1.f)); }
    if (colors.empty()) colors.push_back(ColorRGBA(0.8f, 0.8f, 0.8f, 1.f));   // GLVolume::NEUTRAL_COLOR
    std::vector<RenderVolume> out;
    const bool assemble = p.assemble_view;
    for (int oi = 0; oi < int(m.objects.size()); ++oi) {
        const ModelObject *mo = m.objects[oi];
        int obj_extruder = mo->config.has("extruder") ? mo->config.opt_int("extruder") : 0;
        for (int ii = 0; ii < int(mo->instances.size()); ++ii) {
            const ModelInstance *mi = mo->instances[ii];
            const double inst_radius = assemble ? 0. : mo->instance_bounding_box(ii).radius();
            int plate = p.plate_of(oi, ii);
            bool outside = !assemble && plate >= 0 && p.plates[plate].outside.count({oi, ii}) > 0;
            bool inst_selected = p.selection.count({oi, -1, -1}) || p.selection.count({oi, ii, -1});
            for (int vi = 0; vi < int(mo->volumes.size()); ++vi) {
                const ModelVolume *mv = mo->volumes[vi];
                if (assemble && !mv->is_model_part()) continue;   // GLCanvas3D::reload_scene: the Assembly view shows the parts only
                RenderVolume r;
                r.mesh = mv->mesh_ptr();
                if (assemble) {
                    // GLVolume::world_matrix with the explosion ratio (load_object_volume in_assemble_view)
                    r.matrix = mi->get_assemble_transformation().get_matrix() * mv->get_matrix();
                    const double ratio = p.explosion_ratio;
                    r.matrix.translate(mi->get_offset_to_assembly() * (ratio - 1.0) + mv->get_offset() * (ratio - 1.0));
                } else
                    r.matrix = mi->get_matrix() * mv->get_matrix();
                r.obj = oi; r.inst = ii; r.vol = vi;
                r.selected = inst_selected || p.selection.count({oi, ii, vi}) || p.selection.count({oi, -1, vi});
                r.outside = outside;
                r.model_part = mv->is_model_part();
                r.bbox = r.mesh->transformed_bounding_box(r.matrix);
                r.printable = mi->printable;
                r.inst_offset = mi->get_offset();
                r.inst_radius = inst_radius;
                ColorRGBA c = color_from_model_volume(*mv);
                if (mv->is_model_part()) {   // GLVolumeCollection::update_colors_by_extruder
                    int e = mv->extruder_id() - 1;
                    c = colors[(e < 0 || e >= int(colors.size())) ? 0 : e];
                }
                r.color[0] = c.r(); r.color[1] = c.g(); r.color[2] = c.b(); r.color[3] = c.a();
                // GLCanvas3D::_render_objects: the build volume, grown by SceneEpsilon, for the partly outside ones
                BoundingBoxf3 pb = p.plate_box(plate >= 0 ? plate : p.current_plate);
                r.print_volume[0] = float(pb.min.x() - BuildVolume::SceneEpsilon); r.print_volume[1] = float(pb.min.y() - BuildVolume::SceneEpsilon);
                r.print_volume[2] = float(pb.max.x() + BuildVolume::SceneEpsilon); r.print_volume[3] = float(pb.max.y() + BuildVolume::SceneEpsilon);
                r.print_volume[4] = 0.f; r.print_volume[5] = float(p.printable_height());
                out.push_back(std::move(r));
            }
        }
    }
    // GLCanvas3D::reload_scene: the plates' prime towers (GLWipeTowerVolume, one volume per coloured part)
    if (!assemble)
    for (const TowerVolume &t : wipe_tower_volumes())
        for (size_t k = 0; k < t.parts.size(); ++k) {
            RenderVolume r;
            r.mesh = t.parts[k].second;
            r.matrix = t.matrix;
            r.obj = 1000 + t.plate; r.inst = 0; r.vol = int(k);
            r.wipe_tower = true;
            r.selected = r.outside = r.model_part = false;
            r.printable = true;
            r.bbox = t.bbox;
            const ColorRGBA &c = t.parts[k].first;
            r.color[0] = c.r(); r.color[1] = c.g(); r.color[2] = c.b(); r.color[3] = c.a();
            std::fill(std::begin(r.print_volume), std::end(r.print_volume), 0.f);
            out.push_back(std::move(r));
        }
    Gizmos::get().update_planes();
    std::lock_guard<std::mutex> l(m_scene_mutex);
    m_volumes = std::move(out);
    m_volumes_revision = m_revision;
}

// GLVolume colours (3DScene.cpp)
static const ColorRGBA MODEL_MIDIFIER_COL   = {1.0f, 1.0f, 0.0f, 0.6f};
static const ColorRGBA MODEL_NEGTIVE_COL    = {0.3f, 0.3f, 0.3f, 0.4f};
static const ColorRGBA SUPPORT_ENFORCER_COL = {0.3f, 0.3f, 1.0f, 0.4f};
static const ColorRGBA SUPPORT_BLOCKER_COL  = {1.0f, 0.3f, 0.3f, 0.4f};
static const ColorRGBA UNPRINTABLE_COLOR    = {0.0f, 0.0f, 0.0f, 0.5f};

// color_from_model_volume (ENABLE_MODIFIERS_ALWAYS_TRANSPARENT)
static ColorRGBA color_from_model_volume(const ModelVolume &v)
{
    if (v.is_negative_volume()) return MODEL_NEGTIVE_COL;
    if (v.is_precise_seam()) {
        switch (v.type()) {
        case ModelVolumeType::PRECISE_SEAM_CENTER:   return {1.0f, 0.627f, 0.082f, 0.6f};
        case ModelVolumeType::PRECISE_SEAM_LEFT:     return {1.0f, 0.753f, 0.0f, 0.6f};
        case ModelVolumeType::PRECISE_SEAM_RIGHT:    return {1.0f, 0.514f, 0.0f, 0.6f};
        case ModelVolumeType::PRECISE_SEAM_ENFORCED: return {0.412f, 0.820f, 0.412f, 0.6f};
        case ModelVolumeType::PRECISE_SEAM_NEUTRAL:  return {0.655f, 0.655f, 0.655f, 0.6f};
        case ModelVolumeType::PRECISE_SEAM_BLOCKED:  return {0.820f, 0.412f, 0.412f, 0.6f};
        default: return MODEL_MIDIFIER_COL;
        }
    }
    if (v.is_modifier()) return MODEL_MIDIFIER_COL;
    if (v.is_support_blocker()) return SUPPORT_BLOCKER_COL;
    if (v.is_support_enforcer()) return SUPPORT_ENFORCER_COL;
    return {};
}

// GLVolume::brighten_color: HSL lightness + 0.25
static ColorRGBA brighten_color(const ColorRGBA &color)
{
    float r = color.r(), g = color.g(), b = color.b();
    float max_val = std::max({r, g, b}), min_val = std::min({r, g, b});
    float l = (max_val + min_val) / 2.0f, h = 0.0f, s = 0.0f;
    if (max_val != min_val) {
        float delta = max_val - min_val;
        s = l > 0.5f ? delta / (2.0f - max_val - min_val) : delta / (max_val + min_val);
        if (max_val == r) h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
        else if (max_val == g) h = (b - r) / delta + 2.0f;
        else h = (r - g) / delta + 4.0f;
        h /= 6.0f;
    }
    l = std::min(l + 0.25f, 1.0f);
    auto hue_to_rgb = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };
    if (s == 0.0f) r = g = b = l;
    else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s, p = 2.0f * l - q;
        r = hue_to_rgb(p, q, h + 1.0f / 3.0f); g = hue_to_rgb(p, q, h); b = hue_to_rgb(p, q, h - 1.0f / 3.0f);
    }
    return ColorRGBA(r, g, b, color.a());
}

// GLVolume::set_render_color (BBS: no outside colour, selection brightened, unprintable dark)
static ColorRGBA render_color(const RenderVolume &r)
{
    const float FULL_BLACK_THRESHOLD = 0.18f, FullyTransparentMaterialThreshold = 0.1f, FullTransparentModdifiedToFixAlpha = 0.3f;
    ColorRGBA base(r.color[0], r.color[1], r.color[2], r.color[3]);
    // adjust_color_for_rendering
    if (base.a() < FullyTransparentMaterialThreshold) base = {1, 1, 1, FullTransparentModdifiedToFixAlpha};
    else if (base.r() < FULL_BLACK_THRESHOLD && base.g() < FULL_BLACK_THRESHOLD && base.b() < FULL_BLACK_THRESHOLD)
        base = {FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, base.a()};
    ColorRGBA c = r.selected ? brighten_color(base) : base;
    if (!r.printable) c = r.selected ? brighten_color(UNPRINTABLE_COLOR) : UNPRINTABLE_COLOR;
    return c;
}

// one lit mesh (gouraud_light), used for the bed model and the objects
static void draw_lit_mesh(const std::shared_ptr<const TriangleMesh> &mesh, const Eigen::Matrix4f &model, const float color[4],
                          const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    upload_mesh(mesh);
    auto it = g_meshes.find(mesh.get());
    if (it == g_meshes.end()) return;
    glUseProgram(g_lit);
    Eigen::Matrix4f vm = view * model;
    Eigen::Matrix3f nm = vm.block<3, 3>(0, 0).inverse().transpose();
    glUniformMatrix4fv(glGetUniformLocation(g_lit, "projection_matrix"), 1, GL_FALSE, proj.data());
    glUniformMatrix4fv(glGetUniformLocation(g_lit, "view_model_matrix"), 1, GL_FALSE, vm.data());
    glUniformMatrix4fv(glGetUniformLocation(g_lit, "model_matrix"), 1, GL_FALSE, model.data());
    glUniformMatrix3fv(glGetUniformLocation(g_lit, "view_normal_matrix"), 1, GL_FALSE, nm.data());
    glUniform1f(glGetUniformLocation(g_lit, "sinking_z"), -1e9f);
    glUniform4fv(glGetUniformLocation(g_lit, "uniform_color"), 1, color);
    glBindVertexArray(it->second.vao);
    glDrawArrays(GL_TRIANGLES, 0, it->second.count);
    glBindVertexArray(0);
}

void gl_draw_lit(const std::shared_ptr<const TriangleMesh> &mesh, const Eigen::Matrix4f &model, const float color[4],
                 const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    draw_lit_mesh(mesh, model, color, view, proj);
}

// RGBA pixels of the bed texture: SVG through nanosvg, PNG through libslic3r (Bed3D / PartPlateList logo texture)
static bool load_texture_pixels(const std::string &path, std::vector<unsigned char> &rgba, int &w, int &h)
{
    if (boost::algorithm::iends_with(path, ".svg")) {
        NSVGimage *img = nsvgParseFromFile(path.c_str(), "px", 96.0f);
        if (!img) return false;
        float scale = 2048.f / std::max(img->width, img->height);
        w = std::max(1, int(img->width * scale)); h = std::max(1, int(img->height * scale));
        rgba.assign(size_t(w) * h * 4, 0);
        NSVGrasterizer *r = nsvgCreateRasterizer();
        nsvgRasterize(r, img, 0, 0, scale, rgba.data(), w, h, w * 4);
        nsvgDeleteRasterizer(r);
        nsvgDelete(img);
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    png::ReadBuf rb{data.data(), data.size()};
    png::ImageColorscale img;
    png::ReadBufStream stream(rb);
    if (data.empty() || !png::decode_colored_png(stream, img)) return false;
    w = int(img.cols); h = int(img.rows);
    rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 4; ++c)
            rgba[i * 4 + c] = c < img.bytes_per_pixel ? img.buf[i * img.bytes_per_pixel + c] : (c == 3 ? 255 : img.buf[i * img.bytes_per_pixel]);
    return true;
}

// the IDEX/IQEX overlays of the plates, rebuilt when the project or the printer's / process' IDEX settings change
// (PartPlate::build_imex_cache_key); under the core lock
void Viewport::update_imex()
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    const DynamicPrintConfig &pcfg = core.preset_bundle->printers.get_edited_preset().config;
    std::string key = std::to_string(p.revision.load());
    for (const std::string &k : pcfg.keys())
        if (k.rfind("imex_", 0) == 0 || k == "is_imex" || k == "printable_area") key += "|" + pcfg.opt_serialize(k);
    if (auto *pm = core.preset_bundle->prints.get_edited_preset().config.option<ConfigOptionString>("imex_parallel_mode")) key += "|" + pm->value;
    if (auto *fc = core.preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")) key += "|" + fc->serialize();
    for (const Plate &pl : p.plates)
        if (auto *m = pl.config.option<ConfigOptionString>("imex_parallel_mode")) key += "|" + m->value;
    if (key == m_imex_key) return;
    m_imex_key = key;
    std::vector<ImexOverlay> imex;
    std::vector<std::string> kinds;
    for (int i = 0; i < int(p.plates.size()); ++i) {
        imex.push_back(imex_overlay(i));
        // the icon shows the kind of mode the plate slices as (imex_mode_kind of its tools)
        std::string kind = "normal";
        if (!imex.back().choices.empty()) {
            const ImexMode m = find_imex_mode(pcfg, imex.back().mode);
            if (m.found() && imex.back().mode != kImexPrimaryMode)
                switch (imex_mode_kind(m.active_tools)) {
                case ImexModeKind::Copy: kind = "copy"; break;
                case ImexModeKind::Mirror: kind = "mirror"; break;
                case ImexModeKind::Custom: kind = "custom"; break;
                default: break;
                }
        }
        kinds.push_back(kind);
    }
    std::vector<ImexMarkers> markers;
    for (int i = 0; i < int(p.plates.size()); ++i) markers.push_back(imex_markers(i));
    std::lock_guard<std::mutex> il(m_imex_mutex);
    m_imex = std::move(imex);
    m_imex_kind = std::move(kinds);
    m_imex_markers = std::move(markers);
}

// the ghosts: translucent, after the scene, not writing depth
void Viewport::draw_imex_ghosts(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    std::lock_guard<std::mutex> il(m_imex_mutex);
    bool any = false;
    for (const ImexOverlay &o : m_imex) any |= !o.ghosts.empty();
    if (!any) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (size_t i = 0; i < m_imex.size(); ++i) {
        if (m_focus_mode && !m_preview && int(i) != Plater::get().current_plate) continue;
        for (const ImexOverlay::Ghost &g : m_imex[i].ghosts) {
            const float col[4] = {g.color.r(), g.color.g(), g.color.b(), g.color.a()};
            draw_lit_mesh(g.mesh, g.xf.matrix().cast<float>(), col, view, proj);
        }
    }
    glDepthMask(GL_TRUE);
}

// Plater::set_bed_shape / Bed3D::detect_type: the printer's bed model and texture (custom ones first)
void Viewport::update_bed_assets()
{
    Core &core = Core::get();
    const Preset &curr = core.preset_bundle->printers.get_selected_preset();
    {   // GCodeViewer::init: the tool marker is the printer's hotend model (Orca's default printer's without one)
        std::string file;
        if (curr.is_system) file = PresetUtils::system_printer_hotend_model(curr);
        else if (auto *pm = curr.config.opt<ConfigOptionString>("printer_model"); pm && !pm->value.empty())
            file = core.preset_bundle->get_hotend_model_for_printer_model(pm->value);
        if (file.empty()) file = core.preset_bundle->get_hotend_model_for_printer_model(PresetBundle::ORCA_DEFAULT_PRINTER_MODEL);
        if (file != m_marker_file) {
            m_marker_file = file;
            auto mesh = std::make_shared<TriangleMesh>();
            if (file.empty() || !mesh->ReadSTLFile(file.c_str()))
                // Marker::init without a file: stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f), the tip down at the origin
                *mesh = TriangleMesh(its_make_cylinder(0.8, 3.0, 2. * PI / 16.));
            std::lock_guard<std::mutex> il(m_imex_mutex);
            m_marker_mesh = std::move(mesh);
        }
    }
    const DynamicPrintConfig &cfg = core.preset_bundle->printers.get_edited_preset().config;
    std::string texture = cfg.opt_string("bed_custom_texture"), model = cfg.opt_string("bed_custom_model");
    std::string sys_texture, sys_model;
    if (curr.is_system) { sys_texture = PresetUtils::system_printer_bed_texture(curr); }
    else if (auto *pm = cfg.opt<ConfigOptionString>("printer_model"); pm && !pm->value.empty()) sys_texture = core.preset_bundle->get_texture_for_printer_model(pm->value);
    for (const Preset *p = &curr; p != nullptr && sys_model.empty(); p = core.preset_bundle->printers.get_preset_parent(*p)) {
        if (p->is_system) sys_model = PresetUtils::system_printer_bed_model(*p);
        else if (auto *pm = p->config.opt<ConfigOptionString>("printer_model"); pm && !pm->value.empty())
            sys_model = core.preset_bundle->get_stl_model_for_printer_model(pm->value);
    }
    if (texture.empty()) texture = sys_texture;
    if (model.empty()) model = sys_model;
    std::string key = texture + "|" + model;
    if (key == m_bed_key) return;
    m_bed_key = key;
    m_bed_model.reset();
    if (!model.empty() && boost::filesystem::exists(model)) {
        auto mesh = std::make_shared<TriangleMesh>();
        if (mesh->ReadSTLFile(model.c_str(), true)) m_bed_model = mesh;
        // Bed3D::update_model_offset: "hack for current stl for BBL printer"
        m_bed_model_bbl = model.find("bbl-3dp-") != std::string::npos;
    }
    if (m_bed_texture) { glDeleteTextures(1, &m_bed_texture); m_bed_texture = 0; }
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (!texture.empty() && boost::filesystem::exists(texture) && load_texture_pixels(texture, px, w, h)) {
        glGenTextures(1, &m_bed_texture);
        glBindTexture(GL_TEXTURE_2D, m_bed_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

// PartPlate icon textures (resources/images/<name>.svg), by file name: g_icon_textures

static GLuint icon_texture(const std::string &name);
unsigned gl_icon_texture(const std::string &name) { return icon_texture(name); }

static GLuint icon_texture(const std::string &name)
{
    auto it = g_icon_textures.find(name);
    if (it != g_icon_textures.end()) return it->second;
    GLuint tex = 0;
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    std::string path = resources_dir() + "/images/" + name + ".svg";
    if (load_texture_pixels(path, px, w, h)) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    g_icon_textures[name] = tex;
    return tex;
}

static void draw_textured_quad(GLuint tex, float x0, float y0, float x1, float y1, float z, const Eigen::Matrix4f &vp)
{
    if (!tex) return;
    float q[] = {x0, y0, z, 0, 1, 0, x1, y0, z, 1, 1, 0, x1, y1, z, 1, 0, 0, x0, y0, z, 0, 1, 0, x1, y1, z, 1, 0, 0, x0, y1, z, 0, 0, 0};
    glUseProgram(g_tex);
    glUniformMatrix4fv(glGetUniformLocation(g_tex, "mvp"), 1, GL_FALSE, vp.data());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(g_tex, "tex"), 0);
    glBindVertexArray(g_lines_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_lines_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(1);
    glBindVertexArray(0);
}

// PartPlate.cpp: icon size and gaps (mm at a 200 mm deep bed), scaled by PartPlate::icon_scale_factor
static const float PARTPLATE_ICON_SIZE = 16, PARTPLATE_ICON_GAP_TOP = 3, PARTPLATE_ICON_GAP_LEFT = 3, PARTPLATE_ICON_GAP_Y = 5;
static float icon_scale_factor(const BoundingBoxf &bed)
{
    return float(std::min(bed.size().y() / 200., bed.size().x() * (1. / 5.) / (PARTPLATE_ICON_SIZE + 2 * PARTPLATE_ICON_GAP_LEFT)));
}

// PartPlate::calc_vertex_for_icons: slot index -> rectangle right of the plate, from the top down
static BoundingBoxf icon_rect(const BoundingBoxf &bed, int index)
{
    float f = icon_scale_factor(bed), size = PARTPLATE_ICON_SIZE * f;
    Vec2d p(bed.max.x() + PARTPLATE_ICON_GAP_LEFT * f, bed.max.y() - (index * (size + PARTPLATE_ICON_GAP_Y * f) + PARTPLATE_ICON_GAP_TOP * f));
    return BoundingBoxf(Vec2d(p.x(), p.y() - size), Vec2d(p.x() + size, p.y()));
}

// the plate's tool icons, in PartPlate's slots (0 delete, 1 orient, 2 arrange, 3 lock, 4 settings, 5 move to front)
static const char *PLATE_ICONS[] = {"plate_close", "plate_orient", "plate_arrange", "plate_lock", "plate_settings", "plate_move_front"};

// Bed_2D::calculate_grid_step / generate_grid: lines every step from the origin, every 5th bold, clipped to the bed
static void grid_lines(const ExPolygon &poly, const BoundingBox &bb, Polylines &thin, Polylines &bold)
{
    int min_edge = int(std::min(unscale<double>(bb.size().x()), unscale<double>(bb.size().y())));
    double step = scale_(min_edge >= 6000 ? 100 : min_edge >= 1200 ? 50 : min_edge >= 600 ? 20 : 10);
    int count = 0;
    for (coord_t x = 0; x >= bb.min(0); x -= coord_t(step), ++count) (count % 5 ? thin : bold).push_back(Polyline(Point(x, bb.min(1)), Point(x, bb.max(1))));
    count = 0;
    for (coord_t x = 0; x <= bb.max(0); x += coord_t(step), ++count) (count % 5 ? thin : bold).push_back(Polyline(Point(x, bb.min(1)), Point(x, bb.max(1))));
    count = 0;
    for (coord_t y = 0; y >= bb.min(1); y -= coord_t(step), ++count) (count % 5 ? thin : bold).push_back(Polyline(Point(bb.min(0), y), Point(bb.max(0), y)));
    count = 0;
    for (coord_t y = 0; y <= bb.max(1); y += coord_t(step), ++count) (count % 5 ? thin : bold).push_back(Polyline(Point(bb.min(0), y), Point(bb.max(0), y)));
    thin = intersection_pl(thin, offset(poly, float(SCALED_EPSILON)));
    bold = intersection_pl(bold, offset(poly, float(SCALED_EPSILON)));
}

void Viewport::draw_bed(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    if (Plater::get().assemble_view) return;   // GLCanvas3D::_render: the Assembly view has no bed (_render_plane is empty)
    const Eigen::Matrix4f vp = proj * view;
    Pointfs shape, exclude;
    std::vector<Vec3d> origins;
    int current = 0;
    {
        std::unique_lock<std::recursive_mutex> lock(Core::get().mutex, std::try_to_lock);
        static Pointfs s_shape, s_exclude; static std::vector<Vec3d> s_origins; static int s_current = 0;
        if (lock.owns_lock()) {
            Plater &p = Plater::get();
            // GLCanvas3D::bed_shape_changed: another bed is zoomed to on the next frame (camera.requires_zoom_to_bed)
            Pointfs new_shape = p.bed_shape();
            if (!s_shape.empty() && new_shape != s_shape) m_zoom_to_bed = true;
            s_shape = std::move(new_shape);
            s_origins.clear();
            for (size_t i = 0; i < p.plates.size(); ++i) s_origins.push_back(p.plate_origin(int(i)));
            s_current = p.current_plate;
            m_plate_locked.clear(); m_plate_has_settings.clear();
            for (auto &pl : p.plates) {
                m_plate_locked.push_back(pl.locked);
                // PartPlate::render_icons' has_plate_settings: bed type, print sequence, layer sequences, spiral mode
                // (the IDEX/IQEX mode has its own icon)
                auto opt_set = [&](const char *k) { return pl.config.has(k); };
                const bool bed = pl.config.has("curr_bed_type") && pl.config.opt_enum<BedType>("curr_bed_type") != btDefault;
                const bool seq = pl.config.has("print_sequence") && pl.config.opt_enum<PrintSequence>("print_sequence") != PrintSequence::ByDefault;
                const auto *l1 = pl.config.option<ConfigOptionInts>("first_layer_print_sequence");
                const bool other = pl.config.has("other_layers_print_sequence") && pl.config.has("other_layers_print_sequence_nums") &&
                                   !get_other_layers_print_sequence(pl.config.opt_int("other_layers_print_sequence_nums"),
                                                                    pl.config.option<ConfigOptionInts>("other_layers_print_sequence")->values).empty();
                m_plate_has_settings.push_back(bed || seq || (l1 && !l1->values.empty()) || other || opt_set("spiral_mode"));
            }
            s_exclude.clear();
            if (auto *ex = Core::get().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("bed_exclude_area"); ex && ex->values.size() >= 3)
                s_exclude = ex->values;
            update_bed_assets();
            update_imex();
        } else
            m_bed_stale = true;   // the engine is busy: this frame shows the last bed, another one follows
        shape = s_shape; origins = s_origins; current = s_current; exclude = s_exclude;
    }
    if (shape.size() < 3) return;
    const BoundingBoxf box(shape);
    if (current < int(origins.size()))
        m_current_plate_box = BoundingBoxf3(Vec3d(box.min.x() + origins[current].x(), box.min.y() + origins[current].y(), -1e10),
                                            Vec3d(box.max.x() + origins[current].x(), box.max.y() + origins[current].y(), 1e10));
    const bool dark = dark_mode();
    // Bed3D::DEFAULT_MODEL_COLOR(_DARK); the flat fill is 0.8x darker (Bed3D::render_default)
    const float model_col[4] = {dark ? 0.255f : 0.3255f, dark ? 0.255f : 0.337f, dark ? 0.283f : 0.337f, 1.f};
    const ColorRGBA fill_col(model_col[0] * 0.8f, model_col[1] * 0.8f, model_col[2] * 0.8f, 1.f);
    // PartPlate::LINE_TOP_COLOR / LINE_TOP_SEL_COLOR (and their dark variants)
    const ColorRGBA line_col = dark ? ColorRGBA(0.329f, 0.329f, 0.353f, 1.f) : ColorRGBA(0.89f, 0.89f, 0.89f, 1.f);
    const ColorRGBA line_sel = dark ? ColorRGBA(0.298f, 0.298f, 0.3333f, 1.f) : ColorRGBA(0.5294f, 0.5451f, 0.5333f, 1.f);
    ExPolygon poly;
    for (auto &v : shape) poly.contour.append(Point::new_scale(v.x(), v.y()));
    Polylines thin, bold;
    grid_lines(poly, poly.contour.bounding_box(), thin, bold);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // the plate shadow's receivers (GLCanvas3D::_render_shadows' plate mask, z = 0) and the plates' extent
    m_plate_mask.clear();
    m_plates_box = BoundingBoxf3();
    for (size_t i = 0; i < origins.size(); ++i) {
        if (m_focus_mode && !m_preview && int(i) != current) continue;
        const Vec3d &o = origins[i];
        const Vec2d c = box.center();
        for (size_t k = 0; k < shape.size(); ++k) {
            const Vec2d &a = shape[k], &b = shape[(k + 1) % shape.size()];
            m_plate_mask.insert(m_plate_mask.end(), {float(c.x() + o.x()), float(c.y() + o.y()), 0.f, float(a.x() + o.x()), float(a.y() + o.y()), 0.f,
                                                     float(b.x() + o.x()), float(b.y() + o.y()), 0.f});
        }
        m_plates_box.merge(Vec3d(box.min.x() + o.x(), box.min.y() + o.y(), 0.));
        m_plates_box.merge(Vec3d(box.max.x() + o.x(), box.max.y() + o.y(), 0.));
    }
    for (size_t i = 0; i < origins.size(); ++i) {
        if (m_focus_mode && !m_preview && int(i) != current) continue;   // GLCanvas3D::_hides_inactive_plates
        const Vec3d &o = origins[i];
        Eigen::Matrix4f t = Eigen::Matrix4f::Identity();
        t(0, 3) = float(o.x()); t(1, 3) = float(o.y());
        // bed: the printer's model (Bed3D::render_model) or the flat outline (render_default)
        if (m_bed_model) {
            Eigen::Matrix4f m = t;
            m(0, 3) += float(box.center().x()); m(1, 3) += float(box.center().y()); m(2, 3) = -0.41f - 0.03f;
            if (m_bed_model_bbl && shape.size() > 2) { m(0, 3) -= float(shape[2].x() / 2.0); m(1, 3) -= float(shape[2].y() / 2.0); }
            glEnable(GL_DEPTH_TEST);
            draw_lit_mesh(m_bed_model, m, model_col, view, proj);
        } else {
            std::vector<float> tri;
            Vec2d c = box.center();
            for (size_t k = 0; k < shape.size(); ++k) {
                const Vec2d &a = shape[k], &b = shape[(k + 1) % shape.size()];
                tri.insert(tri.end(), {float(c.x() + o.x()), float(c.y() + o.y()), -0.03f, float(a.x() + o.x()), float(a.y() + o.y()), -0.03f,
                                       float(b.x() + o.x()), float(b.y() + o.y()), -0.03f});
            }
            glDepthMask(GL_FALSE);
            draw_flat(tri, GL_TRIANGLES, vp, fill_col);
            glDepthMask(GL_TRUE);
        }
        // the logo texture over the printable area (PartPlate::render_logo)
        if (m_bed_texture) {
            float x0 = float(box.min.x() + o.x()), x1 = float(box.max.x() + o.x()), y0 = float(box.min.y() + o.y()), y1 = float(box.max.y() + o.y());
            float q[] = {x0, y0, -0.02f, 0, 1, 0, x1, y0, -0.02f, 1, 1, 0, x1, y1, -0.02f, 1, 0, 0,
                         x0, y0, -0.02f, 0, 1, 0, x1, y1, -0.02f, 1, 0, 0, x0, y1, -0.02f, 0, 0, 0};
            glUseProgram(g_tex);
            glUniformMatrix4fv(glGetUniformLocation(g_tex, "mvp"), 1, GL_FALSE, vp.data());
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_bed_texture);
            glUniform1i(glGetUniformLocation(g_tex, "tex"), 0);
            glBindVertexArray(g_lines_vao);
            glBindBuffer(GL_ARRAY_BUFFER, g_lines_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
            glDepthMask(GL_FALSE);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDepthMask(GL_TRUE);
            glDisableVertexAttribArray(1);
            glBindVertexArray(0);
        }
        // PartPlate::render_imex_zones: the copy / mirror zones, danger strips and margin bands, under the grid
        {
            std::lock_guard<std::mutex> il(m_imex_mutex);
            if (i < m_imex.size()) {
                glDepthMask(GL_FALSE);
                for (auto &[tri, col] : m_imex[i].fills) {
                    std::vector<float> t = tri;
                    for (size_t k = 2; k < t.size(); k += 3) t[k] = -0.015f;
                    draw_flat(t, GL_TRIANGLES, vp, col);
                }
                glDepthMask(GL_TRUE);
            }
        }
        // grid (PartPlate::render_grid): thin lines and the bold ones, plus the contour
        auto lines = [&](const Polylines &pls) {
            std::vector<float> v;
            for (auto &pl : pls)
                for (size_t k = 0; k + 1 < pl.points.size(); ++k)
                    v.insert(v.end(), {float(unscale<double>(pl.points[k].x()) + o.x()), float(unscale<double>(pl.points[k].y()) + o.y()), -0.01f,
                                       float(unscale<double>(pl.points[k + 1].x()) + o.x()), float(unscale<double>(pl.points[k + 1].y()) + o.y()), -0.01f});
            return v;
        };
        // PartPlate::render_icons
        glDepthMask(GL_FALSE);
        for (int k = 0; k < 6; ++k) {
            std::string name = PLATE_ICONS[k];
            if (k == 3) name = m_plate_locked.size() > i && m_plate_locked[i] ? "plate_locked" : "plate_unlocked";
            if (k == 4 && m_plate_has_settings.size() > i && m_plate_has_settings[i]) name = "plate_settings_changed";
            if (m_pressed_icon == std::make_pair(int(i), k)) name += "_hover";
            if (dark) name += "_dark";
            BoundingBoxf r = icon_rect(box, k);
            draw_textured_quad(icon_texture(name), float(r.min.x() + o.x()), float(r.min.y() + o.y()), float(r.max.x() + o.x()), float(r.max.y() + o.y()), -0.01f, vp);
        }
        {   // the IDEX/IQEX mode icon in slot 6 (PLATE_IMEX_MODE_ID): normal / copy / mirror / custom
            std::lock_guard<std::mutex> il(m_imex_mutex);
            if (i < m_imex.size() && !m_imex[i].choices.empty()) {
                std::string name = "plate_imex_" + m_imex_kind[i] + "_mode";
                if (m_pressed_icon == std::make_pair(int(i), 6)) name += "_hover";
                if (dark) name += "_dark";
                BoundingBoxf r = icon_rect(box, 6);
                draw_textured_quad(icon_texture(name), float(r.min.x() + o.x()), float(r.min.y() + o.y()), float(r.max.x() + o.x()), float(r.max.y() + o.y()), -0.01f, vp);
                if (m_imex[i].mm_conflict) {   // calc_vertex_for_imex_warn_badge: obj_warning at the icon's bottom-right
                    const double badge = r.size().y() * 0.55;
                    draw_textured_quad(icon_texture("obj_warning"), float(r.max.x() - badge + o.x()), float(r.min.y() + o.y()),
                                       float(r.max.x() + o.x()), float(r.min.y() + badge + o.y()), -0.009f, vp);
                }
            }
        }
        glDepthMask(GL_TRUE);
        const ColorRGBA &lc = int(i) == current ? line_sel : line_col;
        if (m_show_gridlines) {   // GUI_App::show_plate_gridlines
            draw_flat(lines(thin), GL_LINES, vp, lc, 1.f);
            draw_flat(lines(bold), GL_LINES, vp, lc, 2.f);
        }
        std::vector<float> edge;
        for (auto &a : shape) edge.insert(edge.end(), {float(a.x() + o.x()), float(a.y() + o.y()), -0.01f});
        draw_flat(edge, GL_LINE_LOOP, vp, lc, 2.f);
        if (exclude.size() >= 3) {
            std::vector<float> ex;
            Vec2d ec(0, 0);
            for (auto &a : exclude) ec += a;
            ec /= double(exclude.size());
            for (size_t k = 0; k < exclude.size(); ++k) {
                const Vec2d &a = exclude[k], &b = exclude[(k + 1) % exclude.size()];
                ex.insert(ex.end(), {float(ec.x() + o.x()), float(ec.y() + o.y()), -0.005f, float(a.x() + o.x()), float(a.y() + o.y()), -0.005f,
                                     float(b.x() + o.x()), float(b.y() + o.y()), -0.005f});
            }
            draw_flat(ex, GL_TRIANGLES, vp, ColorRGBA(0.7f, 0.3f, 0.3f, 0.6f));
        }
    }
    glDisable(GL_BLEND);
    // Bed3D::Axes::render: flat cylinders of 0.1 x the bed size from the origin (the current plate's in focus mode)
    if (m_show_axes) {
        const float len = 0.1f * float(std::max(box.size().x(), box.size().y()));
        const Vec3d at = m_focus_mode && !m_preview && current < int(origins.size()) ? origins[current] : Vec3d::Zero();
        static indexed_triangle_set cyl;
        static float cyl_len = 0;
        if (cyl_len != len) { cyl = its_make_cylinder(len / 75.f, len, 2. * PI / 16.); cyl_len = len; }
        auto axis = [&](const Transform3d &t, const ColorRGBA &c) {
            std::vector<float> tri;
            for (const Vec3i32 &f : cyl.indices)
                for (int k = 0; k < 3; ++k) {
                    const Vec3d v = t * cyl.vertices[f[k]].cast<double>();
                    tri.insert(tri.end(), {float(v.x()), float(v.y()), float(v.z())});
                }
            draw_flat(tri, GL_TRIANGLES, vp, c);
        };
        glEnable(GL_DEPTH_TEST);
        const Vec3d origin(at.x(), at.y(), -0.04);   // GROUND_Z
        axis(Geometry::assemble_transform(origin, {0.0, 0.5 * PI, 0.0}), ColorRGBA::X());
        axis(Geometry::assemble_transform(origin, {-0.5 * PI, 0.0, 0.0}), ColorRGBA::Y());
        axis(Geometry::assemble_transform(origin), ColorRGBA::Z());
    }
}

// a volume's world matrix as drawn: moved by the finger, the gizmo's drag, or a finished drag not committed yet
Transform3d Viewport::drawn_matrix(const RenderVolume &r, unsigned revision, const Transform3d &gizmo) const
{
    Transform3d m = r.matrix;
    if (m_dragging && r.selected) m.pretranslate(m_drag_delta);
    if (m_gizmo_drag && r.selected) m = gizmo * m;
    if (!m_gizmo_drag && !m_dragging && r.selected && m_pending_tower < 0 && m_pending_revision == revision) m = m_pending * m;
    if (r.wipe_tower && r.obj - 1000 == m_tower_drag) m.pretranslate(m_drag_delta);
    if (r.wipe_tower && r.obj - 1000 == m_pending_tower && m_pending_revision == revision) m = m_pending * m;
    return m;
}

// GLCanvas3D::_render_shadows: the depth map from the light (the printable model parts in Prepare, the toolpaths
// in Preview), then the plate shadow over the drawn beds. Not in the Assembly view.
void Viewport::draw_shadows(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    Realistic &rv = Realistic::get();
    rv.invalidate();
    if (rv.shadow_mode(m_preview) == 0 || Plater::get().assemble_view) return;
    BoundingBoxf3 casters;
    size_t key = 1;
    auto combine = [&key](size_t v) { key ^= v + 0x9e3779b9 + (key << 6) + (key >> 2); };
    if (m_preview) {
        if (!Preview::get().shadow_casters(casters, key)) return;
        rv.build_shadow_map(true, casters, m_plates_box, view, key,
                            [](const Eigen::Matrix4f &lv, const Eigen::Matrix4f &lp, const Vec3d &eye) { Preview::get().render_shadow_casters(lv, lp, eye); });
    } else {
        std::vector<RenderVolume> vols;
        unsigned revision;
        {
            std::lock_guard<std::mutex> l(m_scene_mutex);
            vols = m_volumes;
            revision = m_volumes_revision;
        }
        const Transform3d gizmo = m_gizmo_drag ? Gizmos::get().drag_transform() : Transform3d::Identity();
        std::vector<std::pair<const RenderVolume *, Transform3d>> list;
        for (const RenderVolume &r : vols) {
            if (!r.printable || !r.model_part || r.wipe_tower) continue;
            if (Simplify::get().hides(r.obj, r.inst, r.vol) || Gizmos::get().cut_parts_hide(r.obj)) continue;
            const Transform3d m = drawn_matrix(r, revision, gizmo);
            casters.merge(r.bbox.transformed(m * r.matrix.inverse()));   // the world box, moved as drawn
            list.emplace_back(&r, m);
            // _shadow_casters_signature: the volumes, their meshes and where they are
            combine(std::hash<const void *>()(r.mesh.get()));
            for (int k = 0; k < 16; ++k) combine(std::hash<double>()(m.matrix().data()[k]));
        }
        rv.build_shadow_map(false, casters, m_plates_box, view, key,
                            [&](const Eigen::Matrix4f &lv, const Eigen::Matrix4f &lp, const Vec3d &) {
                                glUseProgram(g_flat);
                                const GLint u_mvp = glGetUniformLocation(g_flat, "mvp");
                                for (auto &[r, m] : list) {
                                    upload_mesh(r->mesh);
                                    auto it = g_meshes.find(r->mesh.get());
                                    if (it == g_meshes.end()) continue;
                                    const Eigen::Matrix4f mvp = lp * lv * m.matrix().cast<float>();
                                    glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.data());
                                    glBindVertexArray(it->second.vao);
                                    glDrawArrays(GL_TRIANGLES, 0, it->second.count);
                                }
                                glBindVertexArray(0);
                            });
    }
    rv.draw_plate_shadow(m_plate_mask, view, proj);
}

// GCodeViewer::render_scene after the toolpaths: the tool marker (SequentialView::Marker::render), on an IDEX/IQEX
// printer the secondary carriages' markers where the plan puts them, and the carriages' toolhead boxes
void Viewport::draw_tool_markers(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    Vec3f pos;
    float z_offset;
    bool by_filament;
    if (!Preview::get().tool_marker(pos, z_offset, by_filament)) return;
    std::shared_ptr<const TriangleMesh> mesh;
    ImexMarkers plan;
    {
        std::lock_guard<std::mutex> il(m_imex_mutex);
        mesh = m_marker_mesh;
        const int plate = Plater::get().current_plate;
        if (plate >= 0 && plate < int(m_imex_markers.size())) plan = m_imex_markers[plate];
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    auto marker = [&](const Vec3f &at, const ColorRGBA &c) {
        if (!mesh) return;
        // the model upside down, its tip at the position, 0.5 mm up (m_model_z_offset)
        const double h = mesh->bounding_box().size().z();
        const Transform3d m = Geometry::translation_transform((at + 0.5f * Vec3f::UnitZ()).cast<double>()) *
                              Geometry::translation_transform(h * Vec3d::UnitZ()) * Geometry::rotation_transform({M_PI, 0.0, 0.0});
        const float col[4] = {c.r(), c.g(), c.b(), c.a()};
        draw_lit_mesh(mesh, m.matrix().cast<float>(), col, view, proj);
    };
    const ColorRGBA default_color(1.0f, 1.0f, 1.0f, 0.5f);   // Marker::default_color
    if (plan.carriages.empty()) {
        marker(pos, default_color);
        glDisable(GL_BLEND);
        return;
    }
    struct Draw { Vec3f pos; ColorRGBA color; float box_x, box_y; };
    std::vector<Draw> draws;
    draws.push_back({pos, by_filament ? plan.pri_filament : plan.pri_palette, plan.pri_box_offset_x, plan.pri_box_offset_y});
    for (const ImexMarkers::Carriage &c : plan.carriages) {
        // tracking an axis translates the primary by the gap between the zone centres, mirroring reflects it
        const Vec3f sec(c.mirror_x ? c.x_term - pos.x() : pos.x() + c.x_term, c.mirror_y ? c.y_term - pos.y() : pos.y() + c.y_term, pos.z());
        draws.push_back({sec, by_filament ? c.filament : c.palette, c.box_offset_x, c.box_offset_y});
    }
    for (const Draw &d : draws) marker(d.pos, d.color);
    // the toolhead footprint boxes (View > Show IDEX/IQEX Toolhead Boxes), the nozzle at the collision-side edge
    if (m_show_toolhead_boxes && plan.box_wx > 0.f && plan.box_wy > 0.f) {
        if (!m_box_mesh || m_box_size != Vec2f(plan.box_wx, plan.box_wy)) {
            m_box_size = Vec2f(plan.box_wx, plan.box_wy);
            m_box_mesh = std::make_shared<const TriangleMesh>(its_make_cube(plan.box_wx, plan.box_wy, std::max(plan.box_wx, plan.box_wy)));
        }
        for (const Draw &d : draws) {
            const Transform3d m = Geometry::translation_transform((d.pos + Vec3f(d.box_x, d.box_y, z_offset)).cast<double>());
            const float col[4] = {d.color.r(), d.color.g(), d.color.b(), d.color.a()};
            draw_lit_mesh(m_box_mesh, m.matrix().cast<float>(), col, view, proj);
        }
    }
    glDisable(GL_BLEND);
}

void Viewport::draw_volumes(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    std::vector<RenderVolume> vols;
    unsigned revision;
    {
        std::lock_guard<std::mutex> l(m_scene_mutex);
        vols = m_volumes;
        revision = m_volumes_revision;
    }
    const Transform3d gizmo = m_gizmo_drag ? Gizmos::get().drag_transform() : Transform3d::Identity();
    // GLCanvas3D::_render_objects with Orca's "gouraud" shader (resources/shaders/ES/gouraud.*), "phong" in the
    // realistic view with Phong shading
    Realistic &rv = Realistic::get();
    GLuint prog = rv.enabled(m_preview) && rv.phong() ? orca_program("phong") : 0;
    if (!prog) prog = orca_program("gouraud");
    if (!prog) return;
    glUseProgram(prog);
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    const Camera cam = camera();
    glUniformMatrix4fv(U("projection_matrix"), 1, GL_FALSE, proj.data());
    glUniform1f(U("z_far"), float(cam.distance * 4 + 2000));
    glUniform1f(U("z_near"), float(std::max(1.0, cam.distance * 0.01)));
    rv.set_object_uniforms(prog, m_preview, view);
    glUniform1i(U("is_outline"), 0);
    glUniform1i(U("use_environment_tex"), 0);
    glUniform3f(U("extruder_printable_heights"), 0.f, 0.f, 0.f);
    glUniform2f(U("z_range"), -FLT_MAX, FLT_MAX);
    // GLCanvas3D::_get_volumes_clipping_plane: the section view or a painter's object clipper
    Vec3d cut_n;
    double cut_d;
    const bool cut_on = clipping_plane(cut_n, cut_d);
    glUniform4f(U("clipping_plane"), 0.f, 0.f, 1.f, FLT_MAX);   // ClippingPlane::ClipsNothing
    // GLGizmoCut3D::apply_color_clip_plane_colors: UPPER_PART_COLOR / LOWER_PART_COLOR
    if (Gizmos::get().cut_plane().groove) {   // CUT_PLANE_DEF_COLOR for both parts in the dovetail mode
        glUniform4f(U("uniform_color_clip_plane_1"), 0.9f, 0.9f, 0.9f, 0.5f);
        glUniform4f(U("uniform_color_clip_plane_2"), 0.9f, 0.9f, 0.9f, 0.5f);
    } else {
        glUniform4f(U("uniform_color_clip_plane_1"), 0.f, 1.f, 1.f, 1.f);
        glUniform4f(U("uniform_color_clip_plane_2"), 1.f, 0.f, 1.f, 1.f);
    }
    GLint u_ucc = U("use_color_clip_plane"), u_ccp = U("color_clip_plane"), u_clp = U("clipping_plane");
    // GLVolumeCollection::render: the overhang highlight (Plater::show_view3D_overhang) from the support threshold
    GLint u_slope = U("slope.actived");
    glUniform1i(u_slope, 0);
    glUniform1f(U("slope.normal_z"), m_overhang_normal_z);
    {
        const Vec3f up = m_up_direction;
        glUniform3f(U("slope.up_direction"), up.x(), up.y(), up.z());
    }
    glUniform2f(U("screen_size"), float(cam.width), float(cam.height));
    GLint u_vm = U("view_model_matrix"), u_n = U("view_normal_matrix"), u_w = U("volume_world_matrix"), u_c = U("uniform_color"),
          u_pt = U("print_volume.type"), u_px = U("print_volume.xy_data"), u_pz = U("print_volume.z_data"), u_sn = U("slope.volume_world_normal_matrix");
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    // GLVolumeCollection::render: opaque first, then the transparent ones (modifiers, blockers, unprintable)
    // GLCanvas3D::_is_xray_view_active: X-Ray replaces both shaded passes, except under a painting gizmo / the layer editor
    const bool xray = m_show_xray && !Painter::get().active() && LayersEditing::get().object() < 0;
    // focus mode (GLCanvas3D::_update_inactive_plates_visibility): the objects off the current plate are hidden
    const bool focus = m_focus_mode;
    auto hidden_by_focus = [&](const RenderVolume &r) {
        return focus && !r.selected && !r.wipe_tower &&
               !(r.bbox.max.x() >= m_current_plate_box.min.x() && r.bbox.min.x() <= m_current_plate_box.max.x() &&
                 r.bbox.max.y() >= m_current_plate_box.min.y() && r.bbox.min.y() <= m_current_plate_box.max.y());
    };
    // GUI_App::show_outline: the selected volumes get the silhouette pass (Prepare only)
    const bool  outline   = m_show_outline && !m_preview;
    const GLint u_outline = U("is_outline");
    glUniform1i(U("depth_tex"), 5);   // primed off unit 0 every frame, as GLVolumeCollection::render does
    for (int pass = 0; pass < (xray ? 0 : 2); ++pass) {
        if (pass == 1) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_CULL_FACE); }
        const int painted = Painter::get().object();
        const int layers_obj = LayersEditing::get().object();
        for (const RenderVolume &r : vols) {
            if (r.obj == painted && r.model_part) continue;   // drawn by the painter in its state colours
            if (Simplify::get().hides(r.obj, r.inst, r.vol)) continue;   // the simplify preview replaces it
            if (Gizmos::get().cut_parts_hide(r.obj)) continue;           // the cut's part selection replaces it
            if (r.obj == layers_obj && r.model_part) continue;           // drawn with the layer height texture below
            if (hidden_by_focus(r)) continue;
            const ColorRGBA c = render_color(r);
            if ((c.a() < 1.f) != (pass == 1)) continue;
            glUniform1i(u_slope, m_show_overhang && r.model_part && !r.wipe_tower ? 1 : 0);
            upload_mesh(r.mesh);
            auto it = g_meshes.find(r.mesh.get());
            if (it == g_meshes.end()) continue;
            const Transform3d m = drawn_matrix(r, revision, gizmo);
            Eigen::Matrix4f model = m.matrix().cast<float>();
            Eigen::Matrix4f vm = view * model;
            Eigen::Matrix3f nm = view.block<3, 3>(0, 0) * model.block<3, 3>(0, 0).inverse().transpose();
            Eigen::Matrix3f wn = model.block<3, 3>(0, 0).inverse().transpose();
            glUniformMatrix4fv(u_vm, 1, GL_FALSE, vm.data());
            glUniformMatrix3fv(u_n, 1, GL_FALSE, nm.data());
            glUniformMatrix4fv(u_w, 1, GL_FALSE, model.data());
            if (u_sn >= 0) glUniformMatrix3fv(u_sn, 1, GL_FALSE, wn.data());
            glUniform4f(u_c, c.r(), c.g(), c.b(), c.a());
            float cp[4];
            const bool clip = Gizmos::get().color_clip(r.obj, cp);
            glUniform1i(u_ucc, clip ? 1 : 0);
            if (clip) glUniform4fv(u_ccp, 1, cp);
            float clp[4];
            if (Gizmos::get().clip(r.obj, clp)) glUniform4fv(u_clp, 1, clp);
            else if (cut_on) glUniform4f(u_clp, float(-cut_n.x()), float(-cut_n.y()), float(-cut_n.z()), float(cut_d));
            else glUniform4f(u_clp, 0.f, 0.f, 1.f, FLT_MAX);
            // partly_inside: the part outside the plate is darkened by the shader
            glUniform1i(u_pt, r.outside ? 0 : -1);
            glUniform4f(u_px, r.print_volume[0], r.print_volume[1], r.print_volume[2], r.print_volume[3]);
            glUniform2f(u_pz, r.print_volume[4], r.print_volume[5]);
            glFrontFace(model.block<3, 3>(0, 0).determinant() < 0 ? GL_CW : GL_CCW);
            glBindVertexArray(it->second.vao);
            if (outline && r.selected && pass == 0 && !r.wipe_tower) {
                // GLVolume::render_with_outline: mark the volume in the stencil, draw its depth alone into a texture,
                // then draw it again where the stencil is clear with is_outline, which keeps only the silhouette
                glEnable(GL_STENCIL_TEST);
                glStencilMask(0xFF);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_REPLACE);
                glClearStencil(0);
                glClear(GL_STENCIL_BUFFER_BIT);
                glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
                glStencilFunc(GL_NOTEQUAL, 0xFF, 0xFF);
                glStencilMask(0x00);
                GLint prev_fbo = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
                GLuint depth_fbo = 0, depth_tex = 0;
                glGenFramebuffers(1, &depth_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo);
                glActiveTexture(GL_TEXTURE0 + 5);   // OUTLINE_DEPTH_TEX_UNIT
                glGenTextures(1, &depth_tex);
                glBindTexture(GL_TEXTURE_2D, depth_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // float depth textures are not filterable in GLES 3
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, cam.width, cam.height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_tex, 0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE0);
                glClear(GL_DEPTH_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
                glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
                glUniform1i(u_outline, 1);
                glActiveTexture(GL_TEXTURE0 + 5);
                glBindTexture(GL_TEXTURE_2D, depth_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(U("depth_tex"), 5);
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
                glActiveTexture(GL_TEXTURE0 + 5);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(u_outline, 0);
                glDeleteFramebuffers(1, &depth_fbo);
                glDeleteTextures(1, &depth_tex);
                glStencilMask(0xFF);
                glDisable(GL_STENCIL_TEST);
            } else
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
        }
    }
    glFrontFace(GL_CCW);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    if (!xray) draw_section_caps(vols, revision, gizmo, proj * view);
    // the per-volume matrix of a dragged / pending selection, as in the shaded pass
    auto volume_matrix = [&](const RenderVolume &r) {
        Transform3d m = r.matrix;
        if (m_dragging && r.selected) m.pretranslate(m_drag_delta);
        if (m_gizmo_drag && r.selected) m = gizmo * m;
        if (!m_gizmo_drag && !m_dragging && r.selected && m_pending_tower < 0 && m_pending_revision == revision) m = m_pending * m;
        return m;
    };
    // GLCanvas3D::_render_xray_volumes: every surface a thin translucent layer, depth writes off, back faces kept
    if (xray)
        if (GLuint xp = orca_program("xray")) {
            glUseProgram(xp);
            auto X = [xp](const char *n) { return glGetUniformLocation(xp, n); };
            glUniformMatrix4fv(X("projection_matrix"), 1, GL_FALSE, proj.data());
            glUniform2f(X("z_range"), -FLT_MAX, FLT_MAX);
            if (cut_on) glUniform4f(X("clipping_plane"), float(-cut_n.x()), float(-cut_n.y()), float(-cut_n.z()), float(cut_d));
            else glUniform4f(X("clipping_plane"), 0.f, 0.f, 1.f, FLT_MAX);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_CULL_FACE);
            for (const RenderVolume &r : vols) {
                if (hidden_by_focus(r) || Simplify::get().hides(r.obj, r.inst, r.vol) || Gizmos::get().cut_parts_hide(r.obj)) continue;
                upload_mesh(r.mesh);
                auto it = g_meshes.find(r.mesh.get());
                if (it == g_meshes.end()) continue;
                const Eigen::Matrix4f model = volume_matrix(r).matrix().cast<float>();
                const Eigen::Matrix4f vm = view * model;
                const Eigen::Matrix3f nm = view.block<3, 3>(0, 0) * model.block<3, 3>(0, 0).inverse().transpose();
                glUniformMatrix4fv(X("view_model_matrix"), 1, GL_FALSE, vm.data());
                glUniformMatrix3fv(X("view_normal_matrix"), 1, GL_FALSE, nm.data());
                glUniformMatrix4fv(X("volume_world_matrix"), 1, GL_FALSE, model.data());
                const ColorRGBA c = render_color(r);
                glUniform4f(X("uniform_color"), c.r(), c.g(), c.b(), c.a());
                glBindVertexArray(it->second.vao);
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
            }
            glBindVertexArray(0);
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    // GLCanvas3D::_render_wireframe_overlay: the triangle edges of the opaque volumes (ES "wireframe" shader)
    if (m_show_wireframe)
        if (GLuint wp = orca_program("wireframe")) {
            glUseProgram(wp);
            auto W = [wp](const char *n) { return glGetUniformLocation(wp, n); };
            glUniformMatrix4fv(W("projection_matrix"), 1, GL_FALSE, proj.data());
            glUniform1f(W("offset"), 0.00001f);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (const RenderVolume &r : vols) {
                const ColorRGBA c = render_color(r);
                if (c.a() < 1.f || hidden_by_focus(r) || Simplify::get().hides(r.obj, r.inst, r.vol)) continue;
                upload_mesh(r.mesh);
                auto it = g_meshes.find(r.mesh.get());
                if (it == g_meshes.end()) continue;
                const Eigen::Matrix4f vm = view * volume_matrix(r).matrix().cast<float>();
                glUniformMatrix4fv(W("view_model_matrix"), 1, GL_FALSE, vm.data());
                glUniform4f(W("uniform_color"), c.r(), c.g(), c.b(), c.a());
                glBindVertexArray(it->second.vao);
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
            }
            glBindVertexArray(0);
            glDisable(GL_BLEND);
        }
    // LayersEditing::render_volumes: the edited object's parts with the layer height stripes
    if (const int lobj = LayersEditing::get().object(); lobj >= 0)
        if (GLuint vprog = LayersEditing::get().begin_volumes(proj)) {
            auto V = [vprog](const char *n) { return glGetUniformLocation(vprog, n); };
            for (const RenderVolume &r : vols) {
                if (r.obj != lobj || !r.model_part) continue;
                upload_mesh(r.mesh);
                auto it = g_meshes.find(r.mesh.get());
                if (it == g_meshes.end()) continue;
                const Eigen::Matrix4f model = r.matrix.matrix().cast<float>();
                const Eigen::Matrix4f vm = view * model;
                const Eigen::Matrix3f nm = view.block<3, 3>(0, 0) * model.block<3, 3>(0, 0).inverse().transpose();
                glUniformMatrix4fv(V("view_model_matrix"), 1, GL_FALSE, vm.data());
                glUniformMatrix3fv(V("view_normal_matrix"), 1, GL_FALSE, nm.data());
                glUniformMatrix4fv(V("volume_world_matrix"), 1, GL_FALSE, model.data());
                glFrontFace(model.block<3, 3>(0, 0).determinant() < 0 ? GL_CW : GL_CCW);
                glBindVertexArray(it->second.vao);
                glDrawArrays(GL_TRIANGLES, 0, it->second.count);
            }
            glFrontFace(GL_CCW);
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    glDisable(GL_CULL_FACE);
    Simplify::get().render(view, proj);
    render_mesh_boolean(view, proj);
    BrimEarsGizmo::get().render(view, proj);
    MeasureGizmo::get().render(camera(), view, proj);
    LayersEditing::get().render_overlay();   // the thickness bar, over everything
    Painter::get().render(view, proj);
    Gizmos::get().set_emboss_ring_hidden(m_text_drag != 0 || m_dragging);   // not while the volume is dragged
    Gizmos::get().render(vols, revision, camera(), view, proj);
}

bool Viewport::draw()
{
    if (!m_gl_ready) return false;
    const bool zoom_to_bed = m_zoom_to_bed.exchange(false);
    m_bed_stale = false;
    if (zoom_to_bed) zoom_to("plate");
    snapshot();
    const bool benchmark = Benchmark::get().frame();   // SceneBenchmarkDialog::on_idle: the camera / layers of this frame
    Camera cam = camera();
    const bool dark = dark_mode();
    // DEFAULT_BG_LIGHT_COLOR / _DARK (GLCanvas3D.cpp)
    if (dark) glClearColor(0.329f, 0.329f, 0.353f, 1.f); else glClearColor(0.906f, 0.906f, 0.906f, 1.f);
    Realistic::get().begin_scene(m_preview, cam.width, cam.height);   // _render_ssao_pass reads the scene back
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    double far_z = cam.distance * 4 + 2000;
    Eigen::Matrix4f view = cam.view().cast<float>(), proj = cam.projection(std::max(1.0, cam.distance * 0.01), far_z).cast<float>();
    Eigen::Matrix4f vp = proj * view;
    glEnable(GL_DEPTH_TEST);
    if (m_preview) Preview::get().update();
    update_clipping_plane();
    if (m_preview) {   // before the shadow pass, so the cut away part casts no shadow
        Vec3d n;
        double d;
        const bool on = clipping_plane(n, d);
        Preview::get().set_clipping_plane(on ? std::array<float, 4>{float(-n.x()), float(-n.y()), float(-n.z()), float(d)}
                                             : std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
    }
    draw_bed(view, proj);
    draw_shadows(view, proj);
    bool animate = false;
    if (m_preview) {
        animate = Preview::get().render(view, proj);
        draw_tool_markers(view, proj);
    }
    else {
        draw_volumes(view, proj);
        draw_imex_ghosts(view, proj);
        animate |= LayersEditing::get().tick();   // the layer editing timer while the bar is held
    }
    Realistic::get().end_scene(view, proj, float(far_z));
    CenterOfMass::get().render(cam, m_preview);   // GLCanvas3D::CenterOfMass::render, over the scene
    // GLCanvas3D::_render_overlays: the 3D navigator and the center of mass details (ImGui)
    {
        Camera c = cam;
        bool   ortho = false, persp = false;
        animate |= ImGuiOverlay::get().frame(cam.width, cam.height, Gizmos::get().ui_scale(), [&] {
            if (m_show_navigator) animate |= navigator_content(c, dark, Gizmos::get().ui_scale(), ortho, persp);
            CenterOfMass::get().render_details(c, m_preview);
            if (Plater::get().assemble_view) render_assemble_overlay(c, dark);
        });
        if (c.azimuth != cam.azimuth || c.elevation != cam.elevation || ortho || persp) {
            std::lock_guard<std::mutex> l(m_cam_mutex);
            m_cam.azimuth   = c.azimuth;
            m_cam.elevation = c.elevation;
            if (ortho) auto_type(true);
            else if (persp) auto_type(false);
            animate = true;   // the scene follows the new camera next frame
        }
    }
    purge_meshes();
    m_dirty = false;
    return animate || benchmark || zoom_to_bed || m_zoom_to_bed || m_bed_stale;
}

// Plater::priv::set_current_panel(assemble_view): the Assembly view keeps its own camera; the first visit zooms to the volumes
void Viewport::set_assemble(bool on)
{
    Plater &p = Plater::get();
    if (p.assemble_view == on) return;
    {
        std::lock_guard<std::mutex> l(m_cam_mutex);
        std::swap(m_cam_saved, m_cam);
        m_cam.width = m_cam_saved.width;
        m_cam.height = m_cam_saved.height;
    }
    p.assemble_view = on;
    if (!on) p.explosion_ratio = 1.f;   // GLCanvas3D::_render_assemble_control resets it outside the view
    m_revision = ~0u;
    snapshot();
    if (on && m_assemble_first) {
        m_assemble_first = false;
        zoom_to("all");
    }
    m_dirty = true;
}

void Viewport::reload() { m_revision = ~0u; m_dirty = true; }

BoundingBoxf3 Viewport::selection_box()
{
    BoundingBoxf3 box;
    std::lock_guard<std::mutex> l(m_scene_mutex);
    for (const RenderVolume &r : m_volumes)
        if (r.selected) box.merge(r.bbox);
    return box;
}

bool Viewport::center_of_mass_tap(double x, double y)
{
    const bool hit = CenterOfMass::get().tap(camera(), x, y, m_preview);
    m_dirty = true;
    return hit;
}

bool Viewport::navigator_touch(float x, float y, int action)
{
    int h;
    {
        std::lock_guard<std::mutex> l(m_cam_mutex);
        h = m_cam.height;
    }
    const bool claim = m_show_navigator && action == 0 && navigator_hit(x, y, h, Gizmos::get().ui_scale());
    const bool taken = ImGuiOverlay::get().touch(x, y, action, claim);
    if (taken) m_dirty = true;
    return taken;
}

// -------------------------------------------------------------------------------------------------------------------
// gestures

// Camera::set_type: saved as use_perspective_camera; a type the user picked holds until an automatic ortho switch
void Viewport::set_type(bool ortho)
{
    if (m_cam.ortho != ortho) {
        m_cam.ortho = ortho;
        m_prevent_auto_type = true;
        if (Core::get().app_config) Core::get().app_config->set_bool("use_perspective_camera", !ortho);
    }
}

// Camera::auto_type with "auto_perspective": ortho for the axis views, perspective again when the view turns
void Viewport::auto_type(bool ortho)
{
    if (!m_auto_perspective) return;
    if (!ortho) {
        if (!m_prevent_auto_type) {
            set_type(false);
            m_prevent_auto_type = false;
        }
    } else {
        set_type(true);
        m_prevent_auto_type = false;
    }
}

void Viewport::orbit(double dx, double dy)
{
    std::lock_guard<std::mutex> l(m_cam_mutex);
    auto_type(false);   // GLCanvas3D::on_mouse rotation
    m_cam.azimuth -= dx * 0.3;
    m_cam.elevation = std::clamp(m_cam.elevation + dy * 0.3, -89.9, 89.9);
    m_dirty = true;
}

void Viewport::pan(double dx, double dy)
{
    std::lock_guard<std::mutex> l(m_cam_mutex);
    // world units per pixel at the target distance
    double k = 2 * m_cam.distance * std::tan(rad(m_cam.fov) / 2) / m_cam.height;
    Eigen::Matrix4d v = m_cam.view();
    Vec3d right = v.block<1, 3>(0, 0).transpose(), up = v.block<1, 3>(1, 0).transpose();
    m_cam.target += (-dx * right + dy * up) * k;
    m_dirty = true;
}

void Viewport::zoom(double factor, double px, double py)
{
    std::lock_guard<std::mutex> l(m_cam_mutex);
    // the zoom keys (Camera::update_zoom): toward the centre of the view
    if (px < 0) { px = m_cam.width / 2.0; py = m_cam.height / 2.0; }
    // zoom toward the fingers: keep the point under them fixed on the plane through the target
    Vec3d o, d;
    m_cam.ray(px, py, o, d);
    Vec3d n = (m_cam.eye() - m_cam.target).normalized();
    double t = (m_cam.target - o).dot(n) / d.dot(n);
    Vec3d before = o + t * d;
    double newd = std::clamp(m_cam.distance / factor, 5.0, 20000.0);
    double real = m_cam.distance / newd;
    m_cam.distance = newd;
    m_cam.target = before + (m_cam.target - before) / real;
    m_dirty = true;
}

static bool ray_triangle(const Vec3d &o, const Vec3d &d, const Vec3d &a, const Vec3d &b, const Vec3d &c, double &t)
{
    Vec3d e1 = b - a, e2 = c - a, p = d.cross(e2);
    double det = e1.dot(p);
    if (std::abs(det) < 1e-12) return false;
    double inv = 1 / det;
    Vec3d s = o - a;
    double u = s.dot(p) * inv;
    if (u < 0 || u > 1) return false;
    Vec3d q = s.cross(e1);
    double v = d.dot(q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = e2.dot(q) * inv;
    return t > 0;
}

static bool ray_box(const Vec3d &o, const Vec3d &d, const BoundingBoxf3 &b)
{
    double t0 = -1e30, t1 = 1e30;
    for (int k = 0; k < 3; ++k) {
        if (std::abs(d[k]) < 1e-12) { if (o[k] < b.min[k] || o[k] > b.max[k]) return false; continue; }
        double a = (b.min[k] - o[k]) / d[k], c = (b.max[k] - o[k]) / d[k];
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a); t1 = std::min(t1, c);
        if (t0 > t1) return false;
    }
    return t1 > 0;
}

nlohmann::json Viewport::pick(double px, double py)
{
    Camera cam = camera();
    Vec3d o, d;
    cam.ray(px, py, o, d);
    // the plate icons (PartPlate picking) lie on the bed plane
    if (std::abs(d.z()) > 1e-9) {
        double t = (-0.01 - o.z()) / d.z();
        if (t > 0) {
            Vec3d g = o + t * d;
            std::unique_lock<std::recursive_mutex> lock(Core::get().mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                Plater &p = Plater::get();
                BoundingBoxf bed = p.bed_box();
                for (size_t i = 0; i < p.plates.size(); ++i) {
                    Vec3d po = p.plate_origin(int(i));
                    for (int k = 0; k < 7; ++k) {
                        if (k == 6) {   // the IDEX/IQEX mode icon, on an IDEX/IQEX printer
                            std::lock_guard<std::mutex> il(m_imex_mutex);
                            if (i >= m_imex.size() || m_imex[i].choices.empty()) continue;
                        }
                        BoundingBoxf r = icon_rect(bed, k);
                        if (g.x() >= r.min.x() + po.x() && g.x() <= r.max.x() + po.x() && g.y() >= r.min.y() + po.y() && g.y() <= r.max.y() + po.y())
                            return {{"hit", false}, {"plate", int(i)}, {"plate_icon", k == 6 ? std::string("imex_mode") : std::string(PLATE_ICONS[k]).substr(6)}};
                    }
                }
            }
        }
    }
    // Lay on face: the planes are on top of the object (GLGizmoFlatten raycasters)
    nlohmann::json plane = Gizmos::get().pick_plane(cam, px, py);
    if (!plane.is_null()) { m_dirty = true; return {{"hit", false}, {"flatten", plane}}; }
    // GLCanvas3D::_picking_pass_imex_ghosts: the ghosts of the current plate, first box hit wins, over the volumes
    // (a tap on one shows its tooltip and, on a head with several lanes, the filament picker)
    if (!m_preview) {
        std::lock_guard<std::mutex> il(m_imex_mutex);
        const int plate = Plater::get().current_plate;
        if (plate >= 0 && plate < int(m_imex.size()))
            for (const ImexOverlay::Ghost &g : m_imex[plate].ghosts)
                if (g.mesh && ray_box(o, d, g.mesh->transformed_bounding_box(g.xf)))
                    return {{"hit", false}, {"imex_ghost", g.head}};
    }
    std::vector<RenderVolume> vols;
    {
        std::lock_guard<std::mutex> l(m_scene_mutex);
        vols = m_volumes;
    }
    double best = 1e30;
    const RenderVolume *hit = nullptr;
    Vec3d hit_point;
    Vec3d cut_n;
    double cut_d;
    const bool cut_on = clipping_plane(cut_n, cut_d);   // get_raycaster_clipping_plane: the cut away part is not hit
    for (const RenderVolume &r : vols) {
        if (!ray_box(o, d, r.bbox)) continue;
        // ray in mesh coordinates
        Transform3d inv = r.matrix.inverse();
        Vec3d lo = inv * o, ld = (inv.linear() * d);
        double scale = ld.norm();
        ld /= scale;
        const indexed_triangle_set &its = r.mesh->its;
        for (const Vec3i32 &f : its.indices) {
            double t;
            if (ray_triangle(lo, ld, its.vertices[f[0]].cast<double>(), its.vertices[f[1]].cast<double>(), its.vertices[f[2]].cast<double>(), t)) {
                double tw = t / scale;
                if (cut_on && cut_n.dot(o + d * tw) > cut_d) continue;
                if (tw < best && (r.model_part || !hit || !hit->model_part)) { best = tw; hit = &r; hit_point = o + d * tw; }
            }
        }
    }
    // the plate under the finger (PartPlateList picking)
    int plate = -1;
    if (std::abs(d.z()) > 1e-9) {
        double t = -o.z() / d.z();
        if (t > 0) {
            Vec3d g = o + t * d;
            std::unique_lock<std::recursive_mutex> lock(Core::get().mutex, std::try_to_lock);
            if (lock.owns_lock()) {
                Plater &p = Plater::get();
                for (size_t i = 0; i < p.plates.size(); ++i) {
                    BoundingBoxf3 b = p.plate_box(int(i));
                    if (g.x() >= b.min.x() && g.x() <= b.max.x() && g.y() >= b.min.y() && g.y() <= b.max.y()) plate = int(i);
                }
            }
        }
    }
    if (!hit) return {{"hit", false}, {"plate", plate}};
    if (hit->wipe_tower)   // the prime tower: not an object, but it can be dragged
        return {{"hit", false}, {"plate", plate}, {"wipe_tower", hit->obj - 1000}, {"point", {hit_point.x(), hit_point.y(), hit_point.z()}}};
    return {{"hit", true}, {"obj", hit->obj}, {"inst", hit->inst}, {"vol", hit->vol}, {"plate", plate},
            {"point", {hit_point.x(), hit_point.y(), hit_point.z()}}};
}

int texture_disp_mouse(int action, double px, double py);   // TextureDisp.cpp: -1 not in Adjust placement, 0 / 1 taken

bool Viewport::drag_start(double px, double py)
{
    // GLGizmoTextureDisplacement::on_mouse: in Adjust placement the handle takes the drag and nothing paints
    if (Painter::get().active() && Painter::get().kind() == "texture") {
        const int taken = texture_disp_mouse(0, px, py);
        if (taken == 1) { m_tex_adjust = true; m_dirty = true; return true; }
        if (taken == 0) return false;
    }
    // a painting gizmo: a drag that starts on the object paints (GLGizmoPainterBase::gizmo_event)
    if (Painter::get().active()) {
        if (Painter::get().stroke_begin(camera(), px, py)) { m_painting = true; m_dirty = true; return true; }
        return false;
    }
    // GLGizmoBrimEars: a drag that starts on an ear moves it
    if (BrimEarsGizmo::get().active()) {
        if (BrimEarsGizmo::get().drag_begin(camera(), px, py)) { m_ear_drag = true; m_dirty = true; return true; }
        return false;
    }
    // GLGizmoEmboss: the rotation ring's grabber, else the text volume slides over the object's surface (SurfaceDrag)
    if ((text_gizmo_active() || svg_gizmo_active()) && Gizmos::get().emboss_ring_drag_start(camera(), px, py)) {
        m_ring_kind = text_gizmo_active() ? 1 : 2;
        m_gizmo_drag = true;
        m_dirty = true;
        return true;
    }
    if (text_gizmo_active() && text_gizmo_drag_begin(camera(), px, py)) { m_text_drag = 1; m_dirty = true; return true; }
    if (svg_gizmo_active() && svg_gizmo_drag_begin(camera(), px, py)) { m_text_drag = 2; m_dirty = true; return true; }
    {
        std::vector<RenderVolume> vols;
        unsigned revision;
        {
            std::lock_guard<std::mutex> l(m_scene_mutex);
            vols = m_volumes;
            revision = m_volumes_revision;
        }
        if (Gizmos::get().drag_start(vols, revision, camera(), px, py)) { m_gizmo_drag = true; m_dirty = true; return true; }
    }
    nlohmann::json h = pick(px, py);
    if (h.contains("wipe_tower")) {   // GLCanvas3D: the tower is picked and moved in one drag
        m_tower_drag = h["wipe_tower"];
        m_drag_z = h["point"][2];
        m_drag_start_world = Vec3d(h["point"][0], h["point"][1], h["point"][2]);
        m_drag_delta = Vec3d::Zero();
        m_dragging = true;
        return true;
    }
    if (!h.value("hit", false)) return false;
    bool selected = false;
    {
        std::lock_guard<std::mutex> l(m_scene_mutex);
        for (auto &r : m_volumes)
            if (r.obj == h["obj"] && r.inst == h["inst"] && r.selected) selected = true;
    }
    if (!selected) return false;
    m_drag_z = h["point"][2];
    m_drag_start_world = Vec3d(h["point"][0], h["point"][1], h["point"][2]);
    m_drag_delta = Vec3d::Zero();
    m_dragging = true;
    return true;
}

void Viewport::drag_move(double px, double py)
{
    if (m_tex_adjust) { texture_disp_mouse(1, px, py); m_dirty = true; return; }
    if (m_painting) { Painter::get().stroke_move(camera(), px, py); m_dirty = true; return; }
    if (m_ear_drag) { BrimEarsGizmo::get().drag_move(camera(), px, py); m_dirty = true; return; }
    if (m_text_drag) {
        if (m_text_drag == 1) text_gizmo_drag_move(camera(), px, py); else svg_gizmo_drag_move(camera(), px, py);
        m_dirty = true;
        return;
    }
    if (m_gizmo_drag) { Gizmos::get().drag_move(camera(), px, py); m_dirty = true; return; }
    if (!m_dragging) return;
    Camera cam = camera();
    Vec3d o, d;
    cam.ray(px, py, o, d);
    if (std::abs(d.z()) < 1e-9) return;
    double t = (m_drag_z - o.z()) / d.z();
    if (t <= 0) return;
    Vec3d g = o + t * d;
    m_drag_delta = Vec3d(g.x() - m_drag_start_world.x(), g.y() - m_drag_start_world.y(), 0);
    m_dirty = true;
}

nlohmann::json Viewport::drag_end()
{
    if (m_tex_adjust) {
        m_tex_adjust = false;
        texture_disp_mouse(2, 0., 0.);
        m_dirty = true;
        return nullptr;
    }
    if (m_painting) {
        m_painting = false;
        Painter::get().stroke_end();
        m_dirty = true;
        return nullptr;
    }
    if (m_text_drag) {
        const bool svg = m_text_drag == 2;
        m_text_drag = 0;
        if (svg) svg_gizmo_drag_end(); else text_gizmo_drag_end();
        m_dirty = true;
        return {{"gizmo", svg ? "svg" : "text"}};
    }
    if (m_ear_drag) {
        m_ear_drag = false;
        BrimEarsGizmo::get().drag_end();
        m_dirty = true;
        return {{"gizmo", "brim_ears"}};
    }
    if (m_gizmo_drag) {
        // the preview stays until the committed model arrives (snapshot), so the object does not jump back
        Transform3d t = Gizmos::get().drag_transform();
        nlohmann::json r = Gizmos::get().drag_end();
        if (!r.is_null()) { m_pending = t; m_pending_revision = m_volumes_revision; }
        m_gizmo_drag = false;
        if (r.is_object() && r.value("gizmo", "") == "emboss_ring") {
            const double a = r.value("angle", 0.0);
            const bool svg = m_ring_kind == 2;
            m_ring_kind = 0;
            if (std::abs(a) > 1e-9) { if (svg) svg_gizmo_ring_rotate(a); else text_gizmo_ring_rotate(a); }
            m_dirty = true;
            return {{"gizmo", svg ? "svg" : "text"}};
        }
        m_dirty = true;
        return r;
    }
    if (!m_dragging) return nullptr;
    Vec3d d = m_drag_delta;
    const int tower = m_tower_drag;
    m_tower_drag = -1;
    m_pending_tower = tower;
    if (d.norm() > 1e-6) { m_pending = Geometry::translation_transform(d); m_pending_revision = m_volumes_revision; }
    if (tower >= 0) {
        m_dragging = false;
        m_drag_delta = Vec3d::Zero();
        return {{"wipe_tower", tower}, {"delta", {d.x(), d.y(), 0.}}};
    }
    m_dragging = false;
    m_drag_delta = Vec3d::Zero();
    return {d.x(), d.y(), d.z()};
}

void Viewport::set_view(const std::string &name)
{
    std::lock_guard<std::mutex> l(m_cam_mutex);
    // Camera::select_view
    if (name == "iso") { m_cam.azimuth = -45; m_cam.elevation = 45; auto_type(false); }          // set_default_orientation
    else if (name == "topfront") { m_cam.azimuth = 0; m_cam.elevation = 45; auto_type(false); }
    else if (name == "top") { m_cam.azimuth = 0; m_cam.elevation = 89.9; auto_type(true); }
    else if (name == "bottom") { m_cam.azimuth = 0; m_cam.elevation = -89.9; auto_type(true); }
    else if (name == "front") { m_cam.azimuth = 0; m_cam.elevation = 0; auto_type(true); }
    else if (name == "rear") { m_cam.azimuth = 180; m_cam.elevation = 0; auto_type(true); }
    else if (name == "left") { m_cam.azimuth = -90; m_cam.elevation = 0; auto_type(true); }
    else if (name == "right") { m_cam.azimuth = 90; m_cam.elevation = 0; auto_type(true); }
    else if (name == "ortho" || name == "perspective") set_type(name == "ortho");   // the user's choice
    m_dirty = true;
}

void Viewport::zoom_to(const std::string &what)
{
    BoundingBoxf3 box;
    if (what == "selection" || what == "all") {
        std::lock_guard<std::mutex> l(m_scene_mutex);
        for (auto &r : m_volumes)
            if (what == "all" || r.selected) box.merge(r.bbox);
    }
    if (!box.defined) {
        std::unique_lock<std::recursive_mutex> lock(Core::get().mutex);
        Plater &p = Plater::get();
        box = p.plate_box(p.current_plate);
        box.max.z() = std::min(box.max.z(), 10.);
    }
    std::lock_guard<std::mutex> l(m_cam_mutex);
    m_cam.target = box.center();
    double r = box.radius();
    m_cam.distance = std::max(10.0, r / std::sin(rad(m_cam.fov) / 2) * (m_cam.width < m_cam.height ? double(m_cam.height) / m_cam.width : 1.0) * 1.05);
    m_dirty = true;
}

// screen positions of the plate labels (PartPlate plate name above the top-left corner, index at the bottom-right)
nlohmann::json Viewport::overlay()
{
    Camera cam = camera();
    Eigen::Matrix4d vp = cam.projection(std::max(1.0, cam.distance * 0.01), cam.distance * 4 + 2000) * cam.view();
    auto project = [&](const Vec3d &w) {
        Eigen::Vector4d c = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.0);
        if (c.w() <= 0) return nlohmann::json(nullptr);
        return nlohmann::json{(c.x() / c.w() * 0.5 + 0.5) * cam.width, (0.5 - c.y() / c.w() * 0.5) * cam.height};
    };
    nlohmann::json plates = nlohmann::json::array();
    std::unique_lock<std::recursive_mutex> lock(Core::get().mutex, std::try_to_lock);
    if (!lock.owns_lock()) return nullptr;
    Plater &p = Plater::get();
    BoundingBoxf bed = p.bed_box();
    float f = icon_scale_factor(bed), size = PARTPLATE_ICON_SIZE * f;
    for (size_t i = 0; i < (p.assemble_view ? 0 : p.plates.size()); ++i) {   // no plates in the Assembly view
        Vec3d o = p.plate_origin(int(i));
        Vec3d name_at(bed.min.x() + o.x(), bed.max.y() + o.y() + size * 0.15, 0), num_at(bed.max.x() + o.x() + PARTPLATE_ICON_GAP_LEFT * f, bed.min.y() + o.y(), 0);
        auto a = project(name_at), b = project(num_at), c = project(name_at + Vec3d(size, 0, 0));
        double px = (a.is_array() && c.is_array()) ? std::hypot(c[0].get<double>() - a[0].get<double>(), c[1].get<double>() - a[1].get<double>()) : 0;
        plates.push_back({{"name", a}, {"number", b}, {"icon_px", px}, {"name_text", p.plates[i].name}, {"index", i}});
    }
    // GLCanvas3D::Labels::render: the name of each instance on the current plate at its box centre, "(n)" when the
    // object has several; unselected first, then by eye depth
    nlohmann::json labels = nlohmann::json::array();
    if (m_show_labels && !m_preview && Gizmos::get().active().empty() && !Painter::get().active()) {   // not while a gizmo runs
        struct Owner { std::string label; Vec3d center; double eye_z; bool selected; };
        std::vector<Owner> owners;
        const Model &model = p.model();
        const Eigen::Matrix4d view = cam.view();
        for (auto &[o, i] : p.plates[p.current_plate].instances) {
            if (o >= int(model.objects.size()) || i >= int(model.objects[o]->instances.size())) continue;
            const ModelObject *mo = model.objects[o];
            Owner ow;
            ow.label = mo->name;
            if (mo->instances.size() > 1) ow.label += " (" + std::to_string(i + 1) + ")";
            ow.center = mo->instance_bounding_box(size_t(i)).center();
            ow.eye_z = (view * Eigen::Vector4d(ow.center.x(), ow.center.y(), ow.center.z(), 1.0)).z();
            ow.selected = p.selection.count(SelItem{o, -1, -1}) || p.selection.count(SelItem{o, i, -1});
            owners.push_back(ow);
        }
        std::sort(owners.begin(), owners.end(), [](const Owner &a, const Owner &b) {
            if (a.selected != b.selected) return !a.selected;
            return a.eye_z < b.eye_z;
        });
        for (const Owner &ow : owners) {
            auto at = project(ow.center);
            if (!at.is_array() || at[0].get<double>() < 0 || at[0].get<double>() > cam.width || at[1].get<double>() < 0 || at[1].get<double>() > cam.height)
                continue;
            labels.push_back({{"text", ow.label}, {"at", at}, {"selected", ow.selected}});
        }
    }
    return {{"plates", plates}, {"current", p.current_plate}, {"measure", MeasureGizmo::get().labels(cam)}, {"labels", labels}};
}

// a tap with a painting gizmo: one dab of the brush or one fill
bool Viewport::paint_tap(double px, double py)
{
    if (!Painter::get().active()) return false;
    // GLGizmoTextureDisplacement::on_mouse: Mark seams / Adjust placement take the click (as a press and a release)
    if (Painter::get().kind() == "texture" && texture_disp_mouse(-1, px, py) >= 0) {
        const bool taken = texture_disp_mouse(0, px, py) == 1;
        if (taken) texture_disp_mouse(2, px, py);
        m_dirty = true;
        return taken;
    }
    if (!Painter::get().stroke_begin(camera(), px, py)) return false;
    Painter::get().stroke_end();
    m_dirty = true;
    return true;
}

void Viewport::set_pressed_icon(int plate, int icon) { m_pressed_icon = {plate, icon}; m_dirty = true; }

void Viewport::set_preview(bool on)
{
    m_preview = on;
    m_dirty = true;
}

} // namespace orca
