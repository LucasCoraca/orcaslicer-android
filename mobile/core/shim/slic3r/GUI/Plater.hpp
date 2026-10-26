// Stand-in for slic3r/GUI/Plater.hpp: the project config, as upstream GUI logic reads it, and what the plugin system
// asks of the plater (Plugins.cpp: the core's project, the app's dock panes and notifications)
#pragma once
#include <functional>
#include <string>
#include <boost/filesystem/path.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "wx/string.h"
#include "wx/gdicmn.h"
#include "wx/window.h"

namespace Slic3r {
class Model;
namespace GUI {

class NotificationManager;
class DockPanel;

class Sidebar
{
public:
    void update_all_preset_comboboxes();
};

class Plater : public wxWindow
{
public:
    // The full config of the selected presets (PresetBundle::full_config()), refreshed by the core.
    const DynamicPrintConfig *config() const { return m_config; }
    void set_config(const DynamicPrintConfig *config) { m_config = config; }

    Model               &model();
    bool                 is_project_dirty() const;
    bool                 is_presets_dirty() const;
    bool                 inside_snapshot_capture();
    wxString             get_project_filename(const wxString &extension = wxString()) const;
    bool                 export_3mf_copy(const boost::filesystem::path &path);
    NotificationManager *get_notification_manager();
    Sidebar             &sidebar();
    // the plugin dock panes (Plugins.cpp: the app's side panels)
    void add_dock_pane(DockPanel *panel, const std::string &name, const wxString &title, const std::string &dock, const wxSize &size,
                       std::function<void()> on_close);
    void show_dock_pane(DockPanel *panel, bool show);
    void revalidate_current_plate_if_plugins_missing();

private:
    const DynamicPrintConfig *m_config{nullptr};
};

}} // namespace Slic3r::GUI
