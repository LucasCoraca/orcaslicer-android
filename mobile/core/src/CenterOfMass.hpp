// "Center of mass" of the canvas menu (GLCanvas3D::CenterOfMass): markers of the plates, objects, supports and bodies,
// a tap on one shows its details. The class is upstream's, compiled from GLCanvas3D.cpp on a stand-in canvas.
#pragma once
#include <Eigen/Core>

namespace orca {

struct Camera;

class CenterOfMass
{
public:
    static CenterOfMass &get();
    // GL thread: the markers over the scene of Prepare or Preview
    void render(const Camera &cam, bool preview);
    // GL thread, in the ImGui frame: the details of the picked marker
    void render_details(const Camera &cam, bool preview);
    // UI thread: a tap on the canvas; true when it hit a marker (or closed the details)
    bool tap(const Camera &cam, double x, double y, bool preview);
};

} // namespace orca
