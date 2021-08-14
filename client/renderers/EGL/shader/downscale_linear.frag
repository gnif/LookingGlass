#version 300 es
precision mediump float;

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D texture;

void main()
{
  fragColor = texture2D(texture, fragCoord);
}
