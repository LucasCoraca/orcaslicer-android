#include "SelectorPatch.hpp"

#include <queue>

using namespace Slic3r;

namespace orca {

bool TrianglePatch::is_fragment() const { return this->area < SelectorPatch::gap_area; }

float SelectorPatch::gap_area = SelectorPatch::GapAreaMin;

#include "slic3r/GUI/SelectorPatch.inc"

} // namespace orca
