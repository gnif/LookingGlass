/*
   _______                               _____ __              __             ____             __
  / ____(_)___  ___  ____ ___  ____ _   / ___// /_  ____ _____/ /__  _____   / __ \____ ______/ /__
 / /   / / __ \/ _ \/ __ `__ \/ __ `/   \__ \/ __ \/ __ `/ __  / _ \/ ___/  / /_/ / __ `/ ___/ //_/
/ /___/ / / / /  __/ / / / / / /_/ /   ___/ / / / / /_/ / /_/ /  __/ /     / ____/ /_/ / /__/ ,<
\____/_/_/ /_/\___/_/ /_/ /_/\__,_/   /____/_/ /_/\__,_/\__,_/\___/_/     /_/    \__,_/\___/_/|_|
        http://en.sbence.hu/        Shader: Try to get the SDR part of HDR content
*/

/**
 * Translated to GLSL, original source is:
 * https://github.com/VoidXH/Cinema-Shader-Pack
 */

// Configuration ---------------------------------------------------------------
const float knee          = 0.75;    // Compressor knee position
const float ratio         = 4.0;     // Compressor ratio: 1 = disabled, <1 = expander
// -----------------------------------------------------------------------------

// Precalculated values
const float compressor = 1.0 / ratio;

// PQ constants
const float m1    = 2610.0 / 16384.0;
const float m2    = 2523.0 / 32.0;
const float m1inv = 16384.0 / 2610.0;
const float m2inv = 32.0 / 2523.0;
const float c1    = 3424.0 / 4096.0;
const float c2    = 2413.0 / 128.0;
const float c3    = 2392.0 / 128.0;

const int TRANSFER_SRGB  = 0;
const int TRANSFER_SCRGB = 1;
const int TRANSFER_PQ    = 2;

vec3 compress(vec3 pixel)
{
  pixel = max(pixel, 0.0);
  float peak = max(pixel.r, max(pixel.g, pixel.b));
  if (peak <= 0.0)
    return vec3(0.0);
  return pixel * (peak < knee ? peak :
      knee + (peak - knee) * compressor) / peak;
}

// Returns luminance in nits
vec3 pq2lin(vec3 pq, float gain)
{
  vec3 p = pow(max(pq, 0.0), vec3(m2inv));
  vec3 d = max(p - c1, vec3(0.0)) / (c2 - c3 * p);
  return pow(d, vec3(m1inv)) * gain;
}

vec3 lin2pq(vec3 linear)
{
  // ST.2084 uses absolute luminance normalized to 10000 cd/m².
  vec3 p = pow(max(linear, 0.0), vec3(m1));
  return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

vec3 srgb2lin(vec3 c)
{
  vec3 v         = c / 12.92;
  vec3 v2        = pow((c + vec3(0.055)) / 1.055, vec3(2.4));
  vec3 threshold = vec3(0.04045);
  vec3 result    = mix(v, v2, greaterThanEqual(c, threshold));
  return result;
}

vec3 lin2srgb(vec3 c)
{
  vec3 v         = c * 12.92;
  vec3 v2        = pow(max(c, 0.0), vec3(1.0/2.4)) * 1.055 - 0.055;
  vec3 threshold = vec3(0.0031308);
  vec3 result    = mix(v, v2, greaterThanEqual(c, threshold));
  return result;
}

// in linear space
vec3 bt2020to709(vec3 bt2020)
{
  return vec3(
    bt2020.r *  1.6604910 + bt2020.g * -0.5876411 + bt2020.b * -0.0728499,
    bt2020.r * -0.1245505 + bt2020.g *  1.1328999 + bt2020.b * -0.0083494,
    bt2020.r * -0.0181508 + bt2020.g * -0.1005789 + bt2020.b * 1.1187297);
}

// in linear space
vec3 bt709to2020(vec3 bt709)
{
  return vec3(
    bt709.r * 0.6274039 + bt709.g * 0.3292830 + bt709.b * 0.0433131,
    bt709.r * 0.0690973 + bt709.g * 0.9195404 + bt709.b * 0.0113623,
    bt709.r * 0.0163914 + bt709.g * 0.0880133 + bt709.b * 0.8955953);
}

vec3 mapToSDR(vec3 color, float gain, float contentPeak, bool pq)
{
  if (pq)
  {
    // HDR10: PQ-encoded BT.2020. Decode the absolute 10000-nit PQ range into
    // values relative to the configured SDR display peak. MaxCLL constrains
    // content luminance without changing that absolute unit conversion.
    color = pq2lin(color.rgb, gain);
    float luminance2020 =
      dot(color, vec3(0.2627002, 0.6779981, 0.0593017));
    if (contentPeak > 0.0 && luminance2020 > contentPeak)
      color *= contentPeak / luminance2020;
    color = bt2020to709(color);
  }
  else
  {
    // scRGB is linear BT.709 with 1.0 fixed at 80 nits. Use the same
    // absolute-luminance mapping as PQ before compressing the highlights.
    color *= gain;
    float luminance709 = dot(color, vec3(0.2126390, 0.7151687, 0.0721923));
    if (contentPeak > 0.0 && luminance709 > contentPeak)
      color *= contentPeak / luminance709;
  }
  return lin2srgb(clamp(compress(color), 0.0, 1.0));
}
