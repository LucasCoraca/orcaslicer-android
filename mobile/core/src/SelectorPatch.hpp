// TriangleSelectorPatch (GLGizmoPainterBase.hpp) without its GL side: the colour painting's patches and the gap fill,
// with the two functions compiled from upstream (tools/extract_functions.py -> SelectorPatch.inc).
#pragma once
#include <set>
#include <vector>

#include "libslic3r/TriangleSelector.hpp"

namespace orca {

struct TrianglePatch
{
    std::vector<float>                      patch_vertices;
    std::vector<int>                        triangle_indices;
    std::vector<int>                        facet_indices;
    Slic3r::EnforcerBlockerType             type = Slic3r::EnforcerBlockerType::NONE;
    std::set<Slic3r::EnforcerBlockerType>   neighbor_types;
    float                                   area = 0.f;   // stops growing past GapAreaMax

    bool is_fragment() const;
};

class SelectorPatch : public Slic3r::TriangleSelector
{
public:
    explicit SelectorPatch(const Slic3r::TriangleMesh &mesh) : TriangleSelector(mesh) {}

    // TriangleSelector.m_triangles => patches of connected triangles of one state (gap fill view)
    void update_triangles_per_patch();
    // patches => TriangleSelector.m_triangles: fragments take their neighbour's state
    void update_selector_triangles();
    void clear_patches() { m_triangle_patches.clear(); }
    const std::vector<TrianglePatch> &patches() const { return m_triangle_patches; }

    constexpr static float GapAreaMin = 0.f;
    constexpr static float GapAreaMax = 5.f;
    static float gap_area;

private:
    std::vector<TrianglePatch> m_triangle_patches;
    bool                       m_need_wireframe{false};
};

} // namespace orca
