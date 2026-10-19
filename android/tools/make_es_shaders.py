#!/usr/bin/env python3
"""Generate resources/shaders/ES (GLSL ES 3.00) from resources/shaders/140.

usage: make_es_shaders.py <resources/shaders>

The 140 shaders are already in/out style, so the conversion is mechanical:
  * "#version 140|150" -> "#version 300 es" + default precisions
  * "#extension" lines are dropped (GL_ARB_texture_multisample: the #ifdef falls back to the plain sampler2D)
  * geometry shaders are skipped: on ES the thick/dashed lines use the hand written dashed_lines.* instead
"""
import os, re, sys

root = sys.argv[1]
src, dst = os.path.join(root, "140"), os.path.join(root, "ES")
os.makedirs(dst, exist_ok=True)

PRECISION = {
    ".vs": "precision highp float;\nprecision highp int;\n",
    ".fs": "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n",
}

def convert(text, ext):
    out = []
    for line in text.splitlines():
        if re.match(r"\s*#extension", line):
            continue
        m = re.match(r"\s*#version\s+(140|150)\b", line)
        if m:
            out.append("#version 300 es")
            out.append(PRECISION[ext].rstrip("\n"))
            continue
        out.append(line)
    return "\n".join(out) + "\n"

n = 0
for name in sorted(os.listdir(src)):
    base, ext = os.path.splitext(name)
    if ext not in (".vs", ".fs"):
        continue
    if base.startswith("dashed_thick_lines"):
        continue
    with open(os.path.join(src, name), encoding="utf-8") as f:
        text = f.read()
    with open(os.path.join(dst, name), "w", encoding="utf-8") as f:
        f.write(convert(text, ext))
    n += 1

HAND = {
# Thick/dashed lines on ES: no geometry shader, so lines are 1 px wide and dashed in the fragment stage.
"dashed_lines.vs": """#version 300 es
precision highp float;
precision highp int;

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;

// v_position.w = coordinate along the line
in vec4 v_position;

out float coord_s;

void main()
{
    coord_s = v_position.w;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position.xyz, 1.0);
}
""",
"dashed_lines.fs": """#version 300 es
precision highp float;
precision highp int;

uniform float dash_size;
uniform float gap_size;
uniform vec4 uniform_color;

in float coord_s;

out vec4 out_color;

void main()
{
    float inv_stride = 1.0 / (dash_size + gap_size);
    if (gap_size > 0.0 && fract(coord_s * inv_stride) > dash_size * inv_stride)
        discard;
    out_color = uniform_color;
}
""",
# Wireframe without glPolygonMode(GL_LINE): barycentric edge detection (needs non-indexed triangles).
"wireframe.vs": """#version 300 es
precision highp float;
precision highp int;

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform float offset;

in vec3 v_position;

out vec3 bary;

void main()
{
    int corner = gl_VertexID % 3;
    bary = vec3(corner == 0 ? 1.0 : 0.0, corner == 1 ? 1.0 : 0.0, corner == 2 ? 1.0 : 0.0);
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
    gl_Position.z -= offset * gl_Position.w;
}
""",
"wireframe.fs": """#version 300 es
precision highp float;
precision highp int;

uniform vec4 uniform_color;

in vec3 bary;

out vec4 out_color;

void main()
{
    vec3 d = fwidth(bary);
    vec3 a = smoothstep(vec3(0.0), d * 1.2, bary);
    float edge = 1.0 - min(min(a.x, a.y), a.z);
    if (edge < 0.05)
        discard;
    out_color = vec4(uniform_color.rgb, uniform_color.a * edge);
}
""",
}
for name, text in HAND.items():
    with open(os.path.join(dst, name), "w", encoding="utf-8") as f:
        f.write(text)
print("ES shaders:", n, "converted +", len(HAND), "hand written ->", dst)
