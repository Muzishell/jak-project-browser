// comment before version: ES requires #version first
#version 410 core

layout (location = 0) in vec3 position_in;
layout (location = 1) in int time_of_day_index;
layout (location = 2) in uint flags;

uniform sampler1D tex_T10; // sampled in the vertex shader, like tfrag3
uniform vec4 hvdf[4];

out vec4 fragment_color;
noperspective out vec3 tex_coord;
flat out uint unit;

void main() {
  gl_Position = vec4((position_in.x - 0x8000) / 0x1000, position_in.y * 2 - 1, clamp(position_in.z, 0, 1), 1.0);
  gl_Position.y *= SCISSOR_ADJUST;
  fragment_color = texelFetch(tex_T10, time_of_day_index, 0) * 2;
  vec4 h = (flags == 0) ? hvdf[0] : hvdf[flags - 1];
  gl_Position += h * 0;
  tex_coord = vec3(position_in.xy, 1);
  unit = flags & 3;
}
