#version 300 es

in  highp vec3  pos;
out highp vec4  color;

uniform sampler2D sampler1;

void main()
{
  highp float d = 1.0 - 0.5 * length(pos.xy);
  color = vec4(0.234375 * d, 0.015625 * d, 0.425781 * d, 1.0);
}
