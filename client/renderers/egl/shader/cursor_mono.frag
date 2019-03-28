#version 300 es

in  highp vec2 uv;
out highp vec4 color;

uniform sampler2D sampler1;

void main()
{
  highp vec4 tmp = texture(sampler1, uv);
  if (tmp.rgb == vec3(0.0, 0.0, 0.0))
    discard;
  color = tmp;
}
