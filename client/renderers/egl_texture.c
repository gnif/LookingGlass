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

#include "egl_texture.h"
#include "debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <SDL2/SDL_egl.h>

struct EGL_Texture
{
  GLuint texture;
  size_t width, height;

  bool   hasPBO;
  GLuint pbo[2];
  int    pboIndex;
  size_t pboBufferSize;
};

bool egl_texture_init(EGL_Texture ** texture)
{
  *texture = (EGL_Texture *)malloc(sizeof(EGL_Texture));
  if (!*texture)
  {
    DEBUG_ERROR("Failed to malloc EGL_Texture");
    return false;
  }

  memset(*texture, 0, sizeof(EGL_Texture));
  glGenTextures(1, &(*texture)->texture);

  return true;
}

void egl_texture_free(EGL_Texture ** texture)
{
  if (!*texture)
    return;

  glDeleteTextures(1, &(*texture)->texture);

  if ((*texture)->hasPBO)
    glDeleteBuffers(2, (*texture)->pbo);

  free(*texture);
  *texture = NULL;
}

bool egl_texture_init_streaming(EGL_Texture * texture, size_t width, size_t height, size_t bufferSize)
{
  texture->width      = width;
  texture->height     = height;
  texture->pboBufferSize = bufferSize;

  glBindTexture(GL_TEXTURE_2D, texture->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!texture->hasPBO)
  {
    glGenBuffers(2, texture->pbo);
    texture->hasPBO = true;
  }

  for(int i = 0; i < 2; ++i)
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->pbo[i]);
    glBufferData(
      GL_PIXEL_UNPACK_BUFFER,
      bufferSize,
      NULL,
      GL_DYNAMIC_DRAW
    );
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  return true;
}

bool egl_texture_stream_buffer(EGL_Texture * texture, const uint8_t * buffer)
{
  if (++texture->pboIndex == 2)
    texture->pboIndex = 0;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->pbo[texture->pboIndex]);
    glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, texture->pboBufferSize, buffer);
      glBindTexture(GL_TEXTURE_2D, texture->texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  return true;
}

void egl_texture_bind(EGL_Texture * texture)
{
  glBindTexture(GL_TEXTURE_2D, texture->texture);
}