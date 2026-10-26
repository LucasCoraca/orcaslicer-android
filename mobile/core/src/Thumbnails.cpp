#include "Thumbnails.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <cfloat>
#include <map>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "Core.hpp"
#include "Plater.hpp"
#include "Shaders.hpp"
#include "libslic3r/BuildVolume.hpp"

using namespace Slic3r;

namespace orca {

#define TLOG(...) __android_log_print(ANDROID_LOG_WARN, "orcacore-thumb", __VA_ARGS__)

// ---------------------------------------------------------------------------------------------------------------
// scene

ThumbScene thumb_scene(int plate, bool use_loaded_id)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    const Model &m = p.model();
    ThumbScene s;
    // PartPlate::get_build_volume, grown by BuildVolume::SceneEpsilon as render_thumbnail_internal does
    s.plate_box = p.plate_box(plate);
    s.plate_box.min -= Vec3d::Constant(BuildVolume::SceneEpsilon);
    s.plate_box.max += Vec3d::Constant(BuildVolume::SceneEpsilon);
    std::vector<ColorRGBA> colors;
    if (auto *fc = core.full_config().option<ConfigOptionStrings>("filament_colour"))
        for (auto &str : fc->values) { ColorRGBA c; colors.push_back(decode_color(str, c) ? c : ColorRGBA(1.f, 1.f, 1.f, 1.f)); }
    if (colors.empty()) colors.push_back(ColorRGBA(1.f, 1.f, 1.f, 1.f));
    BoundingBoxf3 contain = s.plate_box;
    contain.min.z() = -1e10;
    for (int oi = 0; oi < int(m.objects.size()); ++oi) {
        const ModelObject *mo = m.objects[oi];
        for (int ii = 0; ii < int(mo->instances.size()); ++ii) {
            const ModelInstance *mi = mo->instances[ii];
            if (!mi->printable) continue;
            for (const ModelVolume *mv : mo->volumes) {
                if (!mv->is_model_part()) continue;   // modifiers, negative volumes, blockers are not drawn
                ThumbScene::Item it;
                it.mesh = mv->mesh_ptr();
                it.matrix = mi->get_matrix() * mv->get_matrix();
                // render_thumbnail_internal's is_visible: inside the plate (any height below) and above the bed
                it.bbox = it.mesh->transformed_bounding_box(it.matrix);
                if (!contain.contains(it.bbox) || it.bbox.max.z() <= 0) continue;
                int e = mv->extruder_id();
                it.extruder = e > 0 ? e : 1;
                it.color = colors[std::clamp(it.extruder - 1, 0, int(colors.size()) - 1)];
                it.object_id = unsigned(use_loaded_id && mi->loaded_id > 0 ? mi->loaded_id : mi->id().id);
                s.items.push_back(std::move(it));
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// offscreen EGL

namespace {

EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLContext g_ctx = EGL_NO_CONTEXT;
EGLSurface g_surf = EGL_NO_SURFACE;

bool egl_begin()
{
    if (g_ctx == EGL_NO_CONTEXT) {
        g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, nullptr, nullptr)) { TLOG("no EGL display"); return false; }
        const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        if (!eglChooseConfig(g_dpy, attrs, &cfg, 1, &n) || n < 1) { TLOG("no EGL config"); return false; }
        const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        g_surf = eglCreatePbufferSurface(g_dpy, cfg, pb);
        const EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ca);
        if (g_ctx == EGL_NO_CONTEXT || g_surf == EGL_NO_SURFACE) { TLOG("EGL context failed 0x%x", eglGetError()); g_ctx = EGL_NO_CONTEXT; return false; }
    }
    return eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx) == EGL_TRUE;
}


struct Uploaded
{
    GLuint vao{0}, vbo{0};
    GLsizei count{0};
};

// the meshes of one scene, in the thumbnail context (released by ~Meshes)
struct Meshes
{
    std::map<const TriangleMesh *, Uploaded> map;
    const Uploaded &get(const std::shared_ptr<const TriangleMesh> &mesh)
    {
        auto it = map.find(mesh.get());
        if (it != map.end()) return it->second;
        std::vector<float> data;
        data.reserve(mesh->its.indices.size() * 18);
        for (const Vec3i32 &f : mesh->its.indices) {
            const Vec3f &a = mesh->its.vertices[f[0]], &b = mesh->its.vertices[f[1]], &c = mesh->its.vertices[f[2]];
            Vec3f n = (b - a).cross(c - a);
            if (float l = n.norm(); l > 0) n /= l;
            for (const Vec3f *v : {&a, &b, &c}) data.insert(data.end(), {v->x(), v->y(), v->z(), n.x(), n.y(), n.z()});
        }
        Uploaded u;
        glGenVertexArrays(1, &u.vao);
        glBindVertexArray(u.vao);
        glGenBuffers(1, &u.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, u.vbo);
        glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
        glBindVertexArray(0);
        u.count = GLsizei(data.size() / 6);
        return map[mesh.get()] = u;
    }
    ~Meshes()
    {
        for (auto &[k, u] : map) { glDeleteBuffers(1, &u.vbo); glDeleteVertexArrays(1, &u.vao); }
    }
};

// Camera::look_at / get_view_matrix
Eigen::Matrix4d look_at(const Vec3d &eye, const Vec3d &target, const Vec3d &up_hint)
{
    Vec3d f = (target - eye).normalized(), s = f.cross(up_hint).normalized(), u = s.cross(f);
    Eigen::Matrix4d m = Eigen::Matrix4d::Identity();
    m.block<1, 3>(0, 0) = s.transpose();
    m.block<1, 3>(1, 0) = u.transpose();
    m.block<1, 3>(2, 0) = -f.transpose();
    m(0, 3) = -s.dot(eye); m(1, 3) = -u.dot(eye); m(2, 3) = f.dot(eye);
    return m;
}

Eigen::Matrix4d ortho(double l, double r, double b, double t, double n, double f)
{
    Eigen::Matrix4d p = Eigen::Matrix4d::Zero();
    p(0, 0) = 2 / (r - l); p(1, 1) = 2 / (t - b); p(2, 2) = -2 / (f - n);
    p(0, 3) = -(r + l) / (r - l); p(1, 3) = -(t + b) / (t - b); p(2, 3) = -(f + n) / (f - n); p(3, 3) = 1;
    return p;
}

std::vector<Vec3d> corners(const BoundingBoxf3 &b)
{
    return {b.min, {b.max.x(), b.min.y(), b.min.z()}, {b.max.x(), b.max.y(), b.min.z()}, {b.min.x(), b.max.y(), b.min.z()},
            {b.min.x(), b.min.y(), b.max.z()}, {b.max.x(), b.min.y(), b.max.z()}, b.max, {b.min.x(), b.max.y(), b.max.z()}};
}

// GLCanvas3D::adjust_color_for_rendering
ColorRGBA adjust_color(const ColorRGBA &c)
{
    const float FULL_BLACK_THRESHOLD = 0.18f, FullyTransparentMaterialThreshold = 0.1f, FullTransparentModdifiedToFixAlpha = 0.3f;
    if (c.a() < FullyTransparentMaterialThreshold) return {1, 1, 1, FullTransparentModdifiedToFixAlpha};
    if (c.r() < FULL_BLACK_THRESHOLD && c.g() < FULL_BLACK_THRESHOLD && c.b() < FULL_BLACK_THRESHOLD)
        return {FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, c.a()};
    return c;
}

void render_into(Meshes &meshes, const ThumbScene &scene, ThumbnailData &out, ThumbView view, bool for_picking, bool ban_light)
{
    const unsigned w = out.width, h = out.height;
    // render_thumbnail_internal: the box of the visible volumes, grown a little
    BoundingBoxf3 box;
    box.min.z() = box.max.z() = 0;
    for (auto &it : scene.items) box.merge(it.bbox);
    box.min.z() = -BuildVolume::SceneEpsilon;
    Vec3d sz = box.size();
    box.max.x() += sz.x() * 0.01f; box.min.x() -= sz.x() * 0.01f;
    box.max.y() += sz.y() * 0.01f; box.min.y() -= sz.y() * 0.01f;
    box.max.z() += sz.z() * 0.02f; box.min.z() -= sz.z() * 0.02f;

    Eigen::Matrix4d V, P;
    if (view == ThumbView::TopPlate) {
        const BoundingBoxf3 &pb = scene.plate_box;
        Vec3d c(0.5 * (pb.min.x() + pb.max.x()), 0.5 * (pb.min.y() + pb.max.y()), 0);
        double dz = pb.max.z() - pb.min.z();
        double zoom = std::min(double(w) / (pb.max.x() - pb.min.x()), double(h) / (pb.max.y() - pb.min.y()));
        V = look_at(c + dz * Vec3d::UnitZ(), c, Vec3d::UnitY());
        // Camera::apply_projection(plate box): near / far around the box along the view axis
        double n = 1e30, f = -1e30;
        for (const Vec3d &v : corners(pb)) { double d = -(V * Eigen::Vector4d(v.x(), v.y(), v.z(), 1)).z(); n = std::min(n, d); f = std::max(f, d); }
        double hw = 0.5 * w / zoom, hh = 0.5 * h / zoom;
        P = ortho(-hw, hw, -hh, hh, std::max(0.01, n - 1), f + 1);
    } else {
        // Camera::select_view(Iso) = set_default_orientation (zenit 45, phi 45), then zoom_to_box
        const double theta = -M_PI / 4, phi = M_PI / 4;
        Vec3d dir(std::sin(theta) * std::sin(phi), std::sin(theta) * std::cos(phi), std::cos(theta));   // from target to eye
        Vec3d c = box.center();
        double dist = box.radius() * 2 + 10;
        V = look_at(c + dist * dir, c, Vec3d::UnitZ());
        Vec3d right = V.block<1, 3>(0, 0).transpose(), up = V.block<1, 3>(1, 0).transpose();
        double minx = DBL_MAX, maxx = -DBL_MAX, miny = DBL_MAX, maxy = -DBL_MAX;
        for (const Vec3d &v : corners(box)) {   // calc_zoom_to_bounding_box_factor
            Vec3d q = v - c;
            minx = std::min(minx, q.dot(right)); maxx = std::max(maxx, q.dot(right));
            miny = std::min(miny, q.dot(up)); maxy = std::max(maxy, q.dot(up));
        }
        const double margin = 1.025;   // DefaultZoomToBoxMarginFactor
        double dx = (maxx - minx) * margin, dy = (maxy - miny) * margin;
        double zoom = (dx > 0 && dy > 0) ? std::min(double(w) / dx, double(h) / dy) : 1.0;
        double n = 1e30, f = -1e30;
        for (const Vec3d &v : corners(box)) { double d = -(V * Eigen::Vector4d(v.x(), v.y(), v.z(), 1)).z(); n = std::min(n, d); f = std::max(f, d); }
        double hw = 0.5 * w / zoom, hh = 0.5 * h / zoom;
        P = ortho(-hw, hw, -hh, hh, std::max(0.01, n - 1), f + 1);
    }

    glViewport(0, 0, w, h);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    GLuint prog = gl::orca_program(for_picking ? "flat" : "thumbnail");
    if (!prog) return;
    glUseProgram(prog);
    Eigen::Matrix4f Pf = P.cast<float>();
    glUniformMatrix4fv(glGetUniformLocation(prog, "projection_matrix"), 1, GL_FALSE, Pf.data());
    GLint u_vm = glGetUniformLocation(prog, "view_model_matrix"), u_n = glGetUniformLocation(prog, "view_normal_matrix"),
          u_c = glGetUniformLocation(prog, "uniform_color"), u_w = glGetUniformLocation(prog, "volume_world_matrix");
    glUniform1f(glGetUniformLocation(prog, "emission_factor"), 0.1f);
    glUniform1i(glGetUniformLocation(prog, "ban_light"), ban_light ? 1 : 0);
    if (for_picking) glDisable(GL_CULL_FACE);
    for (const auto &it : scene.items) {
        const Uploaded &u = meshes.get(it.mesh);
        Eigen::Matrix4d vm = V * it.matrix.matrix();
        Eigen::Matrix4f vmf = vm.cast<float>(), wf = it.matrix.matrix().cast<float>();
        Eigen::Matrix3f nm = (V.block<3, 3>(0, 0) * it.matrix.matrix().block<3, 3>(0, 0).inverse().transpose()).cast<float>();
        glUniformMatrix4fv(u_vm, 1, GL_FALSE, vmf.data());
        if (u_n >= 0) glUniformMatrix3fv(u_n, 1, GL_FALSE, nm.data());
        if (u_w >= 0) glUniformMatrix4fv(u_w, 1, GL_FALSE, wf.data());
        ColorRGBA c;
        if (for_picking) {   // the instance id in RGB
            unsigned id = it.object_id;
            c = ColorRGBA(float(id & 0xFF) / 255.f, float((id >> 8) & 0xFF) / 255.f, float((id >> 16) & 0xFF) / 255.f, 1.f);
        } else {
            c = adjust_color(it.color);
            if (ban_light) c.a((255 - (it.extruder - 1)) / 255.0f);
        }
        glUniform4f(u_c, c.r(), c.g(), c.b(), c.a());
        // a mirrored instance flips the winding
        glFrontFace(it.matrix.matrix().block<3, 3>(0, 0).determinant() < 0 ? GL_CW : GL_CCW);
        glBindVertexArray(u.vao);
        glDrawArrays(GL_TRIANGLES, 0, u.count);
    }
    glBindVertexArray(0);
    glFrontFace(GL_CCW);
    glDisable(GL_DEPTH_TEST);
}

ThumbnailData render_locked(Meshes &meshes, const ThumbScene &scene, unsigned w, unsigned h, ThumbView view, bool for_picking, bool ban_light)
{
    ThumbnailData data;
    if (w == 0 || h == 0) return data;
    data.set(w, h);
    // render_thumbnail_framebuffer: a multisampled framebuffer resolved into a texture (no MSAA when the colours are ids)
    const bool msaa = !for_picking && !ban_light;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    const GLsizei samples = msaa ? std::min<GLint>(4, max_samples) : 0;
    GLuint fbo, color, depth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        render_into(meshes, scene, data, view, for_picking, ban_light);
        if (samples > 0) {
            GLuint resolve, tex;
            glGenFramebuffers(1, &resolve);
            glBindFramebuffer(GL_FRAMEBUFFER, resolve);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data.pixels.data());
            glDeleteTextures(1, &tex);
            glDeleteFramebuffers(1, &resolve);
        } else
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data.pixels.data());
    } else
        TLOG("thumbnail framebuffer incomplete: 0x%x, gl error 0x%x, %ux%u samples %d, context %p renderer %s", status, glGetError(), w, h, samples,
             eglGetCurrentContext(), (const char *) glGetString(GL_RENDERER));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &color);
    glDeleteRenderbuffers(1, &depth);
    glDeleteFramebuffers(1, &fbo);
    return data;
}

} // namespace

// Every render runs on one thread that owns the EGL context: the context never moves between threads (the emulator's
// EGL produced garbage when the same context was made current on the slicing and the engine threads in turn).
namespace {
class GLWorker
{
public:
    static GLWorker &get() { static GLWorker w; return w; }
    template<class F> auto run(F &&f) -> decltype(f())
    {
        std::packaged_task<decltype(f())()> task(std::forward<F>(f));
        auto fut = task.get_future();
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_jobs.push_back([&task] { task(); });
        }
        m_cv.notify_one();
        return fut.get();
    }

private:
    GLWorker() : m_thread([this] { loop(); }) { m_thread.detach(); }
    void loop()
    {
        bool ok = egl_begin();
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(m_mutex);
                m_cv.wait(l, [this] { return !m_jobs.empty(); });
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            if (!ok) ok = egl_begin();
            job();
        }
    }
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    std::deque<std::function<void()>> m_jobs;
    std::thread                       m_thread;
};
} // namespace

ThumbnailData render_thumbnail(const ThumbScene &scene, unsigned w, unsigned h, ThumbView view, bool for_picking, bool ban_light)
{
    return GLWorker::get().run([&] {
        if (g_ctx == EGL_NO_CONTEXT) return ThumbnailData();
        Meshes meshes;
        return render_locked(meshes, scene, w, h, view, for_picking, ban_light);
    });
}

ThumbnailsList render_thumbnails(const ThumbScene &scene, const ThumbnailsParams &params)
{
    return GLWorker::get().run([&] {
        ThumbnailsList out;
        if (g_ctx == EGL_NO_CONTEXT) return out;
        Meshes meshes;
        for (const Vec2d &size : params.sizes) {
            Point isize(size);   // round to ints
            ThumbnailData d = render_locked(meshes, scene, unsigned(isize.x()), unsigned(isize.y()), ThumbView::Iso, false, false);
            if (d.is_valid()) out.push_back(std::move(d));
        }
        return out;
    });
}

} // namespace orca
