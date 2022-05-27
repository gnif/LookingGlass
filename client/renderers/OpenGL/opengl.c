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

#include "interface/renderer.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>

#include <GL/gl.h>

#include "cimgui.h"
#include "generator/output/cimgui_impl.h"

#include "common/debug.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/locking.h"
#include "gl_dynprocs.h"
#include "util.h"

#define BUFFER_COUNT       2

#define FPS_TEXTURE        0
#define MOUSE_TEXTURE      1
#define SPICE_TEXTURE      2
#define TEXTURE_COUNT      3

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
    .value.x_bool = false,
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

struct IntPoint
{
  int x;
  int y;
};

struct IntRect
{
  int x;
  int y;
  int w;
  int h;
};

struct OpenGL_Options
{
  bool mipmap;
  bool vsync;
  bool preventBuffer;
  bool amdPinnedMem;
};

struct Inst
{
  LG_Renderer base;

  LG_RendererParams     params;
  struct OpenGL_Options opt;

  bool              amdPinnedMemSupport;
  bool              renderStarted;
  bool              configured;
  bool              reconfigure;
  LG_DSGLContext    glContext;

  struct IntPoint   window;
  float             uiScale;
  _Atomic(bool)     frameUpdate;

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
  LG_Lock           frameLock;
  bool              texReady;
  int               texWIndex, texRIndex;
  int               texList;
  int               mouseList;
  int               spiceList;
  LG_RendererRect   destRect;
  struct IntPoint   spiceSize;
  bool              spiceShow;

  bool              hasTextures, hasFrames;
  GLuint            frames[BUFFER_COUNT];
  GLsync            fences[BUFFER_COUNT];
  GLuint            textures[TEXTURE_COUNT];

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
  struct IntRect    mousePos;
};

static bool _checkGLError(unsigned int line, const char * name);
#define check_gl_error(name) _checkGLError(__LINE__, name)

enum ConfigStatus
{
  CONFIG_STATUS_OK,
  CONFIG_STATUS_ERROR,
  CONFIG_STATUS_NOOP
};

static void deconfigure(struct Inst * this);
static enum ConfigStatus configure(struct Inst * this);
static void updateMouseShape(struct Inst * this);
static bool drawFrame(struct Inst * this);
static void drawMouse(struct Inst * this);

const char * opengl_getName(void)
{
  return "OpenGL";
}

static void opengl_setup(void)
{
  option_register(opengl_options);
}

bool opengl_create(LG_Renderer ** renderer, const LG_RendererParams params,
    bool * needsOpenGL)
{
  // create our local storage
  struct Inst * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(*this));
    return false;
  }
  *renderer = &this->base;

  memcpy(&this->params, &params, sizeof(LG_RendererParams));
  this->opt.mipmap        = option_get_bool("opengl", "mipmap"       );
  this->opt.vsync         = option_get_bool("opengl", "vsync"        );
  this->opt.preventBuffer = option_get_bool("opengl", "preventBuffer");
  this->opt.amdPinnedMem  = option_get_bool("opengl", "amdPinnedMem" );

  LG_LOCK_INIT(this->formatLock);
  LG_LOCK_INIT(this->frameLock );
  LG_LOCK_INIT(this->mouseLock );

  *needsOpenGL = true;
  return true;
}

bool opengl_initialize(LG_Renderer * renderer)
{
//  struct Inst * this = UPCAST(struct Inst, renderer);
  return true;
}

void opengl_deinitialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (this->renderStarted)
  {
    ImGui_ImplOpenGL2_Shutdown();

    glDeleteLists(this->texList  , BUFFER_COUNT);
    glDeleteLists(this->mouseList, 1);
    glDeleteLists(this->spiceList, 1);
  }

  deconfigure(this);

  if (this->hasTextures)
  {
    glDeleteTextures(TEXTURE_COUNT, this->textures);
    this->hasTextures = false;
  }

  if (this->mouseData)
    free(this->mouseData);

  if (this->glContext)
  {
    app_glDeleteContext(this->glContext);
    this->glContext = NULL;
  }

  LG_LOCK_FREE(this->formatLock);
  LG_LOCK_FREE(this->frameLock );
  LG_LOCK_FREE(this->mouseLock );

  free(this);
}

void opengl_onRestart(LG_Renderer * renderer)
{
//  struct Inst * this = UPCAST(struct Inst, renderer);
}

static void setupModelView(struct Inst * this)
{
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  if (!this->destRect.valid)
    return;

  int fw, fh;
  if (this->spiceShow)
  {
    fw = this->spiceSize.x;
    fh = this->spiceSize.y;
  }
  else
  {
    fw = this->format.frameWidth;
    fh = this->format.frameHeight;
  }
  glTranslatef(this->destRect.x, this->destRect.y, 0.0f);
  glScalef(
    (float)this->destRect.w / (float)fw,
    (float)this->destRect.h / (float)fh,
    1.0f
  );
}

void opengl_onResize(LG_Renderer * renderer, const int width, const int height, const double scale,
    const LG_RendererRect destRect, LG_RendererRotate rotate)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  this->window.x = width * scale;
  this->window.y = height * scale;
  this->uiScale  = (float) scale;

  if (destRect.valid)
  {
    this->destRect.valid = true;
    this->destRect.x = destRect.x * scale;
    this->destRect.y = destRect.y * scale;
    this->destRect.w = destRect.w * scale;
    this->destRect.h = destRect.h * scale;
  }

  // setup the projection matrix
  glViewport(0, 0, this->window.x, this->window.y);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, this->window.x, this->window.y, 0, -1, 1);

  // this is needed to refresh the font atlas texture
  ImGui_ImplOpenGL2_Shutdown();
  ImGui_ImplOpenGL2_Init();
  ImGui_ImplOpenGL2_NewFrame();
}

bool opengl_onMouseShape(LG_Renderer * renderer, const LG_RendererCursor cursor,
    const int width, const int height, const int pitch, const uint8_t * data)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

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

    this->mouseData = malloc(size);
    if (!this->mouseData)
    {
      DEBUG_ERROR("out of memory");
      return false;
    }

    this->mouseDataSize = size;
  }

  memcpy(this->mouseData, data, size);
  this->newShape = true;
  LG_UNLOCK(this->mouseLock);

  return true;
}

bool opengl_onMouseEvent(LG_Renderer * renderer, const bool visible,
    int x, int y, const int hx, const int hy)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (this->mousePos.x == x && this->mousePos.y == y && this->mouseVisible == visible)
    return true;

  this->mouseVisible = visible;
  this->mousePos.x   = x;
  this->mousePos.y   = y;
  this->mouseUpdate  = true;
  return false;
}

bool opengl_onFrameFormat(LG_Renderer * renderer, const LG_RendererFormat format)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  LG_LOCK(this->formatLock);
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  this->reconfigure = true;
  LG_UNLOCK(this->formatLock);
  return true;
}

bool opengl_onFrame(LG_Renderer * renderer, const FrameBuffer * frame, int dmaFd,
    const FrameDamageRect * damage, int damageCount)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  LG_LOCK(this->frameLock);
  this->frame = frame;
  atomic_store_explicit(&this->frameUpdate, true, memory_order_release);
  LG_UNLOCK(this->frameLock);

  return true;
}

bool opengl_renderStartup(LG_Renderer * renderer, bool useDMA)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  this->glContext = app_glCreateContext();
  if (!this->glContext)
    return false;

  app_glMakeCurrent(this->glContext);

  DEBUG_INFO("Vendor  : %s", glGetString(GL_VENDOR  ));
  DEBUG_INFO("Renderer: %s", glGetString(GL_RENDERER));
  DEBUG_INFO("Version : %s", glGetString(GL_VERSION ));

  const char * exts = (const char *)glGetString(GL_EXTENSIONS);
  if (util_hasGLExt(exts, "GL_AMD_pinned_memory"))
  {
    if (this->opt.amdPinnedMem)
    {
      this->amdPinnedMemSupport = true;
      DEBUG_INFO("Using GL_AMD_pinned_memory");
    }
    else
      DEBUG_INFO("GL_AMD_pinned_memory is available but not in use");
  }

  GLint maj, min;
  glGetIntegerv(GL_MAJOR_VERSION, &maj);
  glGetIntegerv(GL_MINOR_VERSION, &min);

  if ((maj < 3 || (maj == 3 && min < 2)) && !util_hasGLExt(exts, "GL_ARB_sync"))
  {
    DEBUG_ERROR("Need OpenGL 3.2+ or GL_ARB_sync for sync objects");
    return false;
  }

  if (maj < 2 && !util_hasGLExt(exts, "GL_ARB_pixel_buffer_object"))
  {
    DEBUG_ERROR("Need OpenGL 2.0+ or GL_ARB_pixel_buffer_object");
    return false;
  }

  if (this->opt.mipmap && maj < 3 &&
      !util_hasGLExt(exts, "GL_ARB_framebuffer_object") &&
      !util_hasGLExt(exts, "GL_EXT_framebuffer_object"))
  {
    DEBUG_WARN("Need OpenGL 3.0+ or GL_ARB_framebuffer_object or "
      "GL_EXT_framebuffer_object for glGenerateMipmap, disabling mipmaps");
    this->opt.mipmap = false;
  }

  glEnable(GL_TEXTURE_2D);
  glEnable(GL_COLOR_MATERIAL);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glEnable(GL_MULTISAMPLE);

  // generate lists for drawing
  this->texList   = glGenLists(BUFFER_COUNT);
  this->mouseList = glGenLists(1);
  this->spiceList = glGenLists(1);

  // create the overlay textures
  glGenTextures(TEXTURE_COUNT, this->textures);
  if (check_gl_error("glGenTextures"))
  {
    LG_UNLOCK(this->formatLock);
    return false;
  }
  this->hasTextures = true;

  app_glSetSwapInterval(this->opt.vsync ? 1 : 0);

  if (!ImGui_ImplOpenGL2_Init())
  {
    DEBUG_ERROR("Failed to initialize ImGui");
    return false;
  }

  this->renderStarted = true;
  return true;
}

bool opengl_render(LG_Renderer * renderer, LG_RendererRotate rotate, const bool newFrame,
    const bool invalidateWindow, void (*preSwap)(void * udata), void * udata)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  setupModelView(this);

  switch(configure(this))
  {
    case CONFIG_STATUS_ERROR:
      DEBUG_ERROR("configure failed");
      return false;

    case CONFIG_STATUS_NOOP :
    case CONFIG_STATUS_OK   :
      if (!drawFrame(this))
        return false;
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (this->spiceShow)
    glCallList(this->spiceList);
  else
  {
    updateMouseShape(this);
    glCallList(this->texList + this->texRIndex);
    drawMouse(this);
  }

  if (app_renderOverlay(NULL, 0) != 0)
  {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplOpenGL2_RenderDrawData(igGetDrawData());
  }

  preSwap(udata);
  if (this->opt.preventBuffer)
  {
    app_glSwapBuffers();
    glFinish();
  }
  else
    app_glSwapBuffers();

  this->mouseUpdate = false;
  return true;
}

static void * opengl_createTexture(LG_Renderer * renderer,
  int width, int height, uint8_t * data)
{
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
  glTexImage2D(
      GL_TEXTURE_2D,
      0,
      GL_RGBA,
      width,
      height,
      0,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      data);

  glBindTexture(GL_TEXTURE_2D, 0);

  return (void*)(intptr_t)tex;
}

static void opengl_freeTexture(LG_Renderer * renderer, void * texture)
{
  GLuint tex = (GLuint)(intptr_t)texture;
  glDeleteTextures(1, &tex);
}

static void opengl_spiceConfigure(LG_Renderer * renderer, int width, int height)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  this->spiceSize.x = width;
  this->spiceSize.y = height;

  glBindTexture(GL_TEXTURE_2D, this->textures[SPICE_TEXTURE]);
  glTexImage2D
  (
    GL_TEXTURE_2D,
    0      ,
    GL_RGBA,
    width  ,
    height ,
    0      ,
    GL_BGRA,
    GL_UNSIGNED_BYTE,
    0
  );

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  // create the display lists
  glNewList(this->spiceList, GL_COMPILE);
    glBindTexture(GL_TEXTURE_2D, this->textures[SPICE_TEXTURE]);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLE_STRIP);
      glTexCoord2f(0.0f, 0.0f); glVertex2i(0, 0);
      glTexCoord2f(1.0f, 0.0f); glVertex2i(this->spiceSize.x, 0);
      glTexCoord2f(0.0f, 1.0f); glVertex2i(0, this->spiceSize.y);
      glTexCoord2f(1.0f, 1.0f);
      glVertex2i(this->spiceSize.x, this->spiceSize.y);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
  glEndList();
}

static void opengl_spiceDrawFill(LG_Renderer * renderer, int x, int y, int width,
    int height, uint32_t color)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  /* this is a fairly hacky way to do this, but since it's only for the fallback
   * spice display it's not really an issue */

  uint32_t line[width];
  for(int x = 0; x < width; ++x)
    line[x] = color;

  glBindTexture(GL_TEXTURE_2D, this->textures[SPICE_TEXTURE]);
  glPixelStorei(GL_UNPACK_ALIGNMENT , 4    );
  glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
  for(; y < height; ++y)
    glTexSubImage2D
    (
      GL_TEXTURE_2D,
      0      ,
      x      ,
      y      ,
      width  ,
      1      ,
      GL_BGRA,
      GL_UNSIGNED_BYTE,
      line
    );
  glBindTexture(GL_TEXTURE_2D, 0);
}

static void opengl_spiceDrawBitmap(LG_Renderer * renderer, int x, int y, int width,
    int height, int stride, uint8_t * data, bool topDown)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (!topDown)
  {
    // this is non-optimal but as spice is a fallback it's not too critical
    uint8_t line[stride];
    for(int y = 0; y < height / 2; ++y)
    {
      uint8_t * top = data + y                * stride;
      uint8_t * btm = data + (height - y - 1) * stride;
      memcpy(line, top , stride);
      memcpy(top , btm , stride);
      memcpy(btm , line, stride);
    }
  }

  glBindTexture(GL_TEXTURE_2D, this->textures[SPICE_TEXTURE]);
  glPixelStorei(GL_UNPACK_ALIGNMENT , 4         );
  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
  glTexSubImage2D
  (
    GL_TEXTURE_2D,
    0      ,
    x      ,
    y      ,
    width  ,
    height ,
    GL_BGRA,
    GL_UNSIGNED_BYTE,
    data
  );
  glBindTexture(GL_TEXTURE_2D, 0);
}

static void opengl_spiceShow(LG_Renderer * renderer, bool show)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  this->spiceShow = show;
}

const LG_RendererOps LGR_OpenGL =
{
  .getName       = opengl_getName,
  .setup         = opengl_setup,

  .create        = opengl_create,
  .initialize    = opengl_initialize,
  .deinitialize  = opengl_deinitialize,
  .onRestart     = opengl_onRestart,
  .onResize      = opengl_onResize,
  .onMouseShape  = opengl_onMouseShape,
  .onMouseEvent  = opengl_onMouseEvent,
  .onFrameFormat = opengl_onFrameFormat,
  .onFrame       = opengl_onFrame,
  .renderStartup = opengl_renderStartup,
  .render        = opengl_render,
  .createTexture = opengl_createTexture,
  .freeTexture   = opengl_freeTexture,

  .spiceConfigure  = opengl_spiceConfigure,
  .spiceDrawFill   = opengl_spiceDrawFill,
  .spiceDrawBitmap = opengl_spiceDrawBitmap,
  .spiceShow       = opengl_spiceShow
};

static bool _checkGLError(unsigned int line, const char * name)
{
  GLenum error = glGetError();
  if (error == GL_NO_ERROR)
    return false;

  const char * errStr;
  switch (error)
  {
    case GL_INVALID_ENUM:
      errStr = "GL_INVALID_ENUM";
      break;

    case GL_INVALID_VALUE:
      errStr = "GL_INVALID_VALUE";
      break;

    case GL_INVALID_OPERATION:
      errStr = "GL_INVALID_OPERATION";
      break;

    case GL_STACK_OVERFLOW:
      errStr = "GL_STACK_OVERFLOW";
      break;

    case GL_STACK_UNDERFLOW:
      errStr = "GL_STACK_UNDERFLOW";
      break;

    case GL_OUT_OF_MEMORY:
      errStr = "GL_OUT_OF_MEMORY";
      break;

    case GL_TABLE_TOO_LARGE:
      errStr = "GL_TABLE_TOO_LARGE";
      break;

    default:
      errStr = "unknown error";
  }
  DEBUG_ERROR("%d: %s = %d (%s)", line, name, error, errStr);
  return true;
}

static enum ConfigStatus configure(struct Inst * this)
{
  LG_LOCK(this->formatLock);
  if (!this->reconfigure)
  {
    LG_UNLOCK(this->formatLock);
    return CONFIG_STATUS_NOOP;
  }

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

    case FRAME_TYPE_RGBA16F:
      this->intFormat  = GL_RGB16F;
      this->vboFormat  = GL_RGBA;
      this->dataFormat = GL_HALF_FLOAT;
      break;

    default:
      DEBUG_ERROR("Unknown/unsupported compression type");
      return CONFIG_STATUS_ERROR;
  }

  // calculate the texture size in bytes
  this->texSize = this->format.frameHeight * this->format.pitch;
  this->texPos  = 0;

  g_gl_dynProcs.glGenBuffers(BUFFER_COUNT, this->vboID);
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

      g_gl_dynProcs.glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, this->vboID[i]);
      if (check_gl_error("glBindBuffer"))
      {
        LG_UNLOCK(this->formatLock);
        return CONFIG_STATUS_ERROR;
      }

      g_gl_dynProcs.glBufferData(
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
    g_gl_dynProcs.glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 0);
  }
  else
  {
    for(int i = 0; i < BUFFER_COUNT; ++i)
    {
      g_gl_dynProcs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[i]);
      if (check_gl_error("glBindBuffer"))
      {
        LG_UNLOCK(this->formatLock);
        return CONFIG_STATUS_ERROR;
      }

      g_gl_dynProcs.glBufferData(
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
    g_gl_dynProcs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
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
      this->format.frameWidth,
      this->format.frameHeight,
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
        glTexCoord2f(0.0f, 0.0f); glVertex2i(0, 0);
        glTexCoord2f(1.0f, 0.0f); glVertex2i(this->format.frameWidth, 0);
        glTexCoord2f(0.0f, 1.0f); glVertex2i(0, this->format.frameHeight);
        glTexCoord2f(1.0f, 1.0f);
        glVertex2i(this->format.frameWidth, this->format.frameHeight);
      glEnd();
      glBindTexture(GL_TEXTURE_2D, 0);
    glEndList();
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  g_gl_dynProcs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  this->drawStart   = nanotime();
  this->configured  = true;
  this->reconfigure = false;

  LG_UNLOCK(this->formatLock);
  return CONFIG_STATUS_OK;
}

static void deconfigure(struct Inst * this)
{
  if (this->hasFrames)
  {
    glDeleteTextures(BUFFER_COUNT, this->frames);
    this->hasFrames = false;
  }

  if (this->hasBuffers)
  {
    g_gl_dynProcs.glDeleteBuffers(BUFFER_COUNT, this->vboID);
    this->hasBuffers = false;
  }

  if (this->amdPinnedMemSupport)
  {
    for(int i = 0; i < BUFFER_COUNT; ++i)
    {
      if (this->fences[i])
      {
        g_gl_dynProcs.glDeleteSync(this->fences[i]);
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

static void updateMouseShape(struct Inst * this)
{
  LG_LOCK(this->mouseLock);
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

static bool opengl_bufferFn(void * opaque, const void * data, size_t size)
{
  struct Inst * this = (struct Inst *)opaque;

  // update the buffer, this performs a DMA transfer if possible
  g_gl_dynProcs.glBufferSubData(
    GL_PIXEL_UNPACK_BUFFER,
    this->texPos,
    size,
    data
  );
  check_gl_error("glBufferSubData");

  this->texPos += size;
  return true;
}

static bool drawFrame(struct Inst * this)
{
  if (g_gl_dynProcs.glIsSync(this->fences[this->texWIndex]))
  {
    switch(g_gl_dynProcs.glClientWaitSync(this->fences[this->texWIndex], 0, GL_TIMEOUT_IGNORED))
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
        DEBUG_ERROR("Wait failed %d", glGetError());
        break;
    }

    g_gl_dynProcs.glDeleteSync(this->fences[this->texWIndex]);
    this->fences[this->texWIndex] = NULL;

    this->texRIndex = this->texWIndex;
    if (++this->texWIndex == BUFFER_COUNT)
      this->texWIndex = 0;
  }

  LG_LOCK(this->frameLock);
  if (!atomic_exchange_explicit(&this->frameUpdate, false, memory_order_acquire))
  {
    LG_UNLOCK(this->frameLock);
    return true;
  }

  LG_LOCK(this->formatLock);
  glBindTexture(GL_TEXTURE_2D, this->frames[this->texWIndex]);
  g_gl_dynProcs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[this->texWIndex]);

  const int bpp = this->format.bpp / 8;
  glPixelStorei(GL_UNPACK_ALIGNMENT , bpp);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, this->format.frameWidth);

  this->texPos = 0;

  framebuffer_read_fn(
    this->frame,
    this->format.frameHeight,
    this->format.frameWidth,
    bpp,
    this->format.pitch,
    opengl_bufferFn,
    this
  );

  LG_UNLOCK(this->frameLock);

  // update the texture
  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    0,
    0,
    this->format.frameWidth ,
    this->format.frameHeight,
    this->vboFormat,
    this->dataFormat,
    (void*)0
  );
  if (check_gl_error("glTexSubImage2D"))
  {
    DEBUG_ERROR("texWIndex: %u, width: %u, height: %u, vboFormat: %x, texSize: %lu",
      this->texWIndex, this->format.frameWidth, this->format.frameHeight,
      this->vboFormat, this->texSize
    );
  }

  // unbind the buffer
  g_gl_dynProcs.glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  const bool mipmap = this->opt.mipmap && (
    (this->format.frameWidth  > this->destRect.w) ||
    (this->format.frameHeight > this->destRect.h));

  if (mipmap)
  {
    g_gl_dynProcs.glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  }
  else
  {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  // set a fence so we don't overwrite a buffer in use
  this->fences[this->texWIndex] =
    g_gl_dynProcs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();

  LG_UNLOCK(this->formatLock);
  this->texReady = true;
  return true;
}

static void drawMouse(struct Inst * this)
{
  if (!this->mouseVisible)
    return;

  glPushMatrix();
  glTranslatef(this->mousePos.x, this->mousePos.y, 0.0f);
  glCallList(this->mouseList);
  glPopMatrix();
}
