#include "Shaders.hpp"

#include <EGL/egl.h>
#include <android/log.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "libslic3r/Utils.hpp"

namespace orca::gl {

#define GLLOG(...) __android_log_print(ANDROID_LOG_WARN, "orcacore-gl", __VA_ARGS__)

GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); GLLOG("shader: %s", log); }
    return s;
}

GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "v_position");
    glBindAttribLocation(p, 1, "v_normal");
    glBindAttribLocation(p, 1, "v_barycentric");   // mm_gouraud (no v_normal there)
    glBindAttribLocation(p, 2, "v_tex_coord");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof log, nullptr, log); GLLOG("link: %s", log); }
    return p;
}

static std::mutex m;
static std::map<std::pair<EGLContext, std::string>, GLuint> cache;

void forget_programs()
{
    std::lock_guard<std::mutex> l(m);
    EGLContext ctx = eglGetCurrentContext();
    for (auto it = cache.begin(); it != cache.end();) it = it->first.first == ctx ? cache.erase(it) : std::next(it);
}

GLuint orca_program(const std::string &name)
{
    std::lock_guard<std::mutex> l(m);
    auto key = std::make_pair(eglGetCurrentContext(), name);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    auto read = [&](const char *ext) {
        std::ifstream f(Slic3r::resources_dir() + "/shaders/ES/" + name + ext);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    std::string vs = read(".vs"), fs = read(".fs");
    if (vs.empty() || fs.empty()) { GLLOG("missing shader %s", name.c_str()); return 0; }
    GLuint p = program(vs.c_str(), fs.c_str());
    cache[key] = p;
    return p;
}

} // namespace orca::gl
