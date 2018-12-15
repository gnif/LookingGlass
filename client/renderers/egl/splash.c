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

#include "splash.h"
#include "debug.h"
#include "utils.h"

#include "draw.h"
#include "texture.h"
#include "shader.h"
#include "model.h"

#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct EGL_Splash
{
  EGL_Shader * bgShader;
  EGL_Model  * bg;

  EGL_Shader * logoShader;
  EGL_Model  * logo;

  // uniforms
  GLint uBGAlpha;
  GLint uScale;
};

static const char vertex_bgShader[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
\
uniform float alpha;\
\
out highp vec3  pos; \
out highp float a; \
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.w   = 1.0; \
\
  pos = vertexPosition_modelspace; \
  a   = alpha; \
}\
";

static const char frag_bgShader[] = "\
#version 300 es\n\
\
in  highp vec3  pos;\
in  highp float a;\
out highp vec4  color;\
\
uniform sampler2D sampler1;\
\
void main()\
{\
  highp float d = 1.0 - sqrt(pos.x * pos.x + pos.y * pos.y) / 2.0; \
  color = vec4(0.234375 * d, 0.015625f * d, 0.425781f * d, a); \
}\
";

static const char vertex_logoShader[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
\
uniform vec2 scale;\
\
out highp float a; \
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.y  *= scale.y; \
  gl_Position.w   = 1.0; \
\
  a = scale.x; \
}\
";

static const char frag_logoShader[] = "\
#version 300 es\n\
\
out highp vec4 color;\
in  highp float a;\
\
uniform sampler2D sampler1;\
\
void main()\
{\
  color = vec4(1.0, 1.0, 1.0, a);\
}\
";

bool egl_splash_init(EGL_Splash ** splash)
{
  *splash = (EGL_Splash *)malloc(sizeof(EGL_Splash));
  if (!*splash)
  {
    DEBUG_ERROR("Failed to malloc EGL_Splash");
    return false;
  }

  memset(*splash, 0, sizeof(EGL_Splash));

  if (!egl_shader_init(&(*splash)->bgShader))
  {
    DEBUG_ERROR("Failed to initialize the splash bgShader");
    return false;
  }

  if (!egl_shader_compile((*splash)->bgShader,
        vertex_bgShader, sizeof(vertex_bgShader),
        frag_bgShader  , sizeof(frag_bgShader  )))
  {
    DEBUG_ERROR("Failed to compile the splash bgShader");
    return false;
  }

  (*splash)->uBGAlpha = egl_shader_get_uniform_location((*splash)->bgShader, "alpha");

  if (!egl_model_init(&(*splash)->bg))
  {
    DEBUG_ERROR("Failed to intiailize the splash bg model");
    return false;
  }

  egl_model_set_default((*splash)->bg);

  if (!egl_shader_init(&(*splash)->logoShader))
  {
    DEBUG_ERROR("Failed to initialize the splash logoShader");
    return false;
  }

  if (!egl_shader_compile((*splash)->logoShader,
        vertex_logoShader, sizeof(vertex_logoShader),
        frag_logoShader  , sizeof(frag_logoShader  )))
  {
    DEBUG_ERROR("Failed to compile the splash logoShader");
    return false;
  }

  (*splash)->uScale = egl_shader_get_uniform_location((*splash)->logoShader, "scale");

  if (!egl_model_init(&(*splash)->logo))
  {
    DEBUG_ERROR("Failed to intiailize the splash model");
    return false;
  }

  /* build the splash model */
  #define P(x) ((1.0f/800.0f)*(float)(x))
  egl_draw_torus_arc((*splash)->logo, 30, P( 0  ), P(0), P(102), P(98), 0.0f, -M_PI);
  egl_draw_torus    ((*splash)->logo, 30, P(-100), P(8), P(8  ), P(4 ));
  egl_draw_torus    ((*splash)->logo, 30, P( 100), P(8), P(8  ), P(4 ));

  egl_draw_torus    ((*splash)->logo, 60, P(0), P(0), P(83), P(79));
  egl_draw_torus    ((*splash)->logo, 60, P(0), P(0), P(67), P(63));

  static const GLfloat lines[][12] =
  {
    {
      P( -2), P(-140), 0.0f,
      P( -2), P(-100), 0.0f,
      P(  2), P(-140), 0.0f,
      P(  2), P(-100), 0.0f
    },
    {
      P(-26), P(-144), 0.0f,
      P(-26), P(-140), 0.0f,
      P( 26), P(-144), 0.0f,
      P( 26), P(-140), 0.0f
    },
    {
      P(-40), P(-156), 0.0f,
      P(-40), P(-152), 0.0f,
      P( 40), P(-156), 0.0f,
      P( 40), P(-152), 0.0f
    }
  };

  egl_model_add_verticies((*splash)->logo, lines[0], NULL, 4);
  egl_model_add_verticies((*splash)->logo, lines[1], NULL, 4);
  egl_model_add_verticies((*splash)->logo, lines[2], NULL, 4);

  egl_draw_torus_arc((*splash)->logo, 10, P(-26), P(-154), P(10), P(14), M_PI       , -M_PI / 2.0);
  egl_draw_torus_arc((*splash)->logo, 10, P( 26), P(-154), P(10), P(14), M_PI / 2.0f, -M_PI / 2.0);
  #undef P

  return true;
}

void egl_splash_free(EGL_Splash ** splash)
{
  if (!*splash)
    return;

  egl_model_free(&(*splash)->logo);

  free(*splash);
  *splash = NULL;
}

void egl_splash_render(EGL_Splash * splash, float alpha, float scaleY)
{
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  egl_shader_use(splash->bgShader);
  glUniform1f(splash->uBGAlpha, alpha);
  egl_model_render(splash->bg);

  egl_shader_use(splash->logoShader);
  glUniform2f(splash->uScale, alpha, scaleY);
  egl_model_render(splash->logo);

  glDisable(GL_BLEND);
}