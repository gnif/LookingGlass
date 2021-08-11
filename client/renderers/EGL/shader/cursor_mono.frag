#version 300 es
precision mediump float;

in  vec2 uv;
out vec4 color;

uniform sampler2D sampler1;

void main()
{
  vec4 tmp = texture(sampler1, uv);
  if (tmp.rgb == vec3(0.0, 0.0, 0.0))
    discard;
  color = tmp;
}
