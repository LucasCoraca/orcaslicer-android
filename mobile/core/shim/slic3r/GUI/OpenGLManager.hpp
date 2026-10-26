// Stand-in for slic3r/GUI/OpenGLManager.hpp: the 3D view's GL info (Viewport.cpp)
#pragma once
#include <string>

namespace Slic3r { namespace GUI {
class OpenGLManager
{
public:
    class GLInfo
    {
    public:
        bool        is_detected() const { return !m_vendor.empty() || !m_renderer.empty(); }
        std::string get_vendor() const { return m_vendor; }
        std::string get_renderer() const { return m_renderer; }
        std::string get_version() const { return m_version; }
        std::string get_glsl_version() const { return m_glsl; }
        bool        is_core_profile() const { return false; }
        std::string m_vendor, m_renderer, m_version, m_glsl;
    };
    static const GLInfo &get_gl_info();
};
}} // namespace Slic3r::GUI
