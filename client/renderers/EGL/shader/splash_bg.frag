#version 300 es

in  highp vec3  pos;
out highp vec4  color;

uniform sampler2D sampler1;

void main()
{
  highp float d = 1.0 - sqrt(pos.x * pos.x + pos.y * pos.y) / 2.0;
  color = vec4(0.234375 * d, 0.015625f * d, 0.425781f * d, 1);
}
