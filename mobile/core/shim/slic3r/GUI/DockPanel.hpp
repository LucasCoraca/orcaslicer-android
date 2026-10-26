// Stand-in for slic3r/GUI/DockPanel.hpp: a plugin's HTML panel beside the 3D view, shown by the app (Plugins.cpp)
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include "wx/window.h"

namespace Slic3r { namespace GUI {

std::string plugin_pane_name(const std::string &plugin_key, const std::string &title);

class DockPanel : public wxWindow
{
public:
    using MessageHandler = std::function<void(const nlohmann::json &data)>;
    using CloseHandler   = std::function<void()>;

    DockPanel(wxWindow *parent, const std::string &html, MessageHandler on_message, CloseHandler on_close, CloseHandler on_destroyed);
    ~DockPanel() override;

    void push_message(const nlohmann::json &data);
    void request_close();
    void destroy_silently();
    void fire_close();

    // WebPanel::on_page_message
    bool on_page_message(const std::string &kind, const nlohmann::json &data);
    int  id() const { return m_id; }
    const std::string &html() const { return m_html; }

private:
    void remove_pane();

    int            m_id;
    std::string    m_html;
    bool           m_closing{false};
    MessageHandler m_on_message;
    CloseHandler   m_on_close;
    CloseHandler   m_on_destroyed;
};

}} // namespace Slic3r::GUI
