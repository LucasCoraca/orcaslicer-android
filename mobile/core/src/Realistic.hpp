// Realistic View (GLCanvas3D: _render_shadows, _render_ssao_pass, the phong / gouraud shadow uniforms) on GLES.
// GL thread only, besides update().
#pragma once
#include <atomic>
#include <functional>
#include <vector>

#include <Eigen/Geometry>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

namespace orca {

class Realistic
{
public:
    static Realistic &get();

    // the app config keys (SETTING_OPENGL_REALISTIC_*, SETTING_OPENGL_PHONG_SSAO), read on change
    void update();

    // GLCanvas3D::_is_realistic_view_enabled / _is_ssao_enabled / _shadow_mode (0 off, 1 static, 2 orbit)
    bool enabled(bool preview) const { return m_mode && (!preview || m_preview); }
    bool phong() const { return m_phong; }
    bool ssao(bool preview) const { return enabled(preview) && m_ssao; }
    int  shadow_mode(bool preview) const { return enabled(preview) ? m_shadows.load() : 0; }

    // _render_shadows' map: fits the light frustum to the casters and their shadow on the plate, then draws them
    // through draw(light_view, light_proj, eye) into the depth map. cache_key != 0 keeps a static light's map.
    using DrawCasters = std::function<void(const Eigen::Matrix4f &, const Eigen::Matrix4f &, const Slic3r::Vec3d &)>;
    void build_shadow_map(bool preview, const Slic3r::BoundingBoxf3 &casters, const Slic3r::BoundingBoxf3 &plate,
                          const Eigen::Matrix4f &view, size_t cache_key, const DrawCasters &draw);
    void invalidate() { m_valid = false; }
    bool shadow_valid() const { return m_valid; }
    const Eigen::Matrix4f &light_vp() const { return m_light_vp; }
    float texel() const { return 1.f / float(SIZE); }
    unsigned texture() const { return m_tex; }

    // the object shaders' shadow / light uniforms (_render_objects): shadow_map on unit 4, static_light_dir
    void set_object_uniforms(unsigned prog, bool preview, const Eigen::Matrix4f &view);
    // _static_light_dir_eye
    bool static_light_eye(bool preview, const Eigen::Matrix4f &view, Slic3r::Vec3f &dir) const;

    // the unified plate shadow over the drawn beds: triangles (x y z) of the plate masks
    void draw_plate_shadow(const std::vector<float> &mask, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj);

    // SSAO: the scene goes to an offscreen colour + depth target, then the pass composites it to the screen
    void begin_scene(bool preview, int width, int height);
    void end_scene(const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj, float z_far);

    void gl_created();   // a new EGL context: the names are gone

private:
    static constexpr unsigned SIZE = 2048;
    std::atomic<bool> m_mode{false}, m_phong{true}, m_preview{false}, m_ssao{false};
    std::atomic<int>  m_shadows{0};

    unsigned        m_tex{0}, m_fbo{0};
    bool            m_valid{false};
    size_t          m_key{0};
    Eigen::Matrix4f m_light_vp{Eigen::Matrix4f::Identity()};
    float           m_bounds[4]{0, 0, 0, 0};

    unsigned        m_ssao_fbo{0}, m_ssao_color{0}, m_ssao_depth{0}, m_quad_vao{0}, m_quad_vbo{0};
    int             m_ssao_w{0}, m_ssao_h{0};
    int             m_prev_fbo{0};
    bool            m_in_ssao{false};
};

} // namespace orca
