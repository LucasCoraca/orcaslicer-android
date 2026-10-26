// What the Text and SVG gizmos share (slic3r/GUI/Jobs/EmbossJob): the data a volume is embossed from, the mesh of a
// flat or surface-cut emboss (functions compiled from upstream's EmbossJob.cpp), the placement onto an object's surface.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "libslic3r/Emboss.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca::emboss {

struct JobException : std::runtime_error { using std::runtime_error::runtime_error; };

// Slic3r::GUI::Emboss::DataBase
class DataBase
{
public:
    DataBase(const std::string &volume_name, std::shared_ptr<std::atomic<bool>> cancel)
        : volume_name(volume_name), cancel(std::move(cancel)) {}
    DataBase(const std::string &volume_name, std::shared_ptr<std::atomic<bool>> cancel, Slic3r::EmbossShape &&shape)
        : volume_name(volume_name), cancel(std::move(cancel)), shape(std::move(shape)) {}
    DataBase(DataBase &&) = default;
    virtual ~DataBase() = default;
    virtual Slic3r::EmbossShape &create_shape() { return shape; }
    virtual void write(Slic3r::ModelVolume &volume) const;

    bool is_outside = true;                    // raised (part) or engraved (negative volume)
    Slic3r::Emboss::TextLines text_lines = {}; // per glyph placement
    std::optional<float> from_surface;
    std::string volume_name;
    std::shared_ptr<std::atomic<bool>> cancel;
    Slic3r::EmbossShape shape;
};

// Slic3r::GUI::Emboss::SurfaceVolumeData
struct SurfaceVolumeData
{
    Slic3r::Transform3d transform;
    struct ModelSource
    {
        std::shared_ptr<const Slic3r::TriangleMesh> mesh;
        Slic3r::Transform3d tr;
    };
    using ModelSources = std::vector<ModelSource>;
    ModelSources sources;
};

constexpr double UP_LIMIT = 0.9;   // SurfaceDrag.hpp

// UpdateJob / CreateVolumeJob::process: the mesh of the shape (the default text mesh when nothing could be made);
// throws JobException
Slic3r::TriangleMesh create_mesh(DataBase &input);
// UpdateSurfaceVolumeJob / CreateSurfaceVolumeJob::process: the shape cut from the object's surface
Slic3r::TriangleMesh cut_surface_mesh(DataBase &input, const SurfaceVolumeData &surface);
// create_sources: the object's model parts (except the volume itself) to cut a surface from
SurfaceVolumeData::ModelSources model_sources(const Slic3r::ModelVolumePtrs &volumes, std::optional<size_t> skip_volume_id = {});

// a hit of a world ray on an instance's model parts (RaycastManager::closest_hit), in world coordinates
struct Hit { Slic3r::Vec3d position, normal; int volume; double distance; };
std::optional<Hit> ray_hit(const Slic3r::ModelObject &object, int instance, const Slic3r::Vec3d &origin, const Slic3r::Vec3d &dir,
                           std::optional<size_t> skip_volume_id = {});
// the closest point of the instance's model parts to a world point (RaycastManager::closest)
std::optional<Hit> closest_point(const Slic3r::ModelObject &object, int instance, const Slic3r::Vec3d &point, std::optional<size_t> skip_volume_id = {});

// SurfaceDrag: the signed distance of the volume origin from the surface along -Z of the volume (calc_distance), the
// angle of the text's up vector (calc_angle)
std::optional<float> calc_distance(const Slic3r::ModelVolume &volume, int instance);
std::optional<float> calc_angle(const Slic3r::ModelVolume &volume, int instance);

// SurfaceDrag.cpp get_volume_transformation: the volume matrix that turns its Z to world_dir at world_position
Slic3r::Transform3d surface_transformation(const Slic3r::Transform3d &world, const Slic3r::Vec3d &world_dir, const Slic3r::Vec3d &world_position,
                                           const std::optional<Slic3r::Transform3d> &fix, const Slic3r::Transform3d &instance_inv,
                                           std::optional<float> current_angle = {}, const std::optional<double> &up_limit = {});

// UpdateJob::update_volume: the new mesh and data into the volume
void update_volume(Slic3r::ModelVolume &volume, Slic3r::TriangleMesh &&mesh, const DataBase &base);
// create_volume: a new part of the object (near the object when no transformation is given)
Slic3r::ModelVolume *create_volume(Slic3r::TriangleMesh &&mesh, Slic3r::ModelObject &object, Slic3r::ModelVolumeType type,
                                   const std::optional<Slic3r::Transform3d> &trmat, const DataBase &data);

} // namespace orca::emboss
