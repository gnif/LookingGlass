/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * The coefficient and inverse-wavelet shaders are OpenGL ES adaptations of
 * PyroWave's Vulkan shaders at commit
 * 89f7e47d4abbf650c91fae766728af866c5e32a0.
 *
 * Copyright (c) 2025 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

static const char NETWORK_VIDEO_DEQUANT_SHADER_0[] =
  "#version 310 es\n"
  "precision highp float;\n"
  "precision highp int;\n"
  "layout(local_size_x=128) in;\n"
  "layout(r32f,binding=0) writeonly uniform highp image2DArray outputImage;\n"
  "layout(std430,binding=0) readonly buffer OffsetBuffer { uint offsets[]; };\n"
  "layout(std430,binding=1) readonly buffer PayloadBuffer { uint payload[]; };\n"
  "uniform ivec2 resolution;\n"
  "uniform int outputLayer;\n"
  "uniform int blockOffset;\n"
  "uniform int blockStride;\n"
  "shared uint signOffset;\n"
  "shared uint planeOffset[16];\n"
  "shared uint significantPrefix[128];\n"
  "uint readU8(uint byteOffset) {\n"
  "  return (payload[byteOffset >> 2u] >> (8u * (byteOffset & 3u))) & 255u;\n"
  "}\n"
  "uint readU16(uint byteOffset) {\n"
  "  return readU8(byteOffset) | (readU8(byteOffset + 1u) << 8u);\n"
  "}\n"
  "ivec2 unswizzle8x8(uint index) {\n"
  "  uint y = bitfieldExtract(index, 0, 1);\n"
  "  uint x = bitfieldExtract(index, 1, 2);\n"
  "  y |= bitfieldExtract(index, 3, 2) << 1;\n"
  "  x |= bitfieldExtract(index, 5, 1) << 2;\n"
  "  return ivec2(x, y);\n"
  "}\n"
  "float quantScale(uint code) { return float(code) / 8.0 + 0.25; }\n"
  "float blockScale(uint code) {\n"
  "  int exponent = 4 - int(code >> 3u);\n"
  "  return (float(8u + (code & 7u)) / 8.0) * exp2(float(exponent));\n"
  "}\n"
  "void storeValues(ivec2 coord, mat2x4 values) {\n"
  "  for (int y=0; y<2; ++y) for (int x=0; x<4; ++x) {\n"
  "    ivec2 target = coord + ivec2(x,y);\n"
  "    if (all(lessThan(target,resolution)))\n"
  "      imageStore(outputImage,ivec3(target,outputLayer),vec4(values[y][x]));\n"
  "  }\n"
  "}\n"
  "void main() {\n"
  "  uint localIndex = gl_LocalInvocationIndex;\n"
  "  int blockIndex = blockOffset + int(gl_WorkGroupID.y) * blockStride +\n"
  "    int(gl_WorkGroupID.x);\n"
  "  uint sourceWord = offsets[blockIndex];\n"
  "  uint subblock = bitfieldExtract(localIndex,0,3);\n"
  "  uint blockX = bitfieldExtract(localIndex,3,2);\n"
  "  uint blockY = bitfieldExtract(localIndex,5,2);\n"
  "  uint group = blockY * 4u + blockX;\n"
  "  ivec2 coord = ivec2(gl_WorkGroupID.xy) * 32 +\n"
  "    8 * ivec2(blockX,blockY) + unswizzle8x8(subblock << 3u);\n"
  "  if (sourceWord == 0xffffffffu) {\n"
  "    storeValues(coord,mat2x4(vec4(0.0),vec4(0.0)));\n"
  "    return;\n"
  "  }\n"
  "  uint sourceByte = sourceWord * 4u;\n"
  "  uint ballot = payload[sourceWord] & 65535u;\n"
  "  if (localIndex == 0u) {\n"
  "    uint cursor = sourceByte + 8u + 3u * uint(bitCount(ballot));\n"
  "    for (uint index=0u; index<16u; ++index) {\n"
  "      planeOffset[index] = cursor;\n"
  "      if (bitfieldExtract(ballot,int(index),1) != 0u) {\n"
  "        uint compact = uint(bitCount(bitfieldExtract(ballot,0,int(index))));\n"
  "        uint control = readU16(sourceByte + 8u + 2u * compact);\n"
  "        uint qbits = readU8(sourceByte + 8u +\n"
  "          2u * uint(bitCount(ballot)) + compact) & 15u;\n"
  "        uint low = control & 0x5555u;\n"
  "        uint high = control & 0xaaaau;\n"
  "        high |= high >> 1u;\n"
  "        cursor += uint(bitCount(low) + bitCount(high)) + 8u * qbits;\n"
  "      }\n"
  "    }\n"
  "    signOffset = cursor * 8u;\n"
  "  }\n"
  "  barrier();\n";

static const char NETWORK_VIDEO_DEQUANT_SHADER_1[] =
  "  mat2x4 values = mat2x4(vec4(0.0),vec4(0.0));\n"
  "  uint significant = 0u;\n"
  "  if (bitfieldExtract(ballot,int(group),1) != 0u) {\n"
  "    uint compact = uint(bitCount(bitfieldExtract(ballot,0,int(group))));\n"
  "    uint control = readU16(sourceByte + 8u + 2u * compact);\n"
  "    uint scaleCode = readU8(sourceByte + 8u +\n"
  "      2u * uint(bitCount(ballot)) + compact);\n"
  "    uint planes = (scaleCode & 15u) +\n"
  "      bitfieldExtract(control,int(2u * subblock),2);\n"
  "    int magnitude[8];\n"
  "    for (int i=0; i<8; ++i) magnitude[i]=0;\n"
  "    uint cursor = planeOffset[group];\n"
  "    for (int plane=int(planes)-1; plane>=0; --plane) {\n"
  "      uint bits = readU8(cursor++);\n"
  "      for (int i=0; i<8; ++i)\n"
  "        magnitude[i] = (magnitude[i] << 1) | int((bits >> uint(i)) & 1u);\n"
  "    }\n"
  "    float scale = blockScale(payload[sourceWord+1u] & 255u) *\n"
  "      quantScale(scaleCode >> 4u);\n"
  "    for (int x=0; x<4; ++x) for (int y=0; y<2; ++y) {\n"
  "      int index = x * 2 + y;\n"
  "      float value = float(magnitude[index]);\n"
  "      if (value != 0.0) { value += 0.5; ++significant; }\n"
  "      values[y][x] = value * scale;\n"
  "    }\n"
  "  }\n"
  "  significantPrefix[localIndex] = significant;\n"
  "  barrier();\n"
  "  if (localIndex == 0u) {\n"
  "    uint prefix = 0u;\n"
  "    for (uint index=0u; index<128u; ++index) {\n"
  "      uint count = significantPrefix[index];\n"
  "      significantPrefix[index] = prefix;\n"
  "      prefix += count;\n"
  "    }\n"
  "  }\n"
  "  barrier();\n"
  "  uint bitCursor = signOffset + significantPrefix[localIndex];\n"
  "  uint word = payload[bitCursor >> 5u];\n"
  "  uint upper = payload[(bitCursor >> 5u) + 1u];\n"
  "  uint shift = bitCursor & 31u;\n"
  "  if (shift != 0u) word = (word >> shift) | (upper << (32u-shift));\n"
  "  uint signIndex = 0u;\n"
  "  for (int x=0; x<4; ++x) for (int y=0; y<2; ++y) {\n"
  "    if (values[y][x] != 0.0) {\n"
  "      if (((word >> signIndex) & 1u) != 0u) values[y][x] = -values[y][x];\n"
  "      ++signIndex;\n"
  "    }\n"
  "  }\n"
  "  storeValues(coord,values);\n"
  "}\n";

static const char NETWORK_VIDEO_IDWT_SHADER_0[] =
  "#version 310 es\n"
  "precision highp float;\n"
  "precision highp int;\n"
  "layout(local_size_x=64) in;\n"
  "layout(binding=0) uniform highp sampler2DArray coefficients;\n"
  "layout(r32f,binding=1) writeonly uniform highp image2D outputImage;\n"
  "uniform ivec2 resolution;\n"
  "uniform vec2 invResolution;\n"
  "uniform int inputLevel;\n"
  "uniform int baseLayer;\n"
  "uniform bool dcShift;\n"
  "const int APRON=4, HALF_APRON=2, BLOCK=32, HALF_BLOCK=16;\n"
  "const float ALPHA=-1.586134342059924;\n"
  "const float BETA=-0.052980118572961;\n"
  "const float GAMMA=0.882911075530934;\n"
  "const float DELTA=0.443506852043971;\n"
  "const float K=1.230174104914001;\n"
  "const float INV_K=1.0/1.230174104914001;\n"
  "shared vec2 sharedBlock[20][41];\n"
  "ivec2 unswizzle8x8(uint index) {\n"
  "  uint y=bitfieldExtract(index,0,1), x=bitfieldExtract(index,1,2);\n"
  "  y|=bitfieldExtract(index,3,2)<<1; x|=bitfieldExtract(index,5,1)<<2;\n"
  "  return ivec2(x,y);\n"
  "}\n"
  "bvec2 and2(bvec2 a,bvec2 b) { return bvec2(a.x&&b.x,a.y&&b.y); }\n"
  "vec2 mirrorUV(ivec2 coord,bool evenX,bool evenY) {\n"
  "  coord-=ivec2(and2(bvec2(evenX,evenY),lessThan(coord,ivec2(0))));\n"
  "  coord+=1;\n"
  "  coord+=ivec2(and2(bvec2(!evenX,!evenY),greaterThanEqual(coord,resolution)));\n"
  "  return (vec2(coord)*invResolution).yx;\n"
  "}\n"
  "vec4 gatherBand(ivec2 coord,bool evenX,bool evenY,int band) {\n"
  "  return textureGather(coefficients,vec3(mirrorUV(coord,evenX,evenY),\n"
  "    float(baseLayer+band)),0);\n"
  "}\n"
  "void store4(ivec2 coord,vec4 a,vec4 b,vec4 c,vec4 d) {\n"
  "  sharedBlock[coord.y][2*coord.x+0]=vec2(a.x,c.x);\n"
  "  sharedBlock[coord.y][2*coord.x+1]=vec2(b.x,d.x);\n"
  "  sharedBlock[coord.y][2*coord.x+2]=vec2(a.y,c.y);\n"
  "  sharedBlock[coord.y][2*coord.x+3]=vec2(b.y,d.y);\n"
  "  sharedBlock[coord.y+1][2*coord.x+0]=vec2(a.z,c.z);\n"
  "  sharedBlock[coord.y+1][2*coord.x+1]=vec2(b.z,d.z);\n"
  "  sharedBlock[coord.y+1][2*coord.x+2]=vec2(a.w,c.w);\n"
  "  sharedBlock[coord.y+1][2*coord.x+3]=vec2(b.w,d.w);\n"
  "}\n"
  "void loadApron(uint index) {\n"
  "  ivec2 base=ivec2(gl_WorkGroupID.xy)*HALF_BLOCK-HALF_APRON;\n"
  "  ivec2 local=2*unswizzle8x8(index); ivec2 coord=base+local;\n"
  "  store4(local,gatherBand(coord,true,true,0).wxzy,\n"
  "    gatherBand(coord,false,true,2).wxzy,\n"
  "    gatherBand(coord,true,false,1).wxzy,\n"
  "    gatherBand(coord,false,false,3).wxzy);\n"
  "  ivec2 horizontal=ivec2(HALF_BLOCK+2*int(index%2u),2*int(index/2u));\n"
  "  if (horizontal.y<HALF_BLOCK+2*HALF_APRON) { coord=base+horizontal;\n"
  "    store4(horizontal,gatherBand(coord,true,true,0).wxzy,\n"
  "      gatherBand(coord,false,true,2).wxzy,\n"
  "      gatherBand(coord,true,false,1).wxzy,\n"
  "      gatherBand(coord,false,false,3).wxzy); }\n"
  "  ivec2 vertical=horizontal.yx;\n"
  "  if (vertical.x<HALF_BLOCK) { coord=base+vertical;\n"
  "    store4(vertical,gatherBand(coord,true,true,0).wxzy,\n"
  "      gatherBand(coord,false,true,2).wxzy,\n"
  "      gatherBand(coord,true,false,1).wxzy,\n"
  "      gatherBand(coord,false,false,3).wxzy); }\n"
  "  barrier();\n"
  "}\n";

static const char NETWORK_VIDEO_IDWT_SHADER_1[] =
  "void inverse8x2(uint index) {\n"
  "  vec2 values[16]; ivec2 local=ivec2(8*int(index%4u),int(index/4u));\n"
  "  for(int i=0;i<16;i+=2){values[i]=sharedBlock[local.y][local.x+i]*K;\n"
  "    values[i+1]=sharedBlock[local.y][local.x+i+1]*INV_K;}\n"
  "  for(int i=2;i<15;i+=2)values[i]-=DELTA*(values[i-1]+values[i+1]);\n"
  "  for(int i=3;i<14;i+=2)values[i]-=GAMMA*(values[i-1]+values[i+1]);\n"
  "  for(int i=4;i<13;i+=2)values[i]-=BETA*(values[i-1]+values[i+1]);\n"
  "  for(int i=5;i<12;i+=2)values[i]-=ALPHA*(values[i-1]+values[i+1]);\n"
  "  barrier();\n"
  "  for(int i=2;i<6;++i){vec2 a=values[2*i],b=values[2*i+1];\n"
  "    int y=(local.x>>1)+(i-2); sharedBlock[y][2*local.y]=vec2(a.x,b.x);\n"
  "    sharedBlock[y][2*local.y+1]=vec2(a.y,b.y);}\n"
  "}\n"
  "void inverse4x2(uint index,bool activeLane,int yOffset) {\n"
  "  vec2 values[12]; ivec2 local=ivec2(4*int(index%8u),int(index/8u)+yOffset);\n"
  "  if(activeLane){for(int i=0;i<12;i+=2){values[i]=sharedBlock[local.y][local.x+i]*K;\n"
  "    values[i+1]=sharedBlock[local.y][local.x+i+1]*INV_K;}\n"
  "    for(int i=2;i<11;i+=2)values[i]-=DELTA*(values[i-1]+values[i+1]);\n"
  "    for(int i=3;i<10;i+=2)values[i]-=GAMMA*(values[i-1]+values[i+1]);\n"
  "    for(int i=4;i<9;i+=2)values[i]-=BETA*(values[i-1]+values[i+1]);\n"
  "    for(int i=5;i<8;i+=2)values[i]-=ALPHA*(values[i-1]+values[i+1]);}\n"
  "  barrier();\n"
  "  if(activeLane)for(int i=2;i<4;++i){vec2 a=values[2*i],b=values[2*i+1];\n"
  "    int y=(local.x>>1)+(i-2); sharedBlock[y][2*local.y]=vec2(a.x,b.x);\n"
  "    sharedBlock[y][2*local.y+1]=vec2(a.y,b.y);}\n"
  "}\n"
  "void main(){uint index=gl_LocalInvocationIndex;loadApron(index);\n"
  "  inverse8x2(index);inverse4x2(index,index<32u,HALF_BLOCK);barrier();\n"
  "  inverse8x2(index);barrier();ivec2 local=unswizzle8x8(index);\n"
  "  ivec2 outputSize=imageSize(outputImage);\n"
  "  for(int y=local.y;y<HALF_BLOCK;y+=8)for(int x=local.x;x<BLOCK;x+=8){\n"
  "    vec2 value=sharedBlock[y][x];if(dcShift)value+=vec2(0.5);\n"
  "    ivec2 a=ivec2(2*y,x)+BLOCK*ivec2(gl_WorkGroupID.yx);\n"
  "    ivec2 b=a+ivec2(1,0);if(all(lessThan(a,outputSize)))\n"
  "      imageStore(outputImage,a,vec4(value.x));\n"
  "    if(all(lessThan(b,outputSize)))imageStore(outputImage,b,vec4(value.y));}\n"
  "}\n";

static const char NETWORK_VIDEO_YUV_SHADER[] =
  "#version 310 es\n"
  "precision highp float;\n"
  "precision highp int;\n"
  "layout(local_size_x=8,local_size_y=8) in;\n"
  "layout(binding=0) uniform highp sampler2D lumaPlane;\n"
  "layout(binding=1) uniform highp sampler2D cbPlane;\n"
  "layout(binding=2) uniform highp sampler2D crPlane;\n"
  "layout(rgba16f,binding=0) writeonly uniform highp image2D outputImage;\n"
  "uniform ivec2 outputSize;\n"
  "uniform bool bt2020;\n"
  "uniform bool chromaLeft;\n"
  "uniform float lumaOffset;\n"
  "uniform float lumaScale;\n"
  "uniform float chromaMidpoint;\n"
  "uniform float chromaScale;\n"
  "void main(){ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);\n"
  "  if(any(greaterThanEqual(pixel,outputSize)))return;\n"
  "  vec2 uv=(vec2(pixel)+0.5)/vec2(outputSize);\n"
  "  vec2 offset=chromaLeft?vec2(0.5/float(outputSize.x),0.0):vec2(0.0);\n"
  "  float y=clamp((texture(lumaPlane,uv).x-lumaOffset)*lumaScale,0.0,1.0);\n"
  "  float cb=clamp((texture(cbPlane,uv+offset).x-chromaMidpoint)*\n"
  "    chromaScale,-0.5,0.5);\n"
  "  float cr=clamp((texture(crPlane,uv+offset).x-chromaMidpoint)*\n"
  "    chromaScale,-0.5,0.5);\n"
  "  mat3 m=bt2020?mat3(vec3(1.0),vec3(0.0,-0.16455313,1.8814),\n"
  "    vec3(1.4746,-0.57135313,0.0)):\n"
  "    mat3(vec3(1.0),vec3(0.0,-0.13397432/0.7152,1.8556),\n"
  "    vec3(1.5748,-0.33480248/0.7152,0.0));\n"
  "  vec3 rgb=clamp(m*vec3(y,cb,cr),vec3(0.0),vec3(1.0));\n"
  "  imageStore(outputImage,pixel,vec4(rgb,1.0));}\n";

static const char * const NETWORK_VIDEO_DEQUANT_SOURCES[] =
{
  NETWORK_VIDEO_DEQUANT_SHADER_0,
  NETWORK_VIDEO_DEQUANT_SHADER_1,
};

static const char * const NETWORK_VIDEO_IDWT_SOURCES[] =
{
  NETWORK_VIDEO_IDWT_SHADER_0,
  NETWORK_VIDEO_IDWT_SHADER_1,
};

static const char * const NETWORK_VIDEO_YUV_SOURCES[] =
{
  NETWORK_VIDEO_YUV_SHADER,
};
