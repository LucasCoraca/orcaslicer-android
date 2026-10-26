#include <atomic>
#include <cfloat>
#include "Gizmos.hpp"

#include <GLES3/gl3.h>
#include <cmath>
#include <numeric>

#include "Core.hpp"
#include "Plater.hpp"
#include "Viewport.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/LayOnFace.hpp"
#include "libslic3r/Tesselate.hpp"

using namespace Slic3r;

namespace orca {

// GLGizmoBase colours
static const ColorRGBA AXES_COLOR[3] = {ColorRGBA::X(), ColorRGBA::Y(), ColorRGBA::Z()};
static const ColorRGBA AXES_HOVER_COLOR[3] = {
    {ColorRGBA::X().r() * 1.2f, ColorRGBA::X().g() * 1.4f, ColorRGBA::X().b() * 1.4f, 1.0f},
    {ColorRGBA::Y().r() * 1.2f, ColorRGBA::Y().g() * 1.2f, ColorRGBA::Y().b() * 1.2f, 1.0f},
    {ColorRGBA::Z().r() * 1.2f, ColorRGBA::Z().g() * 1.2f, ColorRGBA::Z().b() * 1.2f, 1.0f}};
static const ColorRGBA DEFAULT_DRAG_COLOR = {1.0f, 1.0f, 1.0f, 1.0f};
static const ColorRGBA GRABBER_UNIFORM_COL = {0, 1.0, 1.0, 1.0f};
static const ColorRGBA GRABBER_UNIFORM_HOVER_COL = {0, 0.7, 0.7, 1.0f};
static const ColorRGBA FLATTEN_COLOR = {0.96f, 0.93f, 0.93f, 0.5f};
static const ColorRGBA FLATTEN_HOVER_COLOR = {1.0f, 1.0f, 1.0f, 0.75f};

static const double MOVE_SPACE = 20.;                 // GLGizmoMove3D::on_render: space_size = 20 * INV_ZOOM
static const double ROTATE_OFFSET = 5.;               // GLGizmoRotate::Offset
static const unsigned ROTATE_SCALE_STEPS = 72;        // ScaleStepsCount
static const unsigned ROTATE_SCALE_LONG_EVERY = 2;
static const double ROTATE_SCALE_LONG_TOOTH = 0.1;
static const unsigned ROTATE_SNAP_REGIONS = 8;
// wxGetKeyState(WXK_SHIFT): a hardware keyboard's Shift, held while dragging (the app reports it, Jni vpShift)
std::atomic<bool> shift_down{false};
static const double ROTATE_GRABBER_OFFSET = 0.15;
static const unsigned ROTATE_ANGLE_RESOLUTION = 64;

static const std::shared_ptr<const TriangleMesh> &cube_mesh()
{
    static std::shared_ptr<const TriangleMesh> m = [] {
        indexed_triangle_set its = its_make_cube(1.0, 1.0, 1.0);
        its_translate(its, -0.5f * Vec3f::Ones());
        return std::make_shared<const TriangleMesh>(std::move(its));
    }();
    return m;
}

static const std::shared_ptr<const TriangleMesh> &cone_mesh()
{
    static std::shared_ptr<const TriangleMesh> m = std::make_shared<const TriangleMesh>(its_make_cone(1.0, 1.0, double(PI) / 18.0));
    return m;
}

// the circle planes of GLGizmoRotate::local_transform: (radial, tangent, axis)
static void rotate_basis(int axis, Vec3d &u, Vec3d &v, Vec3d &n)
{
    switch (axis) {
    case 0: u = -Vec3d::UnitY(); v = -Vec3d::UnitZ(); break;
    case 1: u = Vec3d::UnitZ(); v = Vec3d::UnitX(); break;
    default: u = Vec3d::UnitX(); v = Vec3d::UnitY(); break;
    }
    n = u.cross(v);
}

Gizmos &Gizmos::get()
{
    static Gizmos g;
    return g;
}

void Gizmos::set_active(const std::string &name)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_active = name;
    m_drag = Drag();
    m_planes_obj = -1;
    m_planes.clear();
    m_pressed_plane = -1;
    m_cut_edit = CutEdit();
    if (name != "cut") m_cut_line_mode = false;
}

std::string Gizmos::active()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_active;
}

double Gizmos::inv_zoom(const Camera &cam) const
{
    return 2 * cam.distance * std::tan(cam.fov * M_PI / 360.) / std::max(1, cam.height);
}

Gizmos::SelBox Gizmos::selection_box(const std::vector<RenderVolume> &vols, unsigned revision)
{
    if (revision == m_box_revision) return m_box;
    SelBox s;
    for (const RenderVolume &r : vols)
        if (r.selected) s.box.merge(r.bbox);
    if (!s.box.defined) { m_box = s; m_box_revision = revision; return s; }
    s.valid = true;
    s.center = s.box.center();
    // Selection::get_bounding_sphere: the smallest ball around the vertices; centred on the box here
    double r2 = 0;
    for (const RenderVolume &r : vols)
        if (r.selected)
            for (const Vec3f &v : r.mesh->its.vertices) r2 = std::max(r2, (r.matrix * v.cast<double>() - s.center).squaredNorm());
    s.sphere = std::sqrt(r2);
    m_box = s;
    m_box_revision = revision;
    return s;
}

Transform3d Gizmos::drag_transform()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return transform_locked();
}

Transform3d Gizmos::transform_locked() const
{
    if (m_ring.dragging) {   // GLGizmoEmboss::on_dragging: selection.rotate around the volume's Z
        const Vec3d c = m_ring.frame.translation(), n = m_ring.frame.linear().col(2);
        return Geometry::translation_transform(c) * Transform3d(Eigen::AngleAxisd(m_ring.value, n)) * Geometry::translation_transform(-c);
    }
    const Drag &d = m_drag;
    if (d.grabber < 0 || m_active == "cut") return Transform3d::Identity();
    if (m_active == "move") {
        Vec3d t = Vec3d::Zero();
        t[d.grabber] = d.value;
        return Geometry::translation_transform(t);
    }
    if (m_active == "rotate") {
        Vec3d u, v, n;
        rotate_basis(d.grabber, u, v, n);
        return Geometry::translation_transform(d.start.center) * Transform3d(Eigen::AngleAxisd(d.value, n)) *
               Geometry::translation_transform(-d.start.center);
    }
    if (m_active == "scale") {
        // the pivot is the bottom centre (calc_ratio's plane_center), the result stays on the bed like do_scale()
        Vec3d s = Vec3d::Ones();
        if (d.grabber < 2) s.x() = d.value;
        else if (d.grabber < 4) s.y() = d.value;
        else if (d.grabber < 6) s.z() = d.value;
        else s = Vec3d::Constant(d.value);
        Vec3d pivot(d.start.center.x(), d.start.center.y(), d.start.box.min.z());
        return Geometry::translation_transform(pivot) * Geometry::scale_transform(s) * Geometry::translation_transform(-pivot);
    }
    return Transform3d::Identity();
}

// ---------------------------------------------------------------------------------------------------------------
// rendering

namespace {

struct Draw
{
    const Eigen::Matrix4f &view, &proj;
    Eigen::Matrix4f        vp;

    void lines(const std::vector<Vec3d> &pts, const ColorRGBA &c, unsigned mode = GL_LINES) const
    {
        std::vector<float> xyz;
        xyz.reserve(pts.size() * 3);
        for (const Vec3d &p : pts) xyz.insert(xyz.end(), {float(p.x()), float(p.y()), float(p.z())});
        gl_draw_flat(xyz, mode, vp, c, 2.f);
    }
    void mesh(const std::shared_ptr<const TriangleMesh> &m, const Transform3d &t, const ColorRGBA &c) const
    {
        float col[4] = {c.r(), c.g(), c.b(), c.a()};
        gl_draw_lit(m, t.matrix().cast<float>(), col, view, proj);
    }
};

} // namespace

// GLGizmoRotate::on_render of one circle: (u, v) its plane, n its axis; the grabber at angle0 + value (the angle dragged)
static void render_rotate_ring(const Draw &dr, const Vec3d &center, const Vec3d &u, const Vec3d &v, const Vec3d &n, double R,
                               const ColorRGBA &axis_color, const ColorRGBA &hover_color, bool dragging, double angle0, double value,
                               double grabber, double ext)
{
    auto at = [&](double a, double r) { return center + r * (std::cos(a) * u + std::sin(a) * v); };
    const ColorRGBA color = dragging ? DEFAULT_DRAG_COLOR : axis_color;
    std::vector<Vec3d> circle;
    for (unsigned k = 0; k < ROTATE_SCALE_STEPS; ++k) circle.push_back(at(k * 2 * PI / ROTATE_SCALE_STEPS, R));
    dr.lines(circle, color, GL_LINE_LOOP);
    const double angle = angle0 + (dragging ? value : 0);
    if (dragging) {
        std::vector<Vec3d> ticks;   // render_scale
        for (unsigned k = 0; k < ROTATE_SCALE_STEPS; ++k) {
            double a = k * 2 * PI / ROTATE_SCALE_STEPS;
            ticks.push_back(at(a, R));
            ticks.push_back(at(a, k % ROTATE_SCALE_LONG_EVERY == 0 ? R * (1 + ROTATE_SCALE_LONG_TOOTH) : R * (1 + 0.5 * ROTATE_SCALE_LONG_TOOTH)));
        }
        for (unsigned k = 0; k < ROTATE_SNAP_REGIONS; ++k) {   // render_snap_radii
            double a = k * 2 * PI / ROTATE_SNAP_REGIONS;
            ticks.push_back(at(a, R / 3));
            ticks.push_back(at(a, 2 * R / 3));
        }
        ticks.push_back(center);   // render_reference_radius
        ticks.push_back(at(angle0, R * (1 + ROTATE_GRABBER_OFFSET)));
        dr.lines(ticks, color);
        std::vector<Vec3d> arc;    // render_angle_arc
        for (unsigned k = 0; k <= ROTATE_ANGLE_RESOLUTION; ++k) arc.push_back(at(angle0 + value * k / ROTATE_ANGLE_RESOLUTION, R * (1 + ROTATE_GRABBER_OFFSET)));
        dr.lines(arc, axis_color, GL_LINE_STRIP);
    }
    Vec3d g = at(angle, R * (1 + ROTATE_GRABBER_OFFSET));
    dr.lines({center, g}, dragging ? DEFAULT_DRAG_COLOR : ColorRGBA(.6f, .6f, .6f, 1.f));
    // the grabber: a cube with cones along the tangent (EGrabberExtension PosY | NegY)
    Eigen::Matrix3d frame;
    frame.col(0) = std::cos(angle) * u + std::sin(angle) * v;
    frame.col(1) = -std::sin(angle) * u + std::cos(angle) * v;
    frame.col(2) = n;
    Transform3d base = Transform3d::Identity();
    base.translate(g);
    base.rotate(frame);
    const ColorRGBA gc = dragging ? hover_color : axis_color;
    dr.mesh(cube_mesh(), base * Geometry::scale_transform(grabber), gc);
    Vec3d es(0.75 * ext, 0.75 * ext, 3.0 * ext);
    dr.mesh(cone_mesh(), base * Geometry::assemble_transform(2.0 * ext * Vec3d::UnitY(), Vec3d(-0.5 * PI, 0, 0), es), gc);
    dr.mesh(cone_mesh(), base * Geometry::assemble_transform(-2.0 * ext * Vec3d::UnitY(), Vec3d(0.5 * PI, 0, 0), es), gc);
}

void Gizmos::render(const std::vector<RenderVolume> &vols, unsigned revision, const Camera &cam, const Eigen::Matrix4f &view, const Eigen::Matrix4f &proj)
{
    std::lock_guard<std::mutex> l(m_mutex);
    Draw dr{view, proj, proj * view};
    if (m_ring.on && (!m_ring.hidden || m_ring.dragging)) {
        // GLGizmoEmboss::on_render: m_rotate_gizmo.render() over the scene
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        const double iz = inv_zoom(cam);
        const Matrix3d rot = m_ring.frame.linear();
        render_rotate_ring(dr, m_ring.frame.translation(), rot.col(0), rot.col(1), rot.col(2), m_ring.radius, AXES_COLOR[2],
                           ColorRGBA(.6f, .6f, .6f, .3f), m_ring.dragging, 0.5 * PI, m_ring.value, grabber_px() * iz, 0.75 * grabber_px() * iz);
    }
    if (m_active.empty()) return;

    if (m_active == "flatten") {
        // GLGizmoFlatten::on_render
        if (m_planes.empty()) return;
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        Eigen::Matrix4f mvp = dr.vp * m_planes_matrix.matrix().cast<float>();
        for (size_t i = 0; i < m_planes.size(); ++i) {
            std::vector<float> xyz;
            const auto &o = m_planes[i].outline;
            for (size_t k = 1; k + 1 < o.size(); ++k)
                for (const Vec3d *p : {&o[0], &o[k], &o[k + 1]}) xyz.insert(xyz.end(), {float(p->x()), float(p->y()), float(p->z())});
            gl_draw_flat(xyz, GL_TRIANGLES, mvp, int(i) == m_pressed_plane ? FLATTEN_HOVER_COLOR : FLATTEN_COLOR);
        }
        glDisable(GL_BLEND);
        return;
    }

    if (m_active == "cut") {
        // GLGizmoCut3D::render_cut_plane + render_cut_plane_grabbers
        if (!m_cut.valid) return;
        const double iz = inv_zoom(cam);
        const Transform3d base = Geometry::translation_transform(m_cut.center) * m_cut.rotation;
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        // its_make_frustum_dowel(1.5 * radius, plate thickness, 4): the square plate, CUT_PLANE_DEF_COLOR
        static std::shared_ptr<const TriangleMesh> plate;
        static double plate_r = -1;
        const double r = 1.5 * m_cut.radius;
        if (!plate || plate_r != r) { plate = std::make_shared<const TriangleMesh>(its_make_frustum_dowel(r, 0.02 * 32 * iz, 4)); plate_r = r; }
        // CUT_PLANE_DEF_COLOR, CUT_PLANE_ERR_COLOR when the groove is invalid
        const ColorRGBA cp_clr = m_cut.valid_groove ? ColorRGBA(0.9f, 0.9f, 0.9f, 0.5f) : ColorRGBA(1.0f, 0.8f, 0.8f, 0.5f);
        // PartSelection::render: while editing connectors only the side facing away from the camera
        if (!m_cut_parts.empty()) {
            const Vec3d normal = (m_cut.rotation.linear() * Vec3d::UnitZ()).normalized();
            const bool is_looking_forward = (Vec3d(cam.target) - cam.eye()).normalized().dot(normal) < 0.05;
            glEnable(GL_CULL_FACE);
            for (const CutPart &cp : m_cut_parts) {
                if (!cp.modifier && m_cut_edit.editing && ((is_looking_forward && cp.upper) || (!is_looking_forward && !cp.upper))) continue;
                if (cp.modifier) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
                dr.mesh(cp.mesh, cp.matrix, cp.modifier ? ColorRGBA(0.75f, 0.75f, 0.75f, 0.5f) : cp.upper ? ColorRGBA::CYAN() : ColorRGBA::MAGENTA());
                if (cp.modifier) glDisable(GL_BLEND);
            }
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
        }
        if (!m_cut_edit.editing) dr.mesh(m_cut.groove && m_cut.groove_mesh ? m_cut.groove_mesh : plate, base, cp_clr);   // hidden while editing connectors
        glDisable(GL_BLEND);
        if (m_cut_edit.editing) {
            // ObjectClipper::render_cut: the section of the model parts, MeshClipper cap colour
            {
                const Transform3d from_plane = m_cut_edit.to_plane.inverse();
                std::vector<Vec3d> tris;
                for (const Vec2f &v : triangulate_expolygons_2f(m_cut_edit.section, true))
                    tris.push_back(from_plane * Vec3d(v.x(), v.y(), 0.02));   // a hair above the plane
                std::vector<float> xyz;
                for (const Vec3d &v : tris) xyz.insert(xyz.end(), {float(v.x()), float(v.y()), float(v.z())});
                glDisable(GL_CULL_FACE);
                gl_draw_flat(xyz, GL_TRIANGLES, dr.vp, ColorRGBA(0.8f, 0.3f, 0.0f, 1.0f));
            }
            // GLGizmoCut3D::render_connectors: on the cut plane, plugs yellow, dowels dark yellow, selected gray
            glEnable(GL_DEPTH_TEST);
            for (size_t i = 0; i < m_cut_edit.connectors.size(); ++i) {
                const CutConnector &c = m_cut_edit.connectors[i];
                const bool sel = i < m_cut_edit.selected.size() && m_cut_edit.selected[i];
                ColorRGBA col = c.attribs.type == CutConnectorType::Dowel ? (sel ? ColorRGBA::DARK_GRAY() : ColorRGBA::DARK_YELLOW())
                                                                          : (sel ? ColorRGBA::GRAY() : ColorRGBA::YELLOW());
                auto mesh = std::make_shared<const TriangleMesh>(cut_connector_mesh(c.attribs, m_cut_edit.snap_space, m_cut_edit.snap_bulge));
                Vec3d pos = c.pos + m_cut_edit.offset;
                float height = c.height;
                if (c.attribs.type == CutConnectorType::Dowel && c.attribs.style == CutConnectorStyle::Prism) {
                    pos -= height * (m_cut.rotation.linear() * Vec3d::UnitZ());   // a dowel crosses the plane
                    height *= 2;
                }
                Transform3d t = Geometry::translation_transform(pos) * m_cut.rotation * Geometry::rotation_transform(-c.z_angle * Vec3d::UnitZ()) *
                                Geometry::scale_transform(Vec3d(c.radius, c.radius, height));
                dr.mesh(mesh, t, col);
            }
            return;
        }
        glClear(GL_DEPTH_BUFFER_BIT);
        if (m_drag.grabber == 3) {   // render_cut_line
            if (!m_line_end.isApprox(m_line_beg)) dr.lines({m_line_beg, m_line_end}, ColorRGBA::YELLOW());
            return;
        }
        const double len = 0.5 * m_cut.radius;   // m_grabber_connection_len
        const double size = std::max(32.0 * iz * 0.05, 4.0 * iz) * 2.0 * m_ui_scale;   // get_half_size(get_grabber_mean_size), touch sized
        const int g = m_drag.grabber;
        auto to_world = [&](const Vec3d &v) { return Vec3d(base * v); };
        if (g < 0 || g == 0) {
            dr.lines({to_world(Vec3d::Zero()), to_world(len * Vec3d::UnitZ())}, ColorRGBA::YELLOW());
            static std::shared_ptr<const TriangleMesh> sphere = std::make_shared<const TriangleMesh>(its_make_sphere(1.0, PI / 12.0));
            dr.mesh(sphere, base * Geometry::translation_transform(len * Vec3d::UnitZ()) * Geometry::scale_transform(size),
                    g == 0 ? ColorRGBA::YELLOW() : ColorRGBA::GRAY());
        }
        const Vec3d cone_scale(0.75 * size, 0.75 * size, 1.8 * size);
        if (g < 0 || g == 1) {   // X: rotation around the plane's X axis
            dr.mesh(cone_mesh(), base * Geometry::translation_transform(Vec3d(0, 1.25 * size, len)) * Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitX()) * Geometry::scale_transform(cone_scale), ColorRGBA::X());
            dr.mesh(cone_mesh(), base * Geometry::translation_transform(Vec3d(0, -1.25 * size, len)) * Geometry::rotation_transform(0.5 * PI * Vec3d::UnitX()) * Geometry::scale_transform(cone_scale), ColorRGBA::X());
        }
        if (g < 0 || g == 2) {   // Y
            dr.mesh(cone_mesh(), base * Geometry::translation_transform(Vec3d(1.25 * size, 0, len)) * Geometry::rotation_transform(0.5 * PI * Vec3d::UnitY()) * Geometry::scale_transform(cone_scale), ColorRGBA::Y());
            dr.mesh(cone_mesh(), base * Geometry::translation_transform(Vec3d(-1.25 * size, 0, len)) * Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitY()) * Geometry::scale_transform(cone_scale), ColorRGBA::Y());
        }
        if (g == 1 || g == 2) dr.lines({to_world(Vec3d::Zero()), to_world(len * Vec3d::UnitZ())}, g == 1 ? ColorRGBA::X() : ColorRGBA::Y());
        return;
    }

    // the box of the selection as it is now (the drag preview moves / scales it)
    bool dragging = m_drag.grabber >= 0;
    SelBox box = dragging ? m_drag.start : selection_box(vols, revision);
    if (!box.valid) return;
    Transform3d live = transform_locked();
    const double iz = inv_zoom(cam);
    const double grabber = grabber_px() * iz, ext = 0.75 * grabber_px() * iz;

    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    if (m_active == "move") {
        // GLGizmoMove3D::on_render: axes from the centre, a cone grabber past each side of the box
        Vec3d c = live * box.center, half = box.box.size() * 0.5;
        for (int i = 0; i < 3; ++i) {
            if (dragging && m_drag.grabber != i) continue;
            Vec3d axis = Vec3d::Unit(i), g = c + axis * (half[i] + MOVE_SPACE * iz);
            dr.lines({c, g}, AXES_COLOR[i]);
            static const Vec3d angles[3] = {{0, 0.5 * PI, 0}, {-0.5 * PI, 0, 0}, {0, 0, 0}};
            Transform3d m = Geometry::assemble_transform(g, angles[i], Vec3d(0.75 * ext, 0.75 * ext, 2.0 * ext));
            dr.mesh(cone_mesh(), m, dragging ? AXES_HOVER_COLOR[i] : AXES_COLOR[i]);
        }
    } else if (m_active == "rotate") {
        // GLGizmoRotate3D::on_render: the three circles; while dragging only the dragged one, with its scale
        const double R = ROTATE_OFFSET + box.sphere;
        for (int i = 0; i < 3; ++i) {
            if (dragging && m_drag.grabber != i) continue;
            Vec3d u, v, n;
            rotate_basis(i, u, v, n);
            render_rotate_ring(dr, box.center, u, v, n, R, AXES_COLOR[i], AXES_HOVER_COLOR[i], dragging, 0, m_drag.value, grabber, ext);
        }
    } else if (m_active == "scale") {
        // GLGizmoScale3D::update_grabbers_data / on_render: the bottom rectangle, axis and corner grabbers
        BoundingBoxf3 b = box.box.transformed(live);
        Vec3d c = b.center(), h = b.size() * 0.5;
        Vec3d pts[10] = {c + Vec3d(-h.x(), 0, -h.z()), c + Vec3d(h.x(), 0, -h.z()), c + Vec3d(0, -h.y(), -h.z()), c + Vec3d(0, h.y(), -h.z()),
                         c + Vec3d(0, 0, -h.z()), c + Vec3d(0, 0, h.z()),
                         c + Vec3d(-h.x(), -h.y(), -h.z()), c + Vec3d(h.x(), -h.y(), -h.z()), c + Vec3d(h.x(), h.y(), -h.z()), c + Vec3d(-h.x(), h.y(), -h.z())};
        dr.lines({pts[6], pts[7], pts[7], pts[8]}, AXES_COLOR[1]);
        dr.lines({pts[8], pts[9], pts[9], pts[6]}, AXES_COLOR[0]);
        for (int i = 0; i < 10; ++i) {
            if (i == 4) continue;   // BBS: the bottom Z grabber is disabled
            bool hover = dragging && m_drag.grabber == i;
            ColorRGBA col = i < 6 ? (hover ? AXES_HOVER_COLOR[i / 2] : AXES_COLOR[i / 2]) : (hover ? GRABBER_UNIFORM_HOVER_COL : GRABBER_UNIFORM_COL);
            dr.mesh(cube_mesh(), Geometry::translation_transform(pts[i]) * Geometry::scale_transform(grabber), col);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// dragging

static Vec2d project(const Camera &cam, const Vec3d &w)
{
    Eigen::Matrix4d vp = cam.projection(1, 1000) * cam.view();
    Eigen::Vector4d c = vp * Eigen::Vector4d(w.x(), w.y(), w.z(), 1.0);
    if (c.w() <= 0) return Vec2d(-1e9, -1e9);
    return Vec2d((c.x() / c.w() * 0.5 + 0.5) * cam.width, (0.5 - c.y() / c.w() * 0.5) * cam.height);
}

static Vec3d ray_plane(const Vec3d &o, const Vec3d &d, const Vec3d &p, const Vec3d &n)
{
    double den = d.dot(n);
    if (std::abs(den) < 1e-9) return p;
    return o + d * ((p - o).dot(n) / den);
}

void Gizmos::set_emboss_ring(bool on, const Transform3d &frame, double radius)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_ring.dragging) return;   // the frame the drag started in stays until it ends
    m_ring.on = on;
    m_ring.frame = frame;
    m_ring.radius = radius;
}

// the finger on the ring's grabber (GLGizmoEmboss::on_mouse_for_rotation: use_grabbers)
bool Gizmos::emboss_ring_drag_start(const Camera &cam, double px, double py)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_ring.on) return false;
    const Matrix3d rot = m_ring.frame.linear();
    const Vec3d g = m_ring.frame.translation() + m_ring.radius * (1 + ROTATE_GRABBER_OFFSET) * rot.col(1);
    if ((project(cam, g) - Vec2d(px, py)).norm() > touch_px()) return false;
    m_ring.dragging = true;
    m_ring.value = 0;
    return true;
}

bool Gizmos::drag_start(const std::vector<RenderVolume> &vols, unsigned revision, const Camera &cam, double px, double py)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_active == "cut" && m_cut.valid && m_cut_edit.editing) {
        // GLGizmoCut3D: a connector under the finger is dragged over the cut plane (on_dragging, m_connectors_group_id)
        int hit = -1;
        double best_d = touch_px();
        for (size_t i = 0; i < m_cut_edit.connectors.size(); ++i) {
            const double d = (project(cam, m_cut_edit.connectors[i].pos + m_cut_edit.offset) - Vec2d(px, py)).norm();
            if (d < best_d) { best_d = d; hit = int(i); }
        }
        if (hit < 0) return false;
        Drag dg;
        dg.grabber = 4;
        dg.value = hit;
        dg.start_pos = m_cut_edit.connectors[hit].pos;
        m_drag = dg;
        m_cut_edit.selected.assign(m_cut_edit.connectors.size(), false);
        m_cut_edit.selected[hit] = true;
        return true;
    }
    if (m_active == "cut") {
        if (!m_cut.valid || m_cut_edit.editing) return false;
        const double iz = inv_zoom(cam), len = 0.5 * m_cut.radius;
        const double size = std::max(32.0 * iz * 0.05, 4.0 * iz) * 2.0 * m_ui_scale;
        const Transform3d base = Geometry::translation_transform(m_cut.center) * m_cut.rotation;
        const Vec2d f(px, py);
        Drag best;
        double best_dist = touch_px();
        auto consider = [&](int id, const Vec3d &local) {
            Vec3d w = base * local;
            double dist = (project(cam, w) - f).norm();
            if (dist < best_dist) { best_dist = dist; best.grabber = id; best.start_pos = w; }
        };
        consider(0, len * Vec3d::UnitZ());
        consider(1, Vec3d(0, 1.25 * size, len)); consider(1, Vec3d(0, -1.25 * size, len));
        consider(2, Vec3d(1.25 * size, 0, len)); consider(2, Vec3d(-1.25 * size, 0, len));
        if (best.grabber < 0 && m_cut_line_mode) {
            // process_cut_line LeftDown: the point a unit along the ray, so it is not clipped
            Vec3d o, d;
            cam.ray(px, py, o, d);
            m_line_beg = m_line_end = o + d.normalized();
            m_line_dir = d.normalized();
            best.grabber = 3;
            m_drag = best;
            return true;
        }
        if (best.grabber < 0) return false;
        best.inv_zoom = iz;
        best.value = 0;
        best.start.center = m_cut.center;
        best.cut_rotation = m_cut.rotation;
        if (best.grabber > 0) {
            // the rotation angle is measured in the plane perpendicular to the rotation axis through the plane centre
            Vec3d axis = (m_cut.rotation.linear() * Vec3d::Unit(best.grabber == 1 ? 0 : 1)).normalized();
            Vec3d o, d;
            cam.ray(px, py, o, d);
            Vec3d hit = ray_plane(o, d, m_cut.center, axis) - m_cut.center;
            Vec3d u = (m_cut.rotation.linear() * Vec3d::UnitZ()).normalized(), v = axis.cross(u);
            best.start_angle = std::atan2(hit.dot(v), hit.dot(u));
        }
        m_drag = best;
        return true;
    }
    if (m_active != "move" && m_active != "rotate" && m_active != "scale") return false;
    SelBox box = selection_box(vols, revision);
    if (!box.valid) return false;
    const double iz = inv_zoom(cam), tol = touch_px();
    const Vec2d f(px, py);
    Vec3d o, d;
    cam.ray(px, py, o, d);
    Drag best;
    double best_dist = tol;
    auto consider = [&](int id, const Vec3d &world, double penalty = 0) {   // penalty: a grabber wins over a circle
        double dist = (project(cam, world) - f).norm() + penalty;
        if (dist < best_dist) { best_dist = dist; best.grabber = id; best.start_pos = world; }
    };
    if (m_active == "move") {
        Vec3d half = box.box.size() * 0.5;
        for (int i = 0; i < 3; ++i) consider(i, box.center + Vec3d::Unit(i) * (half[i] + MOVE_SPACE * iz + grabber_px() * iz));
    } else if (m_active == "rotate") {
        const double R = ROTATE_OFFSET + box.sphere;
        for (int i = 0; i < 3; ++i) {
            Vec3d u, v, n;
            rotate_basis(i, u, v, n);
            consider(i, box.center + R * (1 + ROTATE_GRABBER_OFFSET) * u);
            // touch: the whole circle is a handle too
            for (unsigned k = 0; k < ROTATE_SCALE_STEPS; ++k) {
                double a = k * 2 * PI / ROTATE_SCALE_STEPS;
                consider(i, box.center + R * (std::cos(a) * u + std::sin(a) * v), 0.5 * tol);
            }
        }
    } else {
        Vec3d c = box.center, h = box.box.size() * 0.5;
        Vec3d pts[10] = {c + Vec3d(-h.x(), 0, -h.z()), c + Vec3d(h.x(), 0, -h.z()), c + Vec3d(0, -h.y(), -h.z()), c + Vec3d(0, h.y(), -h.z()),
                         c + Vec3d(0, 0, -h.z()), c + Vec3d(0, 0, h.z()),
                         c + Vec3d(-h.x(), -h.y(), -h.z()), c + Vec3d(h.x(), -h.y(), -h.z()), c + Vec3d(h.x(), h.y(), -h.z()), c + Vec3d(-h.x(), h.y(), -h.z())};
        for (int i = 0; i < 10; ++i)
            if (i != 4) consider(i, pts[i]);
    }
    if (best.grabber < 0) return false;
    best.start = box;
    best.inv_zoom = iz;
    if (m_active == "rotate") {
        Vec3d u, v, n;
        rotate_basis(best.grabber, u, v, n);
        Vec3d p = ray_plane(o, d, box.center, n) - box.center;
        best.start_angle = std::atan2(p.dot(v), p.dot(u));
        best.value = 0;
    } else if (m_active == "scale") {
        best.value = 1;
        best.plane_normal = Vec3d::UnitZ();
    } else
        best.value = 0;
    m_drag = best;
    return true;
}

void Gizmos::drag_move(const Camera &cam, double px, double py)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_ring.dragging) {
        // GLGizmoRotate::on_dragging in the volume's frame, from the upward grabber; snapped like the rotate gizmo
        Vec3d o, d;
        cam.ray(px, py, o, d);
        const Matrix3d rot = m_ring.frame.linear();
        const Vec3d c = m_ring.frame.translation(), u = rot.col(0), v = rot.col(1), n = rot.col(2);
        const Vec3d p = ray_plane(o, d, c, n) - c;
        const double R = m_ring.radius, len = p.norm();
        double theta = std::atan2(p.dot(v), p.dot(u)) - 0.5 * PI;
        while (theta < 0) theta += 2 * PI;
        while (theta >= 2 * PI) theta -= 2 * PI;
        if (R / 3 <= len && len <= 2 * R / 3) {
            double step = 2 * PI / ROTATE_SNAP_REGIONS;
            theta = step * std::round(theta / step);
        } else if (R <= len && len <= R * (1 + ROTATE_SCALE_LONG_TOOTH)) {
            double step = 2 * PI / ROTATE_SCALE_STEPS;
            theta = step * std::round(theta / step);
        }
        if (std::abs(theta - 2 * PI) < 1e-9) theta = 0;
        m_ring.value = theta;
        return;
    }
    Drag &dg = m_drag;
    if (dg.grabber < 0) return;
    Vec3d o, d;
    cam.ray(px, py, o, d);
    if (m_active == "cut") {
        if (dg.grabber == 3) { m_line_end = o + d.normalized(); m_line_dir = d.normalized(); return; }
        if (dg.grabber == 4) {   // unproject_on_cut_plane + is_projection_inside_cut
            const Vec3d n = (m_cut.rotation.linear() * Vec3d::UnitZ()).normalized();
            const double den = n.dot(d);
            if (std::abs(den) < 1e-9) return;
            const Vec3d w = o + d * ((m_cut.center - o).dot(n) / den);
            const Vec3d local = m_cut_edit.to_plane * w;
            const Point pt(scale_(local.x()), scale_(local.y()));
            if (std::none_of(m_cut_edit.section.begin(), m_cut_edit.section.end(), [&](const ExPolygon &ex) { return ex.contains(pt); })) return;
            const size_t i = size_t(dg.value);
            if (i < m_cut_edit.connectors.size()) m_cut_edit.connectors[i].pos = w - m_cut_edit.offset;
            return;
        }
        if (dg.grabber == 0) {
            // GLGizmoCut3D::dragging_grabber_z: along the plane normal, the closest point of the ray
            Vec3d n = (dg.cut_rotation.linear() * Vec3d::UnitZ()).normalized();
            Vec3d inters = o + (dg.start_pos - o).dot(d) * d;
            double t = (inters - dg.start_pos).dot(n);
            Vec3d c = dg.start.center + t * n;
            c.z() = std::clamp(c.z(), m_cut.box.min.z(), m_cut.box.max.z());
            m_cut.center = c;
        } else {
            // dragging_grabber_xy: rotation about the plane's X / Y axis, snapped every 5 degrees near the circle
            Vec3d axis = (dg.cut_rotation.linear() * Vec3d::Unit(dg.grabber == 1 ? 0 : 1)).normalized();
            Vec3d hit = ray_plane(o, d, m_cut.center, axis) - m_cut.center;
            Vec3d u = (dg.cut_rotation.linear() * Vec3d::UnitZ()).normalized(), v = axis.cross(u);
            double theta = std::atan2(hit.dot(v), hit.dot(u)) - dg.start_angle;
            const double len = hit.norm(), r = 0.5 * m_cut.radius;
            if (len > 0.85 * r && len < 1.15 * r) { const double step = 2 * PI / 72; theta = step * std::round(theta / step); }
            m_cut.rotation = Transform3d(Eigen::AngleAxisd(theta, axis)) * dg.cut_rotation;
        }
        return;
    }
    if (m_active == "move") {
        // GLGizmoMove3D::calc_projection: the closest point of the ray to the start, along the axis
        Vec3d axis = Vec3d::Unit(dg.grabber);
        Vec3d inters = o + (dg.start_pos - o).dot(d) * d;
        dg.value = (inters - dg.start_pos).dot(axis);
        // GLGizmoMove3D::calc_projection: Shift snaps to m_snap_step (1 mm)
        if (shift_down) dg.value = 1.0 * std::round(dg.value / 1.0);
    } else if (m_active == "rotate") {
        // GLGizmoRotate::on_dragging, snapping in the coarse (8) and fine (scale) regions
        Vec3d u, v, n;
        rotate_basis(dg.grabber, u, v, n);
        Vec3d p = ray_plane(o, d, dg.start.center, n) - dg.start.center;
        double R = ROTATE_OFFSET + dg.start.sphere;
        double len = p.norm();
        double theta = std::atan2(p.dot(v), p.dot(u)) - dg.start_angle;
        while (theta < 0) theta += 2 * PI;
        while (theta >= 2 * PI) theta -= 2 * PI;
        if (R / 3 <= len && len <= 2 * R / 3) {
            double step = 2 * PI / ROTATE_SNAP_REGIONS;
            theta = step * std::round(theta / step);
        } else if (R <= len && len <= R * (1 + ROTATE_SCALE_LONG_TOOTH)) {
            double step = 2 * PI / ROTATE_SCALE_STEPS;
            theta = step * std::round(theta / step);
        }
        if (std::abs(theta - 2 * PI) < 1e-9) theta = 0;
        dg.value = theta;
    } else if (m_active == "scale") {
        // GLGizmoScale3D::calc_ratio
        Vec3d pivot(dg.start.center.x(), dg.start.center.y(), dg.start.box.min.z());
        Vec3d starting_vec = dg.start_pos - pivot;
        double len0 = starting_vec.norm();
        if (len0 <= 0) return;
        Vec3d normal = dg.plane_normal;
        if (dg.grabber == 5) normal = d.cross(dg.plane_normal).cross(dg.plane_normal);
        normal.normalize();
        double angle = std::acos(std::clamp(normal.dot(d), -1.0, 1.0)) * 180 / PI;
        if (angle < 95 && angle > 85) return;
        Vec3d inters = ray_plane(o, d, dg.start_pos, normal);
        double proj = (inters - dg.start_pos).dot(starting_vec.normalized());
        double ratio = (len0 + proj) / len0;
        // GLGizmoScale3D::calc_ratio: Shift snaps to m_snap_step (0.05)
        if (shift_down) ratio = 0.05 * std::round(ratio / 0.05);
        if (ratio > 0.0) dg.value = ratio;
    }
}

nlohmann::json Gizmos::drag_end()
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_ring.dragging) {   // GLGizmoEmboss::on_stop_dragging: the gizmo applies the rotation
        m_ring.dragging = false;
        double a = m_ring.value;
        m_ring.value = 0;
        if (a > PI) a -= 2 * PI;
        return {{"gizmo", "emboss_ring"}, {"angle", a}};
    }
    if (m_drag.grabber < 0) return nullptr;
    if (m_active == "cut") {
        if (m_drag.grabber == 4) {
            const size_t i = size_t(m_drag.value);
            const bool moved = i < m_cut_edit.connectors.size() && !m_cut_edit.connectors[i].pos.isApprox(m_drag.start_pos);
            m_drag = Drag();
            if (!moved) return {{"gizmo", "cut"}};
            const Vec3d p = m_cut_edit.connectors[i].pos;
            return {{"gizmo", "cut"}, {"connector", int(i)}, {"pos", {p.x(), p.y(), p.z()}}};
        }
        if (m_drag.grabber == 3) {
            // process_cut_line LeftUp: the plane through the line, perpendicular to the view
            const Vec3d line_dir = m_line_end - m_line_beg;
            const Vec3d pt = m_line_end;
            m_line_beg = m_line_end = Vec3d::Zero();
            m_drag = Drag();
            if (line_dir.norm() < 3.0) return nullptr;
            const Vec3d cross_dir = line_dir.cross(m_line_dir).normalized();
            Transform3d m = Transform3d::Identity();
            m.matrix().block(0, 0, 3, 3) = Eigen::Quaterniond().setFromTwoVectors(Vec3d::UnitZ(), cross_dir).toRotationMatrix();
            const Vec3d new_plane_center = m_cut.bb_center + cross_dir * cross_dir.dot(pt - m_cut.bb_center);
            // the plane has to cross the object (transformed_bounding_box contains the centre)
            double lo = DBL_MAX, hi = -DBL_MAX;
            for (int k = 0; k < 8; ++k) {
                const Vec3d corner((k & 1 ? m_cut.box.max : m_cut.box.min).x(), (k & 2 ? m_cut.box.max : m_cut.box.min).y(),
                                   (k & 4 ? m_cut.box.max : m_cut.box.min).z());
                const double t = cross_dir.dot(corner - new_plane_center);
                lo = std::min(lo, t);
                hi = std::max(hi, t);
            }
            if (lo > 0 || hi < 0) return {{"gizmo", "cut"}};
            m_cut.center = new_plane_center;
            m_cut.rotation = m;
            return {{"gizmo", "cut"}};
        }
        m_drag = Drag();
        return {{"gizmo", "cut"}};
    }
    Transform3d t = transform_locked();
    m_drag = Drag();
    // GLCanvas3D::do_move / do_rotate / do_scale with the gizmo's snapshot name
    std::string name = m_active == "move" ? "Gizmo-Move" : m_active == "rotate" ? "Gizmo-Rotate" : "Gizmo-Scale";
    if (t.isApprox(Transform3d::Identity())) return nullptr;
    nlohmann::json m = nlohmann::json::array();
    for (int i = 0; i < 16; ++i) m.push_back(t.matrix().data()[i]);
    return {{"gizmo", m_active}, {"matrix", m}, {"name", name}};
}

// ---------------------------------------------------------------------------------------------------------------
// Cut

void Gizmos::set_cut_groove(bool groove, std::shared_ptr<const TriangleMesh> mesh, bool valid)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_cut.groove = groove;
    m_cut.groove_mesh = std::move(mesh);
    m_cut.valid_groove = valid;
}

void Gizmos::set_cut_edit(CutEdit e)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_cut_edit = std::move(e);
}

void Gizmos::set_cut_parts(int obj, std::vector<CutPart> parts)
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_cut_parts = std::move(parts);
    m_cut_parts_obj = m_cut_parts.empty() ? -1 : obj;
}

bool Gizmos::cut_parts_hide(int obj)
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_active == "cut" && obj == m_cut_parts_obj && !m_cut_parts.empty();
}

Gizmos::CutEdit Gizmos::cut_edit()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_cut_edit;
}

// GLGizmoCut3D::get_connector_mesh
indexed_triangle_set cut_connector_mesh(const CutConnectorAttributes &a, float space, float bulge)
{
    int sectors = 1;
    switch (a.shape) {
    case CutConnectorShape::Triangle: sectors = 3; break;
    case CutConnectorShape::Square: sectors = 4; break;
    case CutConnectorShape::Circle: sectors = 360; break;
    case CutConnectorShape::Hexagon: sectors = 6; break;
    default: break;
    }
    if (a.type == CutConnectorType::Snap) return its_make_snap(1.0, 1.0, space, bulge);
    if (a.style == CutConnectorStyle::Prism) return its_make_cylinder(1.0, 1.0, 2 * PI / sectors);
    if (a.type == CutConnectorType::Plug) return its_make_frustum(1.0, 1.0, 2 * PI / sectors);
    return its_make_frustum_dowel(1.0, 1.0, sectors);
}

Gizmos::CutPlane Gizmos::cut_plane()
{
    std::lock_guard<std::mutex> l(m_mutex);
    return m_cut;
}

void Gizmos::set_cut_plane(const Vec3d &center, const Transform3d &rotation)
{
    std::lock_guard<std::mutex> l(m_mutex);
    // GLGizmoCut3D::set_center_pos: the centre stays inside the object's box
    Vec3d c = center;
    c.z() = std::clamp(c.z(), m_cut.box.min.z(), m_cut.box.max.z());
    m_cut.center = c;
    m_cut.rotation = rotation;
}

void Gizmos::reset_cut_plane()
{
    std::lock_guard<std::mutex> l(m_mutex);
    m_cut.center = m_cut.bb_center;
    m_cut.rotation = Transform3d::Identity();
}

bool Gizmos::clip(int obj, float plane[4])
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_active != "cut" || !m_cut.valid || !m_cut_edit.editing || obj != m_cut.obj) return false;
    // the object clipper while editing connectors: the part above the plane is hidden
    Vec3d n = (m_cut.rotation.linear() * Vec3d::UnitZ()).normalized();
    plane[0] = float(-n.x()); plane[1] = float(-n.y()); plane[2] = float(-n.z()); plane[3] = float(n.dot(m_cut.center));
    return true;
}

bool Gizmos::color_clip(int obj, float plane[4])
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_active != "cut" || !m_cut.valid || obj != m_cut.obj) return false;
    // GLGizmoCut3D::update_clipper -> set_color_clip_plane(normal, offset): {-normal, offset}
    Vec3d n = (m_cut.rotation.linear() * Vec3d::UnitZ()).normalized();
    const Vec3d &c = m_cut.center;
    plane[0] = float(-n.x()); plane[1] = float(-n.y()); plane[2] = float(-n.z()); plane[3] = float(n.dot(c));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Lay on face

void Gizmos::update_planes()
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_active == "cut") {
        // GLGizmoCut3D::update_bb: a new selection or a changed box puts the plane through the box centre
        Plater &p = Plater::get();
        int obj = -1, inst = 0;
        if (!p.selection.empty() && p.selection.begin()->vol < 0) { obj = p.selection.begin()->obj; inst = std::max(0, p.selection.begin()->inst); }
        const Model &m = p.model();
        if (obj < 0 || obj >= int(m.objects.size())) { m_cut = CutPlane(); return; }
        BoundingBoxf3 box = m.objects[obj]->instance_bounding_box(inst);
        if (!m_cut.valid || m_cut.obj != obj || m_cut.inst != inst || !m_cut.box.min.isApprox(box.min) || !m_cut.box.max.isApprox(box.max)) {
            m_cut.valid = true;
            m_cut.obj = obj;
            m_cut.inst = inst;
            m_cut.box = box;
            m_cut.bb_center = box.center();
            m_cut.center = box.center();
            m_cut.rotation = Transform3d::Identity();
            m_cut.radius = box.radius();
        }
        return;
    }
    if (m_active != "flatten") return;
    Plater &p = Plater::get();
    // GLGizmoFlatten::on_is_activable: a single full instance
    int obj = -1, inst = 0;
    if (p.selection.size() == 1 && p.selection.begin()->vol < 0) { obj = p.selection.begin()->obj; inst = std::max(0, p.selection.begin()->inst); }
    const Model &m = p.model();
    if (obj < 0 || obj >= int(m.objects.size()) || inst >= int(m.objects[obj]->instances.size())) { m_planes.clear(); m_planes_obj = -1; return; }
    const ModelObject *mo = m.objects[obj];
    const Transform3d inst_matrix = mo->instances[inst]->get_matrix_no_offset();
    m_planes_obj = obj;
    m_planes_inst = inst;
    m_planes_matrix = mo->instances[inst]->get_matrix();
    m_planes.clear();
    // GLGizmoFlatten::update_planes
    std::vector<LayOnFacePlane> planes = lay_on_face_planes(*mo, inst_matrix);
    planes.resize(std::min((int) planes.size(), 254));
    for (LayOnFacePlane &plane : planes) {
        Pointf3s &polygon = plane.outline;
        if (polygon.size() < 3) continue;
        Vec3d centroid = std::accumulate(polygon.begin(), polygon.end(), Vec3d(0.0, 0.0, 0.0));
        centroid /= (double) polygon.size();
        for (auto &vertex : polygon) vertex = 0.9f * vertex + 0.1f * centroid;
        const unsigned int k = 10;
        const float aggressivity = 0.2f;
        const unsigned int N = polygon.size();
        std::vector<std::pair<unsigned int, unsigned int>> neighbours;
        Pointf3s points_out(2 * k * N);
        for (unsigned int j = 0; j < N; ++j) {
            points_out[j * 2 * k] = polygon[j];
            neighbours.push_back(std::make_pair((int) (j * 2 * k - k) < 0 ? (N - 1) * 2 * k + k : j * 2 * k - k, j * 2 * k + k));
        }
        for (unsigned int i = 0; i < k; ++i) {
            for (unsigned int j = 0; j < N; ++j)
                if (i == 0)
                    points_out[j * 2 * k + k] = 0.5f * (points_out[j * 2 * k] + points_out[j == N - 1 ? 0 : (j + 1) * 2 * k]);
                else {
                    float r = 0.2 + 0.3 / (k - 1) * i;
                    points_out[neighbours[j].first] = r * points_out[j * 2 * k] + (1 - r) * points_out[neighbours[j].first - 1];
                    points_out[neighbours[j].second] = r * points_out[j * 2 * k] + (1 - r) * points_out[neighbours[j].second + 1];
                }
            for (unsigned int j = 0; j < N; ++j)
                points_out[2 * k * j] = (1 - aggressivity) * points_out[2 * k * j] +
                                        aggressivity * 0.5f * (points_out[neighbours[j].first] + points_out[neighbours[j].second]);
            for (auto &n : neighbours) { ++n.first; --n.second; }
        }
        polygon = points_out;
        for (auto &b : polygon) b(2) += 0.1f;
        Plane pl;
        pl.normal = plane.normal;
        Transform3d back = inst_matrix.inverse() * plane.to_plane_frame.inverse();
        for (const Vec3d &pt : polygon) pl.outline.push_back(back * pt);
        m_planes.push_back(std::move(pl));
    }
    m_pressed_plane = -1;
}

static bool ray_tri(const Vec3d &o, const Vec3d &d, const Vec3d &a, const Vec3d &b, const Vec3d &c, double &t)
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

nlohmann::json Gizmos::pick_plane(const Camera &cam, double px, double py)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_active != "flatten" || m_planes.empty()) return nullptr;
    Vec3d o, d;
    cam.ray(px, py, o, d);
    double best = 1e30;
    int hit = -1;
    for (size_t i = 0; i < m_planes.size(); ++i) {
        const auto &ol = m_planes[i].outline;
        for (size_t k = 1; k + 1 < ol.size(); ++k) {
            double t;
            if (ray_tri(o, d, m_planes_matrix * ol[0], m_planes_matrix * ol[k], m_planes_matrix * ol[k + 1], t) && t < best) { best = t; hit = int(i); }
        }
    }
    if (hit < 0) return nullptr;
    m_pressed_plane = hit;
    const Vec3d &n = m_planes[hit].normal;
    return {{"obj", m_planes_obj}, {"inst", m_planes_inst}, {"normal", {n.x(), n.y(), n.z()}}, {"plane", hit}};
}

} // namespace orca
