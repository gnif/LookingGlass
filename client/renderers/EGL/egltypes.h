/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include <stddef.h>

typedef enum EGL_TexType
{
  EGL_TEXTYPE_BUFFER,
  EGL_TEXTYPE_BUFFER_MAP,
  EGL_TEXTYPE_BUFFER_STREAM,
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

typedef enum EGL_FilterType
{
  EGL_FILTER_TYPE_EFFECT,
  EGL_FILTER_TYPE_UPSCALE,
  EGL_FILTER_TYPE_DOWNSCALE
}
EGL_FilterType;
