#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D sampler1;
uniform vec2      outputSize;

void main()
{
  uvec2 inputSize = uvec2(textureSize(sampler1, 0));
  uvec2 outputPos = uvec2(fragCoord * outputSize);

  uint fst = outputPos.x * 3u / 4u;
  vec4 color_0 = texelFetch(sampler1, ivec2(fst, outputPos.y), 0);

  uint snd = (outputPos.x * 3u + 1u) / 4u;
  vec4 color_1 = texelFetch(sampler1, ivec2(snd, outputPos.y), 0);

  uint trd = (outputPos.x * 3u + 2u) / 4u;
  vec4 color_2 = texelFetch(sampler1, ivec2(trd, outputPos.y), 0);

  OUTPUT = vec4(
    color_0.barg[outputPos.x % 4u],
    color_1.gbar[outputPos.x % 4u],
    color_2.rgba[outputPos.x % 4u],
    1.0
  );
}
