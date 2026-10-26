// The canvas toolbar's section view (GLCanvas3D::set_section_view_ratio / toggle_section_view /
// align_section_view_to_camera / _get_section_view_plane / _render_section_view_caps), shared by Prepare and Preview,
// and the painters' object clipper it drives (GLGizmosManager::update_section_view).
#include "Viewport.hpp"

#include <GLES3/gl3.h>
#include <algorithm>
#include <cfloat>

#include "BrimEars.hpp"
#include "Core.hpp"
#include "Gizmos.hpp"
#include "Painter.hpp"
#include "Plater.hpp"
#include "Preview.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using json = nlohmann::json;

namespace orca {

bool mesh_boolean_active();   // MeshBooleanGizmo.cpp

json Viewport::section_view()
{
    std::lock_guard<std::mutex> l(m_section_mutex);
    return {{"ratio", m_section_ratio}, {"active", m_section_ratio > 0.}};
}

void Viewport::set_section_ratio(double ratio)
{
    ratio = std::clamp(ratio, 0., 1.);
    bool aim = false;
    {
        std::lock_guard<std::mutex> l(m_section_mutex);
        if (ratio == m_section_ratio) return;
        if (ratio == 0.) m_section_last = m_section_ratio;
        m_section_ratio = ratio;
        // Only the first cut faces the camera; later ones keep their angle until it is set again.
        aim = m_section_normal.isZero();
    }
    if (aim) align_section_view();
    m_dirty = true;
}

void Viewport::toggle_section_view()
{
    double ratio, last;
    {
        std::lock_guard<std::mutex> l(m_section_mutex);
        ratio = m_section_ratio;
        last = m_section_last;
    }
    set_section_ratio(ratio > 0. ? 0. : last > 0. ? last : 0.5);
}

void Viewport::align_section_view()
{
    const Camera cam = camera();
    std::lock_guard<std::mutex> l(m_section_mutex);
    m_section_normal = (cam.eye() - cam.target).normalized();   // -Camera::get_dir_forward
    m_dirty = true;
}

bool Viewport::clipping_plane(Vec3d &n, double &d)
{
    std::lock_guard<std::mutex> l(m_section_mutex);
    n = m_clp_n;
    d = m_clp_d;
    return m_clp_on;
}

// _get_volumes_clipping_plane for this frame (GL thread, after the snapshot)
void Viewport::update_clipping_plane()
{
    std::vector<RenderVolume> vols;
    {
        std::lock_guard<std::mutex> l(m_scene_mutex);
        vols = m_volumes;
    }
    const bool painter = Painter::get().active(), ears = BrimEarsGizmo::get().active();
    const int  obj  = painter ? Painter::get().object() : ears ? BrimEarsGizmo::get().object() : -1;
    const int  inst = painter ? Painter::get().instance() : ears ? BrimEarsGizmo::get().instance() : 0;
    const bool other_clipper = mesh_boolean_active() || Gizmos::get().active() == "cut";
    std::lock_guard<std::mutex> l(m_section_mutex);
    m_clp_on = m_clp_section = false;
    const double ratio = m_section_ratio;
    if (ratio == 0. || m_section_normal.isZero()) return;
    if (painter || ears) {
        // ObjectClipper::set_position_by_ratio on the edited instance; brim ears always cut horizontally
        const Vec3d n = ears ? Vec3d::UnitZ() : m_section_normal;
        for (const RenderVolume &r : vols)
            if (r.obj == obj && r.inst == inst && !r.wipe_tower) {
                m_clp_n  = n;
                m_clp_d  = n.dot(r.inst_offset) + r.inst_radius - 2. * r.inst_radius * ratio;
                m_clp_on = true;
                return;
            }
        return;
    }
    if (other_clipper) return;   // a gizmo with its own clipper: its plane, not the section view's
    // The objects on the current plate, so Preview cuts where Prepare does; G-code alone: its toolpaths
    BoundingBoxf3 box;
    for (const RenderVolume &r : vols)
        if (Plater::get().assemble_view || (r.bbox.max.x() >= m_current_plate_box.min.x() && r.bbox.min.x() <= m_current_plate_box.max.x() &&
                                            r.bbox.max.y() >= m_current_plate_box.min.y() && r.bbox.min.y() <= m_current_plate_box.max.y()))
            box.merge(r.bbox);
    if (!box.defined) {
        size_t sig;
        if (!Preview::get().shadow_casters(box, sig)) return;
    }
    // Sweeps the bounding sphere from the camera side (0) to the far side (1), as ObjectClipper does.
    const double radius = box.radius();
    m_clp_n = m_section_normal;
    m_clp_d = m_section_normal.dot(box.center()) + radius - 2. * radius * ratio;
    m_clp_on = m_clp_section = true;
}

// _render_section_view_caps: the cut faces of the model parts the plane crosses (none below the plate)
void Viewport::draw_section_caps(const std::vector<RenderVolume> &vols, unsigned revision, const Transform3d &gizmo, const Eigen::Matrix4f &vp)
{
    Vec3d n;
    double d;
    bool section;
    {
        std::lock_guard<std::mutex> l(m_section_mutex);
        n = m_clp_n;
        d = m_clp_d;
        section = m_clp_on && m_clp_section;
    }
    if (!section) { m_caps.clear(); m_caps_key.clear(); return; }
    // the plane frame: z along the normal, through the plane
    const Transform3d to_world = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), n)).pretranslate(n * d);
    const Transform3d to_plane = to_world.inverse();
    std::string key = std::to_string(revision);
    std::vector<std::pair<const RenderVolume *, Transform3d>> cut;
    for (const RenderVolume &r : vols) {
        if (!r.model_part || r.wipe_tower) continue;
        const Transform3d m = drawn_matrix(r, revision, gizmo);
        const BoundingBoxf3 box = r.bbox.transformed(m * r.matrix.inverse());
        // Skip volumes the plane misses.
        if (std::abs(n.dot(box.center()) - d) > 0.5 * box.size().dot(n.cwiseAbs())) continue;
        cut.emplace_back(&r, m);
        for (int k = 0; k < 16; ++k) key += "," + std::to_string(m.matrix().data()[k]);
    }
    key += "|" + std::to_string(n.x()) + "," + std::to_string(n.y()) + "," + std::to_string(n.z()) + "," + std::to_string(d);
    if (key != m_caps_key) {
        m_caps_key = key;
        m_caps.clear();
        for (auto &[r, m] : cut) {
            MeshSlicingParams params;
            params.trafo = to_plane * m;
            const ExPolygons section = union_ex(slice_mesh(r->mesh->its, 0.f, params));
            const std::vector<Vec2f> tris = triangulate_expolygons_2f(section, true);
            for (size_t k = 0; k + 2 < tris.size(); k += 3) {
                Vec3d w[3];
                for (int j = 0; j < 3; ++j) w[j] = to_world * Vec3d(tris[k + j].x(), tris[k + j].y(), 0.);
                if (w[0].z() + w[1].z() + w[2].z() < 3. * SINKING_Z_THRESHOLD) continue;   // set_limiting_plane: none below the plate
                for (const Vec3d &v : w) m_caps.insert(m_caps.end(), {float(v.x()), float(v.y()), float(v.z())});
            }
        }
    }
    if (m_caps.empty()) return;
    glDisable(GL_CULL_FACE);
    gl_draw_flat(m_caps, GL_TRIANGLES, vp, ColorRGBA(0.25f, 0.25f, 0.25f, 1.0f));
}

static Commands reg({
    {"section.state", [](const json &) -> json { return Viewport::get().section_view(); }},
    {"section.set", [](const json &a) -> json {
         Viewport::get().set_section_ratio(a.at("ratio").get<double>());
         return Viewport::get().section_view();
     }},
    {"section.toggle", [](const json &) -> json {
         Viewport::get().toggle_section_view();
         return Viewport::get().section_view();
     }},
    {"section.align", [](const json &) -> json {
         Viewport::get().align_section_view();
         return Viewport::get().section_view();
     }},
});

} // namespace orca
