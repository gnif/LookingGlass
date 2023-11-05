/**
 * Looking Glass
 * Copyright © 2017-2023 The Looking Glass Authors
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
#include "egl.h"
#include "egltypes.h"
#include "shader.h"
#include "model.h"
#include "common/framebuffer.h"
#include "common/types.h"

#include "util.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "texture_util.h"

typedef struct EGL_Model EGL_Model;

typedef struct EGL_TexUpdate
{
  /* the type of this update */
  EGL_TexType type;

  int x, y, width, height;

  //pitch     = row length in pixels
  //stride    = row length in bytes
  int pitch, stride;

  union
  {
    /* EGL_TEXTYPE_BUFFER */
    struct
    {
      // true if row 0 is the top of the image
      bool topDown;
      const uint8_t * buffer;
    };

    /* EGL_TEXTYPE_FRAMEBUFFER */
    struct
    {
      const FrameBuffer * frame;
      const FrameDamageRect * rects;
      int rectCount;
    };

    /* EGL_TEXTYPE_DMABUF */
    int dmaFD;
  };
}
EGL_TexUpdate;

typedef struct EGL_Texture EGL_Texture;

typedef struct EGL_TextureOps
{
  /* allocate & initialize an EGL_Texture */
  bool (*init)(EGL_Texture ** texture, EGL_TexType type, EGLDisplay * display);

  /* free the EGL_Texture */
  void (*free)(EGL_Texture * texture);

  /* setup/reconfigure the texture format */
  bool (*setup)(EGL_Texture * texture, const EGL_TexSetup * setup);

  /* update the texture  */
  bool (*update)(EGL_Texture * texture, const EGL_TexUpdate * update);

  /* called from a background job to prepare the texture for use before bind */
  enum EGL_TexStatus (*process)(EGL_Texture * texture);

  /* get the texture for use */
  enum EGL_TexStatus (*get)(EGL_Texture * texture, GLuint * tex);

  /* bind the texture for use */
  enum EGL_TexStatus (*bind)(EGL_Texture * texture);
}
EGL_TextureOps;

struct EGL_Texture
{
  struct EGL_TextureOps ops;
  EGL_TexType type;
  GLuint sampler;

  EGL_TexFormat format;
};

bool egl_textureInit(EGL_Texture ** texture, EGLDisplay * display,
    EGL_TexType type);
void egl_textureFree(EGL_Texture ** tex);

bool egl_textureSetup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt,
    size_t width, size_t height, size_t stride, size_t pitch);

bool egl_textureUpdate(EGL_Texture * texture, const uint8_t * buffer,
    bool topDown);

bool egl_textureUpdateRect(EGL_Texture * texture,
    int x, int y, int width, int height, int stride, int pitch,
    const uint8_t * buffer, bool topDown);

bool egl_textureUpdateFromFrame(EGL_Texture * texture,
    const FrameBuffer * frame, const FrameDamageRect * damageRects,
    int damageRectsCount);

bool egl_textureUpdateFromDMA(EGL_Texture * texture,
    const FrameBuffer * frame, const int dmaFd);

enum EGL_TexStatus egl_textureProcess(EGL_Texture * texture);

static inline EGL_TexStatus egl_textureGet(EGL_Texture * texture, GLuint * tex,
    unsigned int * sizeX, unsigned int * sizeY)
{
  if (sizeX)
    *sizeX = texture->format.width;
  if (sizeY)
    *sizeY = texture->format.height;
  return texture->ops.get(texture, tex);
}

enum EGL_TexStatus egl_textureBind(EGL_Texture * texture);

typedef void * PostProcessHandle;
PostProcessHandle egl_textureAddFilter(EGL_Texture * texture,
    EGL_Shader * shader, bool enabled);

void egl_textureEnableFilter(PostProcessHandle * handle, bool enable);

void egl_textureSetFilterRes(PostProcessHandle * handle,
    unsigned int x, unsigned int y);

void egl_textureInvalidate(EGL_Texture * texture);

void egl_textureGetFinalSize(EGL_Texture * texture, struct Rect * rect);
