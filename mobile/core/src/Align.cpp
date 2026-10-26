// The Move gizmo's Align / Distribute section (GizmoObjectManipulation with GLGizmoAlignment, compiled from upstream):
// the helper runs on a stand-in Selection over the app's selection; its translations are committed like
// GLCanvas3D::do_move (the sinking / auto-drop rule of apply_world_to).
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Core.hpp"
#include "Edit.hpp"
#include "Plater.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI { namespace align {

// a part of the selection as GLCanvas3D's volume list holds it
class GLVolume
{
public:
    GLVolume(int obj, int inst, int vol) : m_obj(obj), m_inst(inst), m_vol(vol) {}
    int object_idx() const { return m_obj; }
    int instance_idx() const { return m_inst; }
    int volume_idx() const { return m_vol; }
    BoundingBoxf3 transformed_convex_hull_bounding_box() const
    {
        const ModelObject *mo = orca::Plater::get().model().objects[m_obj];
        const ModelVolume *mv = mo->volumes[m_vol];
        return mv->get_convex_hull().transformed_bounding_box(mo->instances[m_inst]->get_matrix() * mv->get_matrix());
    }

private:
    int m_obj, m_inst, m_vol;
};

class Selection
{
public:
    enum EMode { Volume, Instance };
    using InstanceIdxsList = std::set<int>;
    using ObjectIdxsToInstanceIdxsMap = std::map<int, InstanceIdxsList>;

    Selection()
    {
        orca::Plater &p = orca::Plater::get();
        Model &m = p.model();
        for (const orca::SelItem &s : p.selection) {
            if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
            if (s.vol >= 0) m_mode = Volume;
        }
        for (const orca::SelItem &s : p.selection) {
            if (s.obj < 0 || s.obj >= int(m.objects.size())) continue;
            const ModelObject *mo = m.objects[s.obj];
            for (int k = 0; k < int(mo->instances.size()); ++k)
                if (s.inst < 0 || s.inst == k) {
                    m_content[s.obj].insert(k);
                    // the GLVolumes of the selection: the selected part, or every part of a selected instance
                    for (int v = 0; v < int(mo->volumes.size()); ++v)
                        if (s.vol < 0 || s.vol == v) m_volumes.emplace_back(s.obj, k, v);
                }
        }
    }
    bool                               is_empty() const { return m_content.empty(); }
    EMode                              get_mode() const { return m_mode; }
    const ObjectIdxsToInstanceIdxsMap &get_content() const { return m_content; }
    Model                             *get_model() const { return &orca::Plater::get().model(); }
    int get_object_idx() const { return m_content.size() == 1 ? m_content.begin()->first : -1; }
    int get_instance_idx() const { return m_content.size() == 1 && m_content.begin()->second.size() == 1 ? *m_content.begin()->second.begin() : -1; }
    std::vector<unsigned int> get_volume_idxs() const
    {
        std::vector<unsigned int> idxs;
        for (size_t i = 0; i < m_volumes.size(); ++i) idxs.push_back(unsigned(i));
        return idxs;
    }
    const GLVolume *get_volume(unsigned int idx) const { return &m_volumes[idx]; }
    void setup_cache() {}
    void synchronize_unselected_volumes() {}
    // the displacements the operation leaves, committed by do_move
    void translate(int obj, int inst, const Vec3d &d) { m_inst_moves[{obj, inst}] += d; }
    void translate(int obj, int inst, int vol, const Vec3d &d, bool) { m_vol_moves[orca::SelItem{obj, inst, vol}] += d; }

    std::map<std::pair<int, int>, Vec3d> m_inst_moves;
    std::map<orca::SelItem, Vec3d>       m_vol_moves;

private:
    EMode                       m_mode{Instance};
    ObjectIdxsToInstanceIdxsMap m_content;
    std::vector<GLVolume>       m_volumes;
};

struct PartPlate
{
    int           idx;
    BoundingBoxf3 get_plate_box() const { return orca::Plater::get().plate_box(idx); }
};
struct PartPlateList
{
    std::vector<PartPlate> plates;
    PartPlateList()
    {
        for (size_t i = 0; i < orca::Plater::get().plates.size(); ++i) plates.push_back({int(i)});
    }
    int        find_instance(int obj, int inst) const { return orca::Plater::get().plate_of(obj, inst); }
    PartPlate *get_plate(int i) { return i >= 0 && i < int(plates.size()) ? &plates[size_t(i)] : nullptr; }
};
struct AlignPlater
{
    PartPlateList  list;
    PartPlateList &get_partplate_list() { return list; }
    void           take_snapshot(const std::string &name) { orca::Plater::get().take_snapshot(name); }
};
struct AlignApp
{
    AlignPlater  plater_;
    AlignPlater *plater() { return &plater_; }
};
AlignApp &wxGetApp()
{
    static AlignApp app;
    app.plater_ = AlignPlater{};
    return app;
}

class GLCanvas3D
{
public:
    Selection  m_selection;
    Selection &get_selection() { return m_selection; }
    // GLCanvas3D::do_move: the instance offsets / part offsets, then the bed rule and the change
    void do_move(const std::string &, bool)
    {
        Model &m = orca::Plater::get().model();
        std::set<std::pair<int, int>> instances;
        std::vector<orca::SelItem>    volumes;
        for (auto &[k, d] : m_selection.m_inst_moves) {
            ModelInstance *mi = m.objects[k.first]->instances[k.second];
            mi->set_offset(mi->get_offset() + d);
            instances.insert(k);
        }
        for (auto &[k, d] : m_selection.m_vol_moves) {
            ModelObject *mo = m.objects[k.obj];
            // a world displacement in the instance's frame (rotation / scale of the instance undone)
            const Transform3d inst = mo->instances[k.inst]->get_matrix_no_offset();
            ModelVolume *mv = mo->volumes[k.vol];
            mv->set_offset(mv->get_offset() + inst.inverse() * d);
            volumes.push_back(k);
        }
        orca::apply_world_to(Transform3d::Identity(), "", instances, volumes);
    }
};

#include "slic3r/GUI/Alignment.inc.cls"
#include "slic3r/GUI/Alignment.inc"

}}} // namespace Slic3r::GUI::align

namespace orca {
using namespace Slic3r::GUI::align;
using AlignType = GLGizmoAlignment::AlignType;

// GizmoObjectManipulation's align section: the modes, and for each button whether it applies (with the reason why not)
static json align_state(const json &a)
{
    GLCanvas3D       canvas;
    GLGizmoAlignment helper(canvas);
    const bool can_selection = helper.can_align(AlignType::X_MIN);
    const bool to_parent     = a.value("parent", false) || !can_selection;
    json modes = json::array();
    if (can_selection) modes.push_back(_u8L("Selection"));
    modes.push_back(helper.is_parts_selection(true) ? _u8L("Object") : _u8L("Plate"));
    json buttons = json::object();
    for (int t = int(AlignType::CENTER_X); t <= int(AlignType::DISTRIBUTE_Z); ++t) {
        const AlignType type = AlignType(t);
        const bool distribute = type >= AlignType::DISTRIBUTE_X;
        const bool enabled = to_parent ? helper.can_align(type, true) : distribute ? helper.can_distribute(type) : helper.can_align(type);
        std::string why;
        if (!enabled) {
            if (helper.get_items_count(type, to_parent, &why) > 0) why = _u8L("Select at least 3 parts or objects");
        }
        buttons[std::to_string(t)] = {{"enabled", enabled}, {"why", why}};
    }
    return {{"modes", modes}, {"parent", to_parent}, {"buttons", buttons}, {"distribute", _u8L("Distribute")},
            {"reference", _u8L("Reference for alignment")}};
}

static json align_run(const json &a)
{
    GLCanvas3D       canvas;
    GLGizmoAlignment helper(canvas);
    const AlignType type = AlignType(a.at("type").get<int>());
    const bool      ok   = type >= AlignType::DISTRIBUTE_X ? helper.distribute_objects(type) : helper.align_objects(type, a.value("parent", false));
    if (!ok) throw CommandError("cannot align");
    return Plater::get().state();
}

static Commands reg({{"align.state", align_state}, {"align.run", align_run}});
} // namespace orca
