// Project commands: import (desktop Plater::priv::load_files / load_model_objects), object and plate operations,
// selection, undo / redo.
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <regex>

#include "Core.hpp"
#include "Edit.hpp"
#include "Host.hpp"
#include "I18n.hpp"
#include "Plater.hpp"
#include "Settings.hpp"
#include "Slicer.hpp"
#include "Preview.hpp"
#include "Published.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <climits>
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;
using GUI::MessageDialog;

namespace orca {

static const std::regex pattern_3mf(".*3mf", std::regex::icase);
static const std::regex pattern_any_amf(".*[.](amf|amf[.]xml|zip[.]amf)", std::regex::icase);

static void progress(const std::string &msg, int percent) { Host::event("progress", {{"text", msg}, {"percent", percent}}); }

// GLCanvas3D::get_empty_cells / get_nearest_empty_cell: free spots on the current plate, nearest to start_point first
Vec2f nearest_empty_cell(Plater &p, Vec2f start_point, Vec2f step = {10, 10})
{
    BoundingBoxf3 bv = p.plate_box(p.current_plate);
    BoundingBoxf  bbox(Vec2d(bv.min.x(), bv.min.y()), Vec2d(bv.max.x(), bv.max.y()));
    std::vector<Vec2f> cells, free;
    float min_x = start_point.x() - step(0) * int((start_point.x() - bbox.min.x()) / step(0));
    float min_y = start_point.y() - step(1) * int((start_point.y() - bbox.min.y()) / step(1));
    for (float x = min_x; x < bbox.max.x() - step(0) / 2; x += step(0))
        for (float y = min_y; y < bbox.max.y() - step(1) / 2; y += step(1)) cells.emplace_back(x, y);
    Polygons hulls;
    for (ModelObject *mo : p.model().objects)
        for (ModelInstance *mi : mo->instances) hulls.push_back(mi->convex_hull_2d());
    for (const Vec2f &c : cells) {
        Point pt(scale_(c.x()), scale_(c.y()));
        if (std::none_of(hulls.begin(), hulls.end(), [&](const Polygon &h) { return h.contains(pt); })) free.push_back(c);
    }
    std::sort(free.begin(), free.end(), [start_point](const Vec2f &a, const Vec2f &b) { return (a - start_point).norm() < (b - start_point).norm(); });
    if (!free.empty()) return free.front();
    double offset = 0.05 * std::max(bbox.size()[0], bbox.size()[1]);
    return {float(start_point(0) + offset), float(start_point(1) + offset)};
}

// Plater::priv::load_model_objects
std::vector<size_t> load_model_objects(const ModelObjectPtrs &model_objects, bool allow_negative_z, bool auto_drop)
{
    Plater &p = Plater::get();
    Model  &model = p.model();
    const BoundingBoxf bed = p.bed_box();
    const Vec3d bed_size = Slic3r::to_3d(bed.size(), 1.0) - 2.0 * Vec3d::Ones();
    const Vec3d origin = p.plate_origin(p.current_plate);
    const Vec2d bed_center = bed.center() + Vec2d(origin.x(), origin.y());
    std::vector<size_t> obj_idxs;
    size_t obj_count = model.objects.size();
    ModelInstancePtrs new_instances;
    for (ModelObject *model_object : model_objects) {
        auto *object = model.add_object(*model_object);
        object->sort_volumes(true);
        obj_idxs.push_back(obj_count++);
        if (model_object->instances.empty()) {
            object->center_around_origin();
            new_instances.emplace_back(object->add_instance());
        }
        for (size_t i = 0; i < object->instances.size(); ++i) {
            ModelInstance *instance = object->instances[i];
            const Vec3d ratio = object->instance_bounding_box(i).size().cwiseQuotient(bed_size);
            const double max_ratio = std::max(ratio(0), ratio(1));
            if (max_ratio > 10000) {
                MessageDialog dlg(nullptr, _L("Your object appears to be too large, do you want to scale it down to fit the print bed automatically?"),
                                  _L("Object too large"), wxICON_QUESTION | wxYES);
                dlg.ShowModal();
                object->scale_mesh_after_creation(1. / max_ratio);
                object->origin_translation = Vec3d::Zero();
                object->center_around_origin();
                break;
            } else if (max_ratio > 10) {
                MessageDialog dlg(nullptr, _L("Your object appears to be too large, do you want to scale it down to fit the print bed automatically?"),
                                  _L("Object too large"), wxICON_QUESTION | wxYES | wxNO);
                if (dlg.ShowModal() == wxID_YES) instance->set_scaling_factor(instance->get_scaling_factor() / max_ratio);
            }
        }
        if (!auto_drop) {
            object->auto_drop = false;
            object->translate_instances(Vec3d(0, 0, -std::min(object->min_z(), 0.)));
        } else
            object->ensure_on_bed(allow_negative_z);
        for (ModelObject *mo : model.objects)
            for (ModelInstance *mi : mo->instances)
                if (!mi->is_assemble_initialized()) mi->set_assemble_transformation(mi->get_transformation());
    }
    // AUTOPLACEMENT_ON_LOAD: new instances go to the bed centre, or the nearest free cell of a non-empty plate
    for (ModelInstance *instance : new_instances) {
        Vec3d offset = instance->get_offset();
        bool plate_empty = p.plates[p.current_plate].instances.empty();
        Vec2f cell = plate_empty ? Vec2f(bed_center.x(), bed_center.y()) : nearest_empty_cell(p, Vec2f(bed_center.x(), bed_center.y()));
        instance->set_offset(Vec3d(cell.x(), cell.y(), offset(2)));
        p.update_plates();
    }
    return obj_idxs;
}

// PartPlateList::load_from_3mf_structure (the plate part; slice results are not restored)
static void load_plates(PlateDataPtrs &plate_data)
{
    Plater &p = Plater::get();
    p.plates.clear();
    for (PlateData *pd : plate_data) {
        Plate plate;
        plate.locked = pd->locked;
        plate.config.apply(pd->config);
        plate.name = pd->plate_name;
        p.plates.push_back(std::move(plate));
    }
    if (p.plates.empty()) p.plates.emplace_back();
    p.current_plate = 0;
}

// Plater::priv::load_files

// Plater::load_gcode: a G-code file shown in the preview only (the G-code viewer mode), with the settings it was sliced
// with loaded like a project's
static json load_gcode(const std::string &path, const std::string &name)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    PresetBundle &preset_bundle = *core.preset_bundle;
    std::optional<DynamicPrintConfig> file_config;
    ConfigSubstitutions substitutions;
    try {
        file_config = preset_bundle.config_from_gcode(path, ForwardCompatibilitySubstitutionRule::Enable, substitutions);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": no settings to load from " << path << ": " << ex.what();
    }
    p.reset_project();   // new_project
    p.only_gcode = true;
    if (file_config) {
        try {
            preset_bundle.load_config_model(path, std::move(*file_config));
            presets_changed();
        } catch (const std::exception &ex) {
            json st = p.state();
            st["notice"] = GUI::format(_L("Failed loading file \"%1%\". An invalid configuration was found."), name) + "\n" + ex.what();
        }
    }
    // process the G-code
    GCodeProcessor processor;
    processor.init_filament_maps_and_nozzle_type_when_import_only_gcode();
    try {
        GCodeProcessor::s_IsBBLPrinter = preset_bundle.is_bbl_vendor();
        processor.process_file(path);
    } catch (const std::exception &ex) {
        p.only_gcode = false;
        MessageDialog(nullptr, ex.what(), _u8L("Error"), wxICON_ERROR | wxOK).ShowModal();
        return p.state();
    }
    PlateSlice *ps = Slicer::get().slice(0);
    {
        std::lock_guard<std::mutex> l(Slicer::get().result_mutex);
        ps->result = std::move(processor.extract_result());
        if (!ps->print) ps->print = std::make_unique<Print>();
        Print &print = *ps->print;
        BedType bed_type = ps->result.bed_type;
        if (bed_type != BedType::btCount) preset_bundle.project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));
        print.apply(p.model(), preset_bundle.full_config());
        print.apply_config_for_render(processor.export_config_for_render());
        // the cost of a G-code that was dragged in
        double total_cost = 0.0;
        for (auto volume : ps->result.print_statistics.total_volumes_per_extruder) {
            const size_t extruder_id = volume.first;
            if (extruder_id >= ps->result.filament_densities.size() || extruder_id >= ps->result.filament_costs.size()) continue;
            const double weight = volume.second * ps->result.filament_densities.at(extruder_id) * 0.001;
            total_cost += weight * ps->result.filament_costs.at(extruder_id) * 0.001;
        }
        print.print_statistics().total_cost = total_cost;
        print.set_gcode_file_ready();
        ps->gcode_path = path;
        ps->valid = true;
        ps->result_id++;
    }
    p.plates[0].slice_valid = true;
    core.invalidate_full_config();
    Preview::get().result_changed(0);
    json st;
    if (ps->result.moves.empty()) {
        MessageDialog(nullptr, GUI::format(_L("The selected file:\n%1%\ndoes not contain valid G-code."), name),
                      std::string("G-code viewer") + " - " + _u8L("An Error has occurred while loading the G-code file."), wxICON_WARNING | wxOK).ShowModal();
        p.only_gcode = false;
        p.project_name = _u8L("Untitled");
    } else
        p.project_name = fs::path(name).stem().string();
    p.dirty = false;
    st = p.state();
    st["show_preview"] = true;
    return st;
}

void add_to_recent_projects(const std::string &filename);

int close_with_confirm();
void reset_calib_params();
bool texture_import(Slic3r::Model &model);   // Filaments.cpp

static json load_files(const json &a)
{
    Core   &core = Core::get();
    Plater &p = Plater::get();
    std::vector<std::string> input_files;
    for (auto &f : a.at("paths")) input_files.push_back(f.get<std::string>());
    if (input_files.empty()) return p.state();
    // Plater::load_project (Open Project, a recent project): a new project first, saving the open one when wanted
    if (a.value("new_project", false)) {
        if (close_with_confirm() == wxID_CANCEL) {
            json st = p.state();
            st["canceled"] = true;
            return st;
        }
        p.reset_project();
        reset_calib_params();
    }
    // Plater::load_files: a G-code goes to the preview (load_gcode)
    if (input_files.size() == 1 && boost::iends_with(a.contains("names") && !a["names"].empty() ? a["names"][0].get<std::string>() : input_files[0], ".gcode"))
        return load_gcode(input_files[0], a.contains("names") && !a["names"].empty() ? a["names"][0].get<std::string>() : fs::path(input_files[0]).filename().string());
    LoadStrategy strategy = LoadStrategy::LoadModel;
    if (a.value("load_config", true)) strategy = strategy | LoadStrategy::LoadConfig;
    // Plater::load_project(last_backup, originfile): the backup of an unsaved project
    const bool restore = a.contains("restore_origin");
    if (restore) strategy = strategy | LoadStrategy::Restore;
    bool load_model = strategy & LoadStrategy::LoadModel;
    bool opened_project = false;
    bool load_config = strategy & LoadStrategy::LoadConfig;
    bool imperial_units = false;
    const bool ask_multi = a.value("ask_multi", true);
    PresetBundle &preset_bundle = *core.preset_bundle;
    int filaments_cnt = int(preset_bundle.filament_presets.size());
    bool one_by_one = input_files.size() == 1;
    std::unique_ptr<Model> new_model = (!load_model || one_by_one) ? nullptr : std::make_unique<Model>();
    std::vector<size_t> obj_idxs;
    p.take_snapshot("Load file");

    for (size_t i = 0; i < input_files.size(); ++i) {
        const fs::path path(input_files[i]);
        const std::string filename = a.contains("names") && i < a["names"].size() ? a["names"][i].get<std::string>() : path.filename().string();
        progress(translate("Loading file") + ": " + filename, int(100 * i / input_files.size()));
        const bool type_3mf = std::regex_match(path.string(), pattern_3mf);
        const bool type_any_amf = !type_3mf && std::regex_match(path.string(), pattern_any_amf);
        Model model;
        bool is_project_file = false;
        bool this_load_config = load_config;
        bool loaded_published = false;   // a published 3MF loads as a new project
        try {
            if (type_3mf) {
                DynamicPrintConfig config, config_loaded;
                Semver file_version;
                En3mfType en_3mf_file_type = En3mfType::From_BBS;
                PlateDataPtrs plate_data;
                ConfigSubstitutionContext config_substitutions{ForwardCompatibilitySubstitutionRule::Enable};
                std::vector<Preset *> project_presets;
                LoadStrategy s = strategy;
                if (this_load_config) s = s | LoadStrategy::CheckVersion | LoadStrategy::LoadAuxiliary;
                model = Model::read_from_archive(path.string(), &config_loaded, &config_substitutions, en_3mf_file_type, s, &plate_data, &project_presets, &file_version,
                    [&](int, int, int, bool &cancel) { cancel = false; });
                // a "published" 3MF: the user's presets stay, only the author-selected keys are overlaid; its values travel
                // in the metadata payload, which fills config_loaded before the version / fallback chain below
                PublishedConfig published_config = read_published(model, config_loaded, config_substitutions);
                if (en_3mf_file_type == En3mfType::From_Prusa || (load_model && !this_load_config)) {
                    // add filaments for the extruders the objects use
                    std::set<int> extruder_ids;
                    for (ModelObject *o : model.objects) {
                        if (o->config.option("extruder")) extruder_ids.insert(o->config.extruder());
                        for (auto *v : o->volumes) {
                            if (v->config.option("extruder")) extruder_ids.insert(v->config.extruder());
                            for (int e : v->get_extruders()) extruder_ids.insert(e);
                        }
                    }
                    int size = extruder_ids.empty() ? 0 : *extruder_ids.rbegin();
                    while (int(preset_bundle.filament_presets.size()) < std::min(size, int(MAXIMUM_EXTRUDER_NUMBER)))
                        preset_bundle.set_num_filaments(unsigned(preset_bundle.filament_presets.size() + 1));
                }
                Semver app_version = *Semver::parse(SoftFever_VERSION);
                const bool is_bambu_studio_project = en_3mf_file_type == En3mfType::From_BBS && file_version > Semver(2, 3, 2);
                auto info = [&](const std::string &text, const std::string &title) {
                    MessageDialog(nullptr, text, title, wxICON_INFORMATION | wxOK).ShowModal();
                };
                if (en_3mf_file_type == En3mfType::From_Prusa) {
                    this_load_config = false;
                    info(_u8L("The 3MF is not supported by OrcaSlicer, loading geometry data only."), _u8L("Load 3MF"));
                } else if (en_3mf_file_type == En3mfType::From_Orca) {
                    if (this_load_config && file_version > app_version) {
                        if (!config_substitutions.unrecogized_keys.empty()) {
                            std::string text = GUI::format(_u8L("The 3MF file version %s is newer than %s's version %s, found the following unrecognized keys:"),
                                                           file_version.to_string_sf(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string_sf());
                            info(text + "\n\n" + _u8L("You should update your software.\n"), _u8L("Newer 3MF version"));
                        } else if (file_version.min() != app_version.min())
                            info(GUI::format(_u8L("The 3MF file version %s is newer than %s's version %s, we suggest to upgrade your software."),
                                             file_version.to_string_sf(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string_sf()), _u8L("Newer 3MF version"));
                    } else if (this_load_config && config_loaded.empty()) {
                        this_load_config = false;
                        info(_u8L("The 3MF file was generated by an old OrcaSlicer version, loading geometry data only."), _u8L("Load 3MF"));
                    }
                } else if (en_3mf_file_type == En3mfType::From_BBS) {
                    if (this_load_config && config_loaded.empty()) {
                        this_load_config = false;
                        info(is_bambu_studio_project ? _u8L("The 3MF file was generated by BambuStudio, loading geometry data only.")
                                                     : _u8L("The 3MF file was generated by an older version, loading geometry data only."), _u8L("Load 3MF"));
                    }
                } else if (en_3mf_file_type == En3mfType::From_Other) {
                    if (this_load_config && config_loaded.empty()) this_load_config = false;
                } else if (this_load_config && config_loaded.empty()) {
                    this_load_config = false;
                    info(_u8L("The 3MF file was generated by an old OrcaSlicer version, loading geometry data only."), _u8L("Load 3MF"));
                }
                if (!this_load_config)   // keep only the extruder assignment of objects and volumes
                    for (ModelObject *mo : model.objects) {
                        auto keep = [](ModelConfigObject &c) {
                            bool has = c.has("extruder"); int e = has ? c.extruder() : -1;
                            c.reset(); if (has) c.set("extruder", e);
                        };
                        keep(mo->config);
                        for (ModelVolume *mv : mo->volumes) keep(mv->config);
                    }
                if (!plate_data.empty()) {
                    if (this_load_config) load_plates(plate_data);
                    release_PlateData_list(plate_data);
                }
                // project embedded presets: not for a published project (its keys go onto the receiver's own presets)
                if (!project_presets.empty() && this_load_config && !published_config.published)
                    preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                for (Preset *pr : project_presets) delete pr;
                if (this_load_config && !config_loaded.empty()) {
                    config.apply(static_cast<const ConfigBase &>(FullPrintConfig::defaults()));
                    config += std::move(config_loaded);
                    std::map<std::string, std::string> validity = config.validate();
                    if (!validity.empty()) {
                        std::string msg = _u8L("Invalid values found in the 3MF:") + "\n";
                        for (auto &[k, v] : validity) msg += "-" + k + ": " + v + "\n";
                        msg += "\n" + _u8L("Please correct them in the Param tabs");
                        Host::event("toast", {{"text", msg}});
                    }
                    p.model().plates_custom_gcodes = model.plates_custom_gcodes;
                    p.model().design_info = model.design_info;
                    p.model().model_info = model.model_info;
                }
                // the published metadata is consumed here and stripped, so a later save is a normal unpublished 3MF
                loaded_published = published_config.published;
                if (published_config.published && this_load_config) strip_published(p.model());
                if (this_load_config && !config.empty()) {
                    Preset::normalize(config);
                    if (!published_config.published && core.app_config->get("no_warn_when_modified_gcodes") != "true") {
                        std::set<std::string> modified_gcodes;
                        int validated = preset_bundle.validate_presets(filename, config, modified_gcodes);
                        if (validated == VALIDATE_PRESETS_MODIFIED_GCODES || validated == VALIDATE_PRESETS_PRINTER_NOT_FOUND ||
                            validated == VALIDATE_PRESETS_FILAMENTS_NOT_FOUND) {
                            std::string list = "\n";
                            for (auto &g : modified_gcodes) list += "-" + g + "\n";
                            std::string text = validated == VALIDATE_PRESETS_MODIFIED_GCODES
                                ? _u8L("The 3MF has the following modified G-code in filament or printer presets:") + list +
                                      _u8L("Please confirm that all modified G-code is safe to prevent any damage to the machine!")
                                : _u8L("The 3MF has the following customized filament or printer presets:") + list +
                                      _u8L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!");
                            MessageDialog dlg(nullptr, text, _u8L("Modified G-code"), wxICON_WARNING | wxOK);
                            dlg.show_dsa_button();
                            dlg.ShowModal();
                            if (dlg.get_checkbox_state()) core.app_config->set("no_warn_when_modified_gcodes", "true");
                        }
                    }
                    std::optional<ConfigOptionFloats> file_wipe_tower_x, file_wipe_tower_y;
                    if (auto *o = config.opt<ConfigOptionFloats>("wipe_tower_x")) file_wipe_tower_x = *o;
                    if (auto *o = config.opt<ConfigOptionFloats>("wipe_tower_y")) file_wipe_tower_y = *o;
                    preset_bundle.load_config_model(filename, std::move(config), file_version, &published_config);
                    // mixed-filament definitions relocated off a real slot: the model's references follow them
                    if (load_model && !published_config.mixed_slot_relocations.empty())
                        remap_model_filament_slots(model, published_config.mixed_slot_relocations);
                    // published settings that could not be applied, slot materials that were replaced
                    if (!published_config.skipped_keys.empty()) {
                        std::string message = _u8L("Some published settings could not be applied:");
                        for (const std::string &key : published_config.skipped_keys) message += "\n-" + key;
                        Host::event("toast", {{"text", message}});
                    }
                    if (!published_config.material_replacements.empty()) {
                        std::string message = _u8L("Some filament slots were changed:");
                        for (const std::string &replacement : published_config.material_replacements) message += "\n-" + replacement;
                        Host::event("toast", {{"text", message}});
                    }
                    // the Publish dialog starts from the file's selection
                    if (published_config.published) pending_published() = {true, published_config.published_keys, published_config.material_keys};
                    DynamicConfig &proj_cfg = preset_bundle.project_config;
                    if (file_wipe_tower_x) *proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x") = *file_wipe_tower_x;
                    if (file_wipe_tower_y) *proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y") = *file_wipe_tower_y;
                    is_project_file = true;
                    presets_changed();
                }
            } else {
                PlateDataPtrs plate_data;
                std::vector<Preset *> project_presets;
                bool is_xxx = false;
                Semver file_version;
                if (boost::iends_with(path.string(), ".stp") || boost::iends_with(path.string(), ".step")) {
                    double linear = string_to_double_decimal_point(core.app_config->get("linear_deflection"));
                    if (linear <= 0) linear = 0.003;
                    double angle = string_to_double_decimal_point(core.app_config->get("angle_deflection"));
                    if (angle <= 0) angle = 0.5;
                    bool split_compound = core.app_config->get_bool("is_split_compound");
                    model = Model::read_from_step(path.string(), strategy,
                        [&](int, int, int, bool &cancel) { cancel = false; },
                        [&](int is_utf8) {
                            if (!is_utf8 && !core.app_config->get_bool("step_not_utf8_no_warn")) {
                                MessageDialog dlg(nullptr, _u8L("Component name(s) inside step file not in UTF-8 format!") + "\n\n" +
                                                  _u8L("Because of unsupported text encoding, garbage characters may appear!"),
                                                  std::string(SLIC3R_APP_FULL_NAME " - ") + _u8L("Attention!"), wxOK | wxICON_INFORMATION);
                                dlg.show_dsa_button();
                                dlg.ShowModal();
                                if (dlg.get_checkbox_state()) core.app_config->set_bool("step_not_utf8_no_warn", true);
                            }
                        },
                        [&](Step &, double &linear_value, double &angle_value, bool &is_split) -> int {
                            if (core.app_config->get_bool("enable_step_mesh_setting")) {
                                json ans = Host::ask({{"kind", "step_mesh"}, {"linear", linear}, {"angle", angle}, {"split", split_compound}});
                                if (!ans.value("ok", false)) return -1;
                                linear_value = ans.value("linear", linear); angle_value = ans.value("angle", angle); is_split = ans.value("split", split_compound);
                                return 1;
                            }
                            linear_value = linear; angle_value = angle; is_split = split_compound;
                            return 1;
                        },
                        linear, angle, split_compound);
                } else
                    model = Model::read_from_file(path.string(), nullptr, nullptr, strategy, &plate_data, &project_presets, &is_xxx, &file_version);
                if (type_any_amf && is_xxx) imperial_units = true;
                const double preferred_orientation = core.full_config().opt_float("preferred_orientation");
                for (auto *obj : model.objects) {
                    if (obj->name.empty()) obj->name = fs::path(obj->input_file).filename().string();
                    obj->rotate(Geometry::deg2rad(preferred_orientation), Axis::Z);
                }
                if (!plate_data.empty()) { load_plates(plate_data); release_PlateData_list(plate_data); }
                if (!project_presets.empty()) {
                    preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                    for (Preset *pr : project_presets) delete pr;
                }
            }
        } catch (const ConfigurationError &e) {
            MessageDialog(nullptr, GUI::format(_u8L("Failed loading file \"%1%\". An invalid configuration was found."), filename) + "\n\n" + e.what(),
                          _u8L("Error"), wxICON_ERROR | wxOK).ShowModal();
            continue;
        } catch (const std::exception &e) {
            MessageDialog(nullptr, e.what(), _u8L("Error"), wxICON_ERROR | wxOK).ShowModal();
            continue;
        }

        if (load_model) {
            if (!is_project_file) {
                if (model.removed_objects_with_zero_volume() > 0)
                    MessageDialog(nullptr, _u8L("Objects with zero volume removed"), _u8L("The volume of the object is zero"), wxICON_INFORMATION | wxOK).ShowModal();
                if (imperial_units)
                    model.convert_from_imperial_units(false);
                else if (model.looks_like_saved_in_meters()) {
                    MessageDialog dlg(nullptr, GUI::format(_u8L("The object from file %s is too small, and may be in meters or inches.\nDo you want to scale to millimeters?"), filename),
                                      _u8L("Object too small"), wxICON_QUESTION | wxYES | wxNO);
                    if (dlg.ShowModal() == wxID_YES) model.convert_from_meters(true);
                } else if (model.looks_like_imperial_units()) {
                    MessageDialog dlg(nullptr, GUI::format(_u8L("The object from file %s is too small, and may be in meters or inches.\nDo you want to scale to millimeters?"), filename),
                                      _u8L("Object too small"), wxICON_QUESTION | wxYES | wxNO);
                    if (dlg.ShowModal() == wxID_YES) model.convert_from_imperial_units(true);
                }
            }
            if (!is_project_file && model.looks_like_multipart_object()) {
                MessageDialog dlg(nullptr, _u8L("This file contains several objects positioned at multiple heights.\nInstead of considering them as multiple objects, should \nthe file be loaded as a single object having multiple parts?"),
                                  _u8L("Multi-part object detected"), wxICON_WARNING | wxYES | wxNO);
                if (dlg.ShowModal() == wxID_YES) model.convert_multipart_object(filaments_cnt);
            }
        }
        for (ModelObject *mo : model.objects) {
            if (!type_3mf && !type_any_amf) mo->center_around_origin(false);
            if (!mo->instances.empty()) mo->ensure_on_bed(is_project_file);
        }
        if (one_by_one) {
            if (type_3mf && !is_project_file) {
                const Vec3d o = p.plate_origin(p.current_plate);
                model.center_instances_around_point(p.bed_box().center() + Vec2d(o.x(), o.y()));
            }
            // Texture-to-color (Filaments.cpp): a model with UVs and a texture or with vertex / face colours gets its
            // colours clustered and matched against the filaments before the objects are added
            if (!texture_import(model)) {
                json st = p.state();
                st["canceled"] = true;
                return st;
            }
            auto loaded = load_model_objects(model.objects, is_project_file);
            obj_idxs.insert(obj_idxs.end(), loaded.begin(), loaded.end());
            if (is_project_file && loaded_published) {
                // Plater::load_project: a published 3MF is a new project (Save asks where), kept in the recent list
                p.project_name = _u8L("Untitled");
                p.project_path.clear();
                opened_project = true;
                if (!restore) add_to_recent_projects(path.string());
            } else if (is_project_file) {
                p.project_name = fs::path(filename).stem().string();
                p.project_path = path.string();
                opened_project = true;
                if (!restore) add_to_recent_projects(path.string());
            }
        } else
            for (const ModelObject *mo : model.objects) new_model->add_object(*mo);
    }
    if (new_model && new_model->objects.size() > 1) {
        bool auto_drop = true;
        if (ask_multi) {
            json ans = Host::ask({{"kind", "message"},
                                  {"message", _u8L("Load these files as a single object with multiple parts?\n")},
                                  {"caption", _u8L("An object with multiple parts was detected")},
                                  {"icon", "question"},
                                  {"buttons", json::array({{{"id", wxID_YES}, {"label", _u8L("Yes")}}, {{"id", wxID_NO}, {"label", _u8L("No")}}})},
                                  {"checkbox", _u8L("Auto-Drop")}, {"checkbox_value", true}});
            auto_drop = ans.value("checkbox", true);
            if (ans.value("id", wxID_NO) == wxID_YES || !auto_drop) new_model->convert_multipart_object(filaments_cnt);
        }
        auto loaded = load_model_objects(new_model->objects, false, auto_drop);
        obj_idxs.insert(obj_idxs.end(), loaded.begin(), loaded.end());
    } else if (new_model && new_model->objects.size() == 1) {
        auto loaded = load_model_objects(new_model->objects);
        obj_idxs.insert(obj_idxs.end(), loaded.begin(), loaded.end());
    }
    progress("", 100);
    if (p.project_name.empty() && !input_files.empty())
        p.project_name = fs::path(a.contains("names") && !a["names"].empty() ? a["names"][0].get<std::string>() : input_files[0]).stem().string();
    p.selection.clear();
    for (size_t idx : obj_idxs) p.selection.insert({int(idx), -1, -1});
    p.changed();
    if (restore) {   // the restored project keeps its file and stays unsaved
        const std::string origin = a["restore_origin"];
        p.project_path = origin;
        p.project_name = origin.empty() ? std::string() : fs::path(origin).stem().string();
        p.dirty = true;
        Host::event("project", p.state());
        return p.state();
    }
    // an opened project starts unmodified (Plater::load_project: the dirty state is reset after loading)
    if (opened_project && input_files.size() == 1) {
        p.dirty = false;
        Host::event("project", p.state());
    }
    return p.state();
}

// -------------------------------------------------------------------------------------------------------------------

static std::set<SelItem> parse_items(const json &a)
{
    std::set<SelItem> out;
    for (auto &it : a.value("items", json::array())) out.insert({it.value("obj", -1), it.value("inst", -1), it.value("vol", -1)});
    return out;
}

static std::vector<int> selected_objects()
{
    std::set<int> s;
    for (auto &i : Plater::get().selection) s.insert(i.obj);
    return {s.begin(), s.end()};
}

static json select(const json &a)
{
    Plater &p = Plater::get();
    auto items = parse_items(a);
    if (a.value("add", false)) {
        for (auto &i : items) { if (p.selection.count(i)) p.selection.erase(i); else p.selection.insert(i); }
    } else
        p.selection = items;
    ++p.revision;
    json st = p.state();
    Host::event("project", st);
    return st;
}

// Selection::add_all on the current plate; all_plates (Ctrl+Shift+A): every object of the project
static json select_all(const json &a)
{
    Plater &p = Plater::get();
    p.selection.clear();
    if (a.value("all_plates", false))
        for (int o = 0; o < int(p.model().objects.size()); ++o) p.selection.insert({o, -1, -1});
    else
        for (auto &[o, i] : p.plates[p.current_plate].instances) p.selection.insert({o, -1, -1});
    ++p.revision;
    return p.state();
}

// Plater::delete_object_from_model / remove_selected
static json delete_selection(const json &)
{
    Plater &p = Plater::get();
    if (p.selection.empty()) return p.state();
    p.take_snapshot("Delete Selected");
    Model &m = p.model();
    // volumes first (keep at least one model part), then instances, then objects; highest index first
    std::map<int, std::set<int, std::greater<int>>> vols, insts;
    std::set<int, std::greater<int>> objs;
    for (auto &s : p.selection) {
        if (s.vol >= 0) vols[s.obj].insert(s.vol);
        else if (s.inst >= 0) insts[s.obj].insert(s.inst);
        else objs.insert(s.obj);
    }
    for (auto &[o, vs] : vols) {
        ModelObject *mo = m.objects[o];
        for (int v : vs) {
            int parts = 0;
            for (auto *mv : mo->volumes) parts += mv->is_model_part();
            if (mo->volumes[v]->is_model_part() && parts <= 1) {
                MessageDialog(nullptr, _u8L("Deleting the last solid part is not allowed."), _u8L("Delete"), wxICON_WARNING | wxOK).ShowModal();
                continue;
            }
            mo->delete_volume(v);
        }
        mo->invalidate_bounding_box();
        mo->ensure_on_bed();
    }
    for (auto &[o, is] : insts) {
        ModelObject *mo = m.objects[o];
        if (is.size() >= mo->instances.size()) { objs.insert(o); continue; }
        for (int i : is) mo->delete_instance(i);
    }
    for (int o : objs) m.delete_object(size_t(o));
    p.selection.clear();
    p.changed();
    return p.state();
}

static json delete_all(const json &)
{
    Plater &p = Plater::get();
    p.take_snapshot("Delete All");
    p.model().clear_objects();
    p.selection.clear();
    p.changed();
    return p.state();
}

// Transformations from the gizmos / the object manipulation panel: per instance (or volume) world values
static json set_transform(const json &a)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    int o = a.at("obj"), i = a.value("inst", 0), v = a.value("vol", -1);
    if (o < 0 || o >= int(m.objects.size())) throw CommandError("bad object");
    if (a.value("snapshot", true)) p.take_snapshot(a.value("name", std::string("Transform")));
    ModelObject *mo = m.objects[o];
    auto apply = [&](Geometry::Transformation t) {
        if (a.contains("offset")) t.set_offset(Vec3d(a["offset"][0], a["offset"][1], a["offset"][2]));
        if (a.contains("rotation")) t.set_rotation(Vec3d(a["rotation"][0], a["rotation"][1], a["rotation"][2]) * (M_PI / 180.));
        if (a.contains("scale")) t.set_scaling_factor(Vec3d(a["scale"][0], a["scale"][1], a["scale"][2]));
        if (a.contains("mirror")) t.set_mirror(Vec3d(a["mirror"][0], a["mirror"][1], a["mirror"][2]));
        return t;
    };
    if (v >= 0) {
        ModelVolume *mv = mo->volumes.at(v);
        mv->set_transformation(apply(mv->get_transformation()));
    } else {
        for (int k = (a.value("all_instances", false) ? 0 : i); k < (a.value("all_instances", false) ? int(mo->instances.size()) : i + 1); ++k)
            mo->instances.at(k)->set_transformation(apply(mo->instances[k]->get_transformation()));
    }
    mo->invalidate_bounding_box();
    if (a.value("drop", true)) mo->ensure_on_bed();
    p.changed();
    return p.state();
}

static json transform_get(const json &a)
{
    Model &m = Plater::get().model();
    int o = a.at("obj"), i = a.value("inst", 0), v = a.value("vol", -1);
    const ModelObject *mo = m.objects.at(o);
    Geometry::Transformation t = v >= 0 ? mo->volumes.at(v)->get_transformation() : mo->instances.at(i)->get_transformation();
    Vec3d off = t.get_offset(), rot = t.get_rotation() * (180. / M_PI), sc = t.get_scaling_factor(), mir = t.get_mirror();
    BoundingBoxf3 bb = v >= 0 ? mo->volumes[v]->mesh().transformed_bounding_box(mo->instances[i]->get_matrix() * mo->volumes[v]->get_matrix())
                              : mo->instance_bounding_box(i);
    return {{"offset", {off.x(), off.y(), off.z()}}, {"rotation", {rot.x(), rot.y(), rot.z()}}, {"scale", {sc.x(), sc.y(), sc.z()}},
            {"mirror", {mir.x(), mir.y(), mir.z()}}, {"size", {bb.size().x(), bb.size().y(), bb.size().z()}},
            {"bbox_min", {bb.min.x(), bb.min.y(), bb.min.z()}}};
}

// move selected instances by a world delta (drag on the plate)
static json translate_selection(const json &a)
{
    Plater &p = Plater::get();
    Vec3d d(a.at("delta")[0], a.at("delta")[1], a.at("delta")[2]);
    if (a.value("snapshot", true)) p.take_snapshot("Move Object");
    Model &m = p.model();
    for (auto &s : p.selection) {
        ModelObject *mo = m.objects[s.obj];
        if (s.vol >= 0) { mo->volumes[s.vol]->translate(mo->instances[std::max(0, s.inst)]->get_matrix_no_offset().inverse() * d); mo->invalidate_bounding_box(); continue; }
        for (int k = 0; k < int(mo->instances.size()); ++k)
            if (s.inst < 0 || s.inst == k) mo->instances[k]->set_offset(mo->instances[k]->get_offset() + d);
        mo->invalidate_bounding_box();
    }
    p.changed();
    return p.state();
}

static json clone(const json &a)
{
    Plater &p = Plater::get();
    int copies = a.value("copies", 1);
    p.take_snapshot("Clone");
    Model &m = p.model();
    std::vector<int> objs = selected_objects();
    std::set<SelItem> new_sel;
    for (int o : objs) {
        ModelObject *src = m.objects[o];
        for (int c = 0; c < copies; ++c) {
            ModelObject *mo = m.add_object(*src);
            mo->name = src->name;
            ModelInstance *mi = mo->instances[0];
            BoundingBoxf3 bb = src->instance_bounding_box(0);
            Vec2f cell = nearest_empty_cell(p, Vec2f(mi->get_offset().x(), mi->get_offset().y()), Vec2f(float(bb.size().x() + 1), float(bb.size().y() + 1)));
            for (ModelInstance *inst : mo->instances) inst->set_offset(Vec3d(cell.x(), cell.y(), inst->get_offset().z()));
            new_sel.insert({int(m.objects.size()) - 1, -1, -1});
            p.update_plates();
        }
    }
    p.selection = new_sel;
    p.changed();
    return p.state();
}

// Plater::increase_instances / decrease_instances
static json instances(const json &a)
{
    Plater &p = Plater::get();
    int delta = a.value("delta", 1);
    std::vector<int> objs = selected_objects();
    if (objs.empty()) return p.state();
    p.take_snapshot(delta > 0 ? "Add Instances" : "Delete Instances");
    Model &m = p.model();
    for (int o : objs) {
        ModelObject *mo = m.objects[o];
        if (delta > 0) {
            ModelInstance *last = mo->instances.back();
            BoundingBoxf3 bb = mo->instance_bounding_box(mo->instances.size() - 1);
            for (int c = 0; c < delta; ++c) {
                Vec2f cell = nearest_empty_cell(p, Vec2f(last->get_offset().x(), last->get_offset().y()), Vec2f(float(bb.size().x() + 1), float(bb.size().y() + 1)));
                ModelInstance *ni = mo->add_instance(*last);
                ni->set_offset(Vec3d(cell.x(), cell.y(), last->get_offset().z()));
                p.update_plates();
            }
        } else
            for (int c = 0; c < -delta && mo->instances.size() > 1; ++c) mo->delete_instance(mo->instances.size() - 1);
        mo->invalidate_bounding_box();
    }
    p.changed();
    return p.state();
}

static json rename(const json &a)
{
    Plater &p = Plater::get();
    p.take_snapshot("Rename");
    ModelObject *mo = p.model().objects.at(a.at("obj").get<int>());
    if (a.value("vol", -1) >= 0) mo->volumes.at(a["vol"].get<int>())->name = str_arg(a, "name");
    else mo->name = str_arg(a, "name");
    p.changed();
    return p.state();
}

// object list: the extruder (filament) of an object / volume
static json set_extruder(const json &a)
{
    Plater &p = Plater::get();
    p.take_snapshot("Change Filament");
    ModelObject *mo = p.model().objects.at(a.at("obj").get<int>());
    int e = a.at("extruder");
    if (a.value("vol", -1) >= 0) mo->volumes.at(a["vol"].get<int>())->config.set("extruder", e);
    else mo->config.set("extruder", e);
    p.changed();
    return p.state();
}

static json set_printable(const json &a)
{
    Plater &p = Plater::get();
    p.take_snapshot("Set Printable");
    ModelObject *mo = p.model().objects.at(a.at("obj").get<int>());
    bool on = a.at("printable");
    int i = a.value("inst", -1);
    for (int k = 0; k < int(mo->instances.size()); ++k)
        if (i < 0 || i == k) mo->instances[k]->printable = on;
    mo->printable = std::any_of(mo->instances.begin(), mo->instances.end(), [](auto *mi) { return mi->printable; });
    p.changed();
    return p.state();
}

// Plater::split_object / split_volume
static json split(const json &a)
{
    Plater &p = Plater::get();
    Model &m = p.model();
    int o = a.at("obj");
    ModelObject *mo = m.objects.at(o);
    p.take_snapshot(a.value("to_parts", false) ? "Split to Parts" : "Split to Objects");
    if (a.value("to_parts", false)) {
        // GUI_ObjectList::split: every model part of the object becomes its own parts
        int v = a.value("vol", 0);
        ModelVolume *vol = mo->volumes.at(v);
        size_t n = vol->split(unsigned(Core::get().preset_bundle->filament_presets.size()), true);
        if (n <= 1) MessageDialog(nullptr, _u8L("The selected object couldn't be split."), _u8L("Split"), wxICON_INFORMATION | wxOK).ShowModal();
        mo->input_file.clear();
    } else {
        ModelObjectPtrs new_objects;
        mo->split(&new_objects, true);
        if (new_objects.size() <= 1) {
            MessageDialog(nullptr, _u8L("The selected object couldn't be split because it contains only one solid part."), _u8L("Split"), wxICON_INFORMATION | wxOK).ShowModal();
            for (auto *no : new_objects) { if (no != mo) m.delete_object(no); }
        } else {
            m.delete_object(size_t(o));
            p.selection.clear();
            for (auto *no : new_objects) {
                no->center_around_origin();
                no->ensure_on_bed();
                p.selection.insert({int(std::find(m.objects.begin(), m.objects.end(), no) - m.objects.begin()), -1, -1});
            }
        }
    }
    p.changed();
    return p.state();
}

static json undo(const json &) { Plater::get().undo(); return Plater::get().state(); }
static json redo(const json &) { Plater::get().redo(); return Plater::get().state(); }

void reset_calib_params();
bool save_project_through_app();   // ProjectIO.cpp

// Plater::close_with_confirm: an unsaved project is saved first, when the user wants (or chose to be remembered)
int close_with_confirm()
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    if (!p.dirty) return wxID_NO;
    MessageDialog dlg(nullptr, _L("The current project has unsaved changes. Would you like to save before continuing\?"),
        wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE);
    dlg.show_dsa_button(_L("Remember my choice."));
    auto choise = core.app_config->get("save_project_choise");
    auto result = choise.empty() ? dlg.ShowModal() : choise == "yes" ? wxID_YES : wxID_NO;
    if (result == wxID_CANCEL)
        return result;
    if (dlg.get_checkbox_state())
        core.app_config->set("save_project_choise", result == wxID_YES ? "yes" : "no");
    if (result == wxID_YES && !save_project_through_app()) {
        if (choise.empty()) return wxID_CANCEL;
        result = wxID_NO;
    }
    p.dirty = false;
    return result;
}

// Plater::new_project: "canceled" when the user kept the unsaved project
static json new_project(const json &a)
{
    if (!a.value("skip_confirm", false) && close_with_confirm() == wxID_CANCEL) {
        json st = Plater::get().state();
        st["canceled"] = true;
        return st;
    }
    Plater::get().reset_project();
    reset_calib_params();
    return Plater::get().state();
}

// plates (PartPlateList)
static json plate_add(const json &)
{
    Plater &p = Plater::get();
    p.take_snapshot("Add Plate");
    p.resize_plates(p.plates.size() + 1);
    p.current_plate = int(p.plates.size()) - 1;
    p.changed();
    return p.state();
}

// Plater::duplicate_plate / PartPlateList::duplicate_plate: a new plate with copies of the objects on this one, at the
// same place relative to the plate (the copies keep all their instances, as upstream does)
static json plate_duplicate(const json &a)
{
    Plater &p = Plater::get();
    const int index = a.value("plate", p.current_plate);
    p.take_snapshot("Duplicate Plate");
    std::set<int> objs;
    for (auto &[o, inst] : p.plates[index].instances) objs.insert(o);
    p.resize_plates(p.plates.size() + 1);
    const int new_index = int(p.plates.size()) - 1;
    const Vec3d plate_to_plate_offset = p.plate_origin(new_index) - p.plate_origin(index);
    Model &m = p.model();
    for (int o : objs) {
        ModelObject *object_copy = m.add_object(*m.objects[o]);
        for (ModelInstance *inst : object_copy->instances) inst->set_offset(inst->get_offset() + plate_to_plate_offset);
        object_copy->invalidate_bounding_box();
        // Model::add_object hands the source's backup id to a copy made in the same model: both meshes are backed up
        // again, or a restore misses the source's mesh
        Slic3r::save_object_mesh(*m.objects[o]);
        Slic3r::save_object_mesh(*object_copy);
    }
    p.changed();
    return p.state();
}

static json plate_delete(const json &a)
{
    Plater &p = Plater::get();
    int i = a.value("plate", p.current_plate);
    if (p.plates.size() <= 1) return p.state();
    p.take_snapshot("Delete Plate");
    Model &m = p.model();
    // PartPlateList::delete_plate: its objects are deleted, later plates move one slot back
    std::set<int, std::greater<int>> objs;
    for (auto &[o, inst] : p.plates[i].instances) objs.insert(o);
    std::vector<Vec3d> old_origins;
    for (size_t k = 0; k < p.plates.size(); ++k) old_origins.push_back(p.plate_origin(int(k)));
    std::vector<std::set<std::pair<int, int>>> members;
    for (auto &pl : p.plates) members.push_back(pl.instances);
    p.plates.erase(p.plates.begin() + i);
    for (size_t k = i; k < p.plates.size(); ++k) {
        Vec3d d = p.plate_origin(int(k)) - old_origins[k + 1];
        for (auto &[o, inst] : members[k + 1]) m.objects[o]->instances[inst]->set_offset(m.objects[o]->instances[inst]->get_offset() + d);
    }
    for (size_t k = 0; k < (size_t) i; ++k) {
        Vec3d d = p.plate_origin(int(k)) - old_origins[k];
        if (d.norm() > EPSILON)
            for (auto &[o, inst] : members[k]) m.objects[o]->instances[inst]->set_offset(m.objects[o]->instances[inst]->get_offset() + d);
    }
    for (int o : objs) m.delete_object(size_t(o));
    p.selection.clear();
    p.current_plate = std::min(i, int(p.plates.size()) - 1);
    p.changed();
    return p.state();
}

static json plate_select(const json &a)
{
    Plater &p = Plater::get();
    p.current_plate = std::clamp(a.value("plate", 0), 0, int(p.plates.size()) - 1);
    ++p.revision;
    json st = p.state();
    Host::event("project", st);
    return st;
}

static json plate_set(const json &a)
{
    Plater &p = Plater::get();
    int i = a.value("plate", p.current_plate);
    Plate &pl = p.plates.at(i);
    p.take_snapshot("Plate Settings");
    if (a.contains("name")) pl.name = a["name"];
    if (a.contains("locked")) pl.locked = a["locked"];
    if (a.contains("config"))
        for (auto &[k, v] : a["config"].items()) {
            if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) pl.config.erase(k);
            else pl.config.set_deserialize_strict(k, v.get<std::string>());
        }
    p.changed();
    return p.state();
}

int show_spiral_mode_settings_dialog(bool is_object_config);   // Settings.cpp

// PlateSettingsDialog (Plater::priv::on_plate_settings): what the dialog shows for a plate — the bed types the printer
// model supports (Plater::get_curr_printer_model's not_support_bed_types), print sequence, spiral vase, the first layer
// and other layers filament sequences with the filament colours, the mixed-filament warning
static json plate_dialog(const json &a)
{
    Plater &p = Plater::get();
    const int i = a.value("plate", p.current_plate);
    const Plate &pl = p.plates.at(i);
    PresetBundle &bundle = *Core::get().preset_bundle;
    json bed_labels = json::array(), bed_values = json::array();
    bed_labels.push_back(_u8L("Same as Global Plate Type"));
    bed_values.push_back(int(btDefault));
    const ConfigOptionDef *bed_type_def = print_config_def.get("curr_bed_type");
    const Preset *curr = &bundle.printers.get_selected_preset();
    const VendorProfile::PrinterModel *pm = PresetUtils::system_printer_model(*curr);
    if (!pm)
        if (const Preset *parent = bundle.printers.get_selected_preset_parent()) pm = PresetUtils::system_printer_model(*parent);
    if (pm) {
        int index = 0;
        for (const std::string &item : bed_type_def->enum_labels) {
            ++index;
            if (std::find(pm->not_support_bed_types.begin(), pm->not_support_bed_types.end(), item) == pm->not_support_bed_types.end()) {
                bed_labels.push_back(_u8L(item.c_str()));
                bed_values.push_back(index);
            }
        }
    } else
        for (int b = int(btDefault) + 1; b < int(btCount); ++b) {
            bed_labels.push_back(_u8L(bed_type_def->enum_labels[size_t(b) - 1].c_str()));
            bed_values.push_back(b);
        }
    const auto &printer_config = bundle.printers.get_edited_preset().config;
    const bool bed_enabled = bundle.is_bbl_vendor() || printer_config.opt_bool("support_multi_bed_types");
    // sync_bed_type / sync_print_seq / sync_spiral_mode
    int bed = pl.config.has("curr_bed_type") ? int(pl.config.opt_enum<BedType>("curr_bed_type")) : int(btDefault);
    int bed_index = 0;
    for (size_t k = 1; k < bed_values.size(); ++k)
        if (bed_values[k].get<int>() == bed) bed_index = int(k);
    int print_seq = 0;
    if (pl.config.has("print_sequence")) {
        const PrintSequence ps = pl.config.opt_enum<PrintSequence>("print_sequence");
        if (ps != PrintSequence::ByDefault) print_seq = int(ps) + 1;
    }
    const int spiral = !pl.config.has("spiral_mode") ? 0 : pl.config.opt_bool("spiral_mode") ? 1 : 2;
    json first = json::array();
    if (const auto *op = pl.config.option<ConfigOptionInts>("first_layer_print_sequence"))
        for (int v : op->values) first.push_back(v);
    json other = json::array();
    if (pl.config.has("other_layers_print_sequence") && pl.config.has("other_layers_print_sequence_nums"))
        for (const LayerPrintSequence &seq : get_other_layers_print_sequence(pl.config.opt_int("other_layers_print_sequence_nums"),
                                                                             pl.config.option<ConfigOptionInts>("other_layers_print_sequence")->values))
            other.push_back({{"begin", seq.first.first}, {"end", seq.first.second >= INT_MAX - 1 ? -1 : seq.first.second}, {"order", seq.second}});
    json colors = json::array();
    if (auto *fc = Core::get().full_config().option<ConfigOptionStrings>("filament_colour")) colors = fc->values;
    const auto *is_mixed_opt = bundle.project_config.option<ConfigOptionBools>("filament_is_mixed");
    const bool mixed = is_mixed_opt && has_any_mixed_filament(is_mixed_opt->values);
    return {{"name", pl.name}, {"bed_labels", bed_labels}, {"bed_values", bed_values}, {"bed_index", bed_index}, {"bed_enabled", bed_enabled},
            {"print_seq_labels", {_u8L("Same as Global Print Sequence"), _u8L("By Layer"), _u8L("By Object")}}, {"print_seq", print_seq},
            {"spiral_labels", {_u8L("Same as Global"), _u8L("Enable"), _u8L("Disable")}}, {"spiral", spiral},
            {"first_custom", !first.empty()}, {"first", first}, {"other_custom", !other.empty()}, {"other", other}, {"colors", colors},
            {"mixed", mixed}, {"mixed_text", _u8L("The filament list contains mixed filaments. Custom filament sequence will not take effect.")},
            {"strings", {{"title", _u8L("Plate Settings")}, {"name", _u8L("Plate name")}, {"bed", _u8L("Bed type")}, {"seq", _u8L("Print sequence")},
                         {"spiral", _u8L("Spiral vase")}, {"first", _u8L("First layer filament sequence")},
                         {"other", _u8L("Other layer filament sequence")}, {"auto", _u8L("Auto")}, {"custom", _u8L("Customize")},
                         {"layer", _u8L("Layer")}, {"to", _u8L("to")}, {"end", _u8L_CONTEXT("End", "Layer range")}}}};
}

// the dialog's OK (Plater::priv::on_plate_settings after ShowModal): EVT_SET_BED_TYPE_CONFIRM's layer check, then the
// plate's bed type, layer sequences, print sequence, spiral mode (with the vase settings question) and name
static json plate_dialog_apply(const json &a)
{
    Plater &p = Plater::get();
    const int i = a.value("plate", p.current_plate);
    Plate &pl = p.plates.at(i);
    // the layer ranges: begin >= 2 (an End end is INT_MAX - 1), sorted as LayerSeqInfo::operator<
    std::vector<LayerPrintSequence> other;
    if (a.value("other_custom", false))
        for (const json &l : a.value("other", json::array())) {
            const int begin = l.value("begin", -1), end = l.value("end", -1);
            if (begin < 2 || (end != -1 && end < 2)) {
                MessageDialog(nullptr, _L("Please input layer value (>= 2)."), wxEmptyString, wxICON_WARNING | wxOK).ShowModal();
                return json{{"invalid", true}};
            }
            other.push_back({{begin, end == -1 ? INT_MAX - 1 : end}, l.value("order", std::vector<int>())});
        }
    std::stable_sort(other.begin(), other.end(), [](const LayerPrintSequence &x, const LayerPrintSequence &y) {
        return x.first.first != y.first.first ? x.first.first < y.first.first : x.first.second < y.first.second;
    });
    p.take_snapshot("Plate Settings");
    pl.name = a.value("name", pl.name);
    const int bed = a.value("bed", int(btDefault));
    if (bed == int(btDefault)) pl.config.erase("curr_bed_type");
    else pl.config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(BedType(bed)));
    // PartPlate::set_first_layer_print_sequence / set_other_layers_print_sequence
    const std::vector<int> first = a.value("first_custom", false) ? a.value("first", std::vector<int>()) : std::vector<int>();
    if (first.empty() || (first.size() == 1 && first[0] == 0)) pl.config.erase("first_layer_print_sequence");
    else pl.config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(first));
    if (other.empty()) {
        pl.config.erase("other_layers_print_sequence");
        pl.config.erase("other_layers_print_sequence_nums");
    } else {
        int nums;
        std::vector<int> flat;
        get_other_layers_print_sequence(other, nums, flat);
        pl.config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(flat));
        pl.config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(nums));
    }
    const int ps = a.value("print_seq", 0);
    if (ps > 0) pl.config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence(ps - 1)));
    else pl.config.erase("print_sequence");
    // PartPlate::set_spiral_vase_mode
    const int spiral = a.value("spiral", 0);
    if (spiral == 0) pl.config.erase("spiral_mode");
    else if (spiral == 2) pl.config.set_key_value("spiral_mode", new ConfigOptionBool(false));
    else if (!(pl.config.has("spiral_mode") && pl.config.opt_bool("spiral_mode"))) {
        if (show_spiral_mode_settings_dialog(false) == wxID_YES) {
            pl.config.set_key_value("spiral_mode", new ConfigOptionBool(true));
            // PartPlate::set_vase_mode_related_object_config: the plate's objects get the vase settings
            DynamicPrintConfig *global_config = &Core::get().preset_bundle->prints.get_edited_preset().config;
            DynamicPrintConfig new_conf;
            new_conf.set_key_value("wall_loops", new ConfigOptionInt(1));
            new_conf.set_key_value("top_shell_layers", new ConfigOptionInt(0));
            new_conf.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
            new_conf.set_key_value("enable_support", new ConfigOptionBool(false));
            new_conf.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
            new_conf.set_key_value("detect_thin_wall", new ConfigOptionBool(false));
            new_conf.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
            new_conf.set_key_value("overhang_reverse", new ConfigOptionBool(false));
            const auto applying_keys = global_config->diff(new_conf);
            std::set<int> objs;
            for (const auto &[o, inst] : pl.instances) objs.insert(o);
            for (int o : objs) {
                ModelConfigObject &config = p.model().objects[size_t(o)]->config;
                for (const auto &opt_key : applying_keys) config.set_key_value(opt_key, new_conf.option(opt_key)->clone());
                for (const auto &opt_key : config.get().diff(new_conf)) config.set_key_value(opt_key, new_conf.option(opt_key)->clone());
            }
        }
    }
    p.changed();
    return p.state();
}

static json plate_get(const json &a)
{
    Plater &p = Plater::get();
    const Plate &pl = p.plates.at(a.value("plate", p.current_plate));
    json cfg = json::object();
    for (auto &k : pl.config.keys()) cfg[k] = pl.config.opt_serialize(k);
    return {{"name", pl.name}, {"locked", pl.locked}, {"config", cfg}};
}

// PartPlateList "move plate to front": the plate becomes plate 1, its objects move with it
static json plate_move_front(const json &a)
{
    Plater &p = Plater::get();
    int i = a.value("plate", p.current_plate);
    if (i <= 0 || i >= int(p.plates.size())) return p.state();
    p.take_snapshot("Move plate to front");
    p.update_plates();
    Model &m = p.model();
    std::vector<Vec3d> origins;
    for (size_t k = 0; k < p.plates.size(); ++k) origins.push_back(p.plate_origin(int(k)));
    std::vector<Plate> order;
    order.push_back(p.plates[i]);
    for (size_t k = 0; k < p.plates.size(); ++k) if (int(k) != i) order.push_back(p.plates[k]);
    std::vector<int> old_index;
    old_index.push_back(i);
    for (size_t k = 0; k < p.plates.size(); ++k) if (int(k) != i) old_index.push_back(int(k));
    for (size_t k = 0; k < order.size(); ++k) {
        Vec3d d = origins[k] - origins[old_index[k]];
        for (auto &[o, inst] : order[k].instances) m.objects[o]->instances[inst]->set_offset(m.objects[o]->instances[inst]->get_offset() + d);
    }
    for (ModelObject *mo : m.objects) mo->invalidate_bounding_box();
    p.plates = order;
    p.current_plate = 0;
    p.changed();
    return p.state();
}

static Commands reg({
    {"plate.move_front", plate_move_front},
    {"project.state", [](const json &) { return Plater::get().state(); }},
    {"project.new", new_project},
    {"project.load", load_files},
    {"project.undo", undo},
    {"project.redo", redo},
    {"selection.set", select},
    {"selection.all", select_all},
    {"selection.delete", delete_selection},
    {"selection.translate", translate_selection},
    {"objects.delete_all", delete_all},
    {"objects.clone", clone},
    {"objects.instances", instances},
    {"object.transform", set_transform},
    {"project.nearest_empty_cell", [](const json &) {
        Plater &p = Plater::get();
        Vec2d c = p.bed_box().center() + Vec2d(p.plate_origin(p.current_plate).x(), p.plate_origin(p.current_plate).y());
        Vec2f cell = nearest_empty_cell(p, c.cast<float>());
        return json::array({cell.x(), cell.y()});
    }},
    {"object.transform_get", transform_get},
    {"object.rename", rename},
    {"object.extruder", set_extruder},
    {"object.printable", set_printable},
    {"object.split", split},
    {"plate.add", plate_add},
    {"plate.duplicate", plate_duplicate},
    {"plate.delete", plate_delete},
    {"plate.select", plate_select},
    {"plate.set", plate_set},
    {"plate.dialog", plate_dialog},
    {"plate.dialog_apply", plate_dialog_apply},
    {"plate.get", plate_get},
});

} // namespace orca
