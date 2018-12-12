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

#include "fps.h"
#include "debug.h"
#include "utils.h"

#include "texture.h"
#include "shader.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

struct EGL_FPS
{
  const LG_Font * font;
  LG_FontObj      fontObj;

  EGL_Texture * texture;
  EGL_Shader  * shader;
  EGL_Model   * model;

  bool  ready;
  float width, height;

  // uniforms
  GLint uScreen, uSize;
};

static const char vertex_shader[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec2 screen;\
uniform vec2 size;\
\
out highp vec2 uv;\
\
void main()\
{\
  highp vec2 pix  = (vec2(1.0, 1.0) / screen); \
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.w   = 1.0; \
  gl_Position.x  *= pix.x * size.x; \
  gl_Position.y  *= pix.y * size.y; \
  gl_Position.x  -= 1.0 - (pix.x * size.x);\
  gl_Position.y  += 1.0 - (pix.y * size.y);\
  gl_Position.x  += pix.x * 10.0; \
  gl_Position.y  -= pix.y * 10.0; \
\
  uv = vertexUV;\
}\
";

static const char frag_shader[] = "\
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
  color.r = tmp.b; \
  color.g = tmp.g; \
  color.b = tmp.r; \
  color.a = tmp.a; \
  if (color.a == 0.0) \
  {\
    color.a = 0.5; \
    color.r = 0.0; \
    color.g = 0.0; \
  }\
}\
";

bool egl_fps_init(EGL_FPS ** fps, const LG_Font * font, LG_FontObj fontObj)
{
  *fps = (EGL_FPS *)malloc(sizeof(EGL_FPS));
  if (!*fps)
  {
    DEBUG_ERROR("Failed to malloc EGL_FPS");
    return false;
  }

  memset(*fps, 0, sizeof(EGL_FPS));

  (*fps)->font    = font;
  (*fps)->fontObj = fontObj;

  if (!egl_texture_init(&(*fps)->texture))
  {
    DEBUG_ERROR("Failed to initialize the fps texture");
    return false;
  }

  if (!egl_shader_init(&(*fps)->shader))
  {
    DEBUG_ERROR("Failed to initialize the fps shader");
    return false;
  }

  if (!egl_shader_compile((*fps)->shader,
        vertex_shader, sizeof(vertex_shader),
        frag_shader, sizeof(frag_shader)))
  {
    DEBUG_ERROR("Failed to compile the fps shader");
    return false;
  }

  (*fps)->uSize   = egl_shader_get_uniform_location((*fps)->shader, "size"  );
  (*fps)->uScreen = egl_shader_get_uniform_location((*fps)->shader, "screen");

  if (!egl_model_init(&(*fps)->model))
  {
    DEBUG_ERROR("Failed to initialize the fps model");
    return false;
  }

  static const GLfloat square[] =
  {
    -1.0f, -1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f, 0.0f
  };

  static const GLfloat uvs[] =
  {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
  };

  egl_model_set_verticies((*fps)->model, square, sizeof(square) / sizeof(GLfloat));
  egl_model_set_uvs      ((*fps)->model, uvs   , sizeof(uvs   ) / sizeof(GLfloat));
  egl_model_set_texture  ((*fps)->model, (*fps)->texture);

  return true;
}

void egl_fps_free(EGL_FPS ** fps)
{
  if (!*fps)
    return;

  egl_texture_free(&(*fps)->texture);
  egl_shader_free (&(*fps)->shader );
  egl_model_free  (&(*fps)->model  );

  free(*fps);
  *fps = NULL;
}

void egl_fps_update(EGL_FPS * fps, const float avgFPS, const float renderFPS)
{
  char str[128];
  snprintf(str, sizeof(str), "UPS: %8.4f, FPS: %8.4f", avgFPS, renderFPS);

  LG_FontBitmap * bmp = fps->font->render(fps->fontObj, 0xffffff00, str);
  if (!bmp)
  {
    DEBUG_ERROR("Failed to render fps text");
    return;
  }

  egl_texture_setup(
    fps->texture,
    EGL_PF_BGRA,
    bmp->width ,
    bmp->height,
    bmp->width * bmp->height * bmp->bpp,
    false
  );

  egl_texture_update
  (
    fps->texture,
    bmp->pixels
  );

  fps->width  = bmp->width;
  fps->height = bmp->height;
  fps->ready  = true;

  fps->font->release(fps->fontObj, bmp);
}

void egl_fps_render(EGL_FPS * fps, float screenWidth, float screenHeight)
{
  if (!fps->ready)
    return;

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  egl_shader_use(fps->shader);
  glUniform2f(fps->uScreen, screenWidth, screenHeight);
  glUniform2f(fps->uSize  , fps->width , fps->height );
  egl_model_render(fps->model);
  glDisable(GL_BLEND);
}