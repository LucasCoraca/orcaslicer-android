// The host-specific parts of the send dialog (PrintHostDialogs.cpp): ElegooPrintHostSendDialog (upload-and-print
// options, bed side), CrealityPrintHostSendDialog (self test and the CFS filament mapping) and
// FlashforgePrintHostSendDialog (leveling, time-lapse, IFS and its slot mapping). The dialog is the app's; the data it
// shows, the defaults (auto-assignment, matching) and the extended info of the upload are built here like upstream.
#include "SendDialogs.hpp"

#include <algorithm>
#include <limits>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include "Core.hpp"
#include "Slicer.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/CrealityPrint.hpp"
#include "slic3r/Utils/Flashforge.hpp"
#include "slic3r/Utils/PrintHost.hpp"

namespace orca {

using namespace Slic3r;

namespace {

// wxColour for the matching code: #RGB / #RRGGBB (and the rgba constructor of to_wx_colour)
struct wxColour
{
    int  r{0}, g{0}, b{0}, a{255};
    bool ok{false};
    wxColour() = default;
    wxColour(int r, int g, int b, int a = 255) : r(r), g(g), b(b), a(a), ok(true) {}
    explicit wxColour(const wxString &s)
    {
        std::string v = boost::trim_copy(s.ToStdString());
        if (v.empty() || v[0] != '#') return;
        v.erase(0, 1);
        auto hex = [](const std::string &h, int &out) {
            try { size_t n = 0; out = std::stoi(h, &n, 16); return n == h.size(); } catch (...) { return false; }
        };
        if (v.size() == 6) ok = hex(v.substr(0, 2), r) && hex(v.substr(2, 2), g) && hex(v.substr(4, 2), b);
        else if (v.size() == 3) {
            ok = hex(v.substr(0, 1), r) && hex(v.substr(1, 1), g) && hex(v.substr(2, 1), b);
            r *= 17; g *= 17; b *= 17;
        }
    }
    bool IsOk() const { return ok; }
    int  Red() const { return r; }
    int  Green() const { return g; }
    int  Blue() const { return b; }
    bool operator==(const wxColour &o) const { return ok == o.ok && r == o.r && g == o.g && b == o.b; }
};
using Slic3r::GUI::from_u8;

#include "slic3r/GUI/FfSend.inc.fn"

struct FfSendLogic
{
    std::string normalize_material(const std::string &material) const;
    bool        slot_matches_filament(const Slic3r::FlashforgeMaterialSlot &slot, const FilamentInfo &filament) const;
    wxColour    to_wx_colour(const std::string &color) const;
};
#include "slic3r/GUI/FfSend.inc"

std::string hex(const wxColour &c)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

// what the open dialog showed, for its answer
json g_last;

} // namespace

SendExtrasInput send_extras_input(DynamicPrintConfig *physical_printer_config, int plate)
{
    Core &core = Core::get();
    SendExtrasInput in;
    const auto *host_type_opt = physical_printer_config->option<ConfigOptionEnum<PrintHostType>>("host_type");
    in.host_type = host_type_opt != nullptr ? int(host_type_opt->value) : int(htElegooLink);
    in.model_id  = core.preset_bundle->printers.get_edited_preset().get_printer_type(core.preset_bundle.get());
    const auto *ff_serial_opt = physical_printer_config->option<ConfigOptionString>("flashforge_serial_number");
    const auto *ff_code_opt   = physical_printer_config->option<ConfigOptionString>("printhost_apikey");
    in.flashforge_local = in.host_type == int(htFlashforge) && ff_serial_opt != nullptr && !ff_serial_opt->value.empty() &&
                          ff_code_opt != nullptr && !ff_code_opt->value.empty();
    DynamicPrintConfig cfg = core.preset_bundle->full_config();
    if (auto *c = cfg.option<ConfigOptionStrings>("filament_colour")) in.colors = c->values;
    if (auto *t = cfg.option<ConfigOptionStrings>("filament_type")) in.types = t->values;
    // Plater::send_gcode_legacy: the plate's sliced filaments with their type and colour (Flashforge)
    if (in.flashforge_local) {
        if (PlateSlice *ps = Slicer::get().slice(plate); ps && ps->valid) {
            PlateData d;
            d.parse_filament_info(&ps->result);
            in.project_filaments = d.slice_filaments_info;
        }
        const auto *filament_id_opt = cfg.option<ConfigOptionStrings>("filament_ids");
        for (auto &filament : in.project_filaments) {
            if (filament.id < 0) continue;
            std::string display_filament_type;
            try { filament.type = cfg.get_filament_type(display_filament_type, filament.id); } catch (...) {}
            if (filament.type.empty()) filament.type = display_filament_type;
            if (filament.type.empty()) filament.type = "Unknown";
            filament.filament_id = filament_id_opt ? filament_id_opt->get_at(size_t(filament.id)) : "";
            filament.color = filament.id < int(in.colors.size()) ? in.colors[filament.id] : "#FFFFFF";
            if (filament.color.empty()) filament.color = "#FFFFFF";
        }
    }
    AppConfig &ac = *core.app_config;
    for (const char *k : {"elegoolink_upload_and_print", "elegoolink_timelapse", "elegoolink_heated_bed_leveling", "elegoolink_bed_type",
                          "crealityprint_enable_self_test", "flashforge_leveling_before_print", "flashforge_timelapse_video"})
        in.recent[k] = ac.get("recent", k);
    in.curr_bed_type = ac.get("curr_bed_type");
    return in;
}

json send_extras(const SendExtrasInput &in, PrintHost *host)
{
    json e;
    if (in.host_type == int(htElegooLink) && (in.model_id == "Elegoo-CC" || in.model_id == "Elegoo-C")) {
        // ElegooPrintHostSendDialog::init: Upload and Print is a check box with its options; one Upload button
        auto recent_int = [&](const char *k, int def) {
            auto it = in.recent.find(k);
            try { return it == in.recent.end() || it->second.empty() ? def : std::stoi(it->second); } catch (...) { return def; }
        };
        const int bed = recent_int("elegoolink_bed_type", int(btPTE));
        int app_bed = 0;
        try { app_bed = std::stoi(in.curr_bed_type); } catch (...) {}
        e["elegoo"] = {{"upload_print", recent_int("elegoolink_upload_and_print", 0) == int(PrintHostPostUploadAction::StartPrint)},
                       {"upload_print_label", _u8L("Upload and Print")},
                       {"time_lapse", recent_int("elegoolink_timelapse", 0) == 1},
                       {"time_lapse_label", _u8L("Time-lapse")},
                       {"leveling", recent_int("elegoolink_heated_bed_leveling", 0) == 1},
                       {"leveling_label", _u8L("Heated Bed Leveling")},
                       {"side_b", bed == int(btPC)},
                       {"side_a_label", _u8L("Textured Build Plate (Side A)")},
                       {"side_b_label", _u8L("Smooth Build Plate (Side B)")},
                       {"app_side_b", app_bed == int(btPC)},
                       {"warning", _u8L("The selected bed type does not match the file. Please confirm before starting the print.")}};
    } else if (in.host_type == int(htCrealityPrint)) {
        // CrealityPrintHostSendDialog::init: on a multi-colour printer, self test and the CFS mapping
        auto *creality = static_cast<CrealityPrint *>(host);
        if (creality->supports_multi_color_print()) {
            json slots = json::array();
            struct Slot { std::string tool_id, type, color; int box_id, material_id; };
            std::vector<Slot> printer_slots;
            const std::string boxes_json = creality->query_boxes_info();
            if (!boxes_json.empty()) {
                try {
                    auto resp = json::parse(boxes_json);
                    if (resp.contains("boxsInfo") && resp["boxsInfo"].contains("materialBoxs"))
                        for (auto &box : resp["boxsInfo"]["materialBoxs"]) {
                            const int box_id = box["id"].get<int>(), box_type = box.value("type", 0);
                            if (box_type == 0 && box.value("state", 0) != 1) continue;
                            for (auto &mat : box["materials"]) {
                                const int slot_id = mat["id"].get<int>();
                                std::string color = mat.value("color", "#FFFFFF");
                                if (color.size() == 8 && color[0] == '#') color = "#" + color.substr(2);
                                printer_slots.push_back({"T" + std::to_string(box_id) + std::string(1, char('A' + slot_id)), mat.value("type", ""), color, box_id, slot_id});
                            }
                        }
                } catch (const json::exception &ex) {
                    BOOST_LOG_TRIVIAL(error) << "CrealityPrint dialog: Failed to parse boxsInfo: " << ex.what();
                }
            }
            for (const Slot &s : printer_slots)
                slots.push_back({{"label", s.box_id == 0 ? Slic3r::format(_u8L("Ext - %s"), s.type) : s.tool_id.substr(1) + " - " + s.type},
                                 {"color", s.color}, {"ext", s.box_id == 0}, {"tool_id", s.tool_id}, {"type", s.type},
                                 {"box_id", s.box_id}, {"material_id", s.material_id}});
            json filaments = json::array();
            const int count = int(in.colors.size());
            if (count > 0 && !printer_slots.empty())
                for (int i = 0; i < count; ++i) {
                    const std::string gc_color = in.colors[i], gc_type = i < int(in.types.size()) ? in.types[i] : "?";
                    // the best default: CFS colour + type, CFS type, Ext colour + type, Ext type, else positional
                    int default_sel = i < int(printer_slots.size()) ? i : 0;
                    bool matched = false;
                    for (int pass = 0; pass < 4 && !matched; ++pass)
                        for (int s = 0; s < int(printer_slots.size()); ++s) {
                            const bool is_ext = printer_slots[s].box_id == 0, type_match = printer_slots[s].type == gc_type;
                            const bool color_match = wxColour(wxString(printer_slots[s].color)) == wxColour(wxString(gc_color));
                            const bool hit = pass == 0 ? !is_ext && type_match && color_match : pass == 1 ? !is_ext && type_match :
                                             pass == 2 ? is_ext && type_match && color_match : is_ext && type_match;
                            if (hit) { default_sel = s; matched = true; break; }
                        }
                    filaments.push_back({{"label", std::to_string(i + 1) + " (" + gc_type + ")"}, {"color", gc_color}, {"selected", default_sel}});
                }
            auto it = in.recent.find("crealityprint_enable_self_test");
            bool self_test = false;
            try { self_test = it != in.recent.end() && !it->second.empty() && std::stoi(it->second) != 0; } catch (...) {}
            e["creality"] = {{"title", Slic3r::format(_u8L("Printer: %s"), creality->model_name())},
                             {"self_test", self_test},
                             {"self_test_label", _u8L("Calibrate before printing")},
                             {"mapping_label", _u8L("Filament Mapping:")},
                             {"slots", slots},
                             {"filaments", filaments}};
        }
    } else if (in.flashforge_local) {
        // Plater::send_gcode_legacy: the slots first (a login failure stops here), then FlashforgePrintHostSendDialog
        auto *ff = dynamic_cast<Flashforge *>(host);
        if (ff == nullptr) throw CommandError(_u8L("Flashforge host is not available."));
        std::vector<FlashforgeMaterialSlot> slots;
        bool     supports_material_station = false;
        wxString msg;
        if (!ff->fetch_material_slots(slots, &supports_material_station, msg))
            throw CommandError(msg.empty() ? _u8L("Unable to log in to the Flashforge printer.") : msg.ToStdString());
        FfSendLogic logic;
        json js = json::array();
        for (const auto &s : slots)
            js.push_back({{"id", s.slot_id}, {"has", s.has_filament}, {"material", s.material_name}, {"color", hex(logic.to_wx_colour(s.material_color))}});
        // auto_assign_mappings: the closest colour among the loaded slots of the same material
        json filaments = json::array();
        for (const FilamentInfo &f : in.project_filaments) {
            const wxColour fc = logic.to_wx_colour(f.color);
            const FlashforgeMaterialSlot *best = nullptr;
            long long best_distance = std::numeric_limits<long long>::max();
            json candidates = json::array();
            for (const auto &s : slots) {
                if (!s.has_filament || !logic.slot_matches_filament(s, f)) continue;
                candidates.push_back(s.slot_id);
                const long long d = color_distance_sq(fc, logic.to_wx_colour(s.material_color));
                if (best == nullptr || d < best_distance) { best = &s; best_distance = d; }
            }
            filaments.push_back({{"id", f.id}, {"color", hex(fc)}, {"type", f.get_display_filament_type()}, {"candidates", candidates},
                                 {"selected", best ? best->slot_id : 0}});
        }
        auto recent = [&](const char *k) { auto it = in.recent.find(k); return it == in.recent.end() ? std::string() : it->second; };
        const std::string lev = recent("flashforge_leveling_before_print"), tl = recent("flashforge_timelapse_video");
        // local printing defaults to IFS when the printer has one
        if (supports_material_station && !Core::get().app_config->has("recent", "flashforge_use_material_station"))
            Core::get().app_config->set("recent", "flashforge_use_material_station", "1");
        e["flashforge"] = {
            {"leveling", lev.empty() ? true : lev == "1"},
            {"leveling_label", _u8L("Leveling before print")},
            {"time_lapse", tl == "1"},
            {"time_lapse_label", _u8L("Time-lapse")},
            {"ifs", supports_material_station},
            {"ifs_enabled", supports_material_station},
            {"ifs_label", _u8L("Enable IFS")},
            {"status", supports_material_station ? GUI::format(_L_PLURAL("Detected %d IFS slot on printer.", "Detected %d IFS slots on printer.", slots.size()), int(slots.size()))
                                                 : _u8L("This printer does not report a material station.")},
            {"slots", js},
            {"filaments", filaments},
            {"error_multi", _u8L("This plate uses multiple materials. Enable IFS and assign each tool to a printer slot.")},
            {"error_unassigned", _u8L("Each project material must be assigned to an IFS slot before printing.")}};
    }
    g_last = e;
    return e;
}

void send_extras_apply(const json &a, std::map<std::string, std::string> &extended_info, bool &start_print)
{
    AppConfig &ac = *Core::get().app_config;
    const json x = a.value("extras", json::object());
    if (g_last.contains("elegoo")) {
        // ElegooPrintHostSendDialog::EndModal + extendedInfo
        start_print = x.value("upload_print", false);
        const int bed = x.value("side_b", false) ? int(btPC) : int(btPTE);
        const int tl = x.value("time_lapse", false) ? 1 : 0, lev = x.value("leveling", false) ? 1 : 0;
        ac.set("recent", "elegoolink_upload_and_print", std::to_string(int(start_print ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None)));
        ac.set("recent", "elegoolink_timelapse", std::to_string(tl));
        ac.set("recent", "elegoolink_heated_bed_leveling", std::to_string(lev));
        ac.set("recent", "elegoolink_bed_type", std::to_string(bed));
        extended_info["bedType"] = std::to_string(bed);
        extended_info["timeLapse"] = std::to_string(tl);
        extended_info["heatedBedLeveling"] = std::to_string(lev);
    } else if (g_last.contains("creality")) {
        // CrealityPrintHostSendDialog::extendedInfo: self test, colorMatch_<i> per G-code filament
        const bool self_test = x.value("self_test", false);
        ac.set("recent", "crealityprint_enable_self_test", self_test ? "1" : "0");
        extended_info["enableSelfTest"] = self_test ? "1" : "0";
        const json &slots = g_last["creality"]["slots"];
        const json  sel = x.value("selected", json::array());
        for (size_t i = 0; i < sel.size(); ++i) {
            const int s = sel[i].get<int>();
            if (s < 0 || s >= int(slots.size())) continue;
            const json &slot = slots[s];
            const std::string gcode_tool = "T1" + std::string(1, char('A' + i));
            extended_info["colorMatch_" + std::to_string(i)] = gcode_tool + "\t" + slot["type"].get<std::string>() + "\t" + slot["color"].get<std::string>() +
                                                               "\t" + std::to_string(slot["box_id"].get<int>()) + "\t" + std::to_string(slot["material_id"].get<int>());
        }
    } else if (g_last.contains("flashforge")) {
        // FlashforgePrintHostSendDialog::EndModal + extendedInfo
        const json &ff = g_last["flashforge"];
        const bool lev = x.value("leveling", true), tl = x.value("time_lapse", false), ifs = x.value("ifs", false) && ff.value("ifs_enabled", false);
        ac.set("recent", "flashforge_leveling_before_print", lev ? "1" : "0");
        ac.set("recent", "flashforge_timelapse_video", tl ? "1" : "0");
        ac.set("recent", "flashforge_use_material_station", ifs ? "1" : "0");
        json mappings = json::array();
        int  mapped_count = 0;
        if (ifs) {
            const json sel = x.value("selected", json::array());
            for (size_t i = 0; i < sel.size() && i < ff["filaments"].size(); ++i) {
                const int slot_id = sel[i].get<int>();
                if (slot_id <= 0) continue;
                const json &f = ff["filaments"][i];
                for (const json &s : ff["slots"])
                    if (s["id"].get<int>() == slot_id) {
                        mappings.push_back({{"toolId", f["id"]}, {"slotId", slot_id}, {"materialName", s["material"]},
                                            {"toolMaterialColor", f["color"]}, {"slotMaterialColor", s["color"]}});
                        ++mapped_count;
                    }
            }
        }
        extended_info["levelingBeforePrint"] = lev ? "1" : "0";
        extended_info["timeLapseVideo"] = tl ? "1" : "0";
        extended_info["useMatlStation"] = ifs ? "1" : "0";
        extended_info["gcodeToolCnt"] = std::to_string(mapped_count);
        extended_info["materialMappings"] = mappings.dump();
    }
}

} // namespace orca
