Texture2D    texTexture;
SamplerState texSampler;

struct VS
{
  float4 pos : SV_Position;
  float2 tex : TEXCOORD;
};

float4 main(VS input): SV_Target
{
  return texTexture.Sample(texSampler, input.tex);
}