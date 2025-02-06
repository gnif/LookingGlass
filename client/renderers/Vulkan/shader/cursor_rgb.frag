#version 460

const uint COLOR_SPACE_SRGB = 0;
const uint COLOR_SPACE_EXTENDED_SRGB_LINEAR = 1;
const uint COLOR_SPACE_HDR10_ST2084 = 2;

layout (constant_id = 0) const uint colorSpace = COLOR_SPACE_SRGB;

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput framebuffer;

layout (set = 1, binding = 1) uniform sampler2D sampler1;

layout (location = 0) in vec2 uv;

layout (location = 0) out vec4 color;

float srgbToLinear(float value)
{
  const float U = 0.04045;
  const float A = 12.92;
  const float C = 0.055;
  const float G = 2.4;

  if (value <= U)
  {
    return value / A;
  }
  else
  {
    return pow((value + C) / (1.0 + C), G);
  }
}

vec3 srgbToLinear(vec3 value)
{
  return vec3(
    srgbToLinear(value.r),
    srgbToLinear(value.g),
    srgbToLinear(value.b));
}

float linearToSrgb(float value)
{
  const float V = 0.0031308;
  const float A = 12.92;
  const float C = 0.055;
  const float G_INV = 1.0 / 2.4;

  if (value <= V)
  {
    return A * value;
  }
  else
  {
    return (1.0 + C) * pow(value, G_INV) - C;
  }
}

vec3 linearToSrgb(vec3 value)
{
  return vec3(
    linearToSrgb(value.r),
    linearToSrgb(value.g),
    linearToSrgb(value.b));
}

vec3 st2084ToLinear(vec3 value)
{
  // SMPTE ST 2084 perceptual quantizer (PQ) EOTF
  const float M1_INV = 8192.0 / 1305.0;
  const float M2_INV = 32.0 / 2523.0;
  const float C1 = 107.0 / 128.0;
  const float C2 = 2413.0 / 128.0;
  const float C3 = 2392.0 / 128.0;

  vec3 num = max(pow(value, vec3(M2_INV)) - C1, 0.0);
  vec3 den = C2 - C3 * pow(value, vec3(M2_INV));
  vec3 l = pow(num / den, vec3(M1_INV));
  vec3 c = 10000.0 * l;

  return c;
}

vec3 linearToSt2084(vec3 value)
{
  // SMPTE ST 2084 perceptual quantizer (PQ) inverse EOTF
  const float M1 = 1305.0 / 8192.0;
  const float M2 = 2523.0 / 32.0;
  const float C1 = 107.0 / 128.0;
  const float C2 = 2413.0 / 128.0;
  const float C3 = 2392.0 / 128.0;

  vec3 l = value / 10000.0;
  vec3 lM1 = pow(l, vec3(M1));
  vec3 num = C1 + C2 * lM1;
  vec3 den = 1.0 + C3 * lM1;
  vec3 n = pow(num / den, vec3(M2));

  return n;
}

vec3 bt709ToBt2020(vec3 value)
{
  // Matrix values from BT.2087-0
  const mat3 BT709_TO_BT2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956
  );
  return BT709_TO_BT2020 * value;
}

void main()
{
  // TODO: Get this from the LG host
  const float WHITE_LEVEL_NITS = 80.0;

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
      const float REFERENCE_LUMINANCE_NITS = 80.0;
      cursorPx = vec4(
          cursorPx.rgb * WHITE_LEVEL_NITS / REFERENCE_LUMINANCE_NITS,
          cursorPx.a);
      break;

    case COLOR_SPACE_HDR10_ST2084:
      // Convert cursor from BT.709 (sRGB colors) to BT.2020 (HDR10 colors) and
      // adjust to match current white level
      cursorPx = vec4(
          bt709ToBt2020(cursorPx.rgb) * WHITE_LEVEL_NITS,
          cursorPx.a);
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
