#version 300 es
precision highp float;

#include "hdr.h"

in vec2 fragCoord;
out vec4 color;

uniform sampler2D sampler1;
uniform bool      pq;
uniform float     referenceWhiteLevel;

void main()
{
  color = texture(sampler1, fragCoord);
  if (color.a <= 0.0)
  {
    color.rgb = vec3(0.0);
    return;
  }

  // ImGui's framebuffer contains premultiplied sRGB. Convert the straight
  // color, then premultiply again for composition onto the HDR framebuffer.
  vec3 linear = srgb2lin(clamp(color.rgb / color.a, 0.0, 1.0));
  if (pq)
  {
    linear = bt709to2020(linear);
    color.rgb = lin2pq(linear * (referenceWhiteLevel / 10000.0)) * color.a;
  }
  else
  {
    // scRGB is linear sRGB and defines 1.0 as 80 nits.
    color.rgb = linear * (referenceWhiteLevel / 80.0) * color.a;
  }
}
