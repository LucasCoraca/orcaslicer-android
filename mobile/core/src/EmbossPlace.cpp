#include "EmbossPlace.hpp"

#include <cfloat>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Polygon.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
using namespace Slic3r::Emboss;

namespace orca::emboss {

static Vec2d to_screen(const Camera &cam, const Vec3d &p)
{
    const Eigen::Matrix4d vp = cam.projection(1., 10000.) * cam.view();
    const Eigen::Vector4d h = vp * Eigen::Vector4d(p.x(), p.y(), p.z(), 1.);
    return Vec2d((h.x() / h.w() * 0.5 + 0.5) * cam.width, (0.5 - h.y() / h.w() * 0.5) * cam.height);
}

SelItem index_of(const ModelVolume &volume, int instance)
{
    const Model &m = Plater::get().model();
    const ModelObject *mo = volume.get_object();
    const int oi = int(std::find(m.objects.begin(), m.objects.end(), mo) - m.objects.begin());
    const int vi = int(std::find(mo->volumes.begin(), mo->volumes.end(), &volume) - mo->volumes.begin());
    return {oi, instance, vi};
}

Created create(DataBase &base, ModelVolumeType volume_type, std::optional<Vec2d> screen, std::optional<float> angle, std::optional<float> distance,
               const std::string &object_snapshot, const std::string &volume_snapshot)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    const Camera cam = Viewport::get().camera();
    const Vec2d center(cam.width / 2., cam.height / 2.);
    int sel_obj = -1, sel_inst = 0;
    for (const SelItem &s : p.selection) { sel_obj = s.obj; sel_inst = std::max(0, s.inst); break; }
    if (sel_obj >= int(m.objects.size())) sel_obj = -1;

    if (sel_obj < 0) {   // CreateObjectJob: on the bed under the point (else the bed centre)
        base.shape.projection.use_surface = false;
        TriangleMesh mesh = create_mesh(base);
        const Vec2d screen_coor = screen.value_or(center);
        Vec3d o, d;
        cam.ray(screen_coor.x(), screen_coor.y(), o, d);
        Vec2d bed_coor = std::abs(d.z()) > 1e-9 ? Vec2d((o + d * (-o.z() / d.z())).head<2>()) : Vec2d::Zero();
        Points bed_pts;
        const Vec3d origin = p.plate_origin(p.current_plate);
        for (const Vec2d &v : p.bed_shape()) bed_pts.emplace_back(scaled(v.x() + origin.x()), scaled(v.y() + origin.y()));
        Slic3r::Polygon bed(bed_pts);
        if (!bed.contains(Point::new_scale(bed_coor.x(), bed_coor.y()))) bed_coor = unscaled(bed.centroid());
        const double z = base.shape.projection.depth / 2;
        Vec3d offset(bed_coor.x(), bed_coor.y(), z);
        offset -= mesh.center();
        Transform3d transformation(Eigen::Translation3d(offset.x(), offset.y(), offset.z()));
        if (angle.has_value()) apply_transformation(angle, std::optional<float>(), transformation);
        p.take_snapshot(object_snapshot);
        ModelObject *new_object = m.add_object();
        new_object->name = base.volume_name;
        new_object->add_instance();
        new_object->config.set_key_value("extruder", new ConfigOptionInt(1));
        ModelVolume *new_volume = new_object->add_volume(std::move(mesh));
        new_volume->config.set_key_value("extruder", new ConfigOptionInt(1));
        base.write(*new_volume);
        new_object->instances.front()->set_transformation(Geometry::Transformation(transformation));
        new_object->ensure_on_bed();
        const int oi = int(m.objects.size()) - 1;
        p.selection = {{oi, -1, -1}};
        p.changed();
        return {oi, 0, 0};
    }

    ModelObject &obj = *m.objects[sel_obj];
    // CreateVolumeJob / CreateSurfaceVolumeJob
    auto create_on_object = [&](const std::optional<Transform3d> &transform) -> Created {
        bool use_surface = base.shape.projection.use_surface;
        TriangleMesh mesh;
        if (use_surface && transform) {
            SurfaceVolumeData surface{*transform, model_sources(obj.volumes)};
            if (surface.sources.empty()) use_surface = false;
            else mesh = cut_surface_mesh(base, surface);
        } else
            use_surface = false;
        if (!use_surface) { base.shape.projection.use_surface = false; mesh = create_mesh(base); }
        p.take_snapshot(volume_snapshot);
        ModelVolume *v = create_volume(std::move(mesh), obj, volume_type, transform, base);
        obj.sort_volumes(true);   // reorder_volumes_and_get_selection
        const int vi = int(std::find(obj.volumes.begin(), obj.volumes.end(), v) - obj.volumes.begin());
        p.selection = {{sel_obj, sel_inst, vi}};
        p.changed();
        return {sel_obj, sel_inst, vi};
    };
    // the point on the surface: at the finger, else through the centre of the part whose projection is closest to the
    // view centre (find_closest)
    Vec2d coor = screen.value_or(center);
    if (!screen) {
        double best = DBL_MAX;
        for (const ModelVolume *v : obj.volumes) {
            if (!v->is_model_part()) continue;
            const Vec2d s = to_screen(cam, v->mesh().transformed_bounding_box(obj.instances[sel_inst]->get_matrix() * v->get_matrix()).center());
            if ((s - center).squaredNorm() < best) { best = (s - center).squaredNorm(); coor = s; }
        }
    }
    Vec3d o, d;
    cam.ray(coor.x(), coor.y(), o, d);
    std::optional<Hit> hit = ray_hit(obj, sel_inst, o, d);
    if (!hit) {
        if (screen) return create(base, volume_type, std::nullopt, angle, distance, object_snapshot, volume_snapshot);   // try_no_coor
        base.shape.projection.use_surface = false;
        return create_on_object(std::nullopt);
    }
    Transform3d surface_trmat = create_transformation_onto_surface(hit->position, hit->normal, UP_LIMIT);
    apply_transformation(angle, distance, surface_trmat);
    return create_on_object(Transform3d(obj.instances[sel_inst]->get_matrix().inverse() * surface_trmat));
}

bool update(ModelVolume &mv, int instance, DataBase &base, const std::string &snapshot)
{
    bool &use_surface = base.shape.projection.use_surface;
    if (use_surface && mv.is_the_only_one_part()) use_surface = false;
    TriangleMesh mesh;
    std::optional<Transform3d> new_tr;
    if (use_surface) {
        SurfaceVolumeData surface;
        surface.sources = model_sources(mv.get_object()->volumes, mv.id().id);
        if (surface.sources.empty()) return false;
        Transform3d volume_tr = mv.get_matrix();
        if (mv.emboss_shape && mv.emboss_shape->fix_3mf_tr) volume_tr = volume_tr * mv.emboss_shape->fix_3mf_tr->inverse();
        // newly using the surface: the origin goes onto the surface (calc_surface_offset)
        if (!mv.emboss_shape->projection.use_surface) {
            const Transform3d to_world = mv.get_object()->instances[instance]->get_matrix() * volume_tr;
            const Vec3d point = to_world.translation(), dir = -get_z_base(to_world);
            std::optional<Hit> hit = ray_hit(*mv.get_object(), instance, point, dir, mv.id().id);
            if (!hit) hit = closest_point(*mv.get_object(), instance, point, mv.id().id);
            if (hit) volume_tr *= Eigen::Translation<double, 3>(to_world.inverse() * hit->position);
        }
        surface.transform = volume_tr;
        mesh = cut_surface_mesh(base, surface);
        new_tr = volume_tr;
    } else
        mesh = create_mesh(base);
    if (mesh.its.empty()) return false;
    Plater &p = Plater::get();
    if (!snapshot.empty()) p.take_snapshot(snapshot);
    if (new_tr) mv.set_transformation(*new_tr);
    else if (mv.emboss_shape && mv.emboss_shape->fix_3mf_tr)   // apply the fix matrix of a 3MF store
        mv.set_transformation(mv.get_matrix() * mv.emboss_shape->fix_3mf_tr->inverse());
    update_volume(mv, std::move(mesh), base);
    // update_name_in_list: the object list row of the selected item shows the new name (the object row of an embossed object)
    if (mv.get_object()->volumes.size() == 1 && !base.volume_name.empty()) mv.get_object()->name = base.volume_name;
    mv.get_object()->invalidate_bounding_box();
    if (mv.type() == ModelVolumeType::MODEL_PART) mv.get_object()->ensure_on_bed();
    p.changed();
    return true;
}

void local_z_rotate(ModelVolume &mv, int instance, double diff)
{
    ModelInstance *mi = mv.get_object()->instances[instance];
    const bool mirrored = has_reflection(mi->get_matrix()) != has_reflection(mv.get_matrix());
    const Transform3d r = Geometry::rotation_transform(Vec3d(0, 0, mirrored ? -diff : diff));
    if (mv.is_the_only_one_part())   // an embossed object turns its instance
        mi->set_transformation(Geometry::Transformation(mi->get_matrix() * r));
    else
        mv.set_transformation(mv.get_matrix() * r);
    mv.get_object()->invalidate_bounding_box();
}

void local_z_move(ModelVolume &mv, double distance)
{
    mv.set_transformation(mv.get_matrix() * Geometry::translation_transform(Vec3d::UnitZ() * distance));
    mv.get_object()->invalidate_bounding_box();
}

void face_camera(ModelVolume &mv, int instance, const Camera &cam, bool keep_up)
{
    const ModelInstance &inst = *mv.get_object()->instances[instance];
    Transform3d volume_tr = mv.get_matrix();
    std::optional<Transform3d> fix;
    if (mv.emboss_shape && mv.emboss_shape->fix_3mf_tr) { fix = mv.emboss_shape->fix_3mf_tr; volume_tr = volume_tr * fix->inverse(); }
    const Transform3d world_tr = inst.get_matrix() * volume_tr;
    std::optional<float> current_angle;
    std::optional<double> up_limit = keep_up ? std::optional<double>(UP_LIMIT) : std::optional<double>();
    if (up_limit) current_angle = Emboss::calc_up(world_tr, *up_limit);
    const Vec3d world_position = (inst.get_matrix() * mv.get_matrix()).translation();
    const Vec3d wanted_direction = cam.ortho ? Vec3d((cam.eye() - cam.target).normalized()) : Vec3d(cam.eye() - world_position);
    const Transform3d new_volume_tr = surface_transformation(world_tr, wanted_direction, world_position, fix, inst.get_matrix().inverse(), current_angle, up_limit);
    if (mv.is_the_only_one_part()) {
        ModelInstance *mi = mv.get_object()->instances[instance];
        mi->set_transformation(Geometry::Transformation(inst.get_matrix() * new_volume_tr * mv.get_matrix().inverse()));
    } else
        mv.set_transformation(new_volume_tr);
    mv.get_object()->invalidate_bounding_box();
    if (mv.type() == ModelVolumeType::MODEL_PART) mv.get_object()->ensure_on_bed();
}

bool change_type(ModelVolume &mv, int instance, ModelVolumeType nt, const std::string &snapshot)
{
    if (nt == mv.type() || mv.is_the_only_one_part()) return false;
    Plater &p = Plater::get();
    p.take_snapshot(snapshot);
    const bool moved = mv.type() == ModelVolumeType::MODEL_PART || nt == ModelVolumeType::MODEL_PART;
    mv.set_type(nt);
    mv.get_object()->sort_volumes(true);
    p.selection = {index_of(mv, instance)};
    p.changed();
    return moved;
}

static Transform3d world_fixed(const ModelVolume &mv, int instance)
{
    Transform3d volume_tr = mv.get_matrix();
    if (mv.emboss_shape && mv.emboss_shape->fix_3mf_tr) volume_tr = volume_tr * mv.emboss_shape->fix_3mf_tr->inverse();
    return mv.get_object()->instances[instance]->get_matrix() * volume_tr;
}

bool SurfaceDrag::begin(ModelVolume &mv, int instance, const Camera &cam, double px, double py, bool keep_up)
{
    if (mv.is_the_only_one_part()) return false;
    const nlohmann::json h = Viewport::get().pick(px, py);
    if (!h.value("hit", false)) return false;
    const SelItem me = index_of(mv, instance);
    if (h["obj"].get<int>() != me.obj || h["vol"].get<int>() != me.vol) return false;
    const Transform3d world_tr = world_fixed(mv, instance);
    std::optional<float> start_angle;
    if (keep_up) {
        start_angle = Emboss::calc_up(world_tr, UP_LIMIT);
        if (start_angle && has_reflection(world_tr)) start_angle = -*start_angle;
    }
    m_drag = Drag{world_tr, mv.get_object()->instances[instance]->get_matrix().inverse(), start_angle,
                  to_screen(cam, world_tr.translation()) - Vec2d(px, py), mv.get_matrix()};
    return true;
}

void SurfaceDrag::move(ModelVolume &mv, int instance, const Camera &cam, double px, double py, bool keep_up)
{
    if (!m_drag) return;
    const Vec2d p = Vec2d(px, py) + m_drag->offset;
    Vec3d o, d;
    cam.ray(p.x(), p.y(), o, d);
    std::optional<Hit> hit = ray_hit(*mv.get_object(), instance, o, d, mv.id().id);
    if (!hit) return;
    std::optional<Transform3d> fix;
    if (mv.emboss_shape && mv.emboss_shape->fix_3mf_tr) fix = mv.emboss_shape->fix_3mf_tr;
    mv.set_transformation(surface_transformation(m_drag->world, hit->normal, hit->position, fix, m_drag->instance_inv, m_drag->start_angle,
                                                 keep_up ? std::optional<double>(UP_LIMIT) : std::optional<double>()));
    mv.get_object()->invalidate_bounding_box();
    ++Plater::get().revision;
    Viewport::get().request_redraw();
}

void SurfaceDrag::end(ModelVolume &mv)
{
    if (!m_drag) return;
    const Transform3d now = mv.get_matrix();
    mv.set_transformation(m_drag->before);
    Plater::get().take_snapshot(_u8L("Move over surface"));
    mv.set_transformation(now);
    m_drag.reset();
    if (mv.type() == ModelVolumeType::MODEL_PART) mv.get_object()->ensure_on_bed();
}


void rotate_ring(const ModelVolume &volume, int instance, Transform3d &frame, double &radius)
{
    const Transform3d world = volume.get_object()->instances[instance]->get_matrix() * volume.get_matrix();
    const BoundingBoxf3 box = volume.mesh().bounding_box();
    // the rotation part (polar decomposition) and the box scaled into it: Selection::get_bounding_box_in_reference_system
    Eigen::JacobiSVD<Matrix3d> svd(world.linear(), Eigen::ComputeFullU | Eigen::ComputeFullV);
    Matrix3d rot = svd.matrixU() * svd.matrixV().transpose();
    if (rot.determinant() < 0) rot.col(2) = -rot.col(2);
    const Matrix3d scaled = rot.transpose() * world.linear();
    const Vec3d size = (scaled * box.size()).cwiseAbs();
    frame = Transform3d::Identity();
    frame.linear() = rot;
    frame.translation() = world * box.center();
    radius = 5.0 + 0.5 * size.norm();   // GLGizmoRotate::Offset + m_bounding_box.radius()
}
} // namespace orca::emboss
