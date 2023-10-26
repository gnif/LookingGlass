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
const float m1inv = 16384.0 / 2610.0;
const float m2inv = 32.0 / 2523.0;
const float c1    = 3424.0 / 4096.0;
const float c2    = 2413.0 / 128.0;
const float c3    = 2392.0 / 128.0;

float minGain(vec3 pixel) { return min(pixel.r, min(pixel.g, pixel.b)); }
float maxGain(vec3 pixel) { return max(pixel.r, max(pixel.g, pixel.b)); }
float midGain(vec3 pixel)
{
  return pixel.r < pixel.g ?
    (pixel.r < pixel.b ?
      min(pixel.g, pixel.b) : // min = r
      min(pixel.r, pixel.g)) : // min = b
    (pixel.g < pixel.b ?
      min(pixel.r, pixel.b) : // min = g
      min(pixel.r, pixel.g)); // min = b
}

vec3 compress(vec3 pixel)
{
  float maxGain = maxGain(pixel);
  return pixel * (maxGain < knee ? maxGain :
      knee + max(maxGain - knee, 0.0) * compressor) / maxGain;
}

vec3 fixClip(vec3 pixel)
{
  // keep the (mid - min) / (max - min) ratio
  float preMin  = minGain(pixel);
  float preMid  = midGain(pixel);
  float preMax  = maxGain(pixel);
  vec3  clip    = clamp(pixel, 0.0, 1.0);
  float postMin = minGain(clip);
  float postMid = midGain(clip);
  float postMax = maxGain(clip);
  float ratio   = (preMid - preMin) / (preMax - preMin);
  float newMid  = ratio * (postMax - postMin) + postMin;
  return vec3(clip.r != postMid ? clip.r : newMid,
                clip.g != postMid ? clip.g : newMid,
                clip.b != postMid ? clip.b : newMid);
}

// Returns luminance in nits
vec3 pq2lin(vec3 pq, float gain)
{
  vec3 p = pow(pq, vec3(m2inv));
  vec3 d = max(p - c1, vec3(0.0)) / (c2 - c3 * p);
  return pow(d, vec3(m1inv)) * gain;
}

vec3 srgb2lin(vec3 c)
{
  vec3 v = c / 12.92;
  vec3 v2 = pow((c + vec3(0.055)) / 1.055, vec3(2.4));
  vec3 threshold = vec3(0.04045);
  vec3 result = mix(v, v2, greaterThanEqual(c, threshold));
  return result;
}

vec3 lin2srgb(vec3 c)
{
  vec3 v = c * 12.92;
  vec3 v2 = pow(c, vec3(1.0/2.4)) * 1.055 - 0.055;
  vec3 threshold = vec3(0.0031308);
  vec3 result = mix(v, v2, greaterThanEqual(c, threshold));
  return result;
}

// in linear space
vec3 bt2020to709(vec3 bt2020)
{
  return vec3(
    bt2020.r *  1.6605 + bt2020.g * -0.5876 + bt2020.b * -0.0728,
    bt2020.r * -0.1246 + bt2020.g *  1.1329 + bt2020.b * -0.0083,
    bt2020.r * -0.0182 + bt2020.g * -0.1006 + bt2020.b * 1.1187);
}

vec3 mapToSDR(vec3 color, float gain, bool pq)
{
  if (pq)
    color = pq2lin(color.rgb, gain);
  color = bt2020to709(color);
  return lin2srgb(compress(color));
}
