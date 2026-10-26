// Edits of the selection shared by the gizmos and the object menus.
#pragma once
#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include <set>
#include <vector>

#include "Plater.hpp"

namespace Slic3r { class ModelObject; }

namespace orca {

// GLCanvas3D::do_move / do_rotate / do_scale: a world transformation of the selected instances / parts, with the
// snapshot `name` (none when empty) and the desktop's drop-to-bed rule; returns the project state
nlohmann::json apply_world(const Slic3r::Transform3d &T, const std::string &name);
// the same for given instances (object, instance) and parts
nlohmann::json apply_world_to(const Slic3r::Transform3d &T, const std::string &name, const std::set<std::pair<int, int>> &instances,
                              const std::vector<SelItem> &volumes);
// the world box of the selection (Selection::get_bounding_box)
Slic3r::BoundingBoxf3 selection_world_box();
// Plater::priv::load_model_objects (Project.cpp): adds copies of the objects, places new instances
std::vector<size_t> load_model_objects(const std::vector<Slic3r::ModelObject *> &model_objects, bool allow_negative_z = false, bool auto_drop = true);

} // namespace orca
