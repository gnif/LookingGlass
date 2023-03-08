#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D sampler1;

void main()
{
  fragColor = texture(sampler1, fragCoord);
}
