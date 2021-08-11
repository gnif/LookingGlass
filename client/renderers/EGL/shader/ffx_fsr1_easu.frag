#version 300 es
precision mediump float;

#include "compat.h"

in  vec2  iFragCoord;
out vec4  fragColor;

uniform sampler2D iChannel0;
uniform uvec2     uInRes[8];
uniform uvec2     uOutRes;

#define A_GPU  1
#define A_GLSL 1
#define A_FULL 1

#include "ffx_a.h"

#define FSR_EASU_F 1

#if __VERSION__ <= 310
  vec4 textureGather(sampler2D tex, vec2 uv, int comp)
  {
    ivec2 p = ivec2((uv * vec2(uInRes[0])) - 0.5f);
    vec4 c0 = texelFetchOffset(tex, p, 0, ivec2(0,1));
    vec4 c1 = texelFetchOffset(tex, p, 0, ivec2(1,1));
    vec4 c2 = texelFetchOffset(tex, p, 0, ivec2(1,0));
    vec4 c3 = texelFetchOffset(tex, p, 0, ivec2(0,0));
    return vec4(c0[comp], c1[comp], c2[comp], c3[comp]);
  }
#endif

AF4 FsrEasuRF(AF2 p){return AF4(textureGather(iChannel0, p, 0));}
AF4 FsrEasuGF(AF2 p){return AF4(textureGather(iChannel0, p, 1));}
AF4 FsrEasuBF(AF2 p){return AF4(textureGather(iChannel0, p, 2));}

#include "ffx_fsr1.h"

void main()
{
  AU4 con0, con1, con2, con3;
  vec2 inRes  = vec2(uInRes[0]);
  vec2 outRes = vec2(uOutRes);

  FsrEasuCon(
    con0,
    con1,
    con2,
    con3,
    inRes.x , inRes.y,
    inRes.x , inRes.y,
    outRes.x, outRes.y
  );

  vec3 color;
  uvec2 point = uvec2(iFragCoord * outRes);
  FsrEasuF(color, point, con0, con1, con2, con3);
  fragColor = vec4(color.xyz, 1);
}
