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
#include <assert.h>

#include <GL/gl.h>

struct Pixel
{
  uint8_t b, g, r, a;
};

struct Inst
{
  LG_RendererFormat  format;
  struct Pixel     * pixels;
  unsigned int       yBytes;
};

static bool            lgd_yuv420_create          (void ** opaque);
static void            lgd_yuv420_destroy         (void  * opaque);
static bool            lgd_yuv420_initialize      (void  * opaque, const LG_RendererFormat format, SDL_Window * window);
static void            lgd_yuv420_deinitialize    (void  * opaque);
static LG_OutFormat    lgd_yuv420_get_out_format  (void  * opaque);
static unsigned int    lgd_yuv420_get_frame_pitch (void  * opaque);
static unsigned int    lgd_yuv420_get_frame_stride(void  * opaque);
static bool            lgd_yuv420_decode          (void  * opaque, const uint8_t * src, size_t srcSize);
static const uint8_t * lgd_yuv420_get_buffer      (void  * opaque);

static bool lgd_yuv420_create(void ** opaque)
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

static void lgd_yuv420_destroy(void * opaque)
{
  free(opaque);
}

static bool lgd_yuv420_initialize(void * opaque, const LG_RendererFormat format, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));

  this->yBytes  = format.width * format.height;
  this->pixels = malloc(sizeof(struct Pixel) * (format.width * format.height));
  return true;
}

static void lgd_yuv420_deinitialize(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  free(this->pixels);
}

static LG_OutFormat lgd_yuv420_get_out_format(void * opaque)
{
  return LG_OUTPUT_BGRA;
}

static unsigned int lgd_yuv420_get_frame_pitch(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.width * 4;
}

static unsigned int lgd_yuv420_get_frame_stride(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.width;
}

static bool lgd_yuv420_decode(void * opaque, const uint8_t * src, size_t srcSize)
{
  //FIXME: implement this properly using GLSL

  struct Inst * this = (struct Inst *)opaque;
  const unsigned int hw = this->format.width / 2;
  const unsigned int hp = this->yBytes / 4;

  for(size_t y = 0; y < this->format.height; ++y)
    for(size_t x = 0; x < this->format.width; ++x)
    {
      const unsigned int yoff = y * this->format.width + x;
      const unsigned int uoff = this->yBytes + ((y / 2) * hw + x / 2);
      const unsigned int voff = uoff + hp;

      float b = 1.164f * ((float)src[yoff] - 16.0f) + 2.018f * ((float)src[uoff] - 128.0f);
      float g = 1.164f * ((float)src[yoff] - 16.0f) - 0.813f * ((float)src[voff] - 128.0f) - 0.391f * ((float)src[uoff] - 128.0f);
      float r = 1.164f * ((float)src[yoff] - 16.0f) + 1.596f * ((float)src[voff] - 128.0f);

      #define CLAMP(x) (x < 0 ? 0 : (x > 255 ? 255 : x))
      this->pixels[yoff].b = CLAMP(b);
      this->pixels[yoff].g = CLAMP(g);
      this->pixels[yoff].r = CLAMP(r);
    }

  return true;
}

static const uint8_t * lgd_yuv420_get_buffer(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return (uint8_t *)this->pixels;
}

bool lgd_yuv420_init_gl_texture(void * opaque, GLenum target, GLuint texture, void ** ref)
{
  return false;
}

void lgd_yuv420_free_gl_texture(void * opaque, void * ref)
{
}

bool lgd_yuv420_update_gl_texture(void * opaque, void * ref)
{
  return false;
}

const LG_Decoder LGD_YUV420 =
{
  .name              = "YUV420",
  .create            = lgd_yuv420_create,
  .destroy           = lgd_yuv420_destroy,
  .initialize        = lgd_yuv420_initialize,
  .deinitialize      = lgd_yuv420_deinitialize,
  .get_out_format    = lgd_yuv420_get_out_format,
  .get_frame_pitch   = lgd_yuv420_get_frame_pitch,
  .get_frame_stride  = lgd_yuv420_get_frame_stride,
  .decode            = lgd_yuv420_decode,
  .get_buffer        = lgd_yuv420_get_buffer,

  .has_gl            = false, //FIXME: Implement this
  .init_gl_texture   = lgd_yuv420_init_gl_texture,
  .free_gl_texture   = lgd_yuv420_free_gl_texture,
  .update_gl_texture = lgd_yuv420_update_gl_texture
};