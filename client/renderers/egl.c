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
#include "utils.h"
#include "lg-fonts.h"

#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_egl.h>

#include "egl/model.h"
#include "egl/shader.h"
#include "egl/desktop.h"
#include "egl/cursor.h"
#include "egl/fps.h"

struct Options
{
  bool vsync;
};

static struct Options defaultOptions =
{
  .vsync = false
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

  EGL_Desktop     * desktop; // the desktop
  EGL_Cursor      * cursor;  // the mouse cursor
  EGL_FPS         * fps;     // the fps display

  LG_RendererFormat    format;
  bool                 sourceChanged;

  int             width, height;
  LG_RendererRect destRect;

  float translateX, translateY;
  float scaleX    , scaleY;

  float        mouseWidth , mouseHeight;
  float        mouseScaleX, mouseScaleY;

  const LG_Font     * font;
  LG_FontObj        fontObj;
};


void update_mouse_shape(struct Inst * this);

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

  this->translateX = 0;
  this->translateY = 0;
  this->scaleX     = 1.0f;
  this->scaleY     = 1.0f;

  this->font = LG_Fonts[0];
  if (!this->font->create(&this->fontObj, NULL, 14))
  {
    DEBUG_ERROR("Failed to create a font instance");
    return false;
  }

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

  if (this->font && this->fontObj)
    this->font->destroy(this->fontObj);

  egl_desktop_free(&this->desktop);
  egl_cursor_free (&this->cursor);
  egl_fps_free    (&this->fps   );

  free(this);
}

void egl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct Inst * this = (struct Inst *)opaque;

  this->width  = width;
  this->height = height;
  memcpy(&this->destRect, &destRect, sizeof(LG_RendererRect));

  glViewport(0, 0, width, height);

  if (destRect.valid)
  {
    this->translateX = 1.0f - (((destRect.w / 2) + destRect.x) * 2) / (float)width;
    this->translateY = 1.0f - (((destRect.h / 2) + destRect.y) * 2) / (float)height;
    this->scaleX     = (float)destRect.w / (float)width;
    this->scaleY     = (float)destRect.h / (float)height;
  }

  this->mouseScaleX = 2.0f / this->format.width ;
  this->mouseScaleY = 2.0f / this->format.height;
  egl_cursor_set_size(this->cursor,
    (this->mouseWidth  * (1.0f / this->format.width )) * this->scaleX,
    (this->mouseHeight * (1.0f / this->format.height)) * this->scaleY
  );
}

bool egl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!egl_cursor_set_shape(this->cursor, cursor, width, height, pitch, data))
  {
    DEBUG_ERROR("Failed to update the cursor shape");
    return false;
  }

  this->mouseWidth  = width;
  this->mouseHeight = height;
  egl_cursor_set_size(this->cursor,
    (this->mouseWidth  * (1.0f / this->format.width )) * this->scaleX,
    (this->mouseHeight * (1.0f / this->format.height)) * this->scaleY
  );

  return true;
}

bool egl_on_mouse_event(void * opaque, const bool visible , const int x, const int y)
{
  struct Inst * this = (struct Inst *)opaque;

  egl_cursor_set_state(
    this->cursor,
    visible,
    (((float)x * this->mouseScaleX) - 1.0f) * this->scaleX,
    (((float)y * this->mouseScaleY) - 1.0f) * this->scaleY
  );

  return true;
}

bool egl_on_frame_event(void * opaque, const LG_RendererFormat format, const uint8_t * data)
{
  struct Inst * this = (struct Inst *)opaque;
  this->sourceChanged = (
    this->sourceChanged ||
    this->format.type   != format.type   ||
    this->format.width  != format.width  ||
    this->format.height != format.height ||
    this->format.pitch  != format.pitch
  );

  if (this->sourceChanged)
    memcpy(&this->format, &format, sizeof(LG_RendererFormat));

  if (!egl_desktop_prepare_update(this->desktop, this->sourceChanged, format, data))
  {
    DEBUG_INFO("Failed to prepare to update the desktop");
    return false;
  }

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
    EGL_BUFFER_SIZE    , 16,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
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

  eglSwapInterval(this->display, this->opt.vsync ? 1 : 0);

  if (!egl_desktop_init(&this->desktop))
  {
    DEBUG_ERROR("Failed to initialize the desktop");
    return false;
  }

  if (!egl_cursor_init(&this->cursor))
  {
    DEBUG_ERROR("Failed to initialize the cursor");
    return false;
  }

  if (!egl_fps_init(&this->fps, this->font, this->fontObj))
  {
    DEBUG_ERROR("Failed to initialize the FPS display");
    return false;
  }

  return true;
}

bool egl_render(void * opaque, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  egl_desktop_render(this->desktop, this->translateX, this->translateY, this->scaleX, this->scaleY);
  egl_cursor_render(this->cursor);
  egl_fps_render(this->fps, this->width, this->height);

  eglSwapBuffers(this->display, this->surface);

  // defer texture uploads until after the flip to avoid stalling
  if (!egl_desktop_perform_update(this->desktop, this->sourceChanged))
  {
    DEBUG_ERROR("Failed to perform the desktop update");
    return false;
  }
  this->sourceChanged = false;

  return true;
}

void egl_update_fps(void * opaque, const float avgFPS, const float renderFPS)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this->params.showFPS)
    return;

  egl_fps_update(this->fps, avgFPS, renderFPS);
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
  .render         = egl_render,
  .update_fps     = egl_update_fps
};