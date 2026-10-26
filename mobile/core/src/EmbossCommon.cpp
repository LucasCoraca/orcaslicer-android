#include "EmbossCommon.hpp"

#include <map>
#include <mutex>

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CutSurface.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
using namespace Slic3r::Emboss;

namespace orca::emboss {

// EmbossJob.cpp
constexpr float SAFE_SURFACE_OFFSET = 0.015f;   // [in mm]
constexpr float safe_extension      = 1.0f;

#include "slic3r/GUI/EmbossJob.inc"
#include "slic3r/GUI/SurfaceDrag.inc"

Transform3d surface_transformation(const Transform3d &world, const Vec3d &world_dir, const Vec3d &world_position, const std::optional<Transform3d> &fix,
                                   const Transform3d &instance_inv, std::optional<float> current_angle, const std::optional<double> &up_limit)
{
    return get_volume_transformation(world, world_dir, world_position, fix, instance_inv, current_angle, up_limit);
}

void DataBase::write(ModelVolume &volume) const
{
    volume.name         = volume_name;
    volume.emboss_shape = shape;
    volume.emboss_shape->fix_3mf_tr.reset();
}

TriangleMesh create_mesh(DataBase &input)
{
    auto was_canceled = [c = input.cancel]() { return c && c->load(); };
    TriangleMesh result = try_create_mesh(input, was_canceled);
    if (was_canceled()) return {};
    if (result.its.empty())   // the text could not be made: the default text mesh (CreateVolumeJob shows a message)
        result = create_default_mesh();
    return result;
}

TriangleMesh cut_surface_mesh(DataBase &input, const SurfaceVolumeData &surface)
{
    std::function<bool()> was_canceled = [c = input.cancel]() { return c && c->load(); };
    return cut_surface(input, surface, was_canceled);
}

SurfaceVolumeData::ModelSources model_sources(const ModelVolumePtrs &volumes, std::optional<size_t> skip_volume_id)
{
    return create_sources(volumes, skip_volume_id);
}

// ---------------------------------------------------------------------------------------------- raycasting

// the AABB trees of the meshes (RaycastManager keeps them per volume)
static std::shared_ptr<const AABBMesh> aabb(const std::shared_ptr<const TriangleMesh> &mesh)
{
    static std::mutex m;
    static std::map<const TriangleMesh *, std::pair<std::weak_ptr<const TriangleMesh>, std::shared_ptr<const AABBMesh>>> cache;
    std::lock_guard<std::mutex> l(m);
    for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
    auto &e = cache[mesh.get()];
    if (!e.second || e.first.lock() != mesh) e = {mesh, std::make_shared<const AABBMesh>(*mesh)};
    return e.second;
}

std::optional<Hit> ray_hit(const ModelObject &object, int instance, const Vec3d &origin, const Vec3d &dir, std::optional<size_t> skip_volume_id)
{
    std::optional<Hit> best;
    const Transform3d inst = object.instances[instance]->get_matrix();
    for (int vi = 0; vi < int(object.volumes.size()); ++vi) {
        const ModelVolume *v = object.volumes[vi];
        if (!v->is_model_part() || (skip_volume_id && v->id().id == *skip_volume_id)) continue;
        const Transform3d tr = inst * v->get_matrix(), tr_inv = tr.inverse();
        const Vec3d o = tr_inv * origin, d = (tr_inv.linear() * dir).normalized();
        for (const AABBMesh::hit_result &h : aabb(v->get_mesh_shared_ptr())->query_ray_hits(o, d)) {
            if (!h.is_hit() || h.is_inside()) continue;
            const Vec3d p = tr * h.position();
            const double dist = (p - origin).norm();
            if (!best || dist < best->distance)
                best = Hit{p, (tr.linear().inverse().transpose() * h.normal()).normalized(), vi, dist};
            break;   // hits are sorted
        }
    }
    return best;
}

std::optional<Hit> closest_point(const ModelObject &object, int instance, const Vec3d &point, std::optional<size_t> skip_volume_id)
{
    std::optional<Hit> best;
    const Transform3d inst = object.instances[instance]->get_matrix();
    for (int vi = 0; vi < int(object.volumes.size()); ++vi) {
        const ModelVolume *v = object.volumes[vi];
        if (!v->is_model_part() || (skip_volume_id && v->id().id == *skip_volume_id)) continue;
        const Transform3d tr = inst * v->get_matrix();
        int face = 0;
        Vec3d c;
        auto mesh = aabb(v->get_mesh_shared_ptr());
        mesh->squared_distance(tr.inverse() * point, face, c);
        const Vec3d p = tr * c;
        const double dist = (p - point).norm();
        if (!best || dist < best->distance) {
            const auto &its = v->mesh().its;
            const Vec3i32 &f = its.indices[face];
            const Vec3d n = (its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).cast<double>();
            best = Hit{p, (tr.linear().inverse().transpose() * n).normalized(), vi, dist};
        }
    }
    return best;
}

// the volume's world matrix without the 3MF fix (world_matrix_fixed)
static Transform3d world_fixed(const ModelVolume &volume, int instance)
{
    Transform3d w = volume.get_object()->instances[instance]->get_matrix() * volume.get_matrix();
    if (volume.emboss_shape && volume.emboss_shape->fix_3mf_tr) w = w * volume.emboss_shape->fix_3mf_tr->inverse();
    return w;
}

std::optional<float> calc_distance(const ModelVolume &volume, int instance)
{
    if (volume.is_the_only_one_part() || !volume.emboss_shape.has_value()) return {};
    constexpr MinMax<double> surface_distance_sq{1e-4, 10.};   // [in mm]
    const Transform3d w = world_fixed(volume, instance);
    const Vec3d p = w.translation();
    const Vec3d dir = -get_z_base(w);
    auto hit = ray_hit(*volume.get_object(), instance, p, dir, volume.id().id);
    if (!hit) return {};
    const Vec3d p_to_hit = hit->position - p;
    const double distance_sq = p_to_hit.squaredNorm();
    if (distance_sq < surface_distance_sq.min) return {};
    const BoundingBoxf3 bb = volume.mesh().transformed_bounding_box(w);
    const double max_squared_distance = std::max(std::pow(2 * bb.size().z(), 2), surface_distance_sq.max);
    if (distance_sq > max_squared_distance) return {};
    const float sign = (p_to_hit.dot(dir) > 0) ? 1.f : -1.f;
    return sign * static_cast<float>(sqrt(distance_sq));
}

std::optional<float> calc_angle(const ModelVolume &volume, int instance)
{
    return Emboss::calc_up(world_fixed(volume, instance), UP_LIMIT);
}

void update_volume(ModelVolume &volume, TriangleMesh &&mesh, const DataBase &base)
{
    if (mesh.empty() || base.volume_name.empty()) return;
    volume.set_mesh(std::move(mesh));
    volume.set_new_unique_id();
    volume.calculate_convex_hull();
    base.write(volume);
}

ModelVolume *create_volume(TriangleMesh &&mesh, ModelObject &obj, ModelVolumeType type, const std::optional<Transform3d> &trmat, const DataBase &data)
{
    if (mesh.its.empty()) throw JobException(_u8L("Can't create empty volume."));
    BoundingBoxf3 instance_bb;
    if (!trmat.has_value()) instance_bb = obj.instance_bounding_box(0);
    // add a simple shape first (the mesh would be centered), then set the mesh
    ModelVolume *volume = obj.add_volume(make_cube(1., 1., 1.), type);
    volume->set_mesh(std::move(mesh));
    volume->calculate_convex_hull();
    volume->config.set_key_value("extruder", new ConfigOptionInt(0));
    volume->source.is_from_builtin_objects = true;
    volume->name = data.volume_name;
    if (trmat.has_value())
        volume->set_transformation(*trmat);
    else {
        // near the object, like ObjectList::load_generic_subobject
        Vec3d volume_size = volume->mesh().bounding_box().size();
        Vec3d offset_tr(0, -instance_bb.size().y() / 2 - volume_size.y() / 2, volume_size.z() / 2 - instance_bb.size().z() / 2);
        Transform3d tr = obj.instances.front()->get_transformation().get_matrix_no_offset().inverse();
        volume->set_transformation(tr * Eigen::Translation3d(offset_tr));
    }
    data.write(*volume);
    if (type == ModelVolumeType::MODEL_PART) volume->get_object()->ensure_on_bed();
    return volume;
}

} // namespace orca::emboss
