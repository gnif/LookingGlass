/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#pragma once

#include <stdbool.h>
#include "shader.h"
#include "common/framebuffer.h"

#include "util.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

typedef enum EGL_TexType
{
  EGL_TEXTYPE_BUFFER,
  EGL_TEXTYPE_FRAMEBUFFER,
  EGL_TEXTYPE_DMABUF
}
EGL_TexType;

typedef enum EGL_PixelFormat
{
  EGL_PF_RGBA,
  EGL_PF_BGRA,
  EGL_PF_RGBA10,
  EGL_PF_RGBA16F
}
EGL_PixelFormat;

typedef enum EGL_TexStatus
{
  EGL_TEX_STATUS_NOTREADY,
  EGL_TEX_STATUS_OK,
  EGL_TEX_STATUS_ERROR
}
EGL_TexStatus;

typedef struct EGL_TexSetup
{
  /* the pixel format of the texture */
  EGL_PixelFormat pixFmt;

  /* the width of the texture in pixels */
  size_t width;

  /* the height of the texture in pixels */
  size_t height;

  /* the stide of the texture in bytes */
  size_t stride;
}
EGL_TexSetup;

typedef struct EGL_TexUpdate
{
  /* the type of this update */
  EGL_TexType type;

  union
  {
    /* EGL_TEXTURE_BUFFER */
    const uint8_t * buffer;

    /* EGL_TEXTURE_FRAMEBUFFER */
    struct
    {
      const FrameBuffer * frame;
      const FrameDamageRect * rects;
      int rectCount;
    };

    /* EGL_TEXTURE_DMABUF */
    int dmaFD;
  };
}
EGL_TexUpdate;

typedef struct EGL_Texture EGL_Texture;

typedef struct EGL_TextureOps
{
  /* allocate & initialize an EGL_Texture */
  bool (*init)(EGL_Texture ** texture, EGLDisplay * display);

  /* free the EGL_Texture */
  void (*free)(EGL_Texture * texture);

  /* setup/reconfigure the texture format */
  bool (*setup)(EGL_Texture * texture, const EGL_TexSetup * setup);

  /* update the texture  */
  bool (*update)(EGL_Texture * texture, const EGL_TexUpdate * update);

  /* called from a background job to prepare the texture for use before bind */
  enum EGL_TexStatus (*process)(EGL_Texture * texture);

  /* bind the texture for use */
  enum EGL_TexStatus (*bind)(EGL_Texture * texture);
}
EGL_TextureOps;

struct EGL_Texture
{
  struct EGL_TextureOps ops;

  // needed for dmabuf
  size_t size;
};

bool egl_textureInit(EGL_Texture ** texture, EGLDisplay * display,
    EGL_TexType type, bool streaming);
void egl_texture_free(EGL_Texture ** tex);

bool egl_textureSetup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt,
    size_t width, size_t height, size_t stride);

bool egl_textureUpdate(EGL_Texture * texture, const uint8_t * buffer);

bool egl_textureUpdateFromFrame(EGL_Texture * texture,
    const FrameBuffer * frame, const FrameDamageRect * damageRects,
    int damageRectsCount);

bool egl_textureUpdateFromDMA(EGL_Texture * texture,
    const FrameBuffer * frame, const int dmaFd);

enum EGL_TexStatus egl_textureProcess(EGL_Texture * texture);

enum EGL_TexStatus egl_textureBind(EGL_Texture * texture);
