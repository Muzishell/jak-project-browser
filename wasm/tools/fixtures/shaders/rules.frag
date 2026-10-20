#version 410 core

out vec4 color;
in vec4 fragment_color;
noperspective in vec3 tex_coord;
flat in uint unit;
uniform sampler2D tex_T0;
uniform sampler2D tex_T1;

vec4 sample_tex(vec2 coord, uint u) {
  switch (u) {
    case 0: return texture(tex_T0, coord);
    case 1: return texture(tex_T1, coord);
    default: return vec4(1, 0, 1, 1);
  }
}

void main() {
  vec4 T0 = sample_tex(tex_coord.xy / tex_coord.z, unit);
  color = fragment_color * T0 / 2;
  color.a = max(color.a, 0);
}
