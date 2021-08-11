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
