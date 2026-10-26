// The dovetail cut plane mesh (GLGizmoCut3D::its_make_groove_plane), compiled from upstream's own code: the function
// bodies are copied out of GLGizmoCut.cpp at build time (tools/extract_functions.py -> GroovePlane.inc).
#pragma once
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace orca {

struct GroovePlane
{
    Slic3r::Cut::Groove        m_groove;
    double                     m_radius{0};
    int                        m_groove_count{1};
    float                      m_groove_gap{10.f};
    std::vector<Slic3r::Vec3d> m_groove_vertices;   // the groove's edges, for has_valid_groove
    Slic3r::BoundingBoxf3      m_bounding_box;

    std::vector<Slic3r::Vec3i32> offset_indices(const std::vector<Slic3r::Vec3i32> &base_indices, size_t vo);
    indexed_triangle_set         its_make_groove_plane();
};

} // namespace orca
