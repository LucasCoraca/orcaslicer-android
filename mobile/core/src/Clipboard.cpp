// Cut / Copy / Paste (Plater::cut_selection_to_clipboard, copy_selection_to_clipboard, paste_from_clipboard).
// Selection's clipboard and its copy / paste bodies are compiled from upstream Selection.cpp (extract_functions.py) on a
// stand-in Selection whose GLVolume list is made from the project's selection.
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;

namespace orca {

Vec2f nearest_empty_cell(Plater &p, Vec2f start_point, Vec2f step);

namespace clip {

// the GLVolume of one model volume of one instance
struct GLVolume
{
    int o, i, v;
    int object_idx() const { return o; }
    int instance_idx() const { return i; }
    int volume_idx() const { return v; }
};
using GLVolumePtrs = std::vector<GLVolume *>;
using ObjectIdxsToInstanceIdxsMap = std::map<int, std::set<int>>;

// PartPlate::intersects / get_build_volume of the current plate
struct PartPlate
{
    BoundingBoxf3 box;
    bool intersects(const BoundingBoxf3 &bb) const
    {
        BoundingBoxf3 print_volume(Vec3d(box.min(0), box.min(1), 0.0), Vec3d(box.max(0), box.max(1), 1e3));
        print_volume.min(2) = -1e10;
        print_volume.min(0) -= BuildVolume::BedEpsilon;
        print_volume.min(1) -= BuildVolume::BedEpsilon;
        print_volume.max(0) += BuildVolume::BedEpsilon;
        print_volume.max(1) += BuildVolume::BedEpsilon;
        return print_volume.intersects(bb);
    }
    BoundingBoxf3 get_build_volume() const { return box; }
};

struct Selection
{
    enum EMode { Volume, Instance };
#include "slic3r/GUI/SelectionClipboardDecl.inc"

    struct Cache { ObjectIdxsToInstanceIdxsMap content; };

    bool                    m_valid{true};
    EMode                   m_mode{Instance};
    Model                  *m_model{nullptr};
    GLVolumePtrs           *m_volumes{nullptr};
    std::set<unsigned int>  m_list;
    Cache                   m_cache;
    Clipboard               m_clipboard;
    std::vector<std::unique_ptr<GLVolume>> m_owned;
    GLVolumePtrs            m_glvolumes;
    std::set<SelItem>       m_pasted;

    // the selection of the project as Selection sees it
    void load(Plater &p)
    {
        m_model = &p.model();
        m_owned.clear();
        m_glvolumes.clear();
        m_list.clear();
        m_cache.content.clear();
        m_mode = Instance;
        for (const SelItem &s : p.selection)
            if (s.vol >= 0) m_mode = Volume;
        for (int o = 0; o < int(m_model->objects.size()); ++o) {
            const ModelObject *mo = m_model->objects[o];
            for (int i = 0; i < int(mo->instances.size()); ++i)
                for (int v = 0; v < int(mo->volumes.size()); ++v) {
                    bool sel = false;
                    for (const SelItem &s : p.selection)
                        if (s.obj == o && (s.inst < 0 || s.inst == i || (s.vol >= 0 && i == 0)) && (s.vol < 0 || s.vol == v)) sel = true;
                    m_owned.push_back(std::make_unique<GLVolume>(GLVolume{o, i, v}));
                    m_glvolumes.push_back(m_owned.back().get());
                    if (sel) {
                        m_list.insert(unsigned(m_glvolumes.size() - 1));
                        m_cache.content[o].insert(i);
                    }
                }
        }
        m_volumes = &m_glvolumes;
        m_valid = !m_list.empty();
    }
    bool is_from_single_instance() const { return m_cache.content.size() == 1 && m_cache.content.begin()->second.size() == 1; }
    int  get_object_idx() const { return m_cache.content.size() == 1 ? m_cache.content.begin()->first : -1; }
    int  get_instance_idx() const { return is_from_single_instance() ? *m_cache.content.begin()->second.begin() : -1; }

    void copy_to_clipboard();
    void paste_from_clipboard();
    void paste_volumes_from_clipboard();
    void paste_objects_from_clipboard();

    // what the extracted bodies reach through wxGetApp()
    PartPlate curr_plate() const { return PartPlate{Plater::get().plate_box(Plater::get().current_plate)}; }
    Vec2f nearest_empty_cell(const std::array<double, 2> &start, const std::array<double, 2> &step) const
    {
        return orca::nearest_empty_cell(Plater::get(), Vec2f(float(start[0]), float(start[1])), Vec2f(float(step[0]), float(step[1])));
    }
    // ObjectList::paste_objects_into_list / paste_volumes_into_list: the pasted items become the selection
    void paste_objects_into_list(const std::vector<size_t> &object_idxs)
    {
        for (size_t o : object_idxs) m_pasted.insert(SelItem{int(o), -1, -1});
    }
    void paste_volumes_into_list(int obj_idx, const ModelVolumePtrs &volumes)
    {
        const ModelObject *mo = m_model->objects[obj_idx];
        for (const ModelVolume *v : volumes)
            for (int k = 0; k < int(mo->volumes.size()); ++k)
                if (mo->volumes[k] == v) m_pasted.insert(SelItem{obj_idx, 0, k});
    }
};

#include "slic3r/GUI/SelectionClipboardFns.inc"
#include "slic3r/GUI/SelectionClipboard.inc"

Selection &selection()
{
    static Selection s;
    return s;
}

// Plater::copy_selection_to_clipboard (the object list's settings / layers clipboard is not part of the app yet)
json copy(const json &)
{
    Plater &p = Plater::get();
    Selection &s = selection();
    s.load(p);
    if (!p.selection.empty()) s.copy_to_clipboard();
    return {{"can_paste", !s.m_clipboard.is_empty()}};
}

// Plater::cut_selection_to_clipboard: copy, then erase
json cut(const json &a)
{
    Plater &p = Plater::get();
    if (p.selection.empty()) return p.state();
    copy(a);
    return Core::get().call("selection.delete", json::object());   // its snapshot stands for "Cut Selected Objects"
}

// Plater::can_paste_from_clipboard
bool can_paste()
{
    Plater &p = Plater::get();
    Selection &s = selection();
    if (s.m_clipboard.is_empty()) return false;
    Selection cur;
    cur.load(p);
    const Selection::EMode mode = s.m_clipboard.get_mode();
    if (mode == Selection::Volume && !cur.is_from_single_instance()) return false;
    if (mode == Selection::Instance && cur.m_mode != Selection::Instance) return false;
    return true;
}

json paste(const json &)
{
    Plater &p = Plater::get();
    if (!can_paste()) return p.state();
    p.take_snapshot("Paste From Clipboard");
    Selection &s = selection();
    // the clipboard stays; the selection of the moment decides where volumes go
    Selection cur;
    cur.load(p);
    std::swap(cur.m_clipboard, s.m_clipboard);
    cur.m_valid = true;
    cur.paste_from_clipboard();
    std::swap(cur.m_clipboard, s.m_clipboard);
    if (!cur.m_pasted.empty()) p.selection = cur.m_pasted;
    for (ModelObject *mo : p.model().objects) mo->invalidate_bounding_box();
    p.changed();
    return p.state();
}

} // namespace clip

static Commands reg({
    {"clipboard.copy", clip::copy},
    {"clipboard.cut", clip::cut},
    {"clipboard.paste", clip::paste},
    {"clipboard.can_paste", [](const json &) -> json { return clip::can_paste(); }},
});

} // namespace orca
