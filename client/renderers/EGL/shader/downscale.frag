#version 300 es
precision mediump float;

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D texture;
uniform vec3      uConfig;

void main()
{
  float pixelSize = uConfig.x;
  float vOffset   = uConfig.y;
  float hOffset   = uConfig.z;

  vec2 inRes  = vec2(textureSize(texture, 0));
  ivec2 point = ivec2(
    (floor((fragCoord * inRes) / pixelSize) * pixelSize) +
    pixelSize / 2.0f
  );

  point.x += int(pixelSize * hOffset);
  point.y += int(pixelSize * vOffset);

  fragColor = texelFetch(texture, point, 0);
}
