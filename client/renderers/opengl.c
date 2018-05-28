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
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <malloc.h>

#include <SDL2/SDL_ttf.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include "debug.h"
#include "utils.h"
#include "lg-decoders.h"

#define BUFFER_COUNT       2

#define FPS_TEXTURE        0
#define MOUSE_TEXTURE      1
#define TEXTURE_COUNT      2

struct Options
{
  bool mipmap;
  bool vsync;
  bool preventBuffer;
  bool amdPinnedMem;
};

static struct Options defaultOptions =
{
  .mipmap        = true,
  .vsync         = true,
  .preventBuffer = true,
  .amdPinnedMem  = true,
};

struct Inst
{
  LG_RendererParams params;
  struct Options    opt;

  bool              amdPinnedMemSupport;
  bool              preConfigured;
  bool              configured;
  bool              reconfigure;
  SDL_GLContext     glContext;

  SDL_Point         window;
  bool              resizeWindow;
  bool              frameUpdate;

  LG_Lock           formatLock;
  LG_RendererFormat format;
  GLuint            intFormat;
  GLuint            vboFormat;
  size_t            texSize;
  const LG_Decoder* decoder;
  void            * decoderData;

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
  void            * decoderFrames[BUFFER_COUNT];
  GLuint            textures[TEXTURE_COUNT];

  bool              fpsTexture;
  uint64_t          lastFrameTime;
  uint64_t          renderTime;
  uint64_t          frameCount;
  uint64_t          renderCount;
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
  uint64_t          lastMouseDraw;
  LG_RendererCursor mouseType;
  bool              mouseVisible;
  SDL_Rect          mousePos;
};

static bool _check_gl_error(unsigned int line, const char * name);
#define check_gl_error(name) _check_gl_error(__LINE__, name)

static void deconfigure(struct Inst * this);
static bool pre_configure(struct Inst * this, SDL_Window *window);
static bool configure(struct Inst * this, SDL_Window *window);
static void update_mouse_shape(struct Inst * this, bool * newShape);
static bool draw_frame(struct Inst * this);
static void draw_mouse(struct Inst * this);
static void render_wait();

const char * opengl_get_name()
{
  return "OpenGL";
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
  memcpy(&this->params, &params        , sizeof(LG_RendererParams));
  memcpy(&this->opt   , &defaultOptions, sizeof(struct Options   ));

  LG_LOCK_INIT(this->formatLock);
  LG_LOCK_INIT(this->syncLock  );
  LG_LOCK_INIT(this->mouseLock );

  return true;
}

bool opengl_initialize(void * opaque, Uint32 * sdlFlags)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return false;

  *sdlFlags = SDL_WINDOW_OPENGL;
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  return true;
}

void opengl_deinitialize(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  deconfigure(this);
  if (this->mouseData)
    free(this->mouseData);

  LG_LOCK_FREE(this->formatLock);
  LG_LOCK_FREE(this->syncLock  );
  LG_LOCK_FREE(this->mouseLock );

  free(this);
}

void opengl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  this->window.x = width;
  this->window.y = height;
  memcpy(&this->destRect, &destRect, sizeof(LG_RendererRect));

  this->resizeWindow = true;
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

bool opengl_on_frame_event(void * opaque, const LG_RendererFormat format, const uint8_t * data)
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
    this->format.comp   != format.comp   ||
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
  if (!this->decoder->decode(this->decoderData, data, format.pitch))
  {
    DEBUG_ERROR("decode returned failure");
    LG_UNLOCK(this->syncLock);
    return false;
  }
  this->frameUpdate = true;
  LG_UNLOCK(this->syncLock);

  ++this->frameCount;
  return true;
}

bool opengl_render(void * opaque, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return false;

  if (!pre_configure(this, window))
    return false;

  if (this->resizeWindow)
  {
    // setup the projection matrix
    glViewport(0, 0, this->window.x, this->window.y);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, this->window.x, this->window.y, 0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(this->destRect.x, this->destRect.y, 0.0f);
    glScalef(
      (float)this->destRect.w / (float)this->format.width,
      (float)this->destRect.h / (float)this->format.height,
      1.0f
    );

    this->resizeWindow = false;
  }

  if (!configure(this, window))
  {
    render_wait();
    SDL_GL_SwapWindow(window);
    return true;
  }

  if (!draw_frame(this))
    return false;

  if (!this->texReady)
  {
    render_wait();
    SDL_GL_SwapWindow(window);
    return true;
  }

  if (this->params.showFPS && this->renderTime > 1e9)
  {
    char str[128];
    const float avgFPS    = 1000.0f / (((float)this->renderTime / this->frameCount ) / 1e6f);
    const float renderFPS = 1000.0f / (((float)this->renderTime / this->renderCount) / 1e6f);
    snprintf(str, sizeof(str), "UPS: %8.4f, FPS: %8.4f", avgFPS, renderFPS);
    SDL_Color color = {0xff, 0xff, 0xff};
    SDL_Surface *textSurface = NULL;
    if (!(textSurface = TTF_RenderText_Blended(this->params.font, str, color)))
    {
      DEBUG_ERROR("Failed to render text");
      LG_UNLOCK(this->formatLock);
      return false;
    }

    glBindTexture(GL_TEXTURE_2D       , this->textures[FPS_TEXTURE]);
    glPixelStorei(GL_UNPACK_ALIGNMENT , 4                          );
    glPixelStorei(GL_UNPACK_ROW_LENGTH, textSurface->w             );
    glTexImage2D(
      GL_TEXTURE_2D,
      0,
      textSurface->format->BytesPerPixel,
      textSurface->w,
      textSurface->h,
      0,
      GL_BGRA,
      GL_UNSIGNED_BYTE,
      textSurface->pixels
    );

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    glBindTexture(GL_TEXTURE_2D, 0);

    this->fpsRect.x = 5;
    this->fpsRect.y = 5;
    this->fpsRect.w = textSurface->w;
    this->fpsRect.h = textSurface->h;

    SDL_FreeSurface(textSurface);

    this->renderTime  = 0;
    this->frameCount  = 0;
    this->renderCount = 0;
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
      glDisable(GL_BLEND);

      glPopMatrix();
    glEndList();
  }

  bool newShape;
  update_mouse_shape(this, &newShape);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glCallList(this->texList + this->texIndex);
  draw_mouse(this);
  if (this->fpsTexture)
    glCallList(this->fpsList);

  if (this->opt.preventBuffer)
  {
    SDL_GL_SwapWindow(window);
    glFinish();
  }
  else
    SDL_GL_SwapWindow(window);

  const uint64_t t    = nanotime();
  this->renderTime   += t - this->lastFrameTime;
  this->lastFrameTime = t;
  ++this->renderCount;

  this->mouseUpdate   = false;
  this->lastMouseDraw = t;
  return true;
}

static void render_wait()
{
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void handle_opt_mipmap(void * opaque, const char *value)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  this->opt.mipmap = LG_RendererValueToBool(value);
}

static void handle_opt_vsync(void * opaque, const char *value)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  this->opt.vsync = LG_RendererValueToBool(value);
}

static void handle_opt_prevent_buffer(void * opaque, const char *value)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  this->opt.preventBuffer = LG_RendererValueToBool(value);
}

static void handle_opt_amd_pinned_mem(void * opaque, const char *value)
{
  struct Inst * this = (struct Inst *)opaque;
  if (!this)
    return;

  this->opt.amdPinnedMem = LG_RendererValueToBool(value);
}


static LG_RendererOpt opengl_options[] =
{
  {
    .name      = "mipmap",
    .desc      = "Enable or disable mipmapping [default: enabled]",
    .validator = LG_RendererValidatorBool,
    .handler   = handle_opt_mipmap
  },
  {
    .name      = "vsync",
    .desc      ="Enable or disable vsync [default: enabled]",
    .validator = LG_RendererValidatorBool,
    .handler   = handle_opt_vsync
  },
  {
    .name      = "preventBuffer",
    .desc      = "Prevent the driver from buffering frames [default: disabled]",
    .validator = LG_RendererValidatorBool,
    .handler   = handle_opt_prevent_buffer
  },
  {
    .name      = "amdPinnedMem",
    .desc      = "Use GL_AMD_pinned_memory if it is available [default: enabled]",
    .validator = LG_RendererValidatorBool,
    .handler   = handle_opt_amd_pinned_mem
  }
};

const LG_Renderer LGR_OpenGL =
{
  .get_name       = opengl_get_name,
  .options        = opengl_options,
  .option_count   = LGR_OPTION_COUNT(opengl_options),
  .create         = opengl_create,
  .initialize     = opengl_initialize,
  .deinitialize   = opengl_deinitialize,
  .on_resize      = opengl_on_resize,
  .on_mouse_shape = opengl_on_mouse_shape,
  .on_mouse_event = opengl_on_mouse_event,
  .on_frame_event = opengl_on_frame_event,
  .render         = opengl_render
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

static bool pre_configure(struct Inst * this, SDL_Window *window)
{
  if (this->preConfigured)
    return true;

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

  SDL_GL_SetSwapInterval(this->opt.vsync ? 1 : 0);
  this->preConfigured = true;
  return true;
}

static bool configure(struct Inst * this, SDL_Window *window)
{
  LG_LOCK(this->formatLock);
  if (!this->reconfigure)
  {
    LG_UNLOCK(this->formatLock);
    return this->configured;
  }

  if (this->configured)
    deconfigure(this);

  switch(this->format.comp)
  {
    case LG_COMPRESSION_NONE:
      this->decoder = &LGD_NULL;
      break;

    default:
      DEBUG_ERROR("Unknown/unsupported compression type");
      return false;
  }

  DEBUG_INFO("Using decoder: %s", this->decoder->name);

  if (!this->decoder->create(&this->decoderData))
  {
    DEBUG_ERROR("Failed to create the decoder");
    return false;
  }

  if (!this->decoder->initialize(
    this->decoderData,
    this->format,
    window))
  {
    DEBUG_ERROR("Failed to initialize decoder");
    return false;
  }

  switch(this->decoder->get_out_format(this->decoderData))
  {
    case LG_OUTPUT_BGRA:
      this->intFormat = GL_RGBA8;
      this->vboFormat = GL_BGRA;
      break;

    case LG_OUTPUT_YUV420:
      // fixme
      this->intFormat = GL_RGBA8;
      this->vboFormat = GL_BGRA;
      break;

    default:
      DEBUG_ERROR("Format not supported");
      LG_UNLOCK(this->formatLock);
      return false;
  }

  // calculate the texture size in bytes
  this->texSize =
    this->format.height *
    this->decoder->get_frame_pitch(this->decoderData);

  // generate lists for drawing
  this->texList    = glGenLists(BUFFER_COUNT);
  this->fpsList    = glGenLists(1);
  this->mouseList  = glGenLists(1);

  // generate the pixel unpack buffers if the decoder isn't going to do it for us
  if (!this->decoder->has_gl)
  {
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
      this->texPixels[0] = memalign(pagesize, this->texSize * BUFFER_COUNT);
      memset(this->texPixels[0], 0, this->texSize * BUFFER_COUNT);
      for(int i = 1; i < BUFFER_COUNT; ++i)
        this->texPixels[i] = this->texPixels[0] + this->texSize;

      for(int i = 0; i < BUFFER_COUNT; ++i)
      {
        glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, this->vboID[i]);

        if (check_gl_error("glBindBuffer"))
        {
          LG_UNLOCK(this->formatLock);
          return false;
        }
        glBufferData(
          GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD,
          this->texSize,
          this->texPixels[i],
          GL_STREAM_DRAW);

        if (check_gl_error("glBufferData"))
        {
          LG_UNLOCK(this->formatLock);
          return false;
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
          return false;
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
          return false;
        }
      }
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
  }

  // create the overlay textures
  glGenTextures(TEXTURE_COUNT, this->textures);
  if (check_gl_error("glGenTextures"))
  {
    LG_UNLOCK(this->formatLock);
    return false;
  }
  this->hasTextures = true;

  // create the frame textures
  glGenTextures(BUFFER_COUNT, this->frames);
  if (check_gl_error("glGenTextures"))
  {
    LG_UNLOCK(this->formatLock);
    return false;
  }
  this->hasFrames = true;

  for(int i = 0; i < BUFFER_COUNT; ++i)
  {
    // bind and create the new texture
    glBindTexture(GL_TEXTURE_2D, this->frames[i]);
    if (check_gl_error("glBindTexture"))
    {
      LG_UNLOCK(this->formatLock);
      return false;
    }

    glTexImage2D(
      GL_TEXTURE_2D,
      0,
      this->intFormat,
      this->format.width,
      this->format.height,
      0,
      this->vboFormat,
      GL_UNSIGNED_BYTE,
      (void*)0
    );
    if (check_gl_error("glTexImage2D"))
    {
      LG_UNLOCK(this->formatLock);
      return false;
    }

    if (this->decoder->has_gl)
    {
      if (!this->decoder->init_gl_texture(
        this->decoderData,
        GL_TEXTURE_2D,
        this->frames[i],
        &this->decoderFrames[i]))
      {
        LG_UNLOCK(this->formatLock);
        return false;
      }
    }
    else
    {
      // configure the texture
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

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
    glEndList();
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  glEnable(GL_TEXTURE_2D);
  glEnable(GL_COLOR_MATERIAL);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);

  this->resizeWindow = true;
  this->drawStart    = nanotime();

  this->configured  = true;
  this->reconfigure = false;

  LG_UNLOCK(this->formatLock);
  return true;
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
    if (this->decoder->has_gl)
    {
      for(int i = 0; i < BUFFER_COUNT; ++i)
      {
        if (this->decoderFrames[i])
          this->decoder->free_gl_texture(
            this->decoderData,
            this->decoderFrames[i]
          );
        this->decoderFrames[i] = NULL;
      }
    }

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
    if (this->texPixels[0])
      free(this->texPixels[0]);

    for(int i = 0; i < BUFFER_COUNT; ++i)
    {
      if (this->fences[i])
      {
        glDeleteSync(this->fences[i]);
        this->fences[i] = NULL;
      }
      this->texPixels[i] = NULL;
    }
  }

  if (this->glContext)
  {
    SDL_GL_DeleteContext(this->glContext);
    this->glContext = NULL;
  }

  if (this->decoderData)
  {
    this->decoder->destroy(this->decoderData);
    this->decoderData = NULL;
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
        glDisable(GL_COLOR_LOGIC_OP);
      glEndList();
      break;
    }
  }

  this->mouseUpdate = true;
  LG_UNLOCK(this->mouseLock);
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
  if (this->decoder->has_gl)
  {
    if (!this->decoder->update_gl_texture(
      this->decoderData,
      this->decoderFrames[this->texIndex]
    ))
    {
      LG_UNLOCK(this->formatLock);
      DEBUG_ERROR("Failed to update the texture from the decoder");
      return false;
    }
  }
  else
  {
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

    const uint8_t * data = this->decoder->get_buffer(this->decoderData);
    if (!data)
    {
      LG_UNLOCK(this->formatLock);
      DEBUG_ERROR("Failed to get the buffer from the decoder");
      return false;
    }

    glBindTexture(GL_TEXTURE_2D, this->frames[this->texIndex]);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[this->texIndex]);

    glPixelStorei(GL_UNPACK_ALIGNMENT  , 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH ,
      this->decoder->get_frame_stride(this->decoderData)
    );

    // update the buffer, this performs a DMA transfer if possible
    glBufferSubData(
      GL_PIXEL_UNPACK_BUFFER,
      0,
      this->texSize,
      data
    );
    check_gl_error("glBufferSubData");

    // update the texture
    glTexSubImage2D(
      GL_TEXTURE_2D,
      0,
      0,
      0,
      this->format.width ,
      this->format.height,
      this->vboFormat,
      GL_UNSIGNED_BYTE,
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
  }

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