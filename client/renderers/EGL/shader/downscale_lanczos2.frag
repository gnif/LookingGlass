#version 300 es
precision mediump float;

#define PI 3.141592653589793

in  vec2  fragCoord;
out vec4  fragColor;

uniform sampler2D texture;
float sinc(float x)
{
  return x == 0.0 ? 1.0 : sin(x * PI) / (x * PI);
}

float lanczos(float x)
{
  return sinc(x) * sinc(x * 0.5);
}

float lanczos(vec2 v)
{
  return lanczos(v.x) * lanczos(v.y);
}

void main()
{
  vec2 size = vec2(textureSize(texture, 0));
  vec2 pos = fragCoord * size;
  vec2 invSize = 1.0 / size;
  vec2 uvc = floor(pos) + vec2(0.5, 0.5);

  vec2 uvs[9] = vec2[](
    uvc + vec2(-1.0, -1.0),
    uvc + vec2(-1.0,  0.0),
    uvc + vec2(-1.0,  1.0),
    uvc + vec2( 0.0, -1.0),
    uvc + vec2( 0.0,  0.0),
    uvc + vec2( 0.0,  1.0),
    uvc + vec2( 1.0, -1.0),
    uvc + vec2( 1.0,  0.0),
    uvc + vec2( 1.0,  1.0)
  );

  float factors[9];
  float sum = 0.0;
  for (int i = 0; i < 9; ++i)
  {
    factors[i] = lanczos(uvs[i] - fragCoord * size);
    sum += factors[i];
  }

  for (int i = 0; i < 9; ++i)
    factors[i] /= sum;

  vec3 color = vec3(0.0);
  for (int i = 0; i < 9; ++i)
    color += texture2D(texture, uvs[i] * invSize).rgb * factors[i];

  fragColor = vec4(color, 1.0);
}
