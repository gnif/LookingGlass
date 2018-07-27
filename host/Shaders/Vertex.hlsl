struct VS
{
  float4 pos : POSITION;
  float2 tex : TEXCOORD; 
};

struct PS
{
  float4 pos : SV_Position;
  float2 tex : TEXCOORD;
};

PS main(VS input)
{
  return input;
}