// Realistic View: GLCanvas3D::_render_shadows (the depth shadow map and the unified plate shadow), the shadow and
// static light uniforms of _render_objects, and _render_ssao_pass, on GLES 3 (no CLAMP_TO_BORDER, no depth copy).
#include "Realistic.hpp"

#include <GLES3/gl3.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

#include "Core.hpp"
#include "Shaders.hpp"
#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

namespace orca {

// Above the front left of the plate.
static const Vec3d STATIC_LIGHT_DIR = Vec3d(-0.4, -0.6, 1.0).normalized();
// the light used for shading, fixed to the camera (LIGHT_TOP_DIR)
static const Vec3d LIGHT_DIR_EYE = Vec3d(-0.4574957, 0.4574957, 0.7624929).normalized();

Realistic &Realistic::get()
{
    static Realistic r;
    return r;
}

void Realistic::update()
{
    const AppConfig &cfg = *Core::get().app_config;
    m_mode    = cfg.get_bool(SETTING_OPENGL_REALISTIC_MODE);
    m_phong   = cfg.get_bool(SETTING_OPENGL_REALISTIC_PHONG);
    m_preview = cfg.get_bool(SETTING_OPENGL_REALISTIC_PREVIEW);
    m_ssao    = cfg.get_bool(SETTING_OPENGL_PHONG_SSAO);
    const std::string mode = cfg.get(SETTING_OPENGL_REALISTIC_SHADOWS);
    m_shadows = mode == "static" ? 1 : mode == "orbit" ? 2 : 0;
    m_key     = 0;
}

void Realistic::gl_created()
{
    m_tex = m_fbo = m_ssao_fbo = m_ssao_color = m_ssao_depth = m_quad_vao = m_quad_vbo = 0;
    m_ssao_w = m_ssao_h = 0;
    m_valid = false;
    m_key = 0;
}

bool Realistic::static_light_eye(bool preview, const Eigen::Matrix4f &view, Vec3f &dir) const
{
    if (shadow_mode(preview) != 1) return false;
    dir = (view.block<3, 3>(0, 0) * STATIC_LIGHT_DIR.cast<float>()).normalized();
    return true;
}

void Realistic::build_shadow_map(bool preview, const BoundingBoxf3 &obj_bb, const BoundingBoxf3 &plate, const Eigen::Matrix4f &view,
                                 size_t cache_key, const DrawCasters &draw)
{
    m_valid = false;
    const int mode = shadow_mode(preview);
    if (mode == 0 || !obj_bb.defined) return;
    const Matrix3d view_rot = view.block<3, 3>(0, 0).cast<double>();
    const Vec3d dir_to_light = mode == 1 ? STATIC_LIGHT_DIR : Vec3d((view_rot.transpose() * LIGHT_DIR_EYE).normalized());

    // Orthographic light-space basis (z points toward the light).
    const Vec3d up     = (std::abs(dir_to_light.z()) > 0.99) ? Vec3d::UnitY() : Vec3d::UnitZ();
    const Vec3d z_axis = dir_to_light;
    const Vec3d x_axis = up.cross(z_axis).normalized();
    const Vec3d y_axis = z_axis.cross(x_axis).normalized();
    // Fit the frustum to the object AABB *and* the object's shadow projected onto the plate (clamped to the plate)
    const Vec3d ray_dir = -dir_to_light;
    const BoundingBoxf3 plate_bb = plate.defined ? plate : obj_bb;
    Vec3d lmin(DBL_MAX, DBL_MAX, DBL_MAX), lmax(-DBL_MAX, -DBL_MAX, -DBL_MAX);
    auto enclose = [&](const Vec3d &p) {
        const Vec3d lp(x_axis.dot(p), y_axis.dot(p), z_axis.dot(p));
        lmin = lmin.cwiseMin(lp);
        lmax = lmax.cwiseMax(lp);
    };
    Vec2d reach_min(DBL_MAX, DBL_MAX), reach_max(-DBL_MAX, -DBL_MAX);
    auto reach = [&](const Vec3d &p) {
        reach_min = reach_min.cwiseMin(Vec2d(p.x(), p.y()));
        reach_max = reach_max.cwiseMax(Vec2d(p.x(), p.y()));
    };
    for (int i = 0; i < 8; ++i) {
        const Vec3d corner((i & 1) ? obj_bb.max.x() : obj_bb.min.x(), (i & 2) ? obj_bb.max.y() : obj_bb.min.y(),
                           (i & 4) ? obj_bb.max.z() : obj_bb.min.z());
        enclose(corner);
        reach(corner);
        if (ray_dir.z() < -1e-6) {
            const double t = -corner.z() / ray_dir.z();
            Vec3d s = corner + t * ray_dir;
            s.x() = std::min(std::max(s.x(), plate_bb.min.x()), plate_bb.max.x());
            s.y() = std::min(std::max(s.y(), plate_bb.min.y()), plate_bb.max.y());
            s.z() = 0.0;
            enclose(s);
            reach(s);
        }
    }
    // Light "camera" placed just past the nearest enclosed point, looking toward the scene.
    const double range  = lmax.z() - lmin.z();
    const double margin = std::max(1.0, 0.05 * range);
    const double cx = 0.5 * (lmin.x() + lmax.x()), cy = 0.5 * (lmin.y() + lmax.y());
    const Vec3d  eye = x_axis * cx + y_axis * cy + z_axis * (lmax.z() + margin);
    Eigen::Matrix4d light_view = Eigen::Matrix4d::Identity();
    light_view.block<1, 3>(0, 0) = x_axis.transpose();
    light_view.block<1, 3>(1, 0) = y_axis.transpose();
    light_view.block<1, 3>(2, 0) = z_axis.transpose();
    light_view(0, 3) = -x_axis.dot(eye);
    light_view(1, 3) = -y_axis.dot(eye);
    light_view(2, 3) = -z_axis.dot(eye);
    const double halfx  = std::max(0.5 * (lmax.x() - lmin.x()), 1.0) * 1.02;
    const double halfy  = std::max(0.5 * (lmax.y() - lmin.y()), 1.0) * 1.02;
    const double near_z = margin * 0.5;
    const double far_z  = range + margin * 1.5;
    Eigen::Matrix4d light_proj = Eigen::Matrix4d::Identity();
    light_proj(0, 0) = 1.0 / halfx;
    light_proj(1, 1) = 1.0 / halfy;
    light_proj(2, 2) = -2.0 / (far_z - near_z);
    light_proj(2, 3) = -(far_z + near_z) / (far_z - near_z);
    m_light_vp = (light_proj * light_view).cast<float>();

    // Padded by the filter's 2 texel reach, stretched where the light grazes the plate.
    const double reach_margin = 3.0 * 2.0 * std::max(halfx, halfy) / SIZE / std::max(0.1, std::abs(dir_to_light.z()));
    m_bounds[0] = float(reach_min.x() - reach_margin);
    m_bounds[1] = float(reach_min.y() - reach_margin);
    m_bounds[2] = float(reach_max.x() + reach_margin);
    m_bounds[3] = float(reach_max.y() + reach_margin);

    if (m_tex == 0) {
        glGenTextures(1, &m_tex);
        glBindTexture(GL_TEXTURE_2D, m_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // GLES 3.0 has no CLAMP_TO_BORDER; the frustum encloses every receiver, so the edge texel is never sampled
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SIZE, SIZE, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &m_fbo);
        m_key = 0;
    }

    // A static light keeps the map until its frustum or the casters change.
    if (mode == 1) {
        for (int i = 0; i < 16; ++i) cache_key ^= std::hash<float>()(m_light_vp.data()[i]) + 0x9e3779b9 + (cache_key << 6) + (cache_key >> 2);
    } else
        cache_key = 0;
    if (cache_key != 0 && cache_key == m_key) { m_valid = true; return; }
    m_key = 0;

    GLint prev_viewport[4], prev_fbo = 0;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    const GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_tex, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, SIZE, SIZE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(4.0f, 4.0f);
        glDisable(GL_CULL_FACE);
        draw(light_view.cast<float>(), light_proj.cast<float>(), eye);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        m_valid = true;
        m_key   = cache_key;
    }
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

void Realistic::set_object_uniforms(unsigned prog, bool preview, const Eigen::Matrix4f &view)
{
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    glUniform1i(U("enable_ssao"), m_ssao ? 1 : 0);
    glUniform1i(U("lighting_model"), 0);
    if (m_valid && m_tex != 0) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(U("shadow_map"), 4);
        glUniformMatrix4fv(U("shadow_light_vp"), 1, GL_FALSE, m_light_vp.data());
        glUniform1f(U("shadow_map_texel"), texel());
        glUniform1f(U("shadow_intensity"), 0.35f);
    } else {
        glUniform1i(U("shadow_map"), 4);   // unit 4 holds no texture: the samplers stay off unit 0 (3DScene.cpp)
        glUniform1f(U("shadow_intensity"), 0.f);
    }
    Vec3f dir;
    const bool stat = static_light_eye(preview, view, dir);
    glUniform1i(U("use_static_light"), stat ? 1 : 0);
    if (stat) glUniform3f(U("static_light_dir"), dir.x(), dir.y(), dir.z());
}

void Realistic::draw_plate_shadow(const std::vector<float> &mask, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    if (!m_valid || mask.empty()) return;
    GLuint prog = gl::orca_program("printbed_shadow");
    if (!prog) return;
    // Blend the shadow over the already-drawn plate: depth test on, depth writes off, lifted a little off the bed
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(prog);
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    glUniformMatrix4fv(U("view_model_matrix"), 1, GL_FALSE, view.data());
    glUniformMatrix4fv(U("projection_matrix"), 1, GL_FALSE, proj.data());
    glUniform1i(U("shadow_map"), 4);
    glUniformMatrix4fv(U("shadow_light_vp"), 1, GL_FALSE, m_light_vp.data());
    glUniform1f(U("shadow_intensity"), 0.35f);
    glUniform1f(U("shadow_map_texel"), texel());
    glUniform4fv(U("shadow_bounds"), 1, m_bounds);
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, mask.size() * sizeof(float), mask.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *) 0);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(mask.size() / 3));
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
}

void Realistic::begin_scene(bool preview, int width, int height)
{
    m_in_ssao = false;
    if (!ssao(preview) || width <= 0 || height <= 0) return;
    if (m_ssao_fbo == 0) {
        glGenFramebuffers(1, &m_ssao_fbo);
        glGenTextures(1, &m_ssao_color);
        glGenTextures(1, &m_ssao_depth);
        m_ssao_w = m_ssao_h = 0;
    }
    if (m_ssao_w != width || m_ssao_h != height) {
        glBindTexture(GL_TEXTURE_2D, m_ssao_color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        // depth with stencil: the selection outline marks the stencil in the scene pass
        glBindTexture(GL_TEXTURE_2D, m_ssao_depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_ssao_w = width;
        m_ssao_h = height;
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_ssao_depth, 0);
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(m_prev_fbo));
        return;
    }
    m_in_ssao = true;
}

void Realistic::end_scene(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj, float z_far)
{
    if (!m_in_ssao) return;
    m_in_ssao = false;
    // the scene's depth goes along, for what is drawn over it (upstream's pass reads and writes the same framebuffer)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ssao_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(m_prev_fbo));
    glBlitFramebuffer(0, 0, m_ssao_w, m_ssao_h, 0, 0, m_ssao_w, m_ssao_h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(m_prev_fbo));
    GLuint prog = gl::orca_program("ssao");
    if (!prog) return;
    if (m_quad_vao == 0) {
        // GLCanvas3D::Background's full screen quad: x y z u v
        const float quad[] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, 1, 1, 0, 1, 1, -1, -1, 0, 0, 0, 1, 1, 0, 1, 1, -1, 1, 0, 0, 1};
        glGenVertexArrays(1, &m_quad_vao);
        glBindVertexArray(m_quad_vao);
        glGenBuffers(1, &m_quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *) 0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 20, (void *) 12);
        glBindVertexArray(0);
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(prog);
    auto U = [prog](const char *n) { return glGetUniformLocation(prog, n); };
    const Eigen::Matrix4f id = Eigen::Matrix4f::Identity();
    glUniformMatrix4fv(U("view_model_matrix"), 1, GL_FALSE, id.data());
    glUniformMatrix4fv(U("projection_matrix"), 1, GL_FALSE, id.data());
    glUniform1i(U("color_texture"), 0);
    glUniform1i(U("depth_texture"), 1);
    glUniform2f(U("inv_tex_size"), 1.f / float(m_ssao_w), 1.f / float(m_ssao_h));
    glUniform1f(U("z_far"), z_far);
    glUniform1i(U("is_outline"), 0);
    // the surface normal comes from the depth buffer: unproject a pixel, compare it with world +Z in view space
    const Eigen::Matrix4f inv_proj = proj.inverse();
    glUniformMatrix4fv(U("inv_projection_matrix"), 1, GL_FALSE, inv_proj.data());
    const Eigen::Vector3f up_view = (view.block<3, 3>(0, 0) * Eigen::Vector3f::UnitZ()).normalized();
    glUniform3f(U("up_view"), up_view.x(), up_view.y(), up_view.z());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssao_color);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_ssao_depth);
    glBindVertexArray(m_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_DEPTH_TEST);
}

} // namespace orca
