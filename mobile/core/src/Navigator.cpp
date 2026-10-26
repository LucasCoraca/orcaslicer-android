// The 3D navigator (GLCanvas3D::_render_3d_navigator): ImGuizmo's view cube, compiled from deps_src with imgui and
// drawn by a small GLES renderer of ImGui's draw lists (resources/shaders/ES/imgui.*). The navigator body is upstream's,
// extracted onto a stand-in canvas whose camera maps the full view rotation to the core's azimuth / elevation camera.
#include <GLES3/gl3.h>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#include "Core.hpp"
#include "Gizmos.hpp"
#include "Shaders.hpp"
#include "Viewport.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "libslic3r/Color.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;

namespace orca {

namespace nav {

static constexpr float NAVIGATOR_SIZE = 128.f;
static const float cameraProjection[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};

inline ImVec4 to_imvec4(const ColorRGBA &c) { return ImVec4(c.r(), c.g(), c.b(), c.a()); }

struct NavCanvas;
// Camera of GLCanvas3D: the view rotation and the type, on the core's camera
struct NavCamera
{
    enum class EType { Unknown, Perspective, Ortho };
    orca::Camera &cam;
    int          &type_request;   // 1 ortho, 2 perspective
    Eigen::Quaterniond get_view_rotation() const { return Eigen::Quaterniond(Eigen::Matrix3d(cam.view().block<3, 3>(0, 0))); }
    // Camera::set_rotation: the eye direction of the rotation (the core's camera keeps Z up)
    void set_rotation(const Transform3d &m)
    {
        const Vec3d f = -Vec3d(m.matrix()(2, 0), m.matrix()(2, 1), m.matrix()(2, 2));   // forward = -row 2
        const Vec3d d = -f;                                                              // target -> eye
        cam.elevation = std::clamp(Geometry::rad2deg(std::asin(std::clamp(d.z(), -1.0, 1.0))), -89.9, 89.9);
        if (std::abs(d.z()) < 0.9999) cam.azimuth = Geometry::rad2deg(std::atan2(d.x(), -d.y()));
        else {   // straight down / up: the heading is in the up row (Camera::view's up at the poles)
            const Vec3d u(m.matrix()(1, 0), m.matrix()(1, 1), m.matrix()(1, 2));
            cam.azimuth = d.z() > 0 ? Geometry::rad2deg(std::atan2(-u.x(), u.y())) : Geometry::rad2deg(std::atan2(u.x(), -u.y()));
        }
    }
    void auto_type(EType t) { type_request = t == EType::Ortho ? 1 : 2; }
};
using Camera = NavCamera;

struct NavCanvas
{
    std::array<float, 2> m_canvas_toolbar_pos{0.f, 0.f};
    bool                 m_navigator_dragging{false};
    bool                 m_moving{false};
    bool                 m_is_dark{false};
    float                scale{1.f};
    bool                 shown{true};
    bool                 gizmo_drag{false};
    orca::Camera         cam;
    int                  type_request{0};
    bool                 extra_frame{false};

    float get_scale() const { return scale; }
    void  request_extra_frame() { extra_frame = true; }
    void  _render_3d_navigator();
};
#include "slic3r/GUI/Navigator.inc"

} // namespace nav

// the navigator in the current ImGui frame (ImGuiOverlay): changes the camera; true while it needs more frames
bool navigator_content(orca::Camera &cam, bool dark, float scale, bool &ortho_request, bool &persp_request)
{
    using namespace nav;
    static NavCanvas c;
    c.cam          = cam;
    c.m_is_dark    = dark;
    c.scale        = scale;
    c.type_request = 0;
    c.extra_frame  = false;
    c._render_3d_navigator();
    cam.azimuth   = c.cam.azimuth;
    cam.elevation = c.cam.elevation;
    ortho_request = c.type_request == 1;
    persp_request = c.type_request == 2;
    return c.extra_frame || c.m_navigator_dragging;
}

// whether a press at (x, y) is in the navigator's box (bottom left, NAVIGATOR_SIZE)
bool navigator_hit(float x, float y, int canvas_h, float scale)
{
    const float size = nav::NAVIGATOR_SIZE * scale;
    return x >= 0 && x <= size && y >= canvas_h - size && y <= canvas_h;
}

} // namespace orca
