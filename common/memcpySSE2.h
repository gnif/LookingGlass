/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <tmmintrin.h>
#include <immintrin.h>

#include "debug.h"

#if defined(__GNUC___) || defined(__GNUG__)
#define OP(...) #__VA_ARGS__ "\n\t"

inline static void memcpySSE(void *dst, const void * src, size_t length)
{
#if defined(__x86_64__) || defined(__i386__)
  void * end = dst + (length & ~0x7F);
  size_t rem = (7 - ((length & 0x7F) >> 4)) * 10;

  __asm__ __volatile__ (
    // save the registers we intend to alter, failure to do so causes problems
    // when gcc -O3 is used
    OP(push %[dst])
    OP(push %[src])
    OP(push %[end])

    // perform 128 byte SIMD block copy
    OP(cmp %[dst],%[end])
    OP(je ramain_%=)
    OP(loop_%=:)
    OP(vmovaps  0x00(%[src]),%%xmm0)
    OP(vmovaps  0x10(%[src]),%%xmm1)
    OP(vmovaps  0x20(%[src]),%%xmm2)
    OP(vmovaps  0x30(%[src]),%%xmm3)
    OP(vmovaps  0x40(%[src]),%%xmm4)
    OP(vmovaps  0x50(%[src]),%%xmm5)
    OP(vmovaps  0x60(%[src]),%%xmm6)
    OP(vmovaps  0x70(%[src]),%%xmm7)
    OP(vmovntdq %%xmm0,0x00(%[dst]))
    OP(vmovntdq %%xmm1,0x10(%[dst]))
    OP(vmovntdq %%xmm2,0x20(%[dst]))
    OP(vmovntdq %%xmm3,0x30(%[dst]))
    OP(vmovntdq %%xmm4,0x40(%[dst]))
    OP(vmovntdq %%xmm5,0x50(%[dst]))
    OP(vmovntdq %%xmm6,0x60(%[dst]))
    OP(vmovntdq %%xmm7,0x70(%[dst]))
    OP(add      $0x80,%[dst])
    OP(add      $0x80,%[src])
    OP(cmp      %[dst],%[end])
    OP(jne      loop_%=)

    // copy any remaining 16 byte blocks
    OP(remain_%=:)
#ifdef __x86_64__
    OP(leaq (%%rip), %[end])
    OP(add  $10,%[end])
#else
    OP(call .+5)
    OP(pop  %[end])
    OP(add  $8,%[end])
#endif
    OP(add  %[rem],%[end])
    OP(jmp  *%[end])

    // jump table
    OP(vmovaps  0x60(%[src]),%%xmm0)
    OP(vmovntdq %%xmm0,0x60(%[dst]))
    OP(vmovaps  0x50(%[src]),%%xmm1)
    OP(vmovntdq %%xmm1,0x50(%[dst]))
    OP(vmovaps  0x40(%[src]),%%xmm2)
    OP(vmovntdq %%xmm2,0x40(%[dst]))
    OP(vmovaps  0x30(%[src]),%%xmm3)
    OP(vmovntdq %%xmm3,0x30(%[dst]))
    OP(vmovaps  0x20(%[src]),%%xmm4)
    OP(vmovntdq %%xmm4,0x20(%[dst]))
    OP(vmovaps  0x10(%[src]),%%xmm5)
    OP(vmovntdq %%xmm5,0x10(%[dst]))
    OP(vmovaps  0x00(%[src]),%%xmm6)
    OP(vmovntdq %%xmm6,0x00(%[dst]))

    // alignment as the previous two instructions are only 4 bytes
    OP(nop)
    OP(nop)

    // restore the registers
    OP(pop %[end])
    OP(pop %[src])
    OP(pop %[dst])
    :
    : [dst]"r" (dst),
      [src]"r" (src),
      [end]"c" (end),
      [rem]"d" (rem)
    : "xmm0",
      "xmm1",
      "xmm2",
      "xmm3",
      "xmm4",
      "xmm5",
      "xmm6",
      "xmm7",
      "memory"
  );

  //copy any remaining bytes
  for(size_t i = (length & 0xF); i; --i)
    ((uint8_t *)dst)[length - i] =
      ((uint8_t *)src)[length - i];
#else
  memcpy(dst, src, length);
#endif
}
#else
extern "C" void __fastcall memcpySSE(void *dst, const void * src, size_t length);
#endif