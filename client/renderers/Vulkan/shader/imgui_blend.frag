#version 460
#extension GL_ARB_shading_language_include : enable

#include "color.h"

layout (constant_id = 0) const uint colorSpace = COLOR_SPACE_SRGB;

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput framebuffer;
layout (input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput imGui;

layout (set = 1, binding = 0) uniform u
{
  float whiteLevel;
};

layout (location = 0) out vec4 color;

void main()
{
  /* We do blending here in non-linear sRGB space. This is not really 'correct'
   * (blending is typically done in linear space), but this is how it has always
   * been done within Looking Glass, so we retain this behavior in order to keep
   * the output visually consistent.
   */

  // If no overlay was rendered, leave the framebuffer unmodified
  vec4 imGuiPx = subpassLoad(imGui);
  if (imGuiPx.a == 0.0)
    discard;

  vec4 fbPx = subpassLoad(framebuffer);

  // Convert to sRGB and limit the brightness to prevent bright HDR content from
  // bleeding through and obscuring the overlay. We use an aggressive
  // exponential curve here to allow bright values through at very low overlay
  // opacities. This is to support fade effects without a jarring transition
  if (colorSpace != COLOR_SPACE_SRGB)
  {
    float maxNits = mix(MAX_HDR_NITS, whiteLevel, pow(imGuiPx.a, 0.02));
    fbPx.rgb = colorSpaceToSrgb(colorSpace, fbPx.rgb, whiteLevel, maxNits);
  }

  // The ImGui rendered output is pre-multiplied alpha, so the source color
  // blend factor here is 1.0 (i.e., omitted)
  vec4 blendedPx = vec4(
      imGuiPx.rgb + fbPx.rgb * (1.0 - imGuiPx.a),
      fbPx.a + imGuiPx.a);

  // Convert back to the target color space
  blendedPx.rgb = srgbToColorSpace(colorSpace, blendedPx.rgb, whiteLevel);

  color = blendedPx;
}
