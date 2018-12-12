/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
cahe terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "desktop.h"
#include "debug.h"
#include "utils.h"

#include "texture.h"
#include "shader.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

struct EGL_Desktop
{
  EGL_Texture * texture;
  EGL_Shader  * shader; // the active shader
  EGL_Model   * model;

  // shader instances
  EGL_Shader * shader_generic;
  EGL_Shader * shader_yuv;

  // uniforms
  GLint uDesktopPos;

  // internals
  enum EGL_PixelFormat pixFmt;
  unsigned int         width, height;
  size_t               frameSize;
  const uint8_t      * data;
  bool                 update;
};

static const char vertex_shader[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec4 position;\
\
out highp vec2 uv;\
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.w   = 1.0; \
  gl_Position.x  -= position.x; \
  gl_Position.y  -= position.y; \
  gl_Position.x  *= position.z; \
  gl_Position.y  *= position.w; \
\
  uv = vertexUV;\
}\
";


static const char frag_generic[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
 \
void main()\
{\
  color = texture(sampler1, uv);\
}\
";

static const char frag_yuv[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
uniform sampler2D sampler2;\
uniform sampler2D sampler3;\
\
void main()\
{\
  highp vec4 yuv = vec4(\
    texture(sampler1, uv).r,\
    texture(sampler2, uv).r,\
    texture(sampler3, uv).r,\
    1.0\
  );\
  \
  highp mat4 yuv_to_rgb = mat4(\
    1.0,  0.0  ,  1.402, -0.701,\
    1.0, -0.344, -0.714,  0.529,\
    1.0,  1.772,  0.0  , -0.886,\
    1.0,  1.0  ,  1.0  ,  1.0\
  );\
  \
  color = yuv * yuv_to_rgb;\
}\
";

bool egl_desktop_init(EGL_Desktop ** desktop)
{
  *desktop = (EGL_Desktop *)malloc(sizeof(EGL_Desktop));
  if (!*desktop)
  {
    DEBUG_ERROR("Failed to malloc EGL_Desktop");
    return false;
  }

  memset(*desktop, 0, sizeof(EGL_Desktop));

  if (!egl_texture_init(&(*desktop)->texture))
  {
    DEBUG_ERROR("Failed to initialize the desktop texture");
    return false;
  }

  if (!egl_shader_init(&(*desktop)->shader_generic))
  {
    DEBUG_ERROR("Failed to initialize the generic desktop shader");
    return false;
  }

  if (!egl_shader_init(&(*desktop)->shader_yuv))
  {
    DEBUG_ERROR("Failed to initialize the yuv desktop shader");
    return false;
  }

  if (!egl_shader_compile((*desktop)->shader_generic,
        vertex_shader, sizeof(vertex_shader),
        frag_generic , sizeof(frag_generic)))
  {
    DEBUG_ERROR("Failed to compile the generic desktop shader");
    return false;
  }

  if (!egl_shader_compile((*desktop)->shader_yuv,
        vertex_shader, sizeof(vertex_shader),
        frag_yuv     , sizeof(frag_yuv     )))
  {
    DEBUG_ERROR("Failed to compile the yuv desktop shader");
    return false;
  }

  if (!egl_model_init(&(*desktop)->model))
  {
    DEBUG_ERROR("Failed to initialize the desktop model");
    return false;
  }

  egl_model_set_default((*desktop)->model);
  egl_model_set_texture((*desktop)->model, (*desktop)->texture);

  return true;
}

void egl_desktop_free(EGL_Desktop ** desktop)
{
  if (!*desktop)
    return;

  egl_texture_free(&(*desktop)->texture       );
  egl_shader_free (&(*desktop)->shader_generic);
  egl_shader_free (&(*desktop)->shader_yuv    );
  egl_model_free  (&(*desktop)->model         );

  free(*desktop);
  *desktop = NULL;
}

bool egl_desktop_prepare_update(EGL_Desktop * desktop, const bool sourceChanged, const LG_RendererFormat format, const uint8_t * data)
{
  if (sourceChanged)
  {
    switch(format.type)
    {
      case FRAME_TYPE_BGRA:
        desktop->pixFmt    = EGL_PF_BGRA;
        desktop->shader    = desktop->shader_generic;
        desktop->frameSize = format.height * format.pitch;
        break;

      case FRAME_TYPE_RGBA:
        desktop->pixFmt    = EGL_PF_RGBA;
        desktop->shader    = desktop->shader_generic;
        desktop->frameSize = format.height * format.pitch;
        break;

      case FRAME_TYPE_RGBA10:
        desktop->pixFmt    = EGL_PF_RGBA10;
        desktop->shader    = desktop->shader_generic;
        desktop->frameSize = format.height * format.pitch;
        break;

      case FRAME_TYPE_YUV420:
        desktop->pixFmt    = EGL_PF_YUV420;
        desktop->shader    = desktop->shader_yuv;
        desktop->frameSize = format.width * format.height * 3 / 2;
        break;

      default:
        DEBUG_ERROR("Unsupported frame format");
        return false;
    }

    desktop->width  = format.width;
    desktop->height = format.height;
  }

  desktop->data   = data;
  desktop->update = true;

  return true;
}

bool egl_desktop_perform_update(EGL_Desktop * desktop, const bool sourceChanged)
{
  if (sourceChanged)
  {
    if (desktop->shader)
      desktop->uDesktopPos = egl_shader_get_uniform_location(desktop->shader, "position");

    if (!egl_texture_setup(
      desktop->texture,
      desktop->pixFmt,
      desktop->width,
      desktop->height,
      desktop->frameSize,
      true // streaming texture
    ))
    {
      DEBUG_ERROR("Failed to setup the desktop texture");
      return false;
    }
  }

  if (!desktop->update)
    return true;

  if (!egl_texture_update(desktop->texture, desktop->data))
  {
    DEBUG_ERROR("Failed to update the desktop texture");
    return false;
  }

  desktop->update = false;
  return true;
}

void egl_desktop_render(EGL_Desktop * desktop, const float x, const float y, const float scaleX, const float scaleY)
{
  if (!desktop->shader)
    return;

  egl_shader_use(desktop->shader);
  glUniform4f(desktop->uDesktopPos, x, y, scaleX, scaleY);
  egl_model_render(desktop->model);
}