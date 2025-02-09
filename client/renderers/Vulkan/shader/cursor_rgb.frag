#version 460
#extension GL_ARB_shading_language_include : enable

#include "color.h"

layout (constant_id = 0) const uint colorSpace = COLOR_SPACE_SRGB;

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput framebuffer;

layout (set = 1, binding = 0) uniform u
{
  mat4 transform;
  float whiteLevel;
};

layout (set = 1, binding = 1) uniform sampler2D sampler1;

layout (location = 0) in vec2 uv;

layout (location = 0) out vec4 color;

void main()
{
  // Read the previously rendered value from the framebuffer. This is encoded
  // according to the configured color space
  vec4 fbPx = subpassLoad(framebuffer);

  // Sample the cursor texture. This is sRGB linear
  vec4 cursorPx = texture(sampler1, uv);

  // Convert the framebuffer value to linear space for blending
  switch (colorSpace)
  {
    case COLOR_SPACE_SRGB:
      fbPx.rgb = srgbToLinear(fbPx.rgb);
      break;

    case COLOR_SPACE_EXTENDED_SRGB_LINEAR:
      // Already linear
      break;

    case COLOR_SPACE_HDR10_ST2084:
      fbPx.rgb = st2084ToLinear(fbPx.rgb);
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
  vec4 blendedPx = vec4(
      cursorPx.rgb * cursorPx.a + fbPx.rgb * (1.0 - cursorPx.a),
      fbPx.a + cursorPx.a);

  // Convert back to the target color space
  switch (colorSpace)
  {
    case COLOR_SPACE_SRGB:
      blendedPx.rgb = linearToSrgb(blendedPx.rgb);
      break;

    case COLOR_SPACE_EXTENDED_SRGB_LINEAR:
      // Output is linear
      break;

    case COLOR_SPACE_HDR10_ST2084:
      blendedPx.rgb = linearToSt2084(blendedPx.rgb);
      break;
  }

  color = blendedPx;
}
