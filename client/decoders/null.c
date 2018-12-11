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
  LG_RendererFormat  format;
  const uint8_t    * src;
};

static bool            lgd_null_create          (void ** opaque);
static void            lgd_null_destroy         (void  * opaque);
static bool            lgd_null_initialize      (void  * opaque, const LG_RendererFormat format, SDL_Window * window);
static void            lgd_null_deinitialize    (void  * opaque);
static LG_OutFormat    lgd_null_get_out_format  (void  * opaque);
static unsigned int    lgd_null_get_frame_pitch (void  * opaque);
static unsigned int    lgd_null_get_frame_stride(void  * opaque);
static bool            lgd_null_decode          (void  * opaque, const uint8_t * src, size_t srcSize);
static const uint8_t * lgd_null_get_buffer      (void  * opaque);

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

static bool lgd_null_initialize(void * opaque, const LG_RendererFormat format, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  return true;
}

static void lgd_null_deinitialize(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  memset(this, 0, sizeof(struct Inst));
}

static LG_OutFormat lgd_null_get_out_format(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  switch(this->format.type)
  {
    case FRAME_TYPE_BGRA  : return LG_OUTPUT_BGRA;
    case FRAME_TYPE_RGBA  : return LG_OUTPUT_RGBA;
    case FRAME_TYPE_RGBA10: return LG_OUTPUT_RGBA10;

    default:
      DEBUG_ERROR("Unknown frame type");
      return LG_OUTPUT_INVALID;
  }
}

static unsigned int lgd_null_get_frame_pitch(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.pitch;
}

static unsigned int lgd_null_get_frame_stride(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.stride;
}

static bool lgd_null_decode(void * opaque, const uint8_t * src, size_t srcSize)
{
  struct Inst * this = (struct Inst *)opaque;
  this->src = src;
  return true;
}

static const uint8_t * lgd_null_get_buffer(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this->src)
    return NULL;

  return this->src;
}

const LG_Decoder LGD_NULL =
{
  .name             = "NULL",
  .create           = lgd_null_create,
  .destroy          = lgd_null_destroy,
  .initialize       = lgd_null_initialize,
  .deinitialize     = lgd_null_deinitialize,
  .get_out_format   = lgd_null_get_out_format,
  .get_frame_pitch  = lgd_null_get_frame_pitch,
  .get_frame_stride = lgd_null_get_frame_stride,
  .decode           = lgd_null_decode,
  .get_buffer       = lgd_null_get_buffer
};