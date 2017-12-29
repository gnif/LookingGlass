/*
Looking Glass - KVM FrameRelay (KVMFR) Client
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

#include <stdint.h>
#include <stdbool.h>

#include "lg-renderer.h"

typedef enum LG_OutFormat
{
  LG_OUTPUT_BGRA
}
LG_OutFormat;

typedef bool         (* LG_DecoderCreate       )(void ** opaque);
typedef void         (* LG_DecoderDestroy      )(void  * opaque);
typedef bool         (* LG_DecoderInitialize   )(void  * opaque, const LG_RendererFormat format);
typedef void         (* LG_DecoderDeInitialize )(void  * opaque);
typedef LG_OutFormat (* LG_DecoderGetOutFormat )(void  * opaque);
typedef unsigned int (* LG_DecoderGetFramePitch)(void  * opaque);
typedef bool         (* LG_DecoderDecode       )(void  * opaque, uint8_t * dst, size_t dstSize, const uint8_t * src, size_t srcSize);

typedef struct LG_Decoder
{
  const char *            name;
  LG_DecoderCreate        create;
  LG_DecoderDestroy       destroy;
  LG_DecoderInitialize    initialize;
  LG_DecoderDeInitialize  deinitialize;
  LG_DecoderGetOutFormat  get_out_format;
  LG_DecoderGetFramePitch get_frame_pitch;
  LG_DecoderDecode        decode;
}
LG_Decoder;