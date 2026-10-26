// Plate thumbnails for the G-code and the 3MF (GLCanvas3D::render_thumbnail_internal), rendered offscreen in a private
// EGL context so they can be made on the slicing thread.
#pragma once
#include <memory>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca {

// what one plate's thumbnails show, copied out of the model under the core lock
struct ThumbScene
{
    struct Item
    {
        std::shared_ptr<const Slic3r::TriangleMesh> mesh;
        Slic3r::Transform3d                         matrix;     // world
        Slic3r::ColorRGBA                           color;      // the extruder colour (GLVolume::color)
        int                                         extruder;   // 1 based
        unsigned                                    object_id;  // GLVolume::model_object_ID
        Slic3r::BoundingBoxf3                       bbox;       // world
    };
    std::vector<Item>     items;
    Slic3r::BoundingBoxf3 plate_box;   // PartPlate::get_build_volume
};

// the printable model parts of a plate (call with the core lock held)
ThumbScene thumb_scene(int plate, bool use_loaded_id = false);

enum class ThumbView { Iso, TopPlate };

// one thumbnail; pixels as glReadPixels returns them (bottom row first), like ThumbnailData on the desktop
Slic3r::ThumbnailData render_thumbnail(const ThumbScene &scene, unsigned w, unsigned h, ThumbView view = ThumbView::Iso,
                                       bool for_picking = false, bool ban_light = false);

// Plater::priv::generate_thumbnails for GCode::export (the thumbnails listed by the printer profile)
Slic3r::ThumbnailsList render_thumbnails(const ThumbScene &scene, const Slic3r::ThumbnailsParams &params);

} // namespace orca
