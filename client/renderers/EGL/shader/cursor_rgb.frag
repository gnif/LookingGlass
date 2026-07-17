#version 300 es
precision highp float;
precision highp int;

#include "color_blind.h"
#include "hdr.h"

in  vec2  uv;
out vec4  color;

uniform sampler2D sampler1;
uniform float     scale;
uniform int       cbMode;
uniform bool      mapSDRtoPQ;

void main()
{
  if (scale > 1.0)
  {
    vec2 ts = vec2(textureSize(sampler1, 0));
    vec2 px = (uv - (0.5 / ts)) * ts;
    if (px.x < 0.0 || px.y < 0.0)
      discard;

    color = texelFetch(sampler1, ivec2(px), 0);
  }
  else
    color = texture(sampler1, uv);

  if (cbMode > 0)
    color = cbTransform(color, cbMode);

  if (mapSDRtoPQ)
  {
    if (color.a > 0.0)
    {
      // Cursor pixels are premultiplied. Convert the straight color to PQ,
      // then premultiply again so the blend operation remains valid.
      vec3 srgb = clamp(color.rgb / color.a, 0.0, 1.0);
      vec3 linear = bt709to2020(srgb2lin(srgb));
      color.rgb = lin2pq(linear * (203.0 / 10000.0)) * color.a;
    }
    else
      color.rgb = vec3(0.0);
  }
}
