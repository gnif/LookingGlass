#if __VERSION__ == 300
  vec4 textureGather(sampler2D tex, vec2 uv, int comp)
  {
    vec4 c0 = textureOffset(tex, uv, ivec2(0,1));
    vec4 c1 = textureOffset(tex, uv, ivec2(1,1));
    vec4 c2 = textureOffset(tex, uv, ivec2(1,0));
    vec4 c3 = textureOffset(tex, uv, ivec2(0,0));
    return vec4(c0[comp], c1[comp], c2[comp],c3[comp]);
  }
#elif __VERSION__ < 300
  vec4 textureGather(sampler2D tex, vec2 uv, int comp)
  {
    vec4 c3 = texture2D(tex, uv);
    return vec4(c3[comp], c3[comp], c3[comp],c3[comp]);
  }
#endif

#if __VERSION__ < 310
  uint bitfieldExtract(uint val, int off, int size)
  {
    uint mask = uint((1 << size) - 1);
    return uint(val >> off) & mask;
  }

  uint bitfieldInsert(uint a, uint b, int c, int d)
  {
    uint mask = ~(0xffffffffu << d) << c;
    mask = ~mask;
    a &= mask;
    return a | (b << c);
  }
#endif
