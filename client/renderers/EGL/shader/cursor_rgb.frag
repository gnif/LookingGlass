#version 300 es
precision highp float;
precision highp int;

#include "color_blind.h"
#include "hdr.h"

in  vec2  uv;
out vec4  color;

uniform sampler2D sampler1;
uniform sampler2D sampler2;
uniform float     scale;
uniform int       cbMode;
uniform int       sourceTransfer;
uniform float     sdrWhiteLevel;
uniform bool      mapHDRtoSDR;
uniform float     mapHDRGain;
uniform float     mapHDRContentPeak;
uniform uint      colorTransformFlags;
uniform vec4      colorMatrix[3];
uniform float     colorTransformScalar;
uniform bool      linearComposition;

const uint COLOR_TRANSFORM_MATRIX = 1u;
const uint COLOR_TRANSFORM_LUT    = 2u;

vec3 bt709ToXYZ(vec3 rgb)
{
  return vec3(
    dot(rgb, vec3(0.4123908, 0.3575843, 0.1804808)),
    dot(rgb, vec3(0.2126390, 0.7151687, 0.0721923)),
    dot(rgb, vec3(0.0193308, 0.1191948, 0.9505322)));
}

vec3 xyzToBT709(vec3 xyz)
{
  return vec3(
    dot(xyz, vec3( 3.2409699, -1.5373832, -0.4986108)),
    dot(xyz, vec3(-0.9692436,  1.8759675,  0.0415551)),
    dot(xyz, vec3( 0.0556301, -0.2039770,  1.0569715)));
}

vec3 xyzToBT2020(vec3 xyz)
{
  return vec3(
    dot(xyz, vec3( 1.7166512, -0.3556708, -0.2533663)),
    dot(xyz, vec3(-0.6666844,  1.6164812,  0.0157685)),
    dot(xyz, vec3( 0.0176399, -0.0427706,  0.9421031)));
}

vec3 applyColorLUT(vec3 value)
{
  vec3  pos = clamp(value, 0.0, 1.0) * 4095.0;
  ivec3 lo  = ivec3(floor(pos));
  ivec3 hi  = min(lo + ivec3(1), ivec3(4095));
  vec3  f   = fract(pos);
  return vec3(
    mix(texelFetch(sampler2, ivec2(lo.r, 0), 0).r,
        texelFetch(sampler2, ivec2(hi.r, 0), 0).r, f.r),
    mix(texelFetch(sampler2, ivec2(lo.g, 0), 0).g,
        texelFetch(sampler2, ivec2(hi.g, 0), 0).g, f.g),
    mix(texelFetch(sampler2, ivec2(lo.b, 0), 0).b,
        texelFetch(sampler2, ivec2(hi.b, 0), 0).b, f.b));
}

void main()
{
  if (scale > 1.0)
  {
    vec2 ts = vec2(textureSize(sampler1, 0));
    vec2 px = (uv - (0.5 / ts)) * ts;
    if (px.x < 0.0 || px.y < 0.0)
      discard;

    color = texelFetch(sampler1, ivec2(px), 0);
  }
  else
    color = texture(sampler1, uv);

  if (color.a > 0.0 &&
      (sourceTransfer != TRANSFER_SRGB ||
       colorTransformFlags != 0u || cbMode > 0))
  {
    // Cursor pixels are premultiplied sRGB. Work on straight, linear BT.709
    // values through the XYZ calibration stage, encode for the active wire
    // format, apply its 1D LUT, then restore premultiplication.
    vec3 value = srgb2lin(clamp(color.rgb / color.a, 0.0, 1.0));
    if ((colorTransformFlags & COLOR_TRANSFORM_MATRIX) != 0u)
    {
      vec3 xyz = bt709ToXYZ(value);
      xyz = vec3(
        dot(vec4(xyz, 1.0), colorMatrix[0]),
        dot(vec4(xyz, 1.0), colorMatrix[1]),
        dot(vec4(xyz, 1.0), colorMatrix[2])) * colorTransformScalar;
      value = sourceTransfer == TRANSFER_PQ ?
        xyzToBT2020(xyz) : xyzToBT709(xyz);
    }
    else if (sourceTransfer == TRANSFER_PQ)
      value = bt709to2020(value);

    if (sourceTransfer == TRANSFER_PQ)
      value = lin2pq(max(value, 0.0) * (sdrWhiteLevel / 10000.0));
    else if (sourceTransfer == TRANSFER_SCRGB)
      value *= sdrWhiteLevel / 80.0;
    else
      value = lin2srgb(max(value, 0.0));

    if ((colorTransformFlags & COLOR_TRANSFORM_LUT) != 0u)
      value = applyColorLUT(value);

    if (mapHDRtoSDR)
      value = mapToSDR(value, mapHDRGain,
          mapHDRContentPeak, sourceTransfer == TRANSFER_PQ);
    // Native PQ surfaces are composed through a linear scRGB framebuffer.
    // Preserve the guest's transfer-domain LUT by decoding it only after the
    // complete guest transform has been applied.
    else if (linearComposition && sourceTransfer == TRANSFER_PQ)
      value = bt2020to709(pq2lin(value, 1.0)) * 125.0;

    // Apply SDR-only accessibility transforms after HDR-to-SDR mapping and
    // to straight colour so translucent cursor edges remain premultiplied
    // correctly.
    if (cbMode > 0)
      value = cbTransform(vec4(value, 1.0), cbMode).rgb;
    color.rgb = value * color.a;
  }
  else if (color.a == 0.0)
    color.rgb = vec3(0.0);
}
