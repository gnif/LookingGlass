#version 300 es
precision highp float;
precision highp int;

#include "hdr.h"

in  vec2 uv;
out vec4 color;

uniform sampler2D sampler1;
uniform float     scale;
uniform bool      mapSDRtoPQ;

void main()
{
  vec4 tmp;
  if (scale > 1.0)
  {
    vec2 ts = vec2(textureSize(sampler1, 0));
    vec2 px = (uv - (0.5 / ts)) * ts;
    if (px.x < 0.0 || px.y < 0.0)
      discard;

    tmp = texelFetch(sampler1, ivec2(px), 0);
  }
  else
    tmp = texture(sampler1, uv);

  if (tmp.rgb == vec3(0.0, 0.0, 0.0))
    discard;

  if (mapSDRtoPQ)
  {
    vec3 linear = bt709to2020(srgb2lin(tmp.rgb));
    tmp.rgb = lin2pq(linear * (203.0 / 10000.0));
  }

  color = tmp;
}
