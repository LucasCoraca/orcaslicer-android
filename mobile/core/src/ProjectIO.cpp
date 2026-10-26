// Saving: the project 3MF (Plater::save_project), the sliced plate 3MF (Plater::export_gcode_3mf) and STL export
// (Plater::export_stl), all through libslic3r's writers like on the desktop. The app copies the file to the
// document the user picked.
#include <cstdio>
#include <fstream>
#include <unistd.h>
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "Plater.hpp"
#include "Slicer.hpp"
#include "Thumbnails.hpp"
#include "libslic3r/CSGMesh/CSGMeshCopy.hpp"
#include "libslic3r/CSGMesh/ModelToCSGMesh.hpp"
#include "libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/GCodeStatistics.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

void add_to_recent_projects(const std::string &filename);   // Home.cpp

namespace {

// Plater.cpp publish(): unsaved SVG shapes get their path inside the archive
using SvgFile = EmbossShape::SvgFile;
using SvgFiles = std::vector<SvgFile *>;
#include "slic3r/GUI/Publish.inc"


const unsigned THUMBNAIL_3MF = 512;   // Plater.cpp THUMBNAIL_SIZE_3MF
const int      PLATE_ALL_IDX = -2;    // PartPlate.hpp

// the per plate thumbnails Plater::export_3mf makes (lit iso, unlit iso, top, top picking)
struct PlateThumbs
{
    std::vector<ThumbnailData> lit, no_light, top, pick;
};

PlateThumbs plate_thumbnails()
{
    Plater &p = Plater::get();
    PlateThumbs t;
    for (int i = 0; i < int(p.plates.size()); ++i) {
        ThumbScene scene = thumb_scene(i, true);
        t.lit.push_back(render_thumbnail(scene, THUMBNAIL_3MF, THUMBNAIL_3MF, ThumbView::Iso));
        t.no_light.push_back(render_thumbnail(scene, THUMBNAIL_3MF, THUMBNAIL_3MF, ThumbView::Iso, false, true));
        t.top.push_back(render_thumbnail(scene, THUMBNAIL_3MF, THUMBNAIL_3MF, ThumbView::TopPlate));
        t.pick.push_back(render_thumbnail(scene, THUMBNAIL_3MF, THUMBNAIL_3MF, ThumbView::TopPlate, true, true));
    }
    return t;
}

// PartPlateList::store_to_3mf_structure
PlateDataPtrs plate_structure(bool with_slice_info, int plate_idx, bool with_statistics, PlateThumbs &thumbs)
{
    Plater &p = Plater::get();
    PlateDataPtrs list;
    for (int i = 0; i < int(p.plates.size()); ++i) {
        const Plate &pl = p.plates[i];
        PlateData *d = new PlateData();
        d->locked = pl.locked;
        d->plate_index = i;
        d->plate_name = pl.name;
        d->plate_thumbnail.load_from(thumbs.lit[i]);
        d->config.apply(pl.config);
        if (thumbs.no_light[i].is_valid()) d->no_light_thumbnail_file = "valid_no_light";
        if (thumbs.top[i].is_valid()) d->top_file = "valid_top";
        if (thumbs.pick[i].is_valid()) d->pick_file = "valid_pick";
        for (auto &oi : pl.instances) d->objects_and_instances.emplace_back(oi.first, oi.second);
        if (with_slice_info && (plate_idx == i || plate_idx == PLATE_ALL_IDX)) {
            PlateSlice *ps = Slicer::get().slice(i);
            if (ps && ps->valid && pl.slice_valid && ps->print) {
                std::lock_guard<std::mutex> l(Slicer::get().result_mutex);
                GCodeProcessorResult &r = ps->result;
                d->gcode_file = r.filename;
                d->is_sliced_valid = true;
                d->gcode_prediction = std::to_string(int(r.print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)].time));
                d->toolpath_outside = r.toolpath_outside;
                d->timelapse_warning_code = r.timelapse_warning_code;
                d->is_label_object_enabled = r.label_object_enabled;
                d->limit_filament_maps = r.limit_filament_maps;
                d->layer_filaments = r.layer_filaments;
                d->filament_change_sequence = r.filament_change_sequence;
                d->nozzle_change_sequence = r.nozzle_change_sequence;
                d->optimal_assignment = r.optimal_assignment;
                d->first_layer_time = std::to_string(r.initial_layer_time);
                const PrintStatistics &st = ps->print->print_statistics();
                if (st.total_weight != 0.0) {
                    CNumericLocalesSetter locales;
                    char buf[64];
                    snprintf(buf, sizeof buf, "%.2f", st.total_weight);
                    d->gcode_weight = buf;
                }
                d->is_support_used = ps->print->is_support_used();
                d->parse_filament_info(&r);
                if (with_statistics) d->statistics = gcode_statistics_json(r);
            }
        }
        list.push_back(d);
    }
    return list;
}

// Plater::export_3mf
void export_3mf(const std::string &path, SaveStrategy strategy, int export_plate_idx)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    PresetBundle &bundle = *core.preset_bundle;
    publish(p.model(), strategy);   // take care about private data stored into .3mf
    DynamicPrintConfig cfg = bundle.full_config_secure();
    // Plater::export_3mf: no thumbnails for a backup
    PlateThumbs thumbs;
    if (strategy & SaveStrategy::Backup) {
        thumbs.lit.resize(p.plates.size()); thumbs.no_light.resize(p.plates.size());
        thumbs.top.resize(p.plates.size()); thumbs.pick.resize(p.plates.size());
    } else
        thumbs = plate_thumbnails();
    PlateDataPtrs plates = plate_structure(strategy & SaveStrategy::WithGcode || strategy & SaveStrategy::WithSliceInfo, export_plate_idx,
                                           strategy & SaveStrategy::WithStatistics, thumbs);
    std::vector<ThumbnailData *> lit, no_light, top, pick, calib;
    std::vector<PlateBBoxData *> bboxes;
    std::vector<PlateBBoxData> bbox_store(p.plates.size());
    for (size_t i = 0; i < p.plates.size() && !(strategy & SaveStrategy::Backup); ++i) {
        lit.push_back(&thumbs.lit[i]);
        no_light.push_back(&thumbs.no_light[i]);
        top.push_back(&thumbs.top[i]);
        pick.push_back(&thumbs.pick[i]);
        bboxes.push_back(&bbox_store[i]);
    }
    StoreParams sp;
    sp.path = path;
    sp.model = &p.model();
    sp.plate_data_list = plates;
    sp.export_plate_idx = export_plate_idx;
    sp.project_presets = bundle.get_current_project_embedded_presets();
    sp.config = &cfg;
    sp.thumbnail_data = lit;
    sp.no_light_thumbnail_data = no_light;
    sp.top_thumbnail_data = top;
    sp.pick_thumbnail_data = pick;
    sp.calibration_thumbnail_data = calib;
    sp.id_bboxes = bboxes;
    sp.strategy = strategy | SaveStrategy::Zip64;
    // the type and colour of the filaments each plate uses
    auto *filament_color = dynamic_cast<const ConfigOptionStrings *>(cfg.option("filament_colour"));
    auto *nozzle = dynamic_cast<const ConfigOptionFloats *>(cfg.option("nozzle_diameter"));
    auto *filament_ids = dynamic_cast<const ConfigOptionStrings *>(cfg.option("filament_ids"));
    std::string printer_model_id = bundle.printers.get_edited_preset().get_printer_type(&bundle);
    for (PlateData *d : plates) {
        d->printer_model_id = printer_model_id;
        if (nozzle) d->nozzle_diameters = nozzle->serialize();
        for (auto &f : d->slice_filaments_info) {
            std::string display_type;
            f.type = cfg.get_filament_type(display_type, f.id);
            f.filament_id = filament_ids ? filament_ids->get_at(f.id) : "";
            f.color = filament_color ? filament_color->get_at(f.id) : "#FFFFFF";
        }
    }
    bool ok = store_bbs_3mf(sp);
    release_PlateData_list(plates);
    if (!ok) throw CommandError(_u8L("Failed to save the project.\nPlease check whether the folder exists online or if other programs have the project file open."));
}

// Plater::save_project
json save_project(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    std::string path = str_arg(a, "path");
    SaveStrategy strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    if (core.app_config->get_bool("export_sources_full_pathnames")) strategy = strategy | SaveStrategy::FullPathSources;
    export_3mf(path, strategy, -1);
    Slic3r::remove_backup(p.model(), false);   // Plater::export_3mf: a saved project is not offered for restoring
    add_to_recent_projects(path);
    if (a.contains("name")) p.project_name = fs::path(a["name"].get<std::string>()).stem().string();
    p.dirty = false;
    json st = p.state();
    Host::event("project", st);
    return st;
}

// Plater::export_gcode_3mf: the sliced plate(s) with their G-code
json export_sliced(const json &a)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    std::string path = str_arg(a, "path");
    int plate = a.value("plate", p.current_plate);
    if (a.value("all", false)) plate = PLATE_ALL_IDX;
    SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    if (core.app_config->get_bool("export_statistics")) strategy = strategy | SaveStrategy::WithStatistics;
    export_3mf(path, strategy, plate);
    return nullptr;
}

// Plater::export_stl, FFF without booleans (EXPORT_WITH_BOOLEAN 0): negative parts subtracted when possible
TriangleMesh mesh_to_export(const ModelObject &mo, int instance_id)
{
    TriangleMesh mesh;
    std::vector<csg::CSGPart> csgmesh;
    csgmesh.reserve(2 * mo.volumes.size());
    csg::model_to_csgmesh(mo, Transform3d::Identity(), std::back_inserter(csgmesh), csg::mpartsPositive | csg::mpartsNegative | csg::mpartsDoSplits);
    auto csgrange = range(csgmesh);
    if (csg::is_all_positive(csgrange))
        mesh = TriangleMesh{csg::csgmesh_merge_positive_parts(csgrange)};
    else if (std::get<2>(csg::check_csgmesh_booleans(csgrange)) == csgrange.end()) {
        try {
            auto cgalm = csg::perform_csgmesh_booleans(csgrange);
            mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalm);
        } catch (...) {}
    }
    if (mesh.empty()) {
        for (const ModelVolume *v : mo.volumes)
            if (v->is_model_part()) {
                TriangleMesh vol_mesh(v->mesh());
                vol_mesh.transform(v->get_matrix(), true);
                mesh.merge(vol_mesh);
            }
    }
    if (instance_id == -1) {
        TriangleMesh vols_mesh(mesh);
        mesh = TriangleMesh();
        for (const ModelInstance *i : mo.instances) {
            TriangleMesh m = vols_mesh;
            m.transform(i->get_matrix(), true);
            mesh.merge(m);
        }
    } else if (0 <= instance_id && instance_id < int(mo.instances.size()))
        mesh.transform(mo.instances[instance_id]->get_matrix(), true);
    return mesh;
}

// Plater::export_stl(extended, selection_only, multi_stls, file_type): one STL / DRC, or one file per object into a
// folder ("dir": the app copies the files into the folder the user picked); returns the written files
json export_stl(const json &a)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    if (m.objects.empty()) throw CommandError(_u8L("No objects to export."));
    const bool drc = a.value("format", std::string("stl")) == "drc";
    const bool multi = a.contains("dir");
    int quality = DRC_BITS_DEFAULT;
    if (drc) { const std::string bits = Core::get().app_config->get("drc_bits"); if (!bits.empty()) quality = std::stoi(bits); }
    auto store = [&](const std::string &path, TriangleMesh &mesh) {
        if (!(drc ? store_drc(path.c_str(), &mesh, quality) : store_stl(path.c_str(), &mesh, true))) throw CommandError("export failed");
    };
    json files = json::array();
    auto get_save_file = [&](const std::string &dir, const std::string &name) {
        const std::string ext = drc ? ".drc" : ".stl";
        std::string path = dir + "/" + name + ext;
        int n = 1;
        while (fs::exists(path)) path = dir + "/" + name + "(" + std::to_string(n++) + ")" + ext;
        return path;
    };
    TriangleMesh mesh;
    std::set<int> objs;
    if (a.value("selection_only", false)) {
        for (auto &s : p.selection) objs.insert(s.obj);
        if (objs.empty()) throw CommandError(_u8L("No object is selected."));
    } else
        for (int o = 0; o < int(m.objects.size()); ++o) objs.insert(o);
    if (multi) {
        const std::string dir = a["dir"];
        fs::create_directories(dir);
        for (int o : objs) {
            const ModelObject *mo = m.objects[o];
            TriangleMesh om = mesh_to_export(*mo, -1);
            om.translate(-mo->origin_translation.cast<float>());
            const std::string path = get_save_file(dir, mo->name);
            store(path, om);
            files.push_back(path);
        }
        return {{"files", files}};
    }
    if (a.value("selection_only", false) && objs.size() == 1) {
        const ModelObject *mo = m.objects[*objs.begin()];
        const SelItem &s = *p.selection.begin();
        if (s.vol >= 0) {   // a part: its mesh in the object
            mesh = mo->volumes[s.vol]->mesh();
            mesh.transform(mo->volumes[s.vol]->get_matrix(), true);
        } else
            mesh = mesh_to_export(*mo, mo->instances.size() > 1 ? -1 : std::max(0, s.inst));
        if (mo->instances.size() == 1) mesh.translate(-mo->origin_translation.cast<float>());
    } else
        for (int o : objs) mesh.merge(mesh_to_export(*m.objects[o], -1));
    const std::string path = str_arg(a, "path");
    store(path, mesh);
    files.push_back(path);
    return {{"files", files}};
}

// Plater::export_core_3mf: a 3MF without the BBS extensions (SaveStrategy::Silence)
json export_generic_3mf(const json &a)
{
    export_3mf(str_arg(a, "path"), SaveStrategy::Silence, -1);
    return nullptr;
}

} // namespace

// Plater::export_3mf for the other commands (Plater::export_published_3mf)
void write_3mf(const std::string &path, SaveStrategy strategy, int export_plate_idx) { export_3mf(path, strategy, export_plate_idx); }

// Plater::save_project called inside another command (close_with_confirm, the Troubleshoot Center): the app picks the
// document (the opened one, or Save Project as) and passes it open; the 3MF is written into it. false: canceled
bool save_project_through_app()
{
    json ans = Host::ask({{"kind", "save_project"}});
    if (!ans.contains("fd")) return false;
    const int fd = ans["fd"].get<int>();
    Plater &p = Plater::get();
    fs::path tmp = fs::path(temporary_dir()) / "save_project.3mf";
    SaveStrategy strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    if (Core::get().app_config->get_bool("export_sources_full_pathnames")) strategy = strategy | SaveStrategy::FullPathSources;
    export_3mf(tmp.string(), strategy, -1);
    bool ok = false;
    if (FILE *out = fdopen(fd, "wb")) {
        std::ifstream in(tmp.string(), std::ios::binary);
        char buf[1 << 16];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0) fwrite(buf, 1, size_t(in.gcount()), out);
        ok = fclose(out) == 0;
    } else
        ::close(fd);
    fs::remove(tmp);
    if (!ok) return false;
    if (ans.contains("name")) p.project_name = fs::path(ans["name"].get<std::string>()).stem().string();
    p.dirty = false;
    Host::event("project", p.state());
    return true;
}

// for the backup (Backup.cpp)
void export_3mf_file(const std::string &path, SaveStrategy strategy, int export_plate_idx) { export_3mf(path, strategy, export_plate_idx); }

static Commands reg({
    {"project.save", save_project},
    {"project.export_sliced", export_sliced},
    {"export.stl", export_stl},
    {"export.generic_3mf", export_generic_3mf},
});

} // namespace orca
