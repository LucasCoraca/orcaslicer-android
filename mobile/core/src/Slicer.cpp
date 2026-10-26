#include "Slicer.hpp"
#include "ImexPlate.hpp"
#include "Thumbnails.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Host.hpp"
#include "I18n.hpp"
#include "Plater.hpp"
#include "Preview.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace orca {

// auto re-slice state (see schedule_auto_reslice_if_needed)
const Calib_Params &calib_params();
void calib_before_slice();
static std::atomic<bool> g_preview_shown{false};
static std::atomic<bool> g_reslice_after_cancel{false};
static std::atomic<unsigned> g_reslice_generation{0};


Slicer &Slicer::get()
{
    static Slicer s;
    return s;
}

PlateSlice *Slicer::slice(int plate)
{
    if (plate < 0) return nullptr;
    if (size_t(plate) >= m_slices.size()) m_slices.resize(plate + 1);
    if (!m_slices[plate]) m_slices[plate] = std::make_unique<PlateSlice>();
    return m_slices[plate].get();
}

void Slicer::invalidate_all()
{
    for (auto &s : m_slices)
        if (s) s->valid = false;
}

bool Slicer::start(int plate)
{
    if (m_running) return false;
    if (m_thread.joinable()) m_thread.join();
    std::vector<int> plates;
    Plater &p = Plater::get();
    if (plate < 0) for (int i = 0; i < int(p.plates.size()); ++i) plates.push_back(i);
    else plates.push_back(plate);
    m_cancel = false;
    m_running = true;
    m_thread = std::thread([this, plates] { run(plates); });
    return true;
}

void Slicer::cancel()
{
    m_cancel = true;
    std::lock_guard<std::mutex> l(result_mutex);
    if (m_current) m_current->cancel();
}

static std::string exception_text(const std::exception &ex) { return ex.what(); }

// per plate: apply the model and the full config plus the plate's settings, validate, process, export the G-code
// (Plater::priv::update_background_process + BackgroundSlicingProcess::process_fff, as the CLI does it per plate)
void Slicer::run(std::vector<int> plates)
{
    Core &core = Core::get();
    Plater &p = Plater::get();
    int n = 0;
    for (int plate : plates) {
        if (m_cancel) break;
        PlateSlice *ps = slice(plate);
        json ev = {{"plate", plate}, {"index", n++}, {"count", plates.size()}};
        try {
            {
                std::lock_guard<std::recursive_mutex> lock(core.mutex);
                if (!ps->print) ps->print = std::make_unique<Print>();
                Print &print = *ps->print;
                Model &model = p.model();
                // Plater::priv::update_print_volume_state with the plate's shape
                Vec3d origin = p.plate_origin(plate);
                Pointfs shape = p.bed_shape();
                for (Vec2d &v : shape) v += Vec2d(origin.x(), origin.y());
                BuildVolume build_volume(shape, p.printable_height(), {}, {});
                model.update_print_volume_state(build_volume);
                // instances of the other plates are not printed on this one
                for (int oi = 0; oi < int(model.objects.size()); ++oi)
                    for (int ii = 0; ii < int(model.objects[oi]->instances.size()); ++ii)
                        if (!p.plates[plate].instances.count({oi, ii})) model.objects[oi]->instances[ii]->print_volume_state = ModelInstancePVS_Fully_Outside;
                model.curr_plate_index = plate;
                print.set_plate_origin(origin);
                print.set_check_multi_filaments_compatibility(core.app_config->get("enable_high_low_temp_mixed_printing") == "false");
                DynamicPrintConfig config = core.preset_bundle->full_config(false);
                config.apply(p.plates[plate].config);
                {   // Plater::reslice: the PA pattern's G-code for this plate
                    const int keep = p.current_plate;
                    p.current_plate = plate;
                    calib_before_slice();
                    p.current_plate = keep;
                }
                print.set_calib_params(calib_params());   // the calibration test of the project (Plater::calib_*)
                print.apply(model, config);
                print.is_BBL_printer() = core.preset_bundle->is_bbl_vendor();
                if (print.empty()) {
                    ev["state"] = "empty";
                    ev["text"] = _u8L("Nothing to be sliced, either the print is empty or no object is fully inside the print volume.");
                    Host::event("slice", ev);
                    continue;
                }
                std::vector<StringObjectException> warnings;
                StringObjectException err = print.validate(&warnings);
                if (!err.string.empty()) {
                    ev["state"] = "error";
                    ev["text"] = err.string;
                    Host::event("slice", ev);
                    continue;
                }
                json w = json::array();
                for (auto &x : warnings) w.push_back(x.string);
                ev["warnings"] = w;
                print.set_status_callback([ev](const PrintBase::SlicingStatus &s) mutable {
                    json e = ev;
                    e["state"] = "progress";
                    e["percent"] = s.percent;
                    e["text"] = s.text;
                    if (s.flags & (PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS | PrintBase::SlicingStatus::UPDATE_PRINT_OBJECT_STEP_WARNINGS)) {
                        e["state"] = "warning";
                        e["level"] = int(s.warning_level);
                    }
                    Host::event("slice", e);
                });
                Model::setExtruderParams(config, int(core.preset_bundle->filament_presets.size()));
                Model::setPrintSpeedTable(config, print.config());
                std::lock_guard<std::mutex> l(result_mutex);
                m_current = &print;
            }
            ev["state"] = "progress";
            ev["percent"] = 0;
            ev["text"] = _u8L("Slicing");
            Host::event("slice", ev);
            ps->print->process();
            if (m_cancel) throw CanceledException();
            fs::path out = fs::path(temporary_dir()) / ("plate_" + std::to_string(plate + 1) + ".gcode");
            // the thumbnails the printer profile asks for (Plater::priv::generate_thumbnails via the thumbnail callback)
            ThumbScene scene;
            {
                std::lock_guard<std::recursive_mutex> lock(core.mutex);
                scene = thumb_scene(plate);
            }
            ThumbnailsGeneratorCallback thumbnail_cb = [scene](const ThumbnailsParams &params) { return render_thumbnails(scene, params); };
            {
                std::lock_guard<std::mutex> l(result_mutex);
                // BackgroundSlicingProcess::process_fff: a finished print is not exported again (GCode::do_export
                // returns early on a done step), its previous G-code file is processed instead
                if (ps->print->finished() && ps->valid && fs::exists(out)) {
                } else if (ps->print->finished() && fs::exists(out)) {
                    ps->result.reset();
                    ps->print->export_gcode_from_previous_file(out.string(), &ps->result, thumbnail_cb);
                    ps->gcode_path = out.string();
                } else {
                    ps->result.reset();
                    ps->gcode_path = ps->print->export_gcode(out.string(), &ps->result, thumbnail_cb);
                }
                ps->valid = true;
                ps->result_id++;
            }
            const PrintStatistics &st = ps->print->print_statistics();
            ev["state"] = "done";
            ev["percent"] = 100;
            ev["conflict"] = ps->print->get_conflict_string();
            ev["time"] = ps->result.print_statistics.modes[0].time;
            ev["weight"] = st.total_weight;
            ev["filament_m"] = st.total_used_filament / 1000.;
            ev["cost"] = st.total_cost;
            {
                std::lock_guard<std::recursive_mutex> lock(core.mutex);
                if (plate < int(p.plates.size())) p.plates[plate].slice_valid = true;
                ++p.revision;   // the 3D view shows the real prime tower (load_real_wipe_tower_preview)
            }
            Preview::get().result_changed(plate);
            Host::event("slice", ev);
        } catch (const CanceledException &) {
            ev["state"] = "canceled";
            Host::event("slice", ev);
            break;
        } catch (const std::exception &ex) {
            ev["state"] = "error";
            ev["text"] = exception_text(ex);
            Host::event("slice", ev);
        }
        std::lock_guard<std::mutex> l(result_mutex);
        m_current = nullptr;
    }
    {
        std::lock_guard<std::mutex> l(result_mutex);
        m_current = nullptr;
    }
    m_running = false;
    Host::event("slice_finished", {{"canceled", bool(m_cancel)}});
    if (g_reslice_after_cancel.exchange(false)) {
        std::lock_guard<std::recursive_mutex> l(Core::get().mutex);
        schedule_auto_reslice_if_needed();
    }
}

// --- Plater::priv::schedule_auto_reslice_if_needed / trigger_auto_reslice_now (Preferences: Auto slice after changes)

namespace {

bool can_auto_reslice()
{
    AppConfig *cfg = Core::get().app_config.get();
    if (cfg == nullptr || !cfg->get_bool("auto_slice_after_change") || !g_preview_shown) return false;
    Plater &p = Plater::get();
    if (p.model().objects.empty() || p.current_plate >= int(p.plates.size()) || p.plates[p.current_plate].instances.empty()) return false;
    return true;
}

void trigger_auto_reslice_now()
{
    std::lock_guard<std::recursive_mutex> l(Core::get().mutex);
    if (!can_auto_reslice() || Slicer::get().running()) return;
    if (Slicer::get().start(Plater::get().current_plate)) Host::event("slice_started", {{"plate", Plater::get().current_plate}});
}

int auto_slice_delay_seconds()
{
    long delay_seconds = 0;
    try { delay_seconds = std::stol(Core::get().app_config->get("auto_slice_change_delay_seconds")); } catch (...) {}
    return int(std::clamp<long>(delay_seconds, 0, std::numeric_limits<int>::max() / 1000));
}
} // namespace

void schedule_auto_reslice_if_needed()
{
    if (!can_auto_reslice()) return;
    if (Slicer::get().running()) {   // restart once the current slice stops, and cancel it now
        g_reslice_after_cancel = true;
        Slicer::get().cancel();
        return;
    }
    const unsigned gen = ++g_reslice_generation;
    const int delay = auto_slice_delay_seconds();
    std::thread([gen, delay] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay > 0 ? delay * 1000 : 100));
        if (gen == g_reslice_generation) trigger_auto_reslice_now();
    }).detach();
}

static json slice(const json &a)
{
    int plate = a.value("plate", Plater::get().current_plate);
    std::vector<int> plates;
    if (plate >= 0) plates.push_back(plate);
    else for (int i = 0; i < int(Plater::get().plates.size()); ++i) plates.push_back(i);
    if (!imex_pre_slice_check(plates)) return json{{"canceled", true}};   // back to the 3D view (select_view_3D("3D"))
    return Slicer::get().start(plate);
}

static json slice_cancel(const json &)
{
    Slicer::get().cancel();
    return true;
}

// export the last G-code of a plate to a path (the app copies it to the user's chosen document)
static json export_gcode(const json &a)
{
    int plate = a.value("plate", Plater::get().current_plate);
    PlateSlice *ps = Slicer::get().slice(plate);
    if (!ps || !ps->valid) throw CommandError(_u8L("The plate is not sliced"));
    std::string dest = str_arg(a, "path");
    std::string err;
    if (copy_file(ps->gcode_path, dest, err, false) != SUCCESS) throw CommandError(err);
    return dest;
}

// the file name the desktop proposes (Print::output_filename, from filename_format)
static json gcode_name(const json &a)
{
    int plate = a.value("plate", Plater::get().current_plate);
    PlateSlice *ps = Slicer::get().slice(plate);
    if (!ps || !ps->print) return "plate.gcode";
    try {
        return fs::path(ps->print->output_filename()).filename().string();
    } catch (...) {
        return "plate_" + std::to_string(plate + 1) + ".gcode";
    }
}

static Commands reg({
    {"slice.start", slice},
    {"slice.preview_shown", [](const json &a) -> json { g_preview_shown = a.value("shown", false); return nullptr; }},
    {"slice.cancel", slice_cancel},
    {"slice.export_gcode", export_gcode},
    {"slice.gcode_name", gcode_name},
});

} // namespace orca
