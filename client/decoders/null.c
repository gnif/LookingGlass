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

#include "lg-decoder.h"

#include "debug.h"
#include "memcpySSE.h"

#include <stdlib.h>
#include <string.h>

struct Inst
{
  LG_RendererFormat format;
};

static bool         lgd_null_create         (void ** opaque);
static void         lgd_null_destroy        (void  * opaque);
static bool         lgd_null_initialize     (void  * opaque, const LG_RendererFormat format);
static void         lgd_null_deinitialize   (void  * opaque);
static LG_OutFormat lgd_null_get_out_format (void  * opaque);
static unsigned int lgd_null_get_frame_pitch(void  * opaque);
static bool         lgd_null_decode         (void  * opaque, uint8_t * dst, size_t dstSize, const uint8_t * src, size_t srcSize);

static bool lgd_null_create(void ** opaque)
{
  // create our local storage
  *opaque = malloc(sizeof(struct Inst));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
    return false;
  }
  memset(*opaque, 0, sizeof(struct Inst));
  return true;
}

static void lgd_null_destroy(void * opaque)
{
  free(opaque);
}

static bool lgd_null_initialize(void * opaque, const LG_RendererFormat format)
{
  struct Inst * this = (struct Inst *)opaque;
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  return true;
}

static void lgd_null_deinitialize(void * opaque)
{
}

static LG_OutFormat lgd_null_get_out_format(void * opaque)
{
  return LG_OUTPUT_BGRA;
}

static unsigned int lgd_null_get_frame_pitch(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.pitch;
}

static bool lgd_null_decode(void * opaque, uint8_t * dst, size_t dstSize, const uint8_t * src, size_t srcSize)
{
  memcpySSE(dst, src, dstSize);
  return true;
}

const LG_Decoder LGD_NULL =
{
  .name            = "NULL",
  .create          = lgd_null_create,
  .destroy         = lgd_null_destroy,
  .initialize      = lgd_null_initialize,
  .deinitialize    = lgd_null_deinitialize,
  .get_out_format  = lgd_null_get_out_format,
  .get_frame_pitch = lgd_null_get_frame_pitch,
  .decode          = lgd_null_decode
};