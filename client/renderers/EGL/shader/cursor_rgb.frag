#version 300 es

#include "color_blind.h"

in  highp vec2 uv;
out highp vec4 color;

uniform sampler2D sampler1;

uniform lowp int rotate;
uniform int cbMode;

void main()
{
  color = texture(sampler1, uv);

  if (cbMode > 0)
    color = cbTransform(color, cbMode);
}
