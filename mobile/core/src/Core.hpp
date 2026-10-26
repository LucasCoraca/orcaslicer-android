// The engine state shared by all core commands: app config, presets, the project (model + plates).
// Commands run on the app's single engine thread (Kotlin Engine), except the slicing workers.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace orca {

using json = nlohmann::json;

struct CommandError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class Core
{
public:
    static Core &get();

    // resources: the unpacked resources/ tree; data: Orca's data dir (user presets, app config, logs)
    json init(const std::string &resources, const std::string &data, const std::string &language);
    json call(const std::string &cmd, const json &args);

    using Command = std::function<json(const json &)>;
    static void add_command(const std::string &name, Command fn);
    // commands on no shared state (colour math of a dialog): run on the caller's thread without the engine lock, so
    // they answer while a modal ask holds the engine
    static void add_pure_command(const std::string &name, Command fn);
    static json call_pure(const std::string &cmd, const json &args);
    // GUI_App::CallAfter: runs fn on the engine thread soon after (the app drains the queue on "call_after")
    static void call_after(std::function<void()> fn);
    // a modal loop on the engine thread (a plugin's modal window): waits up to timeout_ms for queued calls, then runs them
    static void pump_call_after(int timeout_ms);

    // the full config of the selected presets, also what upstream code sees as Plater::config()
    const Slic3r::DynamicPrintConfig &full_config();
    void                              invalidate_full_config() { m_full_config_valid = false; }

    std::unique_ptr<Slic3r::AppConfig>    app_config;
    std::unique_ptr<Slic3r::PresetBundle> preset_bundle;
    Slic3r::Model                         model;
    Slic3r::GUI::Plater                   plater;
    std::recursive_mutex                  mutex;   // guards model/presets against the slicing workers

private:
    Slic3r::DynamicPrintConfig m_full_config;
    bool                       m_full_config_valid{false};
    static std::map<std::string, Command> &commands();
    static std::map<std::string, Command> &pure_commands();
};

// Registers commands at static-init time: static Commands reg({{"name", fn}, ...});
struct Commands
{
    Commands(std::initializer_list<std::pair<const char *, Core::Command>> list)
    {
        for (auto &c : list) Core::add_command(c.first, c.second);
    }
};

struct PureCommands
{
    PureCommands(std::initializer_list<std::pair<const char *, Core::Command>> list)
    {
        for (auto &c : list) Core::add_pure_command(c.first, c.second);
    }
};

// argument helpers
inline std::string str_arg(const json &a, const char *k) { if (!a.contains(k)) throw CommandError(std::string("missing ") + k); return a[k].get<std::string>(); }
Slic3r::Preset::Type preset_type(const std::string &name);
Slic3r::PresetCollection &collection(Slic3r::Preset::Type type);

} // namespace orca
