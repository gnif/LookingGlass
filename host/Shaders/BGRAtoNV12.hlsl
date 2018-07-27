Texture2D    texTexture;
SamplerState texSampler;

struct VS
{
  float4 pos : SV_Position;
  float2 tex : TEXCOORD;
};

float4 RGBtoYUV(float4 rgba)
{
  float4 yuva;
  yuva.r = rgba.r * 0.2126 + 0.7152 * rgba.g + 0.0722 * rgba.b;
  yuva.g = (rgba.b - yuva.r) / 1.8556;
  yuva.b = (rgba.r - yuva.r) / 1.5748;
  yuva.a = rgba.a;
  yuva.gb += 0.5;
  return yuva;
}

float4 main(VS input) : SV_TARGET
{
  const float4 rgba = texTexture.Sample(texSampler, input.tex);
  const float4 yuva = RGBtoYUV(rgba);
  return yuva;
}