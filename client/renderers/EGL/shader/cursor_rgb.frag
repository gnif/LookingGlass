#version 300 es
precision mediump float;

#include "color_blind.h"

in  vec2 uv;
out vec4 color;

uniform sampler2D sampler1;

uniform int cbMode;

void main()
{
  color = texture(sampler1, uv);

  if (cbMode > 0)
    color = cbTransform(color, cbMode);
}
