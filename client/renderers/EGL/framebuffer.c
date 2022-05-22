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

#include "framebuffer.h"
#include "texture.h"

#include <stdlib.h>

#include "common/debug.h"

struct EGL_Framebuffer
{
  GLuint fbo;
  EGL_Texture * tex;
};

bool egl_framebufferInit(EGL_Framebuffer ** fb)
{
  EGL_Framebuffer * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate ram");
    return false;
  }

  if (!egl_textureInit(&this->tex, NULL, EGL_TEXTYPE_BUFFER))
  {
    DEBUG_ERROR("Failed to initialize the texture");
    return false;
  }

  glGenFramebuffers(1, &this->fbo);

  *fb = this;
  return true;
}

void egl_framebufferFree(EGL_Framebuffer ** fb)
{
  EGL_Framebuffer * this = *fb;

  egl_textureFree(&this->tex);
  free(this);
  *fb = NULL;
}

bool egl_framebufferSetup(EGL_Framebuffer * this, enum EGL_PixelFormat pixFmt,
    unsigned int width, unsigned int height)
{
  if (!egl_textureSetup(this->tex, pixFmt, width, height, 0))
  {
    DEBUG_ERROR("Failed to setup the texture");
    return false;
  }

  GLuint tex;
  egl_textureGet(this->tex, &tex, NULL, NULL);

  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_TEXTURE_2D, tex, 0);
  glDrawBuffers(1, &(GLenum){GL_COLOR_ATTACHMENT0});

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    DEBUG_ERROR("Failed to setup the framebuffer: 0x%x", status);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  return true;
}

void egl_framebufferBind(EGL_Framebuffer * this)
{
  glBindFramebuffer(GL_FRAMEBUFFER, this->fbo);
  glViewport(0, 0, this->tex->format.width, this->tex->format.height);
}

GLuint egl_framebufferGetTexture(EGL_Framebuffer * this)
{
  GLuint output;
  egl_textureGet(this->tex, &output, NULL, NULL);
  return output;
}
