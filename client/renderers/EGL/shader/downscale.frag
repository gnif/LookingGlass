#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D sampler1;
uniform vec3      uConfig;

void main()
{
  float pixelSize = uConfig.x;
  float vOffset   = uConfig.y;
  float hOffset   = uConfig.z;

  vec2 inRes  = vec2(textureSize(sampler1, 0));
  ivec2 point = ivec2(
    (floor((fragCoord * inRes) / pixelSize) * pixelSize) +
    pixelSize / 2.0f
  );

  point.x += int(pixelSize * hOffset);
  point.y += int(pixelSize * vOffset);

  fragColor = texelFetch(sampler1, point, 0);
}
