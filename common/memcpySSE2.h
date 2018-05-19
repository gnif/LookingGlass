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

#if defined(__GNUC__) || defined(__GNUG__)

#define OP(...) #__VA_ARGS__ "\n\t"

inline static void memcpySSE(void *dst, const void * src, size_t length)
{
#if !defined(NATIVE_MEMCPY) && (defined(__x86_64__) || defined(__i386__))
  if (length == 0 || dst == src)
    return;

#ifdef __x86_64__
  const void * end = dst + (length & ~0xFF);
  size_t off = (15 - ((length & 0xFF) >> 4));
  off = (off < 8) ? off * 16 : 7 * 16 + (off - 7) * 10;
#else
  const void * end = dst + (length & ~0x7F);
  const size_t off = (7 - ((length & 0x7F) >> 4)) * 10;
#endif

#ifdef __x86_64__
  #define REG "rax"
#else
  #define REG "eax"
#endif

  __asm__ __volatile__ (
   "cmp         %[dst],%[end] \n\t"
   "je          Remain_%= \n\t"

   // perform SIMD block copy
   "loop_%=: \n\t"
   "vmovaps     0x00(%[src]),%%xmm0  \n\t"
   "vmovaps     0x10(%[src]),%%xmm1  \n\t"
   "vmovaps     0x20(%[src]),%%xmm2  \n\t"
   "vmovaps     0x30(%[src]),%%xmm3  \n\t"
   "vmovaps     0x40(%[src]),%%xmm4  \n\t"
   "vmovaps     0x50(%[src]),%%xmm5  \n\t"
   "vmovaps     0x60(%[src]),%%xmm6  \n\t"
   "vmovaps     0x70(%[src]),%%xmm7  \n\t"
#ifdef __x86_64__
   "vmovaps     0x80(%[src]),%%xmm8  \n\t"
   "vmovaps     0x90(%[src]),%%xmm9  \n\t"
   "vmovaps     0xA0(%[src]),%%xmm10 \n\t"
   "vmovaps     0xB0(%[src]),%%xmm11 \n\t"
   "vmovaps     0xC0(%[src]),%%xmm12 \n\t"
   "vmovaps     0xD0(%[src]),%%xmm13 \n\t"
   "vmovaps     0xE0(%[src]),%%xmm14 \n\t"
   "vmovaps     0xF0(%[src]),%%xmm15 \n\t"
#endif
   "vmovntdq    %%xmm0 ,0x00(%[dst]) \n\t"
   "vmovntdq    %%xmm1 ,0x10(%[dst]) \n\t"
   "vmovntdq    %%xmm2 ,0x20(%[dst]) \n\t"
   "vmovntdq    %%xmm3 ,0x30(%[dst]) \n\t"
   "vmovntdq    %%xmm4 ,0x40(%[dst]) \n\t"
   "vmovntdq    %%xmm5 ,0x50(%[dst]) \n\t"
   "vmovntdq    %%xmm6 ,0x60(%[dst]) \n\t"
   "vmovntdq    %%xmm7 ,0x70(%[dst]) \n\t"
#ifdef __x86_64__
   "vmovntdq    %%xmm8 ,0x80(%[dst]) \n\t"
   "vmovntdq    %%xmm9 ,0x90(%[dst]) \n\t"
   "vmovntdq    %%xmm10,0xA0(%[dst]) \n\t"
   "vmovntdq    %%xmm11,0xB0(%[dst]) \n\t"
   "vmovntdq    %%xmm12,0xC0(%[dst]) \n\t"
   "vmovntdq    %%xmm13,0xD0(%[dst]) \n\t"
   "vmovntdq    %%xmm14,0xE0(%[dst]) \n\t"
   "vmovntdq    %%xmm15,0xF0(%[dst]) \n\t"

   "add         $0x100,%[dst] \n\t"
   "add         $0x100,%[src] \n\t"
#else
   "add         $0x80,%[dst] \n\t"
   "add         $0x80,%[src] \n\t"
#endif
   "cmp         %[dst],%[end] \n\t"
   "jne         loop_%= \n\t"

   "Remain_%=: \n\t"

   // copy any remaining 16 byte blocks
#ifdef __x86_64__
   "leaq        (%%rip), %%rax\n\t"
#else
   "call        GetPC_%=\n\t"
#endif
   "Offset_%=:\n\t"
   "add         $(BlockTable_%= - Offset_%=), %%" REG "\n\t"
   "add         %[off],%%" REG " \n\t"
   "jmp         *%%" REG " \n\t"

#ifdef __i386__
  "GetPC_%=:\n\t"
  "mov (%%esp), %%eax \n\t"
  "ret \n\t"
#endif

   "BlockTable_%=:\n\t"
#ifdef __x86_64__
   "vmovaps     0xE0(%[src]),%%xmm14 \n\t"
   "vmovntdq    %%xmm14,0xE0(%[dst]) \n\t"
   "vmovaps     0xD0(%[src]),%%xmm13 \n\t"
   "vmovntdq    %%xmm13,0xD0(%[dst]) \n\t"
   "vmovaps     0xC0(%[src]),%%xmm12 \n\t"
   "vmovntdq    %%xmm12,0xC0(%[dst]) \n\t"
   "vmovaps     0xB0(%[src]),%%xmm11 \n\t"
   "vmovntdq    %%xmm11,0xB0(%[dst]) \n\t"
   "vmovaps     0xA0(%[src]),%%xmm10 \n\t"
   "vmovntdq    %%xmm10,0xA0(%[dst]) \n\t"
   "vmovaps     0x90(%[src]),%%xmm9  \n\t"
   "vmovntdq    %%xmm9 ,0x90(%[dst]) \n\t"
   "vmovaps     0x80(%[src]),%%xmm8  \n\t"
   "vmovntdq    %%xmm8 ,0x80(%[dst]) \n\t"
   "vmovaps     0x70(%[src]),%%xmm7  \n\t"
   "vmovntdq    %%xmm7 ,0x70(%[dst]) \n\t"
#endif
   "vmovaps     0x60(%[src]),%%xmm6  \n\t"
   "vmovntdq    %%xmm6 ,0x60(%[dst]) \n\t"
   "vmovaps     0x50(%[src]),%%xmm5  \n\t"
   "vmovntdq    %%xmm5 ,0x50(%[dst]) \n\t"
   "vmovaps     0x40(%[src]),%%xmm4  \n\t"
   "vmovntdq    %%xmm4 ,0x40(%[dst]) \n\t"
   "vmovaps     0x30(%[src]),%%xmm3  \n\t"
   "vmovntdq    %%xmm3 ,0x30(%[dst]) \n\t"
   "vmovaps     0x20(%[src]),%%xmm2  \n\t"
   "vmovntdq    %%xmm2 ,0x20(%[dst]) \n\t"
   "vmovaps     0x10(%[src]),%%xmm1  \n\t"
   "vmovntdq    %%xmm1 ,0x10(%[dst]) \n\t"
   "vmovaps     0x00(%[src]),%%xmm0  \n\t"
   "vmovntdq    %%xmm0 ,0x00(%[dst]) \n\t"
   "nop\n\t"
   "nop\n\t"

   : [dst]"+r" (dst),
     [src]"+r" (src)
   : [off]"r"  (off),
     [end]"r"  (end)
   : REG,
     "xmm0",
     "xmm1",
     "xmm2",
     "xmm3",
     "xmm4",
     "xmm5",
     "xmm6",
     "xmm7",
#ifdef __x86_64__
     "xmm8",
     "xmm9",
     "xmm10",
     "xmm11",
     "xmm12",
     "xmm13",
     "xmm14",
     "xmm15",
     "rax",
#else
     "eax",
#endif
     "memory"
  );

#undef REG

  //copy any remaining bytes
  for(size_t i = (length & 0xF); i; --i)
    ((uint8_t *)dst)[length - i] =
      ((uint8_t *)src)[length - i];
#else
  memcpy(dst, src, length);
#endif
}
#else
extern "C" void memcpySSE(void *dst, const void * src, size_t length);
#endif