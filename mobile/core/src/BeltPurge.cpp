// Belt printers' purge prism (BeltPurgeTower.cpp, compiled from upstream): one auto-managed prism object per plate,
// kept in sync with the config and the plates' contents whenever the project or the presets change (upstream runs it
// in Plater::priv::update_background_process).
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI { namespace belt {

// PartPlate / PartPlateList over the app's plates
class PartPlate
{
public:
    explicit PartPlate(int idx) : m_idx(idx) {}
    bool contain_instance_totally(int obj, int inst) const
    {
        const orca::Plate &p = orca::Plater::get().plates[size_t(m_idx)];
        return p.instances.count({obj, inst}) > 0 && p.outside.count({obj, inst}) == 0;
    }
    Vec3d get_origin() const { return orca::Plater::get().plate_origin(m_idx); }

private:
    int m_idx;
};
class PartPlateList
{
public:
    PartPlateList()
    {
        for (size_t i = 0; i < orca::Plater::get().plates.size(); ++i) plates.emplace_back(int(i));
    }
    int        get_plate_count() const { return int(plates.size()); }
    PartPlate *get_plate(int i) { return i >= 0 && i < int(plates.size()) ? &plates[size_t(i)] : nullptr; }
    int        find_instance(int obj, int inst) const { return orca::Plater::get().plate_of(obj, inst); }
    // the plates' instance sets are rebuilt from the model afterwards (Plater::update_plates)
    void       notify_instance_removed(int, int) {}

private:
    std::vector<PartPlate> plates;
};
// the object list follows the project state
struct ObjectList
{
    void add_object_to_list(size_t, bool = true, bool = true, bool = true) {}
    void delete_object_from_list(size_t) {}
};
struct BeltApp
{
    PresetBundle *preset_bundle{orca::Core::get().preset_bundle.get()};
};
BeltApp wxGetApp() { return {}; }

#include "slic3r/GUI/BeltPurgeTower.inc.types"
#include "slic3r/GUI/BeltPurgeTower.inc"

}}} // namespace Slic3r::GUI::belt

namespace orca {
// Plater::priv::ensure_belt_purge_tower: true when the model changed (the plates are updated again)
bool ensure_belt_purge_tower()
{
    static std::vector<Slic3r::GUI::belt::BeltPurgeSignature> sigs;
    Slic3r::GUI::belt::PartPlateList plates;
    Slic3r::GUI::belt::ObjectList    list;
    if (!Slic3r::GUI::belt::ensure_belt_purge_tower(Plater::get().model(), plates, &list, sigs)) return false;
    Plater::get().update_plates();
    return true;
}
} // namespace orca
