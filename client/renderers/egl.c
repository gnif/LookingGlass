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
  struct EGL_Model * mouse;
  struct EGL_Model * mouse_mono;
};

struct Shaders
{
  struct EGL_Shader * rgba;
  struct EGL_Shader * bgra;
  struct EGL_Shader * yuv;

  struct EGL_Shader * mouse;
  struct EGL_Shader * mouse_mono;
};

struct Textures
{
  struct EGL_Texture * desktop;
  struct EGL_Texture * mouse;
  struct EGL_Texture * mouse_mono;
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

  LG_RendererFormat    format;
  enum EGL_PixelFormat pixFmt;
  EGL_Shader         * shader;
  bool                 sourceChanged;
  size_t               frameSize;
  const uint8_t      * data;
  bool                 update;

  bool         mouseVisible;
  float        mouseX, mouseY, mouseW, mouseH;
  float        mouseScaleX, mouseScaleY;
  GLint        uMousePos, uMousePosMono;

  LG_Lock           mouseLock;
  LG_RendererCursor mouseCursor;
  int               mouseWidth;
  int               mouseHeight;
  int               mousePitch;
  uint8_t *         mouseData;
  size_t            mouseDataSize;
  bool              mouseUpdate;
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

  LG_LOCK_INIT(this->mouseLock);

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

  egl_model_free  (&this->models  .desktop   );
  egl_model_free  (&this->models  .mouse     );
  egl_model_free  (&this->models  .mouse_mono);
  egl_shader_free (&this->shaders .rgba      );
  egl_shader_free (&this->shaders .bgra      );
  egl_shader_free (&this->shaders .yuv       );
  egl_shader_free (&this->shaders .mouse     );
  egl_shader_free (&this->shaders .mouse_mono);
  egl_texture_free(&this->textures.desktop   );
  egl_texture_free(&this->textures.mouse     );
  egl_texture_free(&this->textures.mouse_mono);

  LG_LOCK_FREE(this->mouseLock);
  if (this->mouseData)
    free(this->mouseData);

  free(this);
}

void egl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct Inst * this = (struct Inst *)opaque;

  glViewport(0, 0, width, height);

  this->mouseScaleX = 2.0f / this->format.width;
  this->mouseScaleY = 2.0f / this->format.height;
  this->mouseW = this->mouseWidth  * (1.0f / this->format.width );
  this->mouseH = this->mouseHeight * (1.0f / this->format.height);
}

bool egl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  struct Inst * this = (struct Inst *)opaque;

  LG_LOCK(this->mouseLock);
  this->mouseCursor = cursor;
  this->mouseWidth  = width;
  this->mouseHeight = (cursor == LG_CURSOR_MONOCHROME ? height / 2 : height);
  this->mousePitch  = pitch;

  this->mouseW = this->mouseWidth  * (1.0f / this->format.width );
  this->mouseH = this->mouseHeight * (1.0f / this->format.height);

  const size_t size = height * pitch;
  if (size > this->mouseDataSize)
  {
    if (this->mouseData)
      free(this->mouseData);
    this->mouseData     = (uint8_t *)malloc(size);
    this->mouseDataSize = size;
  }

  memcpy(this->mouseData, data, size);
  this->mouseUpdate = true;
  LG_UNLOCK(this->mouseLock);

  return true;
}

bool egl_on_mouse_event(void * opaque, const bool visible , const int x, const int y)
{
  struct Inst * this = (struct Inst *)opaque;
  this->mouseVisible = visible;
  this->mouseX       = ((float)x * this->mouseScaleX) - 1.0f;
  this->mouseY       = ((float)y * this->mouseScaleY) - 1.0f;
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
  {
    memcpy(&this->format, &format, sizeof(LG_RendererFormat));

    switch(format.type)
    {
      case FRAME_TYPE_ARGB:
        this->pixFmt    = EGL_PF_RGBA;
        this->shader    = this->shaders.rgba;
        this->frameSize = format.height * format.pitch;
        break;

      case FRAME_TYPE_YUV420:
        this->pixFmt    = EGL_PF_YUV420;
        this->shader    = this->shaders.yuv;
        this->frameSize = format.width * format.height * 3 / 2;
        break;

      default:
        return false;
    }
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

  if (!egl_shader_init(&this->shaders.rgba))
    return false;

  if (!egl_shader_init(&this->shaders.bgra))
    return false;

  if (!egl_shader_init(&this->shaders.yuv))
    return false;

  if (!egl_shader_init(&this->shaders.mouse))
    return false;

  if (!egl_shader_init(&this->shaders.mouse_mono))
    return false;

  if (!egl_shader_compile(this->shaders.rgba, egl_vertex_shader_basic, sizeof(egl_vertex_shader_basic), egl_fragment_shader_rgba, sizeof(egl_fragment_shader_rgba)))
    return false;

  if (!egl_shader_compile(this->shaders.bgra, egl_vertex_shader_basic, sizeof(egl_vertex_shader_basic), egl_fragment_shader_bgra, sizeof(egl_fragment_shader_bgra)))
    return false;

  if (!egl_shader_compile(this->shaders.yuv, egl_vertex_shader_basic, sizeof(egl_vertex_shader_basic), egl_fragment_shader_yuv , sizeof(egl_fragment_shader_yuv )))
    return false;

  if (!egl_shader_compile(this->shaders.mouse, egl_vertex_shader_mouse, sizeof(egl_vertex_shader_mouse), egl_fragment_shader_rgba, sizeof(egl_fragment_shader_rgba)))
    return false;

  if (!egl_shader_compile(this->shaders.mouse_mono, egl_vertex_shader_mouse, sizeof(egl_vertex_shader_mouse), egl_fragment_shader_mouse_mono, sizeof(egl_fragment_shader_mouse_mono)))
    return false;

  this->uMousePos     = egl_shader_get_uniform_location(this->shaders.mouse     , "mouse");
  this->uMousePosMono = egl_shader_get_uniform_location(this->shaders.mouse_mono, "mouse");

  if (!egl_texture_init(&this->textures.desktop))
    return false;

  if (!egl_texture_init(&this->textures.mouse))
    return false;

  if (!egl_texture_init(&this->textures.mouse_mono))
    return false;

  if (!egl_model_init(&this->models.desktop))
    return false;

  if (!egl_model_init(&this->models.mouse))
    return false;

  if (!egl_model_init(&this->models.mouse_mono))
    return false;

  egl_model_set_verticies(this->models.desktop, square , sizeof(square) / sizeof(GLfloat));
  egl_model_set_uvs      (this->models.desktop, uvs    , sizeof(uvs   ) / sizeof(GLfloat));
  egl_model_set_texture  (this->models.desktop, this->textures.desktop);

  egl_model_set_verticies(this->models.mouse     , square , sizeof(square) / sizeof(GLfloat));
  egl_model_set_uvs      (this->models.mouse     , uvs    , sizeof(uvs   ) / sizeof(GLfloat));
  egl_model_set_verticies(this->models.mouse_mono, square , sizeof(square) / sizeof(GLfloat));
  egl_model_set_uvs      (this->models.mouse_mono, uvs    , sizeof(uvs   ) / sizeof(GLfloat));

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
      if (!egl_texture_setup(
        this->textures.desktop,
        this->pixFmt,
        this->format.width,
        this->format.height,
        this->frameSize,
        true
      ))
        return false;

      egl_model_set_shader(this->models.desktop, this->shader);
   }

    if (!egl_texture_update(this->textures.desktop, this->data))
      return false;

    this->update = false;
  }

  if (this->mouseUpdate)
    update_mouse_shape(this);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  egl_model_render(this->models.desktop);

  if (this->mouseVisible)
  {
    egl_shader_use(this->shaders.mouse);
    glUniform4f(this->uMousePos, this->mouseX, this->mouseY, this->mouseW, this->mouseH);
    if (this->mouseCursor == LG_CURSOR_MONOCHROME)
    {
      glEnable(GL_BLEND);
      glBlendFunc(GL_ZERO, GL_SRC_COLOR);
      egl_model_set_texture(this->models.mouse, this->textures.mouse);
      egl_model_render(this->models.mouse);

      egl_shader_use(this->shaders.mouse_mono);
      glUniform4f(this->uMousePosMono, this->mouseX, this->mouseY, this->mouseW, this->mouseH);
      glBlendFunc(GL_ZERO, GL_ONE_MINUS_DST_COLOR);
      egl_model_set_texture(this->models.mouse, this->textures.mouse_mono);
      egl_model_render(this->models.mouse);
      glDisable(GL_BLEND);
    }
    else
    {
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
      egl_model_render(this->models.mouse);
      glDisable(GL_BLEND);
    }
  }

  eglSwapBuffers(this->display, this->surface);
  return true;
}

void update_mouse_shape(struct Inst * this)
{
  LG_LOCK(this->mouseLock);
  this->mouseUpdate = false;

  LG_RendererCursor cursor = this->mouseCursor;
  int               width  = this->mouseWidth;
  int               height = this->mouseHeight;
  int               pitch  = this->mousePitch;
  const uint8_t *   data   = this->mouseData;

  // tmp buffer for masked colour
  uint32_t tmp[width * height];

  switch(cursor)
  {
    case LG_CURSOR_MASKED_COLOR:
    {
      for(int i = 0; i < width * height; ++i)
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
      egl_texture_setup(this->textures.mouse, EGL_PF_BGRA, width, height, width * height * 4, false);
      egl_texture_update(this->textures.mouse, data);
      egl_model_set_texture(this->models.mouse, this->textures.mouse);
      break;
    }

    case LG_CURSOR_MONOCHROME:
    {
      uint32_t and[width * height];
      uint32_t xor[width * height];

      for(int y = 0; y < height; ++y)
        for(int x = 0; x < width; ++x)
        {
          const uint8_t  * srcAnd  = data + (pitch * y) + (x / 8);
          const uint8_t  * srcXor  = srcAnd + pitch * height;
          const uint8_t    mask    = 0x80 >> (x % 8);
          const uint32_t   andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
          const uint32_t   xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;

          and[y * width + x] = andMask;
          xor[y * width + x] = xorMask;
        }

      egl_texture_setup(this->textures.mouse     , EGL_PF_BGRA, width, height, width * height * 4, false);
      egl_texture_setup(this->textures.mouse_mono, EGL_PF_BGRA, width, height, width * height * 4, false);
      egl_texture_update(this->textures.mouse     , (uint8_t *)and);
      egl_texture_update(this->textures.mouse_mono, (uint8_t *)xor);
      break;
    }
  }

  LG_UNLOCK(this->mouseLock);
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