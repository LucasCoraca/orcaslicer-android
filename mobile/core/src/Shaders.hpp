// GLSL ES sources shared by the 3D view and the offscreen thumbnail renderer, and the program builder.
#pragma once
#include <GLES3/gl3.h>
#include <string>

namespace orca::gl {

GLuint compile(GLenum type, const char *src);
GLuint program(const char *vs, const char *fs);   // binds v_position = 0, v_normal = 1
// one of Orca's own shaders, resources/shaders/ES/<name>.vs/.fs (GLShadersManager), built once per EGL context
GLuint orca_program(const std::string &name);
// the current context was (re)created: its cached programs are gone
void forget_programs();

// resources/shaders/ES/gouraud_light.*: Orca's model lighting (two directional lights, top light with specular)
inline const char *LIT_VS = R"(#version 300 es
precision highp float;
#define INTENSITY_CORRECTION 0.6
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.8 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.125 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  20.0
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.3 * INTENSITY_CORRECTION)
#define INTENSITY_AMBIENT    0.3
uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat3 view_normal_matrix;
in vec3 v_position;
in vec3 v_normal;
out vec2 intensity;
out float world_z;
uniform mat4 model_matrix;
void main() {
    vec3 normal = normalize(view_normal_matrix * v_normal);
    float NdotL = max(dot(normal, LIGHT_TOP_DIR), 0.0);
    intensity.x = INTENSITY_AMBIENT + NdotL * LIGHT_TOP_DIFFUSE;
    vec4 position = view_model_matrix * vec4(v_position, 1.0);
    intensity.y = LIGHT_TOP_SPECULAR * pow(max(dot(-normalize(position.xyz), reflect(-LIGHT_TOP_DIR, normal)), 0.0), LIGHT_TOP_SHININESS);
    NdotL = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    intensity.x += NdotL * LIGHT_FRONT_DIFFUSE;
    world_z = (model_matrix * vec4(v_position, 1.0)).z;
    gl_Position = projection_matrix * position;
})";

inline const char *LIT_FS = R"(#version 300 es
precision highp float;
uniform vec4 uniform_color;
uniform float sinking_z;   // below the bed: darker (GLVolume sinking contours)
in vec2 intensity;
in float world_z;
out vec4 frag;
void main() {
    vec3 c = uniform_color.rgb * intensity.x + intensity.y;
    if (world_z < sinking_z) c *= 0.5;
    frag = vec4(c, uniform_color.a);
})";

inline const char *FLAT_VS = R"(#version 300 es
precision highp float;
uniform mat4 mvp;
in vec3 v_position;
void main() { gl_Position = mvp * vec4(v_position, 1.0); })";

inline const char *FLAT_FS = R"(#version 300 es
precision highp float;
uniform vec4 uniform_color;
out vec4 frag;
void main() { frag = uniform_color; })";

inline const char *TEX_VS = R"(#version 300 es
precision highp float;
uniform mat4 mvp;
in vec3 v_position;
in vec3 v_normal;     // xy: texture coordinates
out vec2 uv;
void main() { uv = v_normal.xy; gl_Position = mvp * vec4(v_position, 1.0); })";

inline const char *TEX_FS = R"(#version 300 es
precision highp float;
uniform sampler2D tex;
in vec2 uv;
out vec4 frag;
void main() { frag = texture(tex, uv); })";

} // namespace orca::gl
