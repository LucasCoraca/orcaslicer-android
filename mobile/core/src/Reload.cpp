// Reload from disk / Replace 3D file (Plater::priv::reload_from_disk, reload_all_from_disk, replace_with_stl,
// replace_volume_with_stl). The app copies documents into its cache, so a volume's source file is that copy; the app
// refreshes the copies from the documents it remembers before a reload, and asks for the files that are missing.
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

namespace {

// the volumes of the selection: a whole object selects all its volumes
std::vector<std::pair<int, int>> selected_volumes()
{
    Plater &p = Plater::get();
    const Model &model = p.model();
    std::vector<std::pair<int, int>> out;
    for (const SelItem &s : p.selection) {
        if (s.obj < 0 || s.obj >= int(model.objects.size())) continue;
        const ModelObject *mo = model.objects[s.obj];
        if (s.vol >= 0) { if (s.vol < int(mo->volumes.size())) out.push_back({s.obj, s.vol}); }
        else for (int v = 0; v < int(mo->volumes.size()); ++v) out.push_back({s.obj, v});
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// reloadable_volumes
std::vector<std::pair<int, int>> reloadable_volumes()
{
    const Model &model = Plater::get().model();
    std::vector<std::pair<int, int>> ret;
    for (auto [o_idx, v_idx] : selected_volumes()) {
        const ModelVolume *vol = model.objects[o_idx]->volumes[v_idx];
        if (!vol->source.is_from_builtin_objects && !vol->source.input_file.empty() && !fs::path(vol->source.input_file).extension().string().empty())
            ret.push_back({o_idx, v_idx});
    }
    return ret;
}

Model read_model(const std::string &path)
{
    AppConfig &config = *Core::get().app_config;
    if (boost::iends_with(path, ".stp") || boost::iends_with(path, ".step")) {
        const double linear = std::max(0.003, string_to_double_decimal_point(config.get("linear_deflection")));
        const double angle = std::max(0.5, string_to_double_decimal_point(config.get("angle_deflection")));
        return Model::read_from_step(path, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, nullptr, nullptr, nullptr, linear, angle,
                                     config.get_bool("is_split_compound"));
    }
    PlateDataPtrs plate_data;
    std::vector<Preset *> project_presets;
    Model m = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, &plate_data, &project_presets,
                                    nullptr, nullptr, nullptr, nullptr, nullptr, 0);
    release_PlateData_list(plate_data);
    return m;
}

// Plater::priv::replace_volume_with_stl; an error message, empty when done
std::string replace_volume_with_stl(int object_idx, int volume_idx, const fs::path &new_path, const std::string &snapshot)
{
    Plater &p = Plater::get();
    Model &model = p.model();
    const std::string path = new_path.string();
    Model new_model;
    try {
        new_model = read_model(path);
        for (ModelObject *model_object : new_model.objects) {
            model_object->center_around_origin();
            model_object->ensure_on_bed();
        }
    } catch (std::exception &) {
        return _u8L("Error during replacement");
    }
    if (new_model.objects.empty()) return _u8L("Error during replacement");
    if (new_model.objects.size() > 1 || new_model.objects.front()->volumes.size() > 1) return _u8L("Unable to replace with more than one volume");
    if (!snapshot.empty()) p.take_snapshot(snapshot);
    ModelObject *old_model_object = model.objects[object_idx];
    ModelVolume *old_volume = old_model_object->volumes[volume_idx];
    const bool sinking = old_model_object->min_z() < SINKING_Z_THRESHOLD;
    ModelObject *new_model_object = new_model.objects.front();
    old_model_object->add_volume(*new_model_object->volumes.front());
    ModelVolume *new_volume = old_model_object->volumes.back();
    new_volume->set_new_unique_id();
    new_volume->config.apply(old_volume->config);
    new_volume->set_type(old_volume->type());
    new_volume->set_material_id(old_volume->material_id());
    new_volume->set_transformation(old_volume->get_transformation());
    new_volume->translate(new_volume->get_transformation().get_matrix_no_offset() * (new_volume->source.mesh_offset - old_volume->source.mesh_offset));
    if (old_volume->source.is_converted_from_inches) new_volume->convert_from_imperial_units();
    else if (old_volume->source.is_converted_from_meters) new_volume->convert_from_meters();
    if (Core::get().app_config->get_bool("keep_painting")) {
        auto saved_painting = old_volume->save_painting();
        if (saved_painting) {
            saved_painting->mesh.transform(Geometry::translation_transform(new_volume->mesh().get_init_shift()));
            new_volume->restore_painting(saved_painting);
        }
    } else {
        new_volume->supported_facets.assign(old_volume->supported_facets);
        new_volume->seam_facets.assign(old_volume->seam_facets);
        new_volume->mmu_segmentation_facets.assign(old_volume->mmu_segmentation_facets);
        new_volume->fuzzy_skin_facets.assign(old_volume->fuzzy_skin_facets);
    }
    std::swap(old_model_object->volumes[volume_idx], old_model_object->volumes.back());
    old_model_object->delete_volume(old_model_object->volumes.size() - 1);
    if (!sinking) old_model_object->ensure_on_bed();
    old_model_object->sort_volumes(true);
    // if object has just one volume, rename object too
    if (old_model_object->volumes.size() == 1) old_model_object->name = old_model_object->volumes.front()->name;
    return {};
}

// Plater::priv::reload_from_disk (ENABLE_RELOAD_FROM_DISK_REWORK). "located": {missing source: picked file} for the
// files the app asked for; a picked file of another name replaces the volume (replace_paths).
json reload(const json &a)
{
    Plater &p = Plater::get();
    Model &model = p.model();
    std::vector<std::pair<int, int>> sel = reloadable_volumes();
    if (sel.empty()) return p.state();
    const json located = a.value("located", json::object());
    std::vector<fs::path> input_paths, missing_input_paths;
    std::vector<std::pair<fs::path, fs::path>> replace_paths;
    for (auto [obj_idx, vol_idx] : sel) {
        const ModelObject *object = model.objects[obj_idx];
        const ModelVolume *volume = object->volumes[vol_idx];
        if (fs::exists(volume->source.input_file))
            input_paths.push_back(volume->source.input_file);
        else {
            // searches the source in the same folder containing the object
            bool found = false;
            if (!object->input_file.empty()) {
                fs::path object_path = fs::path(object->input_file).remove_filename();
                if (!object_path.empty()) {
                    object_path /= fs::path(volume->source.input_file).filename();
                    if (fs::exists(object_path)) { input_paths.push_back(object_path); found = true; }
                }
            }
            if (!found) missing_input_paths.push_back(volume->source.input_file);
        }
    }
    std::sort(missing_input_paths.begin(), missing_input_paths.end());
    missing_input_paths.erase(std::unique(missing_input_paths.begin(), missing_input_paths.end()), missing_input_paths.end());
    // the files the user picked for the missing ones
    for (auto it = missing_input_paths.begin(); it != missing_input_paths.end();) {
        auto l = located.find(it->string());
        if (l == located.end()) { ++it; continue; }
        const fs::path sel_path = l->get<std::string>();
        if (boost::algorithm::iequals(it->filename().string(), sel_path.filename().string())) input_paths.push_back(sel_path);
        else replace_paths.emplace_back(*it, sel_path);
        it = missing_input_paths.erase(it);
    }
    if (!missing_input_paths.empty()) {
        // the app asks for the next missing file (the desktop's "Please select a file" dialog) and calls again
        json st = p.state();
        json missing = json::array();
        for (const fs::path &m : missing_input_paths) missing.push_back({{"source", m.string()}, {"name", m.filename().string()}});
        st["missing"] = missing;
        st["title"] = _u8L("Please select a file") + ":";
        st["replace_question"] = _u8L("Do you want to replace it?");
        return st;
    }
    std::sort(input_paths.begin(), input_paths.end());
    input_paths.erase(std::unique(input_paths.begin(), input_paths.end()), input_paths.end());
    std::sort(replace_paths.begin(), replace_paths.end());
    replace_paths.erase(std::unique(replace_paths.begin(), replace_paths.end()), replace_paths.end());

    p.take_snapshot(_u8L("Reload from disk"));
    std::vector<std::string> fail_list;
    const bool keep_painting = Core::get().app_config->get_bool("keep_painting");
    for (const fs::path &input : input_paths) {
        const std::string path = input.string();
        Model new_model;
        try {
            new_model = read_model(path);
            for (ModelObject *model_object : new_model.objects) {
                model_object->center_around_origin();
                model_object->ensure_on_bed();
            }
        } catch (std::exception &) {
            fail_list.push_back(path);
            continue;
        }
        for (auto [obj_idx, vol_idx] : sel) {
            ModelObject *old_model_object = model.objects[obj_idx];
            ModelVolume *old_volume = old_model_object->volumes[vol_idx];
            const bool sinking = old_model_object->min_z() < SINKING_Z_THRESHOLD;
            const bool has_source = !old_volume->source.input_file.empty() &&
                                    boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            const bool has_name = !old_volume->name.empty() && boost::algorithm::iequals(old_volume->name, fs::path(path).filename().string());
            if (!has_source && !has_name) continue;
            int new_volume_idx = -1, new_object_idx = -1;
            bool match_found = false;
            if (has_source && old_volume->source.object_idx < int(new_model.objects.size())) {
                const ModelObject *obj = new_model.objects[old_volume->source.object_idx];
                if (old_volume->source.volume_idx < int(obj->volumes.size())) {
                    const std::string &new_input_file = obj->volumes[old_volume->source.volume_idx]->source.input_file;
                    const std::string &old_input_file = old_volume->source.input_file;
                    if (new_input_file == old_input_file ||
                        boost::algorithm::iequals(fs::path(new_input_file).filename().string(), fs::path(old_input_file).filename().string())) {
                        new_volume_idx = old_volume->source.volume_idx;
                        new_object_idx = old_volume->source.object_idx;
                        match_found = true;
                    }
                }
            }
            if (!match_found && has_name) {
                for (size_t o = 0; o < new_model.objects.size(); ++o) {
                    ModelObject *obj = new_model.objects[o];
                    bool found = false;
                    for (size_t v = 0; v < obj->volumes.size(); ++v)
                        if (obj->volumes[v]->name == old_volume->name) { new_volume_idx = int(v); new_object_idx = int(o); found = true; break; }
                    if (found) break;
                    if (obj->name == old_volume->name) { new_object_idx = int(o); break; }   // a STEP object loaded as a volume
                }
            }
            if (new_object_idx < 0 || int(new_model.objects.size()) <= new_object_idx) {
                fail_list.push_back(has_source ? old_volume->source.input_file : old_volume->name);
                continue;
            }
            ModelObject *new_model_object = new_model.objects[new_object_idx];
            if (int(new_model_object->volumes.size()) <= new_volume_idx) {
                fail_list.push_back(has_source ? old_volume->source.input_file : old_volume->name);
                continue;
            }
            ModelVolume *new_volume = nullptr;
            if (new_volume_idx < 0 && new_object_idx >= 0) {
                TriangleMesh mesh = new_model_object->mesh();
                new_volume = old_model_object->add_volume(std::move(mesh));
                new_volume->name = new_model_object->name;
                new_volume->source.input_file = new_model_object->input_file;
            } else
                new_volume = old_model_object->add_volume(*new_model_object->volumes[new_volume_idx]);
            new_volume->set_new_unique_id();
            new_volume->config.apply(old_volume->config);
            new_volume->set_type(old_volume->type());
            new_volume->set_material_id(old_volume->material_id());
            new_volume->source.mesh_offset = old_volume->source.mesh_offset;
            new_volume->set_transformation(old_volume->get_transformation());
            new_volume->source.object_idx = old_volume->source.object_idx;
            new_volume->source.volume_idx = old_volume->source.volume_idx;
            if (old_volume->source.is_converted_from_inches) new_volume->convert_from_imperial_units();
            else if (old_volume->source.is_converted_from_meters) new_volume->convert_from_meters();
            if (keep_painting) {
                auto saved_painting = old_volume->save_painting();
                if (saved_painting) {
                    saved_painting->mesh.transform(Geometry::translation_transform(new_volume->mesh().get_init_shift()));
                    new_volume->restore_painting(saved_painting);
                }
            }
            std::swap(old_model_object->volumes[vol_idx], old_model_object->volumes.back());
            old_model_object->delete_volume(old_model_object->volumes.size() - 1);
            if (!sinking) old_model_object->ensure_on_bed();
            old_model_object->sort_volumes(Core::get().app_config->get("order_volumes") == "1");
        }
    }
    for (auto [src, dest] : replace_paths)
        for (auto [obj_idx, vol_idx] : sel)
            if (boost::algorithm::iequals(model.objects[obj_idx]->volumes[vol_idx]->source.input_file, src.string()))
                if (!replace_volume_with_stl(obj_idx, vol_idx, dest, "").empty()) break;
    p.changed();
    json st = p.state();
    if (!fail_list.empty()) {
        std::string message = _u8L("Unable to reload:") + "\n";
        for (const std::string &s : fail_list) message += s + "\n";
        st["notice"] = message;
    }
    return st;
}

// reload_all_from_disk: everything, the selection restored after
json reload_all(const json &a)
{
    Plater &p = Plater::get();
    if (p.model().objects.empty()) return p.state();
    const std::set<SelItem> keep = p.selection;
    p.selection.clear();
    for (int o = 0; o < int(p.model().objects.size()); ++o) p.selection.insert({o, -1, -1});
    json st = reload(a);
    p.selection = keep;
    if (!st.contains("missing")) st = p.state();
    return st;
}

// replace_with_stl: the single selected volume from a picked file
json replace(const json &a)
{
    Plater &p = Plater::get();
    std::vector<std::pair<int, int>> sel = selected_volumes();
    if (sel.size() != 1) return p.state();
    const std::string path = a.value("path", std::string());
    if (path.empty()) throw CommandError(_u8L("File for the replacement wasn't selected"));
    const std::string error = replace_volume_with_stl(sel.front().first, sel.front().second, path, _u8L("Replace with 3D file"));
    if (!error.empty()) throw CommandError(error);
    p.changed();
    return p.state();
}

// replace_all_with_stl: each selected volume from the file of the same name in a folder (the app copies the folder
// the user picked into its cache)
json replace_all(const json &a)
{
    Plater &p = Plater::get();
    Model &model = p.model();
    const fs::path out_path = a.value("dir", std::string());
    if (out_path.empty()) throw CommandError(_u8L("Directory for the replace wasn't selected"));
    std::vector<std::pair<int, int>> sel = selected_volumes();
    if (sel.empty())   // plates selected: the volumes of their objects
        for (int o = 0; o < int(model.objects.size()); ++o)
            for (int v = 0; v < int(model.objects[o]->volumes.size()); ++v) sel.push_back({o, v});
    std::string status = _u8L("Replaced with 3D files from directory:\n") + a.value("dir_name", out_path.string()) + "\n\n";
    for (auto [object_idx, volume_idx] : sel) {
        const ModelVolume *volume = model.objects[object_idx]->volumes[volume_idx];
        if (volume->source.input_file.empty()) continue;
        const fs::path input_path = volume->source.input_file;
        const fs::path new_path = out_path / input_path.filename();
        const std::string volume_name = volume->name;
        if (new_path == input_path) { status += GUI::format(_u8L("✖ Skipped %s: same file.\n"), volume_name); continue; }
        if (!fs::exists(new_path)) { status += GUI::format(_u8L("✖ Skipped %s: file does not exist.\n"), volume_name); continue; }
        if (!replace_volume_with_stl(object_idx, volume_idx, new_path, _u8L("Replace with 3D file")).empty()) {
            status += GUI::format(_u8L("✖ Skipped %s: failed to replace.\n"), volume_name);
            continue;
        }
        status += GUI::format(_u8L("✔ Replaced %s.\n"), volume_name);
    }
    p.changed();
    json st = p.state();
    st["message"] = status;
    st["caption"] = _u8L("Replaced volumes");
    return st;
}

// ObjectList::load_subobject + load_modifier: parts / modifiers of the selected object from model files
json load_part(const json &a)
{
    Plater &p = Plater::get();
    Model &model = p.model();
    int obj_idx = -1;
    for (const SelItem &s : p.selection) { obj_idx = s.obj; break; }
    if (obj_idx < 0 || obj_idx >= int(model.objects.size())) return p.state();
    ModelObject &model_object = *model.objects[obj_idx];
    const std::string t = a.value("type", std::string("part"));
    const ModelVolumeType type = t == "negative" ? ModelVolumeType::NEGATIVE_VOLUME : t == "modifier" ? ModelVolumeType::PARAMETER_MODIFIER :
                                 t == "support_blocker" ? ModelVolumeType::SUPPORT_BLOCKER : t == "support_enforcer" ? ModelVolumeType::SUPPORT_ENFORCER :
                                 ModelVolumeType::MODEL_PART;
    p.take_snapshot(type == ModelVolumeType::MODEL_PART ? _u8L("Load Part") : _u8L("Load Modifier"));
    std::vector<ModelVolume *> added_volumes;
    AppConfig &config = *Core::get().app_config;
    for (const auto &f : a.at("paths")) {
        const std::string input_file = f.get<std::string>();
        Model m;
        try {
            if (boost::iends_with(input_file, ".stp") || boost::iends_with(input_file, ".step")) {
                double linear = string_to_double_decimal_point(config.get("linear_deflection"));
                if (linear <= 0) linear = 0.003;
                double angle = string_to_double_decimal_point(config.get("angle_deflection"));
                if (angle <= 0) angle = 0.5;
                m = Model::read_from_step(input_file, LoadStrategy::LoadModel, nullptr, nullptr, nullptr, linear, angle, config.get_bool("is_split_compound"));
            } else
                m = Model::read_from_file(input_file, nullptr, nullptr, LoadStrategy::LoadModel);
        } catch (std::exception &) {
            throw CommandError(_u8L("Error!") + " " + _u8L("Failed to get the model data in the current file."));
        }
        for (auto object : m.objects)
            if (model_object.origin_translation != Vec3d::Zero()) {
                object->center_around_origin();
                const Vec3d delta = model_object.origin_translation - object->origin_translation;
                for (auto volume : object->volumes) volume->translate(delta);
            }
        m.add_default_instances();
        TriangleMesh mesh = m.mesh();
        ModelVolume *new_volume = model_object.add_volume(std::move(mesh), type);
        new_volume->name = fs::path(a.contains("names") ? a["names"][added_volumes.size()].get<std::string>() : input_file).filename().string();
        int extruder_id = 0;
        if (new_volume->type() == ModelVolumeType::MODEL_PART && model_object.config.has("extruder")) extruder_id = model_object.config.opt_int("extruder");
        new_volume->config.set_key_value("extruder", new ConfigOptionInt(extruder_id));
        new_volume->source.input_file = input_file;
        new_volume->source.object_idx = obj_idx;
        new_volume->source.volume_idx = int(model_object.volumes.size()) - 1;
        if (m.objects.size() == 1 && m.objects.front()->volumes.size() == 1)
            new_volume->source.mesh_offset = m.objects.front()->volumes.front()->source.mesh_offset;
        new_volume->set_offset(new_volume->source.mesh_offset - model_object.volumes.front()->source.mesh_offset);
        added_volumes.push_back(new_volume);
    }
    if (added_volumes.empty()) return p.state();
    model_object.sort_volumes(true);   // reorder_volumes_and_get_selection
    p.selection.clear();
    for (int v = 0; v < int(model_object.volumes.size()); ++v)
        if (std::find(added_volumes.begin(), added_volumes.end(), model_object.volumes[v]) != added_volumes.end()) p.selection.insert({obj_idx, 0, v});
    model_object.invalidate_bounding_box();
    p.changed();
    return p.state();
}

} // namespace

// for the menus: whether the selection has volumes to reload / exactly one volume to replace
bool can_reload_from_disk() { return !reloadable_volumes().empty(); }
bool can_replace_with_stl() { return selected_volumes().size() == 1; }

static Commands reg({
    {"object.reload", reload},
    {"object.reload_all", reload_all},
    {"object.replace", replace},
    {"object.replace_all", replace_all},
    {"object.load_part", load_part},
});

} // namespace orca
