Texture2D    texTexture;
SamplerState texSampler;

struct VS
{
  float4 pos : SV_Position;
  float2 tex : TEXCOORD;
};

struct OUT
{
  float4 y: SV_TARGET0;
  float4 u: SV_TARGET1;
  float4 v: SV_TARGET2;
};

OUT main(VS input)
{
  OUT o;
  const float4 rgba = texTexture.Sample(texSampler, input.tex);

  o.y.gba = 1.0f;
  o.u.gba = 1.0f;
  o.v.gba = 1.0f;

  o.y.r  = rgba.r * 0.2126 + 0.7152 * rgba.g + 0.0722 * rgba.b;
  o.u.r = ((rgba.b - o.y.r) / 1.8556) + 0.5;
  o.v.r = ((rgba.r - o.y.r) / 1.5748) + 0.5;

  return o;
}