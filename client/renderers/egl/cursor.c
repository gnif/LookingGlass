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

#include "cursor.h"
#include "debug.h"
#include "utils.h"

#include "texture.h"
#include "shader.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

struct EGL_Cursor
{
  LG_Lock           lock;
  LG_RendererCursor type;
  int               width;
  int               height;
  int               pitch;
  uint8_t *         data;
  size_t            dataSize;
  bool              update;

  // cursor state
  bool              visible;
  float             x, y, w, h;

  // textures
  struct EGL_Texture * texture;
  struct EGL_Texture * textureMono;

  // shaders
  struct EGL_Shader  * shader;
  struct EGL_Shader  * shaderMono;

  // uniforms
  GLuint uMousePos;
  GLuint uMousePosMono;

  // model
  struct EGL_Model   * model;
};

static const char vertex_shader[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec4 mouse;\
\
out highp vec2 uv;\
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace;\
  gl_Position.w   = 1.0;\
  \
  gl_Position.x += 1.0f;\
  gl_Position.y -= 1.0f;\
  \
  gl_Position.x *= mouse.z;\
  gl_Position.y *= mouse.w;\
  \
  gl_Position.x += mouse.x;\
  gl_Position.y -= mouse.y;\
  \
  uv = vertexUV;\
}\
";

static const char frag_mouse_mono[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
 \
void main()\
{\
  highp vec4 tmp = texture(sampler1, uv);\
  if (tmp.rgb == vec3(0.0, 0.0, 0.0))\
    discard;\
  color = tmp;\
}\
";

static const char frag_rgba[] = "\
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

bool egl_cursor_init(EGL_Cursor ** cursor)
{
  *cursor = (EGL_Cursor *)malloc(sizeof(EGL_Cursor));
  if (!*cursor)
  {
    DEBUG_ERROR("Failed to malloc EGL_Cursor");
    return false;
  }

  memset(*cursor, 0, sizeof(EGL_Cursor));
  LG_LOCK_INIT((*cursor)->lock);

  if (!egl_texture_init(&(*cursor)->texture))
  {
    DEBUG_ERROR("Failed to initialize the cursor texture");
    return false;
  }

  if (!egl_texture_init(&(*cursor)->textureMono))
  {
    DEBUG_ERROR("Failed to initialize the cursor mono texture");
    return false;
  }

  if (!egl_shader_init(&(*cursor)->shader))
  {
    DEBUG_ERROR("Failed to initialize the cursor shader");
    return false;
  }

  if (!egl_shader_init(&(*cursor)->shaderMono))
  {
    DEBUG_ERROR("Failed to initialize the cursor mono shader");
    return false;
  }

  if (!egl_shader_compile(
        (*cursor)->shader,
        vertex_shader, sizeof(vertex_shader),
        frag_rgba    , sizeof(frag_rgba    )))
  {
    DEBUG_ERROR("Failed to compile the cursor shader");
    return false;
  }

  if (!egl_shader_compile(
        (*cursor)->shaderMono,
        vertex_shader  , sizeof(vertex_shader  ),
        frag_mouse_mono, sizeof(frag_mouse_mono)))
  {
    DEBUG_ERROR("Failed to compile the cursor mono shader");
    return false;
  }

  (*cursor)->uMousePos     = egl_shader_get_uniform_location((*cursor)->shader    , "mouse");
  (*cursor)->uMousePosMono = egl_shader_get_uniform_location((*cursor)->shaderMono, "mouse");

  if (!egl_model_init(&(*cursor)->model))
  {
    DEBUG_ERROR("Failed to initialize the cursor model");
    return false;
  }

  egl_model_set_default((*cursor)->model);
  return true;
}

void egl_cursor_free(EGL_Cursor ** cursor)
{
  if (!*cursor)
    return;

  LG_LOCK_FREE((*cursor)->lock);
  if ((*cursor)->data)
    free((*cursor)->data);

  egl_texture_free(&(*cursor)->texture    );
  egl_texture_free(&(*cursor)->textureMono);
  egl_shader_free (&(*cursor)->shader     );
  egl_shader_free (&(*cursor)->shaderMono );
  egl_model_free  (&(*cursor)->model      );

  free(*cursor);
  *cursor = NULL;
}

bool egl_cursor_set_shape(EGL_Cursor * cursor, const LG_RendererCursor type, const int width, const int height, const int pitch, const uint8_t * data)
{
  LG_LOCK(cursor->lock);

  cursor->type   = type;
  cursor->width  = width;
  cursor->height = (type == LG_CURSOR_MONOCHROME ? height / 2 : height);
  cursor->pitch  = pitch;

  const size_t size = height * pitch;
  if (size > cursor->dataSize)
  {
    if (cursor->data)
      free(cursor->data);

    cursor->data = (uint8_t *)malloc(size);
    if (!cursor->data)
    {
      DEBUG_ERROR("Failed to malloc buffer for cursor shape");
      return false;
    }

    cursor->dataSize = size;
  }

  memcpy(cursor->data, data, size);
  cursor->update = true;

  LG_UNLOCK(cursor->lock);
  return true;
}

void egl_cursor_set_size(EGL_Cursor * cursor, const float w, const float h)
{
  cursor->w = w;
  cursor->h = h;
}

void egl_cursor_set_state(EGL_Cursor * cursor, const bool visible, const float x, const float y)
{
  cursor->visible = visible;
  cursor->x       = x;
  cursor->y       = y;
}

void egl_cursor_render(EGL_Cursor * cursor)
{
  if (!cursor->visible)
    return;

  if (cursor->update)
  {
    LG_LOCK(cursor->lock);
    cursor->update = false;

    uint8_t * data = cursor->data;

    // tmp buffer for masked colour
    uint32_t  tmp[cursor->width * cursor->height];

    switch(cursor->type)
    {
      case LG_CURSOR_MASKED_COLOR:
      {
        for(int i = 0; i < cursor->width * cursor->height; ++i)
        {
          const uint32_t c = ((uint32_t *)data)[i];
          tmp[i] = (c & ~0xFF000000) | (c & 0xFF000000 ? 0x0 : 0xFF000000);
        }
        data = (uint8_t *)tmp;
        // fall through to LG_CURSOR_COLOR
        //
        // technically we should also create an XOR texture from the data but this
        // usage seems very rare in modern software.
      }

      case LG_CURSOR_COLOR:
      {
        egl_texture_setup(cursor->texture, EGL_PF_BGRA, cursor->width, cursor->height, cursor->width * cursor->height * 4, false);
        egl_texture_update(cursor->texture, data);
        egl_model_set_texture(cursor->model, cursor->texture);
        break;
      }

      case LG_CURSOR_MONOCHROME:
      {
        uint32_t and[cursor->width * cursor->height];
        uint32_t xor[cursor->width * cursor->height];

        for(int y = 0; y < cursor->height; ++y)
          for(int x = 0; x < cursor->width; ++x)
          {
            const uint8_t  * srcAnd  = data + (cursor->pitch * y) + (x / 8);
            const uint8_t  * srcXor  = srcAnd + cursor->pitch * cursor->height;
            const uint8_t    mask    = 0x80 >> (x % 8);
            const uint32_t   andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
            const uint32_t   xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;

            and[y * cursor->width + x] = andMask;
            xor[y * cursor->width + x] = xorMask;
          }

        egl_texture_setup (cursor->texture    , EGL_PF_BGRA, cursor->width, cursor->height, cursor->width * cursor->height * 4, false);
        egl_texture_setup (cursor->textureMono, EGL_PF_BGRA, cursor->width, cursor->height, cursor->width * cursor->height * 4, false);
        egl_texture_update(cursor->texture    , (uint8_t *)and);
        egl_texture_update(cursor->textureMono, (uint8_t *)xor);
        break;
      }
    }
    LG_UNLOCK(cursor->lock);
  }

  if (cursor->type == LG_CURSOR_MONOCHROME)
  {
    glEnable(GL_BLEND);

    egl_shader_use(cursor->shader);
    glUniform4f(cursor->uMousePos, cursor->x, cursor->y, cursor->w, cursor->h / 2);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    egl_model_set_texture(cursor->model, cursor->texture);
    egl_model_render(cursor->model);

    egl_shader_use(cursor->shaderMono);
    glUniform4f(cursor->uMousePosMono, cursor->x, cursor->y, cursor->w, cursor->h / 2);
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
    egl_model_set_texture(cursor->model, cursor->textureMono);
    egl_model_render(cursor->model);

    glDisable(GL_BLEND);
  }
  else
  {
    glEnable(GL_BLEND);

    egl_shader_use(cursor->shader);
    glUniform4f(cursor->uMousePos, cursor->x, cursor->y, cursor->w, cursor->h);
    glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    egl_model_render(cursor->model);

    glDisable(GL_BLEND);
  }
}