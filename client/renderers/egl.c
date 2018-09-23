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

#include "lg-renderer.h"
#include "debug.h"

#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_egl.h>

#include "egl_model.h"
#include "egl_shader.h"
#include "egl_shader_progs.h"

struct Options
{
  bool vsync;
};

static struct Options defaultOptions =
{
  .vsync = true
};

struct Models
{
  struct EGL_Model * desktop;
};

struct Shaders
{
  struct EGL_Shader * desktop;
};

struct Textures
{
  struct EGL_Texture * desktop;
};

struct Inst
{
  LG_RendererParams params;
  struct Options    opt;

  Display         * xDisplay;
  Window            xWindow;
  EGLDisplay        display;
  EGLConfig         configs;
  EGLSurface        surface;
  EGLContext        context;

  struct Models     models;
  struct Shaders    shaders;
  struct Textures   textures;

  LG_RendererFormat  format;
  bool               sourceChanged;
  size_t             frameSize;
  const uint8_t    * data;
  bool               update;
};

const char * egl_get_name()
{
  return "EGL";
}

bool egl_create(void ** opaque, const LG_RendererParams params)
{
  // create our local storage
  *opaque = malloc(sizeof(struct Inst));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
    return false;
  }
  memset(*opaque, 0, sizeof(struct Inst));

  // safe off parameteres and init our default option values
  struct Inst * this = (struct Inst *)*opaque;
  memcpy(&this->params, &params        , sizeof(LG_RendererParams));
  memcpy(&this->opt   , &defaultOptions, sizeof(struct Options   ));

  return true;
}

bool egl_initialize(void * opaque, Uint32 * sdlFlags)
{
  *sdlFlags = SDL_WINDOW_OPENGL;
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER        , 1);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS  , 1);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES  , 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  return true;
}

void egl_deinitialize(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;

  egl_model_free  (&this->models  .desktop);
  egl_shader_free (&this->shaders .desktop);
  egl_texture_free(&this->textures.desktop);
  free(this);
}

void egl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  glViewport(0, 0, width, height);
}

bool egl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  return true;
}

bool egl_on_mouse_event(void * opaque, const bool visible , const int x, const int y)
{
  return true;
}

bool egl_on_frame_event(void * opaque, const LG_RendererFormat format, const uint8_t * data)
{
  struct Inst * this = (struct Inst *)opaque;
  if (format.type != FRAME_TYPE_ARGB)
    return false;

  this->sourceChanged = (
    this->sourceChanged ||
    this->format.type   != format.type   ||
    this->format.width  != format.width  ||
    this->format.height != format.height ||
    this->format.pitch  != format.pitch
  );

  if (this->sourceChanged)
  {
    memcpy(&this->format, &format, sizeof(LG_RendererFormat));
    this->frameSize = format.height * format.pitch;
  }

  this->data   = data;
  this->update = true;

  return true;
}

void egl_on_alert(void * opaque, const LG_RendererAlert alert, const char * message, bool ** closeFlag)
{
}

bool egl_render_startup(void * opaque, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;

  SDL_SysWMinfo wminfo;
  SDL_VERSION(&wminfo.version);
  if (!SDL_GetWindowWMInfo(window, &wminfo))
  {
    DEBUG_ERROR("SDL_GetWindowWMInfo failed");
    return false;
  }

  this->xDisplay = wminfo.info.x11.display;
  this->xWindow  = wminfo.info.x11.window;

  this->display = eglGetDisplay((EGLNativeDisplayType)this->xDisplay);
  if (this->display == EGL_NO_DISPLAY)
  {
    DEBUG_ERROR("eglGetDisplay failed");
    return false;
  }

  if (!eglInitialize(this->display, NULL, NULL))
  {
    DEBUG_ERROR("Unable to initialize EGL");
    return false;
  }

  EGLint attr[] =
  {
    EGL_BUFFER_SIZE, 16,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };

  EGLint num_config;
  if (!eglChooseConfig(this->display, attr, &this->configs, 1, &num_config))
  {
    DEBUG_ERROR("Failed to choose config (eglError: 0x%x)", eglGetError());
    return false;
  }

  this->surface = eglCreateWindowSurface(this->display, this->configs, this->xWindow, NULL);
  if (this->surface == EGL_NO_SURFACE)
  {
    DEBUG_ERROR("Failed to create EGL surface (eglError: 0x%x)", eglGetError());
    return false;
  }

  EGLint ctxattr[] =
  {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };

  this->context = eglCreateContext(this->display, this->configs, EGL_NO_CONTEXT, ctxattr);
  if (this->context == EGL_NO_CONTEXT)
  {
    DEBUG_ERROR("Failed to create EGL context (eglError: 0x%x)", eglGetError());
    return false;
  }

  eglMakeCurrent(this->display, this->surface, this->surface, this->context);

  DEBUG_INFO("Vendor  : %s", glGetString(GL_VENDOR  ));
  DEBUG_INFO("Renderer: %s", glGetString(GL_RENDERER));
  DEBUG_INFO("Version : %s", glGetString(GL_VERSION ));

  static const GLfloat desktop[] =
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

  if (!egl_shader_init(&this->shaders.desktop))
    return false;

  if (!egl_shader_compile(this->shaders.desktop,
        egl_vertex_shader_basic, sizeof(egl_vertex_shader_basic),
        egl_fragment_shader_bgra, sizeof(egl_fragment_shader_bgra)
      ))
    return false;

  if (!egl_texture_init(&this->textures.desktop))
    return false;

  if (!egl_model_init(&this->models.desktop))
    return false;

  egl_model_set_verticies(this->models.desktop, desktop, sizeof(desktop) / sizeof(GLfloat));
  egl_model_set_uvs      (this->models.desktop, uvs    , sizeof(uvs    ) / sizeof(GLfloat));
  egl_model_set_shader   (this->models.desktop, this->shaders .desktop);
  egl_model_set_texture  (this->models.desktop, this->textures.desktop);

  eglSwapInterval(this->display, this->opt.vsync ? 1 : 0);
  return true;
}

bool egl_render(void * opaque, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;

  if (this->update)
  {
    if (this->sourceChanged)
    {
      this->sourceChanged = false;
      egl_texture_init_streaming(
        this->textures.desktop,
        this->format.width,
        this->format.height,
        this->frameSize
      );
    }

    egl_texture_stream_buffer(
      this->textures.desktop,
      this->data,
      this->frameSize
    );

    this->update = false;
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  egl_model_render(this->models.desktop);

  eglSwapBuffers(this->display, this->surface);
  return true;
}

static void handle_opt_vsync(void * opaque, const char *value)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  this->opt.vsync = LG_RendererValueToBool(value);
}

static LG_RendererOpt egl_options[] =
{
  {
    .name      = "vsync",
    .desc      ="Enable or disable vsync [default: enabled]",
    .validator = LG_RendererValidatorBool,
    .handler   = handle_opt_vsync
  }
};

struct LG_Renderer LGR_EGL =
{
  .create         = egl_create,
  .get_name       = egl_get_name,
  .options        = egl_options,
  .option_count   = LGR_OPTION_COUNT(egl_options),
  .initialize     = egl_initialize,
  .deinitialize   = egl_deinitialize,
  .on_resize      = egl_on_resize,
  .on_mouse_shape = egl_on_mouse_shape,
  .on_mouse_event = egl_on_mouse_event,
  .on_frame_event = egl_on_frame_event,
  .on_alert       = egl_on_alert,
  .render_startup = egl_render_startup,
  .render         = egl_render
};