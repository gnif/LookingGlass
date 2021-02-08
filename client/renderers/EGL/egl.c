/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "interface/renderer.h"

#include "common/debug.h"
#include "common/option.h"
#include "common/sysinfo.h"
#include "common/time.h"
#include "common/locking.h"
#include "utils.h"
#include "dynamic/fonts.h"

#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_egl.h>

#if defined(SDL_VIDEO_DRIVER_WAYLAND)
#include <wayland-egl.h>
#endif

#include <assert.h>

#include "app.h"
#include "dynprocs.h"
#include "model.h"
#include "shader.h"
#include "desktop.h"
#include "cursor.h"
#include "fps.h"
#include "splash.h"
#include "alert.h"

#define SPLASH_FADE_TIME 1000000
#define ALERT_TIMEOUT    2000000

struct Options
{
  bool vsync;
  bool doubleBuffer;
};

struct Inst
{
  bool dmaSupport;
  LG_RendererParams params;
  struct Options    opt;

  EGLNativeWindowType  nativeWind;
  EGLDisplay           display;
  EGLConfig            configs;
  EGLSurface           surface;
  EGLContext           context, frameContext;

  EGL_Desktop     * desktop; // the desktop
  EGL_Cursor      * cursor;  // the mouse cursor
  EGL_FPS         * fps;     // the fps display
  EGL_Splash      * splash;  // the splash screen
  EGL_Alert       * alert;   // the alert display

  LG_RendererFormat    format;
  bool                 formatValid;
  bool                 start;
  uint64_t             waitFadeTime;
  bool                 waitDone;

  bool     showAlert;
  uint64_t alertTimeout;
  bool     useCloseFlag;
  bool     closeFlag;

  int               width, height;
  LG_RendererRect   destRect;
  LG_RendererRotate rotate; //client side rotation

  float translateX  , translateY;
  float scaleX      , scaleY;
  float splashRatio;
  float screenScaleX, screenScaleY;
  bool  useNearest;

  bool         cursorVisible;
  int          cursorX    , cursorY;
  float        mouseWidth , mouseHeight;
  float        mouseScaleX, mouseScaleY;

  const LG_Font     * font;
  LG_FontObj        fontObj;
};

static struct Option egl_options[] =
{
  {
    .module       = "egl",
    .name         = "vsync",
    .description  = "Enable vsync",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = false,
  },
  {
    .module       = "egl",
    .name         = "doubleBuffer",
    .description  = "Enable double buffering",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = false
  },
  {
    .module       = "egl",
    .name         = "multisample",
    .description  = "Enable Multisampling",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {
    .module       = "egl",
    .name         = "nvGainMax",
    .description  = "The maximum night vision gain",
    .type         = OPTION_TYPE_INT,
    .value.x_int  = 1
  },
  {
    .module       = "egl",
    .name         = "nvGain",
    .description  = "The initial night vision gain at startup",
    .type         = OPTION_TYPE_INT,
    .value.x_int  = 0
  },
  {
    .module       = "egl",
    .name         = "cbMode",
    .description  = "Color Blind Mode (0 = Off, 1 = Protanope, 2 = Deuteranope, 3 = Tritanope)",
    .type         = OPTION_TYPE_INT,
    .value.x_int  = 0
  },
  {0}
};

void update_mouse_shape(struct Inst * this);

const char * egl_get_name(void)
{
  return "EGL";
}

void egl_setup(void)
{
  option_register(egl_options);
}

bool egl_create(void ** opaque, const LG_RendererParams params)
{
  // Fail if running on Wayland so that OpenGL is used instead. Wayland-EGL
  // is broken (https://github.com/gnif/LookingGlass/issues/306) and isn't
  // fixable until SDL is dropped entirely. Until then, the OpenGL renderer
  // "mostly works".
  if (getenv("WAYLAND_DISPLAY"))
    return false;

  // check if EGL is even available
  if (!eglQueryString(EGL_NO_DISPLAY, EGL_VERSION))
    return false;

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
  memcpy(&this->params, &params, sizeof(LG_RendererParams));

  this->opt.vsync        = option_get_bool("egl", "vsync");
  this->opt.doubleBuffer = option_get_bool("egl", "doubleBuffer");

  this->translateX   = 0;
  this->translateY   = 0;
  this->scaleX       = 1.0f;
  this->scaleY       = 1.0f;
  this->screenScaleX = 1.0f;
  this->screenScaleY = 1.0f;

  this->font = LG_Fonts[0];
  if (!this->font->create(&this->fontObj, NULL, 16))
  {
    DEBUG_ERROR("Failed to create a font instance");
    return false;
  }

  return true;
}

bool egl_initialize(void * opaque, Uint32 * sdlFlags)
{
  struct Inst * this = (struct Inst *)opaque;
  DEBUG_INFO("Double buffering is %s", this->opt.doubleBuffer ? "on" : "off");
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
  egl_splash_free (&this->splash);
  egl_alert_free  (&this->alert );

  LG_LOCK_FREE(this->lock);

  eglMakeCurrent(this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (this->frameContext)
    eglDestroyContext(this->display, this->frameContext);

  if (this->context)
    eglDestroyContext(this->display, this->context);

  eglTerminate(this->display);

  free(this);
}

bool egl_supports(void * opaque, LG_RendererSupport flag)
{
  struct Inst * this = (struct Inst *)opaque;

  switch(flag)
  {
    case LG_SUPPORTS_DMABUF:
      return this->dmaSupport;

    default:
      return false;
  }
}

void egl_on_restart(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;

  eglDestroyContext(this->display, this->frameContext);
  this->frameContext = NULL;
  this->start        = false;
}

static void egl_calc_mouse_size(struct Inst * this)
{
  if (!this->formatValid)
    return;

  int w, h;
  switch(this->format.rotate)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      this->mouseScaleX = 2.0f / this->format.width;
      this->mouseScaleY = 2.0f / this->format.height;
      w = this->format.width;
      h = this->format.height;
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      this->mouseScaleX = 2.0f / this->format.height;
      this->mouseScaleY = 2.0f / this->format.width;
      w = this->format.height;
      h = this->format.width;
      break;

    default:
      assert(!"unreachable");
  }

  switch((this->format.rotate + this->rotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      egl_cursor_set_size(this->cursor,
        (this->mouseWidth  * (1.0f / w)) * this->scaleX,
        (this->mouseHeight * (1.0f / h)) * this->scaleY
      );
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      egl_cursor_set_size(this->cursor,
        (this->mouseWidth  * (1.0f / w)) * this->scaleY,
        (this->mouseHeight * (1.0f / h)) * this->scaleX
      );
      break;
  }
}

static void egl_calc_mouse_state(struct Inst * this)
{
  if (!this->formatValid)
    return;

  switch((this->format.rotate + this->rotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      egl_cursor_set_state(
        this->cursor,
        this->cursorVisible,
        (((float)this->cursorX * this->mouseScaleX) - 1.0f) * this->scaleX,
        (((float)this->cursorY * this->mouseScaleY) - 1.0f) * this->scaleY
      );
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      egl_cursor_set_state(
        this->cursor,
        this->cursorVisible,
        (((float)this->cursorX * this->mouseScaleX) - 1.0f) * this->scaleY,
        (((float)this->cursorY * this->mouseScaleY) - 1.0f) * this->scaleX
      );
      break;
  }
}

void egl_on_resize(void * opaque, const int width, const int height,
    const LG_RendererRect destRect, LG_RendererRotate rotate)
{
  struct Inst * this = (struct Inst *)opaque;

  this->width  = width;
  this->height = height;
  this->rotate = rotate;

  memcpy(&this->destRect, &destRect, sizeof(LG_RendererRect));

  glViewport(0, 0, width, height);

  if (destRect.valid)
  {
    this->translateX = 1.0f - (((destRect.w / 2) + destRect.x) * 2) / (float)width;
    this->translateY = 1.0f - (((destRect.h / 2) + destRect.y) * 2) / (float)height;
    this->scaleX     = (float)destRect.w / (float)width;
    this->scaleY     = (float)destRect.h / (float)height;
  }

  egl_calc_mouse_size(this);

  this->splashRatio  = (float)width / (float)height;
  this->screenScaleX = 1.0f / width;
  this->screenScaleY = 1.0f / height;

  egl_calc_mouse_state(this);
}

bool egl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor,
    const int width, const int height,
    const int pitch, const uint8_t * data)
{
  struct Inst * this = (struct Inst *)opaque;

  if (!egl_cursor_set_shape(this->cursor, cursor, width, height, pitch, data))
  {
    DEBUG_ERROR("Failed to update the cursor shape");
    return false;
  }

  this->mouseWidth  = width;
  this->mouseHeight = height;
  egl_calc_mouse_size(this);

  return true;
}

bool egl_on_mouse_event(void * opaque, const bool visible, const int x, const int y)
{
  struct Inst * this = (struct Inst *)opaque;
  this->cursorVisible = visible;
  this->cursorX       = x;
  this->cursorY       = y;
  egl_calc_mouse_state(this);
  return true;
}

bool egl_on_frame_format(void * opaque, const LG_RendererFormat format, bool useDMA)
{
  struct Inst * this = (struct Inst *)opaque;
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  this->formatValid = true;

  /* this event runs in a second thread so we need to init it here */
  if (!this->frameContext)
  {
    static EGLint attrs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE
    };

    if (!(this->frameContext = eglCreateContext(this->display, this->configs, this->context, attrs)))
    {
      DEBUG_ERROR("Failed to create the frame context");
      return false;
    }

    if (!eglMakeCurrent(this->display, EGL_NO_SURFACE, EGL_NO_SURFACE, this->frameContext))
    {
      DEBUG_ERROR("Failed to make the frame context current");
      return false;
    }
  }

  this->useNearest = this->width < format.width || this->height < format.height;
  return egl_desktop_setup(this->desktop, format, useDMA);
}

bool egl_on_frame(void * opaque, const FrameBuffer * frame, int dmaFd)
{
  struct Inst * this = (struct Inst *)opaque;

  if (!egl_desktop_update(this->desktop, frame, dmaFd))
  {
    DEBUG_INFO("Failed to to update the desktop");
    return false;
  }

  this->start = true;
  return true;
}

void egl_on_alert(void * opaque, const LG_MsgAlert alert, const char * message, bool ** closeFlag)
{
  struct Inst * this = (struct Inst *)opaque;

  static const uint32_t colors[] =
  {
    0x0000CCCC, // LG_ALERT_INFO
    0x00CC00CC, // LG_ALERT_SUCCESS
    0xCC7F00CC, // LG_ALERT_WARNING
    0xFF0000CC  // LG_ALERT_ERROR
  };

  if (alert > LG_ALERT_ERROR || alert < 0)
  {
    DEBUG_ERROR("Invalid alert value");
    return;
  }

  egl_alert_set_color(this->alert, colors[alert]);
  egl_alert_set_text (this->alert, message      );

  if (closeFlag)
  {
    this->useCloseFlag = true;
    *closeFlag = &this->closeFlag;
  }
  else
  {
    this->useCloseFlag = false;
    this->alertTimeout = microtime() + ALERT_TIMEOUT;
  }

  this->showAlert = true;
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

  egl_dynProcsInit();

  EGLNativeDisplayType native;
  EGLenum platform;

  switch(wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      native           = (EGLNativeDisplayType)wminfo.info.x11.display;
      platform         = EGL_PLATFORM_X11_KHR;
      this->nativeWind = (EGLNativeWindowType)wminfo.info.x11.window;
      break;

#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
    {
      int width, height;
      SDL_GetWindowSize(window, &width, &height);
      native           = (EGLNativeDisplayType)wminfo.info.wl.display;
      platform         = EGL_PLATFORM_WAYLAND_KHR;
      this->nativeWind = (EGLNativeWindowType)wl_egl_window_create(
          wminfo.info.wl.surface, width, height);
      break;
    }
#endif

    default:
      DEBUG_ERROR("Unsupported subsystem");
      return false;
  }

  const char *early_exts = eglQueryString(NULL, EGL_EXTENSIONS);
  if (strstr(early_exts, "EGL_KHR_platform_base") != NULL &&
      g_dynprocs.eglGetPlatformDisplay)
  {
    DEBUG_INFO("Using eglGetPlatformDisplay");
    this->display = g_dynprocs.eglGetPlatformDisplay(platform, native, NULL);
  }
  else if (strstr(early_exts, "EGL_EXT_platform_base") != NULL &&
      g_dynprocs.eglGetPlatformDisplayEXT)
  {
    DEBUG_INFO("Using eglGetPlatformDisplayEXT");
    this->display = g_dynprocs.eglGetPlatformDisplayEXT(platform, native, NULL);
  }
  else
  {
    DEBUG_INFO("Using eglGetDisplay");
    this->display = eglGetDisplay(native);
  }

  if (this->display == EGL_NO_DISPLAY)
  {
    DEBUG_ERROR("eglGetDisplay failed");
    return false;
  }

  int maj, min;
  if (!eglInitialize(this->display, &maj, &min))
  {
    DEBUG_ERROR("Unable to initialize EGL");
    return false;
  }

  int maxSamples = 1;
  if (option_get_bool("egl", "multisample"))
  {
    if (app_getProp(LG_DS_MAX_MULTISAMPLE, &maxSamples) && maxSamples > 1)
    {
      if (maxSamples > 4)
        maxSamples = 4;

      DEBUG_INFO("Multisampling enabled, max samples: %d", maxSamples);
    }
  }

  EGLint attr[] =
  {
    EGL_BUFFER_SIZE    , 32,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SAMPLE_BUFFERS , maxSamples > 0 ? 1 : 0,
    EGL_SAMPLES        , maxSamples,
    EGL_NONE
  };

  EGLint num_config;
  if (!eglChooseConfig(this->display, attr, &this->configs, 1, &num_config))
  {
    DEBUG_ERROR("Failed to choose config (eglError: 0x%x)", eglGetError());
    return false;
  }

  const EGLint surfattr[] =
  {
    EGL_RENDER_BUFFER, this->opt.doubleBuffer ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER,
    EGL_NONE
  };

  this->surface = eglCreateWindowSurface(this->display, this->configs, this->nativeWind, surfattr);
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

  EGLint rb = 0;
  eglQuerySurface(this->display, this->surface, EGL_RENDER_BUFFER, &rb);
  switch(rb)
  {
    case EGL_SINGLE_BUFFER:
      DEBUG_INFO("Single buffer mode");
      break;

    case EGL_BACK_BUFFER:
      DEBUG_INFO("Back buffer mode");
      break;

    default:
      DEBUG_WARN("Unknown render buffer mode: %d", rb);
      break;
  }

  eglMakeCurrent(this->display, this->surface, this->surface, this->context);
  const char *client_exts = eglQueryString(this->display, EGL_EXTENSIONS);
  const char *vendor      = (const char *)glGetString(GL_VENDOR);

  DEBUG_INFO("EGL       : %d.%d", maj, min);
  DEBUG_INFO("Vendor    : %s", vendor);
  DEBUG_INFO("Renderer  : %s", glGetString(GL_RENDERER));
  DEBUG_INFO("Version   : %s", glGetString(GL_VERSION ));
  DEBUG_INFO("EGL APIs  : %s", eglQueryString(this->display, EGL_CLIENT_APIS));
  DEBUG_INFO("Extensions: %s", client_exts);

  if (g_dynprocs.glEGLImageTargetTexture2DOES)
  {
    if (strstr(client_exts, "EGL_EXT_image_dma_buf_import") != NULL)
    {
      /*
       * As of version 455.45.01 NVidia started advertising support for this
       * feature, however even on the latest version 460.27.04 this is still
       * broken and does not work, until this is fixed and we have way to detect
       * this early just disable dma for all NVIDIA devices.
       *
       * ref: https://forums.developer.nvidia.com/t/egl-ext-image-dma-buf-import-broken-egl-bad-alloc-with-tons-of-free-ram/165552
       */
      if (strstr(vendor, "NVIDIA") != NULL)
        DEBUG_WARN("NVIDIA driver detected, ignoring broken DMA support");
      else
        this->dmaSupport = true;
    }
  }
  else
  {
    DEBUG_INFO("glEGLImageTargetTexture2DOES unavilable, DMA support disabled");
  }

  eglSwapInterval(this->display, this->opt.vsync ? 1 : 0);

  if (!egl_desktop_init(&this->desktop, this->display))
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

  if (!egl_splash_init(&this->splash))
  {
    DEBUG_ERROR("Failed to initialize the splash screen");
    return false;
  }

  if (!egl_alert_init(&this->alert, this->font, this->fontObj))
  {
    DEBUG_ERROR("Failed to initialize the alert display");
    return false;
  }

  return true;
}

bool egl_render(void * opaque, SDL_Window * window, LG_RendererRotate rotate)
{
  struct Inst * this = (struct Inst *)opaque;

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (this->start && egl_desktop_render(this->desktop,
        this->translateX, this->translateY,
        this->scaleX    , this->scaleY    ,
        this->useNearest,
        rotate))
  {
    if (!this->waitFadeTime)
    {
      if (!this->params.quickSplash)
        this->waitFadeTime = microtime() + SPLASH_FADE_TIME;
      else
        this->waitDone = true;
    }

    egl_cursor_render(this->cursor,
        (this->format.rotate + rotate) % LG_ROTATE_MAX);
  }

  if (!this->waitDone)
  {
    float a = 1.0f;
    if (!this->waitFadeTime)
      a = 1.0f;
    else
    {
      uint64_t t = microtime();
      if (t > this->waitFadeTime)
        this->waitDone = true;
      else
      {
        uint64_t delta = this->waitFadeTime - t;
        a = 1.0f / SPLASH_FADE_TIME * delta;
      }
    }

    if (!this->waitDone)
      egl_splash_render(this->splash, a, this->splashRatio);
  }
  else
  {
    if (!this->start)
      egl_splash_render(this->splash, 1.0f, this->splashRatio);
  }

  if (this->showAlert)
  {
    bool close = false;
    if (this->useCloseFlag)
      close = this->closeFlag;
    else if (this->alertTimeout < microtime())
      close = true;

    if (close)
      this->showAlert = false;
    else
      egl_alert_render(this->alert, this->screenScaleX, this->screenScaleY);
  }

  egl_fps_render(this->fps, this->screenScaleX, this->screenScaleY);
  eglSwapBuffers(this->display, this->surface);
  return true;
}

void egl_update_fps(void * opaque, const float avgUPS, const float avgFPS)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this->params.showFPS)
    return;

  egl_fps_update(this->fps, avgUPS, avgFPS);
}

struct LG_Renderer LGR_EGL =
{
  .get_name        = egl_get_name,
  .setup           = egl_setup,
  .create          = egl_create,
  .initialize      = egl_initialize,
  .deinitialize    = egl_deinitialize,
  .supports        = egl_supports,
  .on_restart      = egl_on_restart,
  .on_resize       = egl_on_resize,
  .on_mouse_shape  = egl_on_mouse_shape,
  .on_mouse_event  = egl_on_mouse_event,
  .on_frame_format = egl_on_frame_format,
  .on_frame        = egl_on_frame,
  .on_alert        = egl_on_alert,
  .render_startup  = egl_render_startup,
  .render          = egl_render,
  .update_fps      = egl_update_fps
};
