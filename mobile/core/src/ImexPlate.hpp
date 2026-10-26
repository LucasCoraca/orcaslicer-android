// The IDEX/IQEX overlay of a plate: see ImexPlate.cpp
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca {

struct ImexOverlay
{
    // the zone fills as triangles (x y z, world), with their colours
    std::vector<std::pair<std::vector<float>, Slic3r::ColorRGBA>> fills;
    struct Ghost
    {
        Slic3r::Transform3d                         xf;
        std::shared_ptr<const Slic3r::TriangleMesh> mesh;
        Slic3r::ColorRGBA                           color;
        int                                         head{-1};   // the physical head printing it
    };
    std::vector<Ghost>       ghosts;
    std::string              mode;      // the plate's own mode ("primary" when none)
    std::vector<std::string> choices;   // the modes a plate can take
    bool                     mm_conflict{false};   // has_imex_multimaterial_conflict: the badge
};

// GCodeViewer::render_scene's IDEX/IQEX carriages for the Preview: where each secondary carriage sits relative to the
// primary (resolve_imex_marker_plan), the toolhead box offsets and the carriage colours (palette, and the head's
// filament for the Filament view)
struct ImexMarkers
{
    struct Carriage
    {
        bool              mirror_x{false}, mirror_y{false};
        float             x_term{0}, y_term{0}, box_offset_x{0}, box_offset_y{0};
        Slic3r::ColorRGBA palette, filament;
    };
    Slic3r::ColorRGBA     pri_palette, pri_filament;
    float                 pri_box_offset_x{0}, pri_box_offset_y{0}, box_wx{0}, box_wy{0};
    std::vector<Carriage> carriages;   // empty: no secondary carriage in this plate's mode
};
ImexMarkers imex_markers(int plate);   // under the core lock

// the IDEX/IQEX pre-slice warnings of these plates, asked about; false when the user does not slice
bool imex_pre_slice_check(const std::vector<int> &plates);

// the overlay of one plate, empty on a printer that is not IDEX/IQEX (under the core lock)
ImexOverlay imex_overlay(int plate);

} // namespace orca
