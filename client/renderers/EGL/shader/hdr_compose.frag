#version 300 es
precision highp float;

#include "hdr.h"

in vec2 fragCoord;
out vec4 color;

uniform sampler2D sampler1;

void main()
{
  vec3 scRGB      = texture(sampler1, fragCoord).rgb;
  // Preserve negative BT.709 components used to represent colours outside
  // the BT.709 gamut. Clamp only after rotating back into BT.2020.
  vec3 linear2020 = bt709to2020(scRGB * (80.0 / 10000.0));
  color = vec4(lin2pq(max(linear2020, 0.0)), 1.0);
}
