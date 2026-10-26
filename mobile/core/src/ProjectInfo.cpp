// The Project tab (ProjectPanel with resources/web/model): the 3MF's model / profile information and its auxiliary
// files (Model Pictures, Bill of Materials, Assembly Guide, Others, Profile Pictures), as ProjectPanel::on_reload
// sends them to the page, and the page's commands (OnScriptMessage).
#include <fstream>
#include <boost/algorithm/string.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem.hpp>

#include "Core.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Preset.hpp"
#include <boost/format.hpp>
#include "slic3r/GUI/I18N.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace orca {

namespace {

#include "slic3r/GUI/ProjectStrings.inc"

// Http::url_encode (curl_easy_escape)
std::string url_encode(const std::string &value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string url_decode(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) { out += char(std::stoi(value.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else out += value[i] == '+' ? ' ' : value[i];
    }
    return out;
}

std::string base64(const std::string &s)
{
    std::string out(boost::beast::detail::base64::encoded_size(s.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), s.data(), s.size()));
    return out;
}

// ProjectPanel::formatBytes / to_base64
std::string format_bytes(unsigned long bytes)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2fMB", std::round(double(bytes) / (1024 * 1024) * 1000) / 1000);
    return buf;
}

std::string to_base64(const std::string &file_path)
{
    std::ifstream f(file_path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    const size_t dot = file_path.find_last_of('.');
    return "data:image/" + (dot != std::string::npos ? file_path.substr(dot + 1) : std::string()) + ";base64," + base64(ss.str());
}

std::string convert_newlines_to_br(const std::string &text)
{
    std::string out = text;
    boost::replace_all(out, "\r\n", "<br>");
    boost::replace_all(out, "\n", "<br>");
    return out;
}

// ProjectPanel::Reload: the auxiliary files by folder
std::map<std::string, std::vector<json>> reload_files(const std::string &aux_path)
{
    static const std::array<std::string, 5> s_default_folders = {"Model Pictures", "Bill of Materials", "Assembly Guide", "Others", "Profile Pictures"};
    std::map<std::string, std::vector<json>> list;
    for (const auto &folder : s_default_folders) list[folder] = {};
    boost::system::error_code ec;
    fs::create_directories(aux_path, ec);
    for (const auto &folder : s_default_folders) fs::create_directories(fs::path(aux_path) / folder, ec);
    for (fs::directory_iterator dir(aux_path, ec), end; dir != end && !ec; dir.increment(ec)) {
        if (!fs::is_directory(dir->path())) continue;
        for (fs::directory_iterator it(dir->path(), ec), e2; it != e2 && !ec; it.increment(ec)) {
            if (fs::is_directory(it->path())) continue;
            const std::string file_path = it->path().string();
            for (const auto &folder : s_default_folders) {
                if (file_path.find(folder) == std::string::npos) continue;
                json obj;
                obj["_filepath"] = file_path;
                obj["filename"] = url_encode(it->path().filename().string());
                obj["size"] = format_bytes((unsigned long) fs::file_size(it->path(), ec));
                std::string ext = boost::algorithm::to_lower_copy(it->path().extension().string());
                const bool image = ext == ".jpg" || ext == ".jpeg" || ext == ".pjpeg" || ext == ".png" || ext == ".jfif" || ext == ".pjp" ||
                                   ext == ".webp" || ext == ".bmp";
                obj["filepath"] = image ? to_base64(file_path) : url_encode(file_path);
                list[folder].push_back(obj);
                break;
            }
        }
    }
    return list;
}

// ProjectPanel::on_reload: the HandleStudio() call the page gets
std::string info_script()
{
    Model &model = Plater::get().model();
    std::string update_type, license, model_name, model_author, cover_file, description, p_name, p_author, p_description, p_cover_file;
    if (auto model_info = model.model_info) {
        license = model_info->license;
        model_name = model_info->model_name;
        cover_file = model_info->cover_file;
        description = model_info->description;
        update_type = model_info->origin;
        try {
            if (!model_info->copyright.empty()) {
                json copy_right = json::parse(model_info->copyright);
                if (copy_right.is_array())
                    for (auto &c : copy_right)
                        if (c.contains("author")) model_author = c["author"].get<std::string>();
            }
        } catch (...) {}
    }
    if (model_author.empty() && model.design_info != nullptr) model_author = model.design_info->Designer;
    if (model.profile_info != nullptr) {
        p_name = model.profile_info->ProfileTile;
        p_description = model.profile_info->ProfileDescription;
        p_cover_file = model.profile_info->ProfileCover;
        p_author = model.profile_info->ProfileUserName;
    }
    auto files = reload_files(model.get_auxiliary_file_temp_path());
    bool has_content = false;
    for (const std::string &v : {update_type, license, model_name, model_author, cover_file, description, p_name, p_author, p_description, p_cover_file})
        has_content |= !v.empty();
    for (const auto &f : files) has_content |= !f.second.empty();
    static int sequence = 0;
    if (!has_content) return "HandleStudio(" + json{{"command", "clear_3mf_info"}, {"sequence_id", std::to_string(sequence++)}}.dump() + ")";
    json j;
    j["model"]["license"] = license;
    j["model"]["name"] = url_encode(model_name);
    j["model"]["author"] = url_encode(model_author);
    j["model"]["cover_img"] = url_encode(cover_file);
    j["model"]["description"] = url_encode(convert_newlines_to_br(description));
    j["model"]["preview_img"] = files["Model Pictures"];
    j["model"]["upload_type"] = update_type;
    j["file"]["BOM"] = files["Bill of Materials"];
    j["file"]["Assembly"] = files["Assembly Guide"];
    j["file"]["Other"] = files["Others"];
    j["profile"]["name"] = url_encode(p_name);
    j["profile"]["author"] = url_encode(p_author);
    j["profile"]["description"] = url_encode(p_description);
    j["profile"]["cover_img"] = url_encode(p_cover_file);
    j["profile"]["preview_img"] = files["Profile Pictures"];
    json res = {{"command", "show_3mf_info"}, {"sequence_id", std::to_string(sequence++)}, {"model", j}};
    return "HandleStudio(" + res.dump(-1, ' ', false, json::error_handler_t::ignore) + ")";
}

// ---- AuxiliaryPanel: the editor behind "Edit Project Info"

const std::array<std::string, 4> s_editable_folders = {"Model Pictures", "Bill of Materials", "Assembly Guide", "Others"};
// Project.cpp license_list
const std::vector<std::string> license_list = {"BSD License", "Apache License", "GPL License", "LGPL License", "MIT License", "CC License"};

std::shared_ptr<ModelInfo> ensure_model_info()
{
    Model &model = Plater::get().model();
    if (model.model_info == nullptr) model.model_info = std::make_shared<ModelInfo>();
    return model.model_info;
}

json aux_state()
{
    Model &model = Plater::get().model();
    const std::string aux = model.get_auxiliary_file_temp_path();
    json folders = json::array();
    for (const std::string &folder : s_editable_folders) {
        json files = json::array();
        boost::system::error_code ec;
        fs::create_directories(fs::path(aux) / folder, ec);
        for (fs::directory_iterator it(fs::path(aux) / folder, ec), end; it != end && !ec; it.increment(ec)) {
            if (fs::is_directory(it->path())) continue;
            const std::string ext = boost::algorithm::to_lower_copy(it->path().extension().string());
            const bool image = ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".webp";
            files.push_back({{"path", it->path().string()}, {"name", it->path().filename().string()},
                             {"stem", it->path().stem().string()}, {"image", image},
                             {"cover", model.model_info && model.model_info->cover_file == it->path().filename().string()}});
        }
        folders.push_back({{"folder", folder}, {"files", files}});
    }
    json st = {{"designer", model.design_info ? model.design_info->Designer : std::string()},
               {"model_name", model.model_info ? model.model_info->model_name : std::string()},
               {"description", model.model_info ? model.model_info->description : std::string()},
               {"license", model.model_info ? model.model_info->license : std::string()},
               {"licenses", license_list}, {"folders", folders},
               {"labels", {{"return", _u8L("Return")}, {"basic", _u8L("Basic Info")}, {"pictures", _u8L("Pictures")},
                           {"bom", _u8L("Bill of Materials")}, {"assembly", _u8L("Assembly Guide")}, {"others", _u8L("Others")},
                           {"author", _u8L("Author")}, {"model_name", _u8L("Model Name")}, {"license", _u8L("License")},
                           {"description", _u8L("Description:")}, {"add", _u8L("Add file")}, {"cover", _u8L("Set as cover")},
                           {"delete", _u8L("Delete")}, {"rename", _u8L("Rename")}}}};
    return st;
}

// AuFile::on_input_enter's checks
std::string check_name(const fs::path &file, const std::string &new_name)
{
    const char *unusable_symbols = "<>[]:/\\|?*\"";
    if (new_name.find_first_of(unusable_symbols) != std::string::npos)
        return _u8L("Name is invalid;") + "\n" + _u8L("illegal characters:") + " " + unusable_symbols;
    if (new_name.find(PresetCollection::get_suffix_modified()) != std::string::npos)
        return _u8L("Name is invalid;") + "\n" + _u8L("illegal suffix:") + "\n\t" + PresetCollection::get_suffix_modified();
    if (fs::exists(file.parent_path() / (new_name + file.extension().string())))
        return (boost::format(_u8L("The name \"%1%\" already exists.")) % new_name).str();
    if (new_name.empty()) return _u8L("The name field is not allowed to be empty.");
    if (new_name.front() == ' ') return _u8L("The name is not allowed to start with a space.");
    if (new_name.back() == ' ') return _u8L("The name is not allowed to end with a space.");
    return {};
}

json aux_set(const json &a)
{
    Plater &p = Plater::get();
    Model &model = p.model();
    const std::string what = a.value("what", std::string());
    json st;
    if (what == "designer") model.SetDesigner(a.at("value").get<std::string>(), "");
    else if (what == "model_name") ensure_model_info()->model_name = a.at("value").get<std::string>();
    else if (what == "description") ensure_model_info()->description = a.at("value").get<std::string>();
    else if (what == "license") ensure_model_info()->license = a.at("value").get<std::string>();
    else if (what == "add") {   // AuxiliaryPanel::on_import_file: copied into the folder
        const fs::path dir = fs::path(model.get_auxiliary_file_temp_path()) / a.at("folder").get<std::string>();
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        for (size_t i = 0; i < a.at("paths").size(); ++i) {
            const fs::path src = a["paths"][i].get<std::string>();
            const std::string name = a.contains("names") ? a["names"][i].get<std::string>() : src.filename().string();
            fs::copy_file(src, dir / name, fs::copy_options::overwrite_existing, ec);
        }
    } else if (what == "delete") {   // AuFile::on_set_delete: the cover's thumbnails go with it
        const fs::path f = a.at("path").get<std::string>();
        fs::remove(f);
        if (model.model_info && model.model_info->cover_file == f.filename().string()) {
            model.model_info->cover_file.clear();
            const fs::path dir = f.parent_path().parent_path() / ".thumbnails";
            boost::system::error_code ec;
            for (const char *t : {"thumbnail_3mf.png", "thumbnail_small.png", "thumbnail_middle.png"}) fs::remove(dir / t, ec);
        }
    } else if (what == "rename") {
        const fs::path f = a.at("path").get<std::string>();
        const std::string name = a.at("name");
        const std::string error = check_name(f, name);
        if (!error.empty()) st["notice"] = error;
        else {
            fs::rename(f, f.parent_path() / (name + f.extension().string()));
            if (model.model_info && model.model_info->cover_file == f.filename().string()) model.model_info->cover_file = name + f.extension().string();
        }
    } else if (what == "cover") {   // AuFile::on_set_cover (the cover image goes into the 3MF on save)
        const fs::path f = a.at("path").get<std::string>();
        ensure_model_info()->cover_file = f.filename().string();
        // the thumbnails generate_image makes next to the folders (.thumbnails, _3MF_COVER_SIZE / PRINTER_THUMBNAIL_*):
        // the app scales the image into them (GERNERATE_IMAGE_RESIZE: filling the size, centred, cropped)
        const fs::path dir = f.parent_path().parent_path() / ".thumbnails";
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        st["thumbnails"] = json::array({{{"path", (dir / "thumbnail_3mf.png").string()}, {"width", 240}, {"height", 240}},
                                        {{"path", (dir / "thumbnail_small.png").string()}, {"width", 252}, {"height", 188}},
                                        {{"path", (dir / "thumbnail_middle.png").string()}, {"width", 680}, {"height", 680}}});
        st["source"] = f.string();
    }
    p.dirty = true;
    json out = aux_state();
    if (st.contains("notice")) out["notice"] = st["notice"];
    if (st.contains("thumbnails")) { out["thumbnails"] = st["thumbnails"]; out["source"] = st["source"]; }
    return out;
}

} // namespace

static Commands reg({
    {"projectinfo.aux", [](const json &) -> json { return aux_state(); }},
    {"projectinfo.aux_set", aux_set},
    {"projectinfo.open", [](const json &) -> json {
        std::string url = "file://" + (fs::path(resources_dir()) / "web/model/index.html").string();
        const std::string lang = Core::get().app_config->get("language");
        if (!lang.empty()) url += "?lang=" + lang;
        return {{"url", url}, {"script", "window.ORCA_UI_STRINGS = " + project_page_strings().dump(-1, ' ', false, json::error_handler_t::ignore) + ";"}};
    }},
    // ProjectPanel::OnScriptMessage
    {"projectinfo.message", [](const json &a) -> json {
        json j = json::parse(a.at("message").get<std::string>(), nullptr, false);
        if (j.is_discarded()) return json::object();
        const std::string cmd = j.value("command", std::string());
        if (cmd == "request_3mf_info") return {{"scripts", {info_script()}}};
        if (cmd == "open_3mf_accessory") return {{"open", url_decode(j.value("accessory_path", std::string()))}};
        if (cmd == "edit_project_info") return {{"edit", true}};
        return json::object();
    }},
    {"projectinfo.reload", [](const json &) -> json { return {{"scripts", {info_script()}}}; }},
});

} // namespace orca
