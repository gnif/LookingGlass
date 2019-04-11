/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#if defined(NATIVE_MEMCPY)
  #define memcpySSE memcpy
#elif defined(_MSC_VER)
  extern "C" void * memcpySSE(void *dst, const void * src, size_t length);
#elif (defined(__GNUC__) || defined(__GNUG__)) && defined(__i386__)
  inline static void * memcpySSE(void *dst, const void * src, size_t length)
  {
    if (length == 0 || dst == src)
      return;

    // copies under 1MB are faster with the inlined memcpy
    // tell the dev to use that instead
    if (length < 1048576)
    {
      static bool smallBufferWarn = false;
      if (!smallBufferWarn)
      {
        DEBUG_WARN("Do not use memcpySSE for copies under 1MB in size!");
        smallBufferWarn = true;
      }
      memcpy(dst, src, length);
      return;
    }

    const void * end = dst + (length & ~0x7F);
    const size_t off = (7 - ((length & 0x7F) >> 4)) * 9;

    __asm__ __volatile__ (
      "cmp         %[dst],%[end] \n\t"
      "je          Remain_%= \n\t"

      // perform SIMD block copy
      "loop_%=: \n\t"
      "movaps     0x00(%[src]),%%xmm0  \n\t"
      "movaps     0x10(%[src]),%%xmm1  \n\t"
      "movaps     0x20(%[src]),%%xmm2  \n\t"
      "movaps     0x30(%[src]),%%xmm3  \n\t"
      "movaps     0x40(%[src]),%%xmm4  \n\t"
      "movaps     0x50(%[src]),%%xmm5  \n\t"
      "movaps     0x60(%[src]),%%xmm6  \n\t"
      "movaps     0x70(%[src]),%%xmm7  \n\t"
      "movntdq    %%xmm0 ,0x00(%[dst]) \n\t"
      "movntdq    %%xmm1 ,0x10(%[dst]) \n\t"
      "movntdq    %%xmm2 ,0x20(%[dst]) \n\t"
      "movntdq    %%xmm3 ,0x30(%[dst]) \n\t"
      "movntdq    %%xmm4 ,0x40(%[dst]) \n\t"
      "movntdq    %%xmm5 ,0x50(%[dst]) \n\t"
      "movntdq    %%xmm6 ,0x60(%[dst]) \n\t"
      "movntdq    %%xmm7 ,0x70(%[dst]) \n\t"
      "add         $0x80,%[dst] \n\t"
      "add         $0x80,%[src] \n\t"
      "cmp         %[dst],%[end] \n\t"
      "jne         loop_%= \n\t"

      "Remain_%=: \n\t"

      // copy any remaining 16 byte blocks
      "call        GetPC_%=\n\t"
      "Offset_%=:\n\t"
      "add         $(BlockTable_%= - Offset_%=), %%eax \n\t"
      "add         %[off],%%eax \n\t"
      "jmp         *%%eax \n\t"

      "GetPC_%=:\n\t"
      "mov (%%esp), %%eax \n\t"
      "ret \n\t"

      "BlockTable_%=:\n\t"
      "movaps     0x60(%[src]),%%xmm6  \n\t"
      "movntdq    %%xmm6 ,0x60(%[dst]) \n\t"
      "movaps     0x50(%[src]),%%xmm5  \n\t"
      "movntdq    %%xmm5 ,0x50(%[dst]) \n\t"
      "movaps     0x40(%[src]),%%xmm4  \n\t"
      "movntdq    %%xmm4 ,0x40(%[dst]) \n\t"
      "movaps     0x30(%[src]),%%xmm3  \n\t"
      "movntdq    %%xmm3 ,0x30(%[dst]) \n\t"
      "movaps     0x20(%[src]),%%xmm2  \n\t"
      "movntdq    %%xmm2 ,0x20(%[dst]) \n\t"
      "movaps     0x10(%[src]),%%xmm1  \n\t"
      "movntdq    %%xmm1 ,0x10(%[dst]) \n\t"
      "movaps     0x00(%[src]),%%xmm0  \n\t"
      "movntdq    %%xmm0 ,0x00(%[dst]) \n\t"
      "nop\n\t"
      "nop\n\t"

      : [dst]"+r" (dst),
        [src]"+r" (src)
      : [off]"r"  (off),
        [end]"r"  (end)
      : "eax",
        "xmm0",
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
    memcpy(dst, src, length & 0xF);
  }
#else
  #define memcpySSE memcpy
#endif
