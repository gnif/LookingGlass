#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D sampler1;
uniform vec2      outputSize;

void main()
{
  uvec2 inputSize   = uvec2(textureSize(sampler1, 0));
  uvec2 pos         = uvec2(fragCoord * outputSize);
  uint  outputWidth = uint(outputSize.x);

  uint output_idx = pos.y * outputWidth + pos.x;

  uint fst = output_idx * 3u / 4u;
  vec4 color_0 = texelFetch(sampler1, ivec2(fst % inputSize.x, fst / inputSize.x), 0);

  uint snd = (output_idx * 3u + 1u) / 4u;
  vec4 color_1 = texelFetch(sampler1, ivec2(snd % inputSize.x, snd / inputSize.x), 0);

  uint trd = (output_idx * 3u + 2u) / 4u;
  vec4 color_2 = texelFetch(sampler1, ivec2(trd % inputSize.x, trd / inputSize.x), 0);

  fragColor.bgra = vec4(
    color_0.barg[output_idx % 4u],
    color_1.gbar[output_idx % 4u],
    color_2.rgba[output_idx % 4u],
    1.0
  );
}
