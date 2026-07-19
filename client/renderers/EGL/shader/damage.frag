#version 300 es
precision highp float;
precision highp int;

#include "hdr.h"

out vec4 color;

uniform bool  linearOutput;
uniform float referenceWhiteLevel;

void main()
{
  vec3 value = vec3(1.0, 1.0, 0.0);
  if (linearOutput)
    value = srgb2lin(value) * (referenceWhiteLevel / 80.0);
  color = vec4(value, 0.5);
}
