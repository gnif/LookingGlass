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
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>

#include <SDL2/SDL_ttf.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include "common/debug.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/locking.h"
#include "dynamic/fonts.h"
#include "ll.h"

#define BUFFER_COUNT       2

#define FPS_TEXTURE        0
#define MOUSE_TEXTURE      1
#define ALERT_TEXTURE      2
#define TEXTURE_COUNT      3

#define ALERT_TIMEOUT_FLAG ((uint64_t)-1)

#define FADE_TIME 1000000

static struct Option opengl_options[] =
{
  {
    .module       = "opengl",
    .name         = "mipmap",
    .description  = "Enable mipmapping",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {
    .module       = "opengl",
    .name         = "vsync",
    .description  = "Enable vsync",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {
    .module       = "opengl",
    .name         = "preventBuffer",
    .description  = "Prevent the driver from buffering frames",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {
    .module       = "opengl",
    .name         = "amdPinnedMem",
    .description  = "Use GL_AMD_pinned_memory if it is available",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true
  },
  {0}
};

struct OpenGL_Options
{
  bool mipmap;
  bool vsync;
  bool preventBuffer;
  bool amdPinnedMem;
};

struct Alert
{
  bool          ready;
  bool          useCloseFlag;

  LG_FontBitmap *text;
  float         r, g, b, a;
  uint64_t      timeout;
  bool          closeFlag;
};

struct Inst
{
  LG_RendererParams     params;
  struct OpenGL_Options opt;

  bool              amdPinnedMemSupport;
  bool              renderStarted;
  bool              configured;
  bool              reconfigure;
  SDL_GLContext     glContext;

  SDL_Point         window;
  bool              frameUpdate;

  const LG_Font   * font;
  LG_FontObj        fontObj, alertFontObj;

  LG_Lock             formatLock;
  LG_RendererFormat   format;
  GLuint              intFormat;
  GLuint              vboFormat;
  GLuint              dataFormat;
  size_t              texSize;
  size_t              texPos;
  const FrameBuffer * frame;

  uint64_t          drawStart;
  bool              hasBuffers;
  GLuint            vboID[BUFFER_COUNT];
  uint8_t         * texPixels[BUFFER_COUNT];
  LG_Lock           syncLock;
  bool              texReady;
  int               texIndex;
  int               texList;
  int               fpsList;
  int               mouseList;
  LG_RendererRect   destRect;

  bool              hasTextures, hasFrames;
  GLuint            frames[BUFFER_COUNT];
  GLsync            fences[BUFFER_COUNT];
  GLuint            textures[TEXTURE_COUNT];
  struct ll       * alerts;
  int               alertList;

  bool              waiting;
  uint64_t          waitFadeTime;
  bool              waitDone;

  bool              fpsTexture;
  SDL_Rect          fpsRect;

  LG_Lock           mouseLock;
  LG_RendererCursor mouseCursor;
  int               mouseWidth;
  int               mouseHeight;
  int               mousePitch;
  uint8_t *         mouseData;
  size_t            mouseDataSize;

  bool              mouseUpdate;
  bool              newShape;
  LG_RendererCursor mouseType;
  bool              mouseVisible;
  SDL_Rect          mousePos;
};

static bool _check_gl_error(unsigned int line, const char * name);
#define check_gl_error(name) _check_gl_error(__LINE__, name)

enum ConfigStatus
{
  CONFIG_STATUS_OK,
  CONFIG_STATUS_ERROR,
  CONFIG_STATUS_NOOP
};

static void deconfigure(struct Inst * this);
static enum ConfigStatus configure(struct Inst * this, SDL_Window *window);
static void update_mouse_shape(struct Inst * this, bool * newShape);
static bool draw_frame(struct Inst * this);
static void draw_mouse(struct Inst * this);
static void render_wait(struct Inst * this);

const char * opengl_get_name()
{
  return "OpenGL";
}

static void opengl_setup()
{
  option_register(opengl_options);
}

bool opengl_create(void ** opaque, const LG_RendererParams params)
{
  // create our local storage
  *opaque = malloc(sizeof(struct Inst));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
    return false;
  }
  memset(*opaque, 0, sizeof(struct Inst));

  struct Inst * this = (struct Inst *)*opaque;
  memcpy(&this->params, &params, sizeof(LG_RendererParams));

  this->opt.mipmap        = option_get_bool("opengl", "mipmap"       );
  this->opt.vsync         = option_get_bool("opengl", "vsync"        );
  this->opt.preventBuffer = option_get_bool("opengl", "preventBuffer");
  this->opt.amdPinnedMem  = option_get_bool("opengl", "amdPinnedMem" );


  LG_LOCK_INIT(this->formatLock);
  LG_LOCK_INIT(this->syncLock  );
  LG_LOCK_INIT(this->mouseLock );

  this->font = LG_Fonts[0];
  if (!this->font->create(&this->fontObj, NULL, 14))
  {
    DEBUG_ERROR("Unable to create the font renderer");
    return false;
  }

  if (!this->font->create(&this->alertFontObj, NULL, 18))
  {
    DEBUG_ERROR("Unable to create the font renderer");
    return false;
  }

  this->alerts = ll_new();

  return true;
}

bool opengl_initialize(void * opaque, Uint32 * sdlFlags)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return false;

  this->waiting  = true;
  this->waitDone = false;

  *sdlFlags = SDL_WINDOW_OPENGL;
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER      , 1);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
  return true;
}

void opengl_deinitialize(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  if (this->renderStarted)
  {
    glDeleteLists(this->texList  , BUFFER_COUNT);
    glDeleteLists(this->mouseList, 1);
    glDeleteLists(this->fpsList  , 1);
    glDeleteLists(this->alertList, 1);
  }

  deconfigure(this);
  if (this->mouseData)
    free(this->mouseData);

  if (this->glContext)
  {
    SDL_GL_DeleteContext(this->glContext);
    this->glContext = NULL;
  }

  LG_LOCK_FREE(this->formatLock);
  LG_LOCK_FREE(this->syncLock  );
  LG_LOCK_FREE(this->mouseLock );

  struct Alert * alert;
  while(ll_shift(this->alerts, (void **)&alert))
  {
    if (alert->text)
      this->font->release(this->alertFontObj, alert->text);
    free(alert);
  }
  ll_free(this->alerts);

  if (this->font && this->fontObj)
    this->font->destroy(this->fontObj);

  free(this);
}

void opengl_on_restart(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  this->waiting = true;
}

void opengl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct Inst * this = (struct Inst *)opaque;

  this->window.x = width;
  this->window.y = height;

  if (destRect.valid)
    memcpy(&this->destRect, &destRect, sizeof(LG_RendererRect));

  // setup the projection matrix
  glViewport(0, 0, this->window.x, this->window.y);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluOrtho2D(0, this->window.x, this->window.y, 0);

  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  if (this->destRect.valid)
  {
    glTranslatef(this->destRect.x, this->destRect.y, 0.0f);
    glScalef(
      (float)this->destRect.w / (float)this->format.width,
      (float)this->destRect.h / (float)this->format.height,
      1.0f
    );
  }
}

bool opengl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return false;

  LG_LOCK(this->mouseLock);
  this->mouseCursor = cursor;
  this->mouseWidth  = width;
  this->mouseHeight = height;
  this->mousePitch  = pitch;

  const size_t size = height * pitch;
  if (size > this->mouseDataSize)
  {
    if (this->mouseData)
      free(this->mouseData);
    this->mouseData     = (uint8_t *)malloc(size);
    this->mouseDataSize = size;
  }

  memcpy(this->mouseData, data, size);
  this->newShape = true;
  LG_UNLOCK(this->mouseLock);

  return true;
}

bool opengl_on_mouse_event(void * opaque, const bool visible, const int x, const int y)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return false;

  if (this->mousePos.x == x && this->mousePos.y == y && this->mouseVisible == visible)
    return true;

  this->mouseVisible = visible;
  this->mousePos.x   = x;
  this->mousePos.y   = y;
  this->mouseUpdate  = true;
  return false;
}

bool opengl_on_frame_event(void * opaque, const LG_RendererFormat format, const FrameBuffer * frame)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
  {
    DEBUG_ERROR("Invalid opaque pointer");
    return false;
  }

  LG_LOCK(this->formatLock);
  if (this->reconfigure)
  {
    LG_UNLOCK(this->formatLock);
    return true;
  }

  if (!this->configured ||
    this->format.type   != format.type   ||
    this->format.width  != format.width  ||
    this->format.height != format.height ||
    this->format.stride != format.stride ||
    this->format.bpp    != format.bpp
  )
  {
    memcpy(&this->format, &format, sizeof(LG_RendererFormat));
    this->reconfigure = true;
    LG_UNLOCK(this->formatLock);
    return true;
  }
  LG_UNLOCK(this->formatLock);

  LG_LOCK(this->syncLock);
  this->frame       = frame;
  this->frameUpdate = true;
  LG_UNLOCK(this->syncLock);

  if (this->waiting)
  {
    this->waiting      = false;
    this->waitFadeTime = microtime() + FADE_TIME;
  }

  return true;
}

void opengl_on_alert(void * opaque, const LG_MsgAlert alert, const char * message, bool ** closeFlag)
{
  struct Inst * this = (struct Inst *)opaque;
  struct Alert * a = malloc(sizeof(struct Alert));
  memset(a, 0, sizeof(struct Alert));

  switch(alert)
  {
    case LG_ALERT_INFO:
      a->r = 0.0f;
      a->g = 0.0f;
      a->b = 0.8f;
      a->a = 0.8f;
      break;

    case LG_ALERT_SUCCESS:
      a->r = 0.0f;
      a->g = 0.8f;
      a->b = 0.0f;
      a->a = 0.8f;
      break;

    case LG_ALERT_WARNING:
      a->r = 0.8f;
      a->g = 0.5f;
      a->b = 0.0f;
      a->a = 0.8f;
      break;

    case LG_ALERT_ERROR:
      a->r = 1.0f;
      a->g = 0.0f;
      a->b = 0.0f;
      a->a = 0.8f;
      break;
  }

  if (!(a->text = this->font->render(this->alertFontObj, 0xffffff00, message)))
  {
    DEBUG_ERROR("Failed to render alert text: %s", TTF_GetError());
    free(a);
    return;
  }

  if (closeFlag)
  {
    a->useCloseFlag = true;
    *closeFlag = &a->closeFlag;
  }

  ll_push(this->alerts, a);
}

void bitmap_to_texture(LG_FontBitmap * bitmap, GLuint texture)
{
  glBindTexture(GL_TEXTURE_2D       , texture      );
  glPixelStorei(GL_UNPACK_ALIGNMENT , 4            );
  glPixelStorei(GL_UNPACK_ROW_LENGTH, bitmap->width);
  glTexImage2D(
    GL_TEXTURE_2D,
    0,
    bitmap->bpp,
    bitmap->width,
    bitmap->height,
    0,
    GL_BGRA,
    GL_UNSIGNED_BYTE,
    bitmap->pixels
  );

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glBindTexture(GL_TEXTURE_2D, 0);
}

bool opengl_render_startup(void * opaque, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;

  this->glContext = SDL_GL_CreateContext(window);
  if (!this->glContext)
  {
    DEBUG_ERROR("Failed to create the OpenGL context");
    return false;
  }

  DEBUG_INFO("Vendor  : %s", glGetString(GL_VENDOR  ));
  DEBUG_INFO("Renderer: %s", glGetString(GL_RENDERER));
  DEBUG_INFO("Version : %s", glGetString(GL_VERSION ));

  GLint n;
  glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for(GLint i = 0; i < n; ++i)
  {
    const GLubyte *ext = glGetStringi(GL_EXTENSIONS, i);
    if (strcmp((const char *)ext, "GL_AMD_pinned_memory") == 0)
    {
      if (this->opt.amdPinnedMem)
      {
        this->amdPinnedMemSupport = true;
        DEBUG_INFO("Using GL_AMD_pinned_memory");
      }
      else
        DEBUG_INFO("GL_AMD_pinned_memory is available but not in use");
      break;
    }
  }

  glEnable(GL_TEXTURE_2D);
  glEnable(GL_COLOR_MATERIAL);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glEnable(GL_MULTISAMPLE);

  // generate lists for drawing
  this->texList   = glGenLists(BUFFER_COUNT);
  this->mouseList = glGenLists(1);
  this->fpsList   = glGenLists(1);
  this->alertList = glGenLists(1);

  // create the overlay textures
  glGenTextures(TEXTURE_COUNT, this->textures);
  if (check_gl_error("glGenTextures"))
  {
    LG_UNLOCK(this->formatLock);
    return false;
  }
  this->hasTextures = true;

  SDL_GL_SetSwapInterval(this->opt.vsync ? 1 : 0);
  this->renderStarted = true;
  return true;
}

bool opengl_render(void * opaque, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return false;

  switch(configure(this, window))
  {
    case CONFIG_STATUS_ERROR:
      DEBUG_ERROR("configure failed");
      return false;

    case CONFIG_STATUS_NOOP :
    case CONFIG_STATUS_OK   :
     if (!draw_frame(this))
       return false;
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (this->waiting)
    render_wait(this);
  else
  {
    bool newShape;
    update_mouse_shape(this, &newShape);
    glCallList(this->texList + this->texIndex);
    draw_mouse(this);

    if (!this->waitDone)
      render_wait(this);
  }

  if (this->fpsTexture)
    glCallList(this->fpsList);

  struct Alert * alert;
  while(ll_peek_head(this->alerts, (void **)&alert))
  {
    if (!alert->ready)
    {
      bitmap_to_texture(alert->text, this->textures[ALERT_TEXTURE]);

      glNewList(this->alertList, GL_COMPILE);
        const int p = 4;
        const int w = alert->text->width  + p * 2;
        const int h = alert->text->height + p * 2;
        glTranslatef(-(w / 2), -(h / 2), 0.0f);
        glEnable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
        glColor4f(alert->r, alert->g, alert->b, alert->a);
        glBegin(GL_TRIANGLE_STRIP);
          glVertex2i(0, 0);
          glVertex2i(w, 0);
          glVertex2i(0, h);
          glVertex2i(w, h);
        glEnd();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, this->textures[ALERT_TEXTURE]);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glTranslatef(p, p, 0.0f);
        glBegin(GL_TRIANGLE_STRIP);
          glTexCoord2f(0.0f, 0.0f); glVertex2i(0                 , 0                  );
          glTexCoord2f(1.0f, 0.0f); glVertex2i(alert->text->width, 0                  );
          glTexCoord2f(0.0f, 1.0f); glVertex2i(0                 , alert->text->height);
          glTexCoord2f(1.0f, 1.0f); glVertex2i(alert->text->width, alert->text->height);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);
      glEndList();

      if (!alert->useCloseFlag)
        alert->timeout = microtime() + 2*1000000;
      alert->ready   = true;

      this->font->release(this->fontObj, alert->text);
      alert->text  = NULL;
      alert->ready = true;
    }
    else
    {
      bool close = false;
      if (alert->useCloseFlag)
        close = alert->closeFlag;
      else if (alert->timeout < microtime())
        close = true;

      if (close)
      {
        free(alert);
        ll_shift(this->alerts, NULL);
        continue;
      }
    }

    glPushMatrix();
      glLoadIdentity();
      glTranslatef(this->window.x / 2, this->window.y / 2, 0.0f);
      glCallList(this->alertList);
    glPopMatrix();
    break;
  }

  if (this->opt.preventBuffer)
  {
    SDL_GL_SwapWindow(window);
    glFinish();
  }
  else
    SDL_GL_SwapWindow(window);

  this->mouseUpdate = false;
  return true;
}

void opengl_update_fps(void * opaque, const float avgUPS, const float avgFPS)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this->params.showFPS)
    return;

  char str[128];
  snprintf(str, sizeof(str), "UPS: %8.4f, FPS: %8.4f", avgUPS, avgFPS);

  LG_FontBitmap *textSurface = NULL;
  if (!(textSurface = this->font->render(this->fontObj, 0xffffff00, str)))
    DEBUG_ERROR("Failed to render text");

  bitmap_to_texture(textSurface, this->textures[FPS_TEXTURE]);

  this->fpsRect.x = 5;
  this->fpsRect.y = 5;
  this->fpsRect.w = textSurface->width;
  this->fpsRect.h = textSurface->height;

  this->font->release(this->fontObj, textSurface);

  this->fpsTexture  = true;

  glNewList(this->fpsList, GL_COMPILE);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glColor4f(0.0f, 0.0f, 1.0f, 0.5f);
    glBegin(GL_TRIANGLE_STRIP);
      glVertex2i(this->fpsRect.x                  , this->fpsRect.y                  );
      glVertex2i(this->fpsRect.x + this->fpsRect.w, this->fpsRect.y                  );
      glVertex2i(this->fpsRect.x                  , this->fpsRect.y + this->fpsRect.h);
      glVertex2i(this->fpsRect.x + this->fpsRect.w, this->fpsRect.y + this->fpsRect.h);
    glEnd();
    glEnable(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, this->textures[FPS_TEXTURE]);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLE_STRIP);
      glTexCoord2f(0.0f , 0.0f); glVertex2i(this->fpsRect.x                  , this->fpsRect.y                  );
      glTexCoord2f(1.0f , 0.0f); glVertex2i(this->fpsRect.x + this->fpsRect.w, this->fpsRect.y                  );
      glTexCoord2f(0.0f , 1.0f); glVertex2i(this->fpsRect.x                  , this->fpsRect.y + this->fpsRect.h);
      glTexCoord2f(1.0f,  1.0f); glVertex2i(this->fpsRect.x + this->fpsRect.w, this->fpsRect.y + this->fpsRect.h);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);

    glPopMatrix();
  glEndList();
}

void draw_torus(float x, float y, float inner, float outer, unsigned int pts)
{
  glBegin(GL_QUAD_STRIP);
  for (unsigned int i = 0; i <= pts; ++i)
  {
    float angle = (i / (float)pts) * M_PI * 2.0f;
    glVertex2f(x + (inner * cos(angle)), y + (inner * sin(angle)));
    glVertex2f(x + (outer * cos(angle)), y + (outer * sin(angle)));
  }
  glEnd();
}

void draw_torus_arc(float x, float y, float inner, float outer, unsigned int pts, float s, float e)
{
  glBegin(GL_QUAD_STRIP);
  for (unsigned int i = 0; i <= pts; ++i)
  {
    float angle = s + ((i / (float)pts) * e);
    glVertex2f(x + (inner * cos(angle)), y + (inner * sin(angle)));
    glVertex2f(x + (outer * cos(angle)), y + (outer * sin(angle)));
  }
  glEnd();
}

static void render_wait(struct Inst * this)
{
  float a;
  if (this->waiting)
    a = 1.0f;
  else
  {
    uint64_t t = microtime();
    if (t > this->waitFadeTime)
    {
      glDisable(GL_MULTISAMPLE);
      this->waitDone = true;
      return;
    }

    uint64_t delta = this->waitFadeTime - t;
    a = 1.0f / FADE_TIME * delta;
  }

  glEnable(GL_BLEND);
  glPushMatrix();
  glLoadIdentity();
  glTranslatef(this->window.x / 2.0f, this->window.y / 2.0f, 0.0f);

  //draw the background gradient
  glBegin(GL_TRIANGLE_FAN);
  glColor4f(0.234375f, 0.015625f, 0.425781f, a);
  glVertex2f(0, 0);
  glColor4f(0, 0, 0, a);
  for (unsigned int i = 0; i <= 100; ++i)
  {
    float angle = (i / (float)100) * M_PI * 2.0f;
    glVertex2f(cos(angle) * this->window.x, sin(angle) * this->window.y);
  }
  glEnd();

  // draw the logo
  glColor4f(1.0f, 1.0f, 1.0f, a);
  glScalef (2.0f, 2.0f, 1.0f);

  draw_torus    (  0,  0, 40, 42, 60);
  draw_torus    (  0,  0, 32, 34, 60);
  draw_torus    (-50, -3,  2,  4, 30);
  draw_torus    ( 50, -3,  2,  4, 30);
  draw_torus_arc(  0,  0, 51, 49, 60, 0.0f, M_PI);

  glBegin(GL_QUADS);
    glVertex2f(-1 , 50);
    glVertex2f(-1 , 76);
    glVertex2f( 1 , 76);
    glVertex2f( 1 , 50);
    glVertex2f(-14, 76);
    glVertex2f(-14, 78);
    glVertex2f( 14, 78);
    glVertex2f( 14, 76);
    glVertex2f(-21, 83);
    glVertex2f(-21, 85);
    glVertex2f( 21, 85);
    glVertex2f( 21, 83);
  glEnd();

  draw_torus_arc(-14, 83, 5, 7, 10, M_PI       , M_PI / 2.0f);
  draw_torus_arc( 14, 83, 5, 7, 10, M_PI * 1.5f, M_PI / 2.0f);

  //FIXME: draw the diagnoal marks on the circle

  glPopMatrix();
  glDisable(GL_BLEND);
}

const LG_Renderer LGR_OpenGL =
{
  .get_name       = opengl_get_name,
  .setup          = opengl_setup,

  .create         = opengl_create,
  .initialize     = opengl_initialize,
  .deinitialize   = opengl_deinitialize,
  .on_restart     = opengl_on_restart,
  .on_resize      = opengl_on_resize,
  .on_mouse_shape = opengl_on_mouse_shape,
  .on_mouse_event = opengl_on_mouse_event,
  .on_frame_event = opengl_on_frame_event,
  .on_alert       = opengl_on_alert,
  .render_startup = opengl_render_startup,
  .render         = opengl_render,
  .update_fps     = opengl_update_fps
};

static bool _check_gl_error(unsigned int line, const char * name)
{
  GLenum error = glGetError();
  if (error == GL_NO_ERROR)
    return false;

  const GLubyte * errStr = gluErrorString(error);
  DEBUG_ERROR("%d: %s = %d (%s)", line, name, error, errStr);
  return true;
}

static enum ConfigStatus configure(struct Inst * this, SDL_Window *window)
{
  LG_LOCK(this->formatLock);
  if (!this->reconfigure)
  {
    LG_UNLOCK(this->formatLock);
    return CONFIG_STATUS_NOOP;
  }

  if (this->configured)
    deconfigure(this);

  switch(this->format.type)
  {
    case FRAME_TYPE_BGRA:
      this->intFormat  = GL_RGBA8;
      this->vboFormat  = GL_BGRA;
      this->dataFormat = GL_UNSIGNED_BYTE;
      break;

    case FRAME_TYPE_RGBA:
      this->intFormat  = GL_RGBA8;
      this->vboFormat  = GL_RGBA;
      this->dataFormat = GL_UNSIGNED_BYTE;
      break;

    case FRAME_TYPE_RGBA10:
      this->intFormat  = GL_RGB10_A2;
      this->vboFormat  = GL_RGBA;
      this->dataFormat = GL_UNSIGNED_INT_2_10_10_10_REV;
      break;

    default:
      DEBUG_ERROR("Unknown/unsupported compression type");
      return CONFIG_STATUS_ERROR;
  }

  // calculate the texture size in bytes
  this->texSize = this->format.height * this->format.pitch;
  this->texPos  = 0;

  glGenBuffers(BUFFER_COUNT, this->vboID);
  if (check_gl_error("glGenBuffers"))
  {
    LG_UNLOCK(this->formatLock);
    return false;
  }
  this->hasBuffers = true;

  if (this->amdPinnedMemSupport)
  {
    const int pagesize = getpagesize();

    for(int i = 0; i < BUFFER_COUNT; ++i)
    {
      this->texPixels[i] = aligned_alloc(pagesize, this->texSize);
      if (!this->texPixels[i])
      {
        DEBUG_ERROR("Failed to allocate memory for texture");
        return CONFIG_STATUS_ERROR;
      }

      memset(this->texPixels[i], 0, this->texSize);

      glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, this->vboID[i]);
      if (check_gl_error("glBindBuffer"))
      {
        LG_UNLOCK(this->formatLock);
        return CONFIG_STATUS_ERROR;
      }

      glBufferData(
        GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD,
        this->texSize,
        this->texPixels[i],
        GL_STREAM_DRAW
      );

      if (check_gl_error("glBufferData"))
      {
        LG_UNLOCK(this->formatLock);
        return CONFIG_STATUS_ERROR;
      }
    }
    glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 0);
  }
  else
  {
    for(int i = 0; i < BUFFER_COUNT; ++i)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[i]);
      if (check_gl_error("glBindBuffer"))
      {
        LG_UNLOCK(this->formatLock);
        return CONFIG_STATUS_ERROR;
      }

      glBufferData(
        GL_PIXEL_UNPACK_BUFFER,
        this->texSize,
        NULL,
        GL_STREAM_DRAW
      );
      if (check_gl_error("glBufferData"))
      {
        LG_UNLOCK(this->formatLock);
        return CONFIG_STATUS_ERROR;
      }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  // create the frame textures
  glGenTextures(BUFFER_COUNT, this->frames);
  if (check_gl_error("glGenTextures"))
  {
    LG_UNLOCK(this->formatLock);
    return CONFIG_STATUS_ERROR;
  }
  this->hasFrames = true;

  for(int i = 0; i < BUFFER_COUNT; ++i)
  {
    // bind and create the new texture
    glBindTexture(GL_TEXTURE_2D, this->frames[i]);
    if (check_gl_error("glBindTexture"))
    {
      LG_UNLOCK(this->formatLock);
      return CONFIG_STATUS_ERROR;
    }

    glTexImage2D(
      GL_TEXTURE_2D,
      0,
      this->intFormat,
      this->format.width,
      this->format.height,
      0,
      this->vboFormat,
      this->dataFormat,
      (void*)0
    );
    if (check_gl_error("glTexImage2D"))
    {
      LG_UNLOCK(this->formatLock);
      return CONFIG_STATUS_ERROR;
    }

    // configure the texture
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    // create the display lists
    glNewList(this->texList + i, GL_COMPILE);
      glBindTexture(GL_TEXTURE_2D, this->frames[i]);
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2i(0                 , 0                  );
        glTexCoord2f(1.0f, 0.0f); glVertex2i(this->format.width, 0                  );
        glTexCoord2f(0.0f, 1.0f); glVertex2i(0                 , this->format.height);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(this->format.width, this->format.height);
     glEnd();
     glBindTexture(GL_TEXTURE_2D, 0);
    glEndList();
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  this->drawStart   = nanotime();
  this->configured  = true;
  this->reconfigure = false;

  LG_UNLOCK(this->formatLock);
  return CONFIG_STATUS_OK;
}

static void deconfigure(struct Inst * this)
{
  if (!this->configured)
    return;

  if (this->hasTextures)
  {
    glDeleteTextures(TEXTURE_COUNT, this->textures);
    this->hasTextures = false;
  }

  if (this->hasFrames)
  {
    glDeleteTextures(BUFFER_COUNT, this->frames);
    this->hasFrames = false;
  }

  if (this->hasBuffers)
  {
    glDeleteBuffers(BUFFER_COUNT, this->vboID);
    this->hasBuffers = false;
  }

  if (this->amdPinnedMemSupport)
  {
    for(int i = 0; i < BUFFER_COUNT; ++i)
    {
      if (this->fences[i])
      {
        glDeleteSync(this->fences[i]);
        this->fences[i] = NULL;
      }

      if (this->texPixels[i])
      {
        free(this->texPixels[i]);
        this->texPixels[i] = NULL;
      }
    }
  }

  this->configured = false;
}

static void update_mouse_shape(struct Inst * this, bool * newShape)
{
  LG_LOCK(this->mouseLock);
  *newShape = this->newShape;
  if (!this->newShape)
  {
    LG_UNLOCK(this->mouseLock);
    return;
  }

  const LG_RendererCursor cursor = this->mouseCursor;
  const int               width  = this->mouseWidth;
  const int               height = this->mouseHeight;
  const int               pitch  = this->mousePitch;
  const uint8_t *         data   = this->mouseData;

  // tmp buffer for masked colour
  uint32_t tmp[width * height];

  this->mouseType = cursor;
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
      glBindTexture(GL_TEXTURE_2D, this->textures[MOUSE_TEXTURE]);
      glPixelStorei(GL_UNPACK_ALIGNMENT , 4    );
      glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
      glTexImage2D
      (
        GL_TEXTURE_2D,
        0      ,
        GL_RGBA,
        width  ,
        height ,
        0      ,
        GL_BGRA, // windows cursors are in BGRA format
        GL_UNSIGNED_BYTE,
        data
      );
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glBindTexture(GL_TEXTURE_2D, 0);

      this->mousePos.w = width;
      this->mousePos.h = height;

      glNewList(this->mouseList, GL_COMPILE);
        glEnable(GL_BLEND);
        glBindTexture(GL_TEXTURE_2D, this->textures[MOUSE_TEXTURE]);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_TRIANGLE_STRIP);
          glTexCoord2f(0.0f, 0.0f); glVertex2i(0    , 0     );
          glTexCoord2f(1.0f, 0.0f); glVertex2i(width, 0     );
          glTexCoord2f(0.0f, 1.0f); glVertex2i(0    , height);
          glTexCoord2f(1.0f, 1.0f); glVertex2i(width, height);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);
      glEndList();
      break;
    }

    case LG_CURSOR_MONOCHROME:
    {
      const int hheight = height / 2;
      uint32_t d[width * height];
      for(int y = 0; y < hheight; ++y)
        for(int x = 0; x < width; ++x)
        {
          const uint8_t  * srcAnd  = data + (pitch * y) + (x / 8);
          const uint8_t  * srcXor  = srcAnd + pitch * hheight;
          const uint8_t    mask    = 0x80 >> (x % 8);
          const uint32_t   andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
          const uint32_t   xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;

          d[y * width + x                  ] = andMask;
          d[y * width + x + width * hheight] = xorMask;
        }

      glBindTexture(GL_TEXTURE_2D, this->textures[MOUSE_TEXTURE]);
      glPixelStorei(GL_UNPACK_ALIGNMENT , 4    );
      glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
      glTexImage2D
      (
        GL_TEXTURE_2D,
        0      ,
        GL_RGBA,
        width  ,
        height ,
        0      ,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        d
      );
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glBindTexture(GL_TEXTURE_2D, 0);

      this->mousePos.w = width;
      this->mousePos.h = hheight;

      glNewList(this->mouseList, GL_COMPILE);
        glEnable(GL_COLOR_LOGIC_OP);
        glBindTexture(GL_TEXTURE_2D, this->textures[MOUSE_TEXTURE]);
        glLogicOp(GL_AND);
        glBegin(GL_TRIANGLE_STRIP);
          glTexCoord2f(0.0f, 0.0f); glVertex2i(0    , 0      );
          glTexCoord2f(1.0f, 0.0f); glVertex2i(width, 0      );
          glTexCoord2f(0.0f, 0.5f); glVertex2i(0    , hheight);
          glTexCoord2f(1.0f, 0.5f); glVertex2i(width, hheight);
        glEnd();
        glLogicOp(GL_XOR);
        glBegin(GL_TRIANGLE_STRIP);
          glTexCoord2f(0.0f, 0.5f); glVertex2i(0    , 0      );
          glTexCoord2f(1.0f, 0.5f); glVertex2i(width, 0      );
          glTexCoord2f(0.0f, 1.0f); glVertex2i(0    , hheight);
          glTexCoord2f(1.0f, 1.0f); glVertex2i(width, hheight);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_COLOR_LOGIC_OP);
      glEndList();
      break;
    }
  }

  this->mouseUpdate = true;
  LG_UNLOCK(this->mouseLock);
}

static bool opengl_buffer_fn(void * opaque, const void * data, size_t size)
{
  struct Inst * this = (struct Inst *)opaque;

  // update the buffer, this performs a DMA transfer if possible
  glBufferSubData(
    GL_PIXEL_UNPACK_BUFFER,
    this->texPos,
    size,
    data
  );
  check_gl_error("glBufferSubData");

  this->texPos += size;
  return true;
}

static bool draw_frame(struct Inst * this)
{
  LG_LOCK(this->syncLock);
  if (!this->frameUpdate)
  {
    LG_UNLOCK(this->syncLock);
    return true;
  }

  if (++this->texIndex == BUFFER_COUNT)
    this->texIndex = 0;

  this->frameUpdate = false;
  LG_UNLOCK(this->syncLock);

  LG_LOCK(this->formatLock);
  if (glIsSync(this->fences[this->texIndex]))
  {
    switch(glClientWaitSync(this->fences[this->texIndex], 0, GL_TIMEOUT_IGNORED))
    {
      case GL_ALREADY_SIGNALED:
        break;

      case GL_CONDITION_SATISFIED:
        DEBUG_WARN("Had to wait for the sync");
        break;

      case GL_TIMEOUT_EXPIRED:
        DEBUG_WARN("Timeout expired, DMA transfers are too slow!");
        break;

      case GL_WAIT_FAILED:
        DEBUG_ERROR("Wait failed %s", gluErrorString(glGetError()));
        break;
    }

    glDeleteSync(this->fences[this->texIndex]);
    this->fences[this->texIndex] = NULL;
  }

  glBindTexture(GL_TEXTURE_2D, this->frames[this->texIndex]);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[this->texIndex]);

  const int bpp = this->format.bpp / 8;
  glPixelStorei(GL_UNPACK_ALIGNMENT , bpp);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, this->format.width);

  this->texPos = 0;

  framebuffer_read_fn(
    this->frame,
    this->format.height,
    this->format.width,
    bpp,
    this->format.pitch,
    opengl_buffer_fn,
    this
  );

  // update the texture
  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    0,
    0,
    this->format.width ,
    this->format.height,
    this->vboFormat,
    this->dataFormat,
    (void*)0
  );
  if (check_gl_error("glTexSubImage2D"))
  {
    DEBUG_ERROR("texIndex: %u, width: %u, height: %u, vboFormat: %x, texSize: %lu",
      this->texIndex, this->format.width, this->format.height, this->vboFormat, this->texSize
    );
  }

  // set a fence so we don't overwrite a buffer in use
  this->fences[this->texIndex] =
    glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

  // unbind the buffer
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  const bool mipmap = this->opt.mipmap && (
    (this->format.width  > this->destRect.w) ||
    (this->format.height > this->destRect.h));

  glBindTexture(GL_TEXTURE_2D, this->frames[this->texIndex]);
  if (mipmap)
  {
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  }
  else
  {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  LG_UNLOCK(this->formatLock);
  this->texReady = true;
  return true;
}

static void draw_mouse(struct Inst * this)
{
  if (!this->mouseVisible)
    return;

  glPushMatrix();
  glTranslatef(this->mousePos.x, this->mousePos.y, 0.0f);
  glCallList(this->mouseList);
  glPopMatrix();
}
