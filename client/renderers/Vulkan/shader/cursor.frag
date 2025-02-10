#version 460
#extension GL_ARB_shading_language_include : enable

#include "color.h"

const uint CURSOR_TYPE_COLOR = 0;
const uint CURSOR_TYPE_MONOCHROME = 1;
const uint CURSOR_TYPE_MASKED_COLOR = 2;

layout (constant_id = 0) const uint colorSpace = COLOR_SPACE_SRGB;
layout (constant_id = 1) const uint cursorType = CURSOR_TYPE_COLOR;

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput framebuffer;

layout (set = 1, binding = 0) uniform u
{
  mat4 transform;
  float whiteLevel;
};

layout (set = 1, binding = 1) uniform sampler2D sampler1;

layout (location = 0) in vec2 uv;

layout (location = 0) out vec4 color;

vec4 applyXor(vec4 outPx, uvec3 cursorPxInt)
{
  // Bail out early if the XOR values are all zero, which is logically a no-op,
  // but because of the sRGB conversion below, would clamp HDR content to the
  // SDR white level
  if (all(equal(cursorPxInt, uvec3(0))))
    return outPx;

  // Windows applies the XOR operation to the non-linear sRGB pixel values when
  // in SDR, but the HDR behavior is unclear, and at times nonsensical. For
  // instance, #fffffe is XOR'd to blue, but #fffffd is XOR'd to magenta.
  // Lacking a better approach for HDR, just always do the operation in sRGB so
  // the result looks the same as SDR
  float maxNits = whiteLevel;
  outPx.rgb = colorSpaceToSrgb(colorSpace, outPx.rgb, whiteLevel, maxNits);
  uvec3 outPxInt = uvec3(round(outPx.rgb * 255.0));
  outPxInt ^= cursorPxInt;
  outPx.rgb = vec3(outPxInt) / 255.0;
  outPx.rgb = srgbToColorSpace(colorSpace, outPx.rgb, whiteLevel);
  return outPx;
}

vec4 blendColor(vec4 outPx, vec4 cursorPx)
{
  // Convert to linear space for blending
  cursorPx.rgb = srgbToLinear(cursorPx.rgb);
  switch (colorSpace)
  {
    case COLOR_SPACE_SRGB:
      outPx.rgb = srgbToLinear(outPx.rgb);
      break;

    case COLOR_SPACE_EXTENDED_SRGB_LINEAR:
      // Already linear
      break;

    case COLOR_SPACE_HDR10_ST2084:
      outPx.rgb = st2084ToLinear(outPx.rgb);
      break;
  }

  // Convert the cursor to the target colorimetry
  switch (colorSpace)
  {
    case COLOR_SPACE_SRGB:
      // Both BT.709. SDR so white level does not apply
      break;

    case COLOR_SPACE_EXTENDED_SRGB_LINEAR:
      // Both BT.709. Adjust cursor to match current white level
      cursorPx = vec4(
          cursorPx.rgb * whiteLevel / REFERENCE_LUMINANCE_NITS,
          cursorPx.a);
      break;

    case COLOR_SPACE_HDR10_ST2084:
      // Convert cursor from BT.709 (sRGB colors) to BT.2020 (HDR10 colors) and
      // adjust to match current white level
      cursorPx = vec4(bt709ToBt2020(cursorPx.rgb) * whiteLevel, cursorPx.a);
      break;
  }

  // Do the blend
  outPx = vec4(
      cursorPx.rgb * cursorPx.a + outPx.rgb * (1.0 - cursorPx.a),
      outPx.a + cursorPx.a);

  // Convert back to the target color space
  switch (colorSpace)
  {
    case COLOR_SPACE_SRGB:
      outPx.rgb = linearToSrgb(outPx.rgb);
      break;

    case COLOR_SPACE_EXTENDED_SRGB_LINEAR:
      // Output is linear
      break;

    case COLOR_SPACE_HDR10_ST2084:
      outPx.rgb = linearToSt2084(outPx.rgb);
      break;
  }

  return outPx;
}

void main()
{
  // Read the previously rendered value from the framebuffer. This is encoded
  // according to the configured color space
  vec4 fbPx = subpassLoad(framebuffer);

  // Sample the cursor texture. This is interpreted according to the configured
  // cursor type. Colors are sRGB non-linear
  vec4 cursorPx = texture(sampler1, uv);

  vec4 outPx = fbPx;
  switch (cursorType)
  {
    case CURSOR_TYPE_COLOR:
      outPx = blendColor(outPx, cursorPx);
      break;

    case CURSOR_TYPE_MONOCHROME:
    {
      float andPx = cursorPx.r;
      float xorPx = cursorPx.a;
      if (andPx >= 0.5)
        outPx = vec4(0.0, 0.0, 0.0, 1.0);
      if (xorPx >= 0.5)
        outPx = applyXor(outPx, uvec3(0xff));
      break;
    }

    case CURSOR_TYPE_MASKED_COLOR:
    {
      float xorPx = cursorPx.a;
      if (xorPx < 0.5)
        outPx = applyXor(outPx, uvec3(round(cursorPx.rgb * 255.0)));
      else
        outPx = blendColor(outPx, vec4(cursorPx.rgb, 1.0));
    }
  }

  color = outPx;
}
