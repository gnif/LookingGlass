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

#include <SDL2/SDL_ttf.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include "debug.h"
#include "memcpySSE.h"
#include "utils.h"

#define BUFFER_COUNT       1

#define FRAME_TEXTURE      0
#define FPS_TEXTURE        1
#define MOUSE_TEXTURE      2
#define TEXTURE_COUNT      3

static PFNGLXGETVIDEOSYNCSGIPROC  glXGetVideoSyncSGI  = NULL;
static PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI = NULL;

struct LGR_OpenGL
{
  LG_RendererParams params;
  bool              configured;
  SDL_GLContext     glContext;
  bool              doneInfo;

  SDL_Point         window;
  bool              resizeWindow;
  bool              frameUpdate;

  LG_RendererFormat format;
  GLuint            intFormat;
  GLuint            vboFormat;
  size_t            texSize;

  uint64_t          drawStart;
  bool              hasBuffers;
  GLuint            vboID[1];
  uint8_t         * texPixels[BUFFER_COUNT];
  int               texIndex;
  int               texList;
  int               fpsList;
  int               mouseList;
  LG_RendererRect   destRect;
  bool              mipmap;

  bool              hasTextures;
  GLuint            textures[TEXTURE_COUNT];

  uint              gpuFrameCount;
  bool              fpsTexture;
  uint64_t          lastFrameTime;
  uint64_t          renderTime;
  uint64_t          frameCount;
  SDL_Rect          fpsRect;

  bool              mouseUpdate;
  uint64_t          lastMouseDraw;
  LG_RendererCursor mouseType;
  bool              mouseVisible;
  SDL_Rect          mousePos;
};

bool lgr_opengl_check_error(const char * name)
{
  GLenum error = glGetError();
  if (error == GL_NO_ERROR)
    return false;

  const GLubyte * errStr = gluErrorString(error);
  DEBUG_ERROR("%s = %d (%s)", name, error, errStr);
  return true;
}

const char * lgr_opengl_get_name()
{
  return "OpenGL";
}

bool lgr_opengl_initialize(void ** opaque, const LG_RendererParams params, Uint32 * sdlFlags)
{
  // create our local storage
  *opaque = malloc(sizeof(struct LGR_OpenGL));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct LGR_OpenGL));
    return false;
  }
  memset(*opaque, 0, sizeof(struct LGR_OpenGL));

  struct LGR_OpenGL * this = (struct LGR_OpenGL *)*opaque;
  memcpy(&this->params, &params, sizeof(LG_RendererParams));

  if (!glXGetVideoSyncSGI)
  {
    glXGetVideoSyncSGI  = (PFNGLXGETVIDEOSYNCSGIPROC )glXGetProcAddress((const GLubyte *)"glXGetVideoSyncSGI" );
    glXWaitVideoSyncSGI = (PFNGLXWAITVIDEOSYNCSGIPROC)glXGetProcAddress((const GLubyte *)"glXWaitVideoSyncSGI");

    if (!glXGetVideoSyncSGI || !glXWaitVideoSyncSGI)
    {
      glXGetVideoSyncSGI = NULL;
      DEBUG_ERROR("Failed to get proc addresses");
      return false;
    }
  }

  *sdlFlags = SDL_WINDOW_OPENGL;
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
  return true;
}

bool lgr_opengl_configure(void * opaque, SDL_Window *window, const LG_RendererFormat format)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this)
    return false;

  if (this->configured)
  {
    DEBUG_ERROR("Renderer already configured, call deconfigure first");
    return false;
  }

  this->glContext = SDL_GL_CreateContext(window);
  if (!this->glContext)
  {
    DEBUG_ERROR("Failed to create the OpenGL context");
    return false;
  }

  if (!this->doneInfo)
  {
    DEBUG_INFO("Vendor  : %s", glGetString(GL_VENDOR  ));
    DEBUG_INFO("Renderer: %s", glGetString(GL_RENDERER));
    DEBUG_INFO("Version : %s", glGetString(GL_VERSION ));
    this->doneInfo = true;
  }

  if (SDL_GL_MakeCurrent(window, this->glContext) != 0)
  {
    DEBUG_ERROR("Failed to make the GL context current");
    return false;
  }

  SDL_GL_SetSwapInterval(0);

  // check if the GPU supports GL_ARB_buffer_storage first
  // there is no advantage to this renderer if it is not present.
  const GLubyte * extensions = glGetString(GL_EXTENSIONS);
  if (!gluCheckExtension((const GLubyte *)"GL_ARB_buffer_storage", extensions))
  {
    DEBUG_INFO("The GPU doesn't support GL_ARB_buffer_storage");
    return false;
  }

  // assume 24 and 32 bit formats are RGB and RGBA
  switch(format.bpp)
  {
    case 24:
      this->intFormat = GL_RGB8;
      this->vboFormat = GL_BGR;
      break;

    case 32:
      this->intFormat = GL_RGBA8;
      this->vboFormat = GL_BGRA;
      break;

    default:
      DEBUG_INFO("%d bpp not supported", format.bpp);
      return false;
  }

  // calculate the texture size in bytes
  this->texSize = format.height * format.pitch;

  // generate lists for drawing
  this->texList    = glGenLists(BUFFER_COUNT);
  this->fpsList    = glGenLists(1);
  this->mouseList  = glGenLists(1);

  // generate the pixel unpack buffers
  glGenBuffers(1, this->vboID);
  if (lgr_opengl_check_error("glGenBuffers"))
    return false;
  this->hasBuffers = true;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[0]);
  if (lgr_opengl_check_error("glBindBuffer"))
    return false;

  glBufferStorage(
    GL_PIXEL_UNPACK_BUFFER,
    this->texSize * BUFFER_COUNT,
    NULL,
    GL_MAP_WRITE_BIT      |
    GL_MAP_PERSISTENT_BIT
  );
  if (lgr_opengl_check_error("glBufferStorage"))
    return false;

  this->texPixels[0] = glMapBufferRange(
    GL_PIXEL_UNPACK_BUFFER,
    0,
    this->texSize * BUFFER_COUNT,
    GL_MAP_WRITE_BIT         |
    GL_MAP_PERSISTENT_BIT    |
    GL_MAP_FLUSH_EXPLICIT_BIT
  );

  if (lgr_opengl_check_error("glMapBufferRange"))
    return false;

  for(int i = 1; i < BUFFER_COUNT; ++i)
    this->texPixels[i] = this->texPixels[i-1] + this->texSize;

  // create the textures
  glGenTextures(TEXTURE_COUNT, this->textures);
  if (lgr_opengl_check_error("glGenTextures"))
    return false;
  this->hasTextures = true;

  // create the frame texture
  glBindTexture(GL_TEXTURE_2D, this->textures[FRAME_TEXTURE]);
  if (lgr_opengl_check_error("glBindTexture"))
    return false;

  glTexImage2D(
    GL_TEXTURE_2D,
    0,
    this->intFormat,
    format.width,
    format.height * BUFFER_COUNT,
    0,
    this->vboFormat,
    GL_UNSIGNED_BYTE,
    (void*)0
  );
  if (lgr_opengl_check_error("glTexImage2D"))
    return false;

  // configure the texture
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  for (int i = 0; i < BUFFER_COUNT; ++i)
  {
    const float ts = (1.0f / BUFFER_COUNT) * i;
    const float te = (1.0f / BUFFER_COUNT) + ts;

    // create the display lists
    glNewList(this->texList + i, GL_COMPILE);
      glBindTexture(GL_TEXTURE_2D, this->textures[FRAME_TEXTURE]);
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, ts); glVertex2i(0           , 0            );
        glTexCoord2f(1.0f, ts); glVertex2i(format.width, 0            );
        glTexCoord2f(0.0f, te); glVertex2i(0           , format.height);
        glTexCoord2f(1.0f, te); glVertex2i(format.width, format.height);
     glEnd();
    glEndList();
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  glEnable(GL_TEXTURE_2D);
  glEnable(GL_COLOR_MATERIAL);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_FUNC_ADD);
  glEnable(GL_SCISSOR_TEST);

  this->resizeWindow = true;
  this->drawStart    = nanotime();
  glXGetVideoSyncSGI(&this->gpuFrameCount);

  // copy the format into the local storage
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  this->configured = true;
  return true;
}

void lgr_opengl_deconfigure(void * opaque)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->configured)
    return;

  if (this->hasTextures)
  {
    glDeleteTextures(TEXTURE_COUNT, this->textures);
    this->hasTextures = false;
  }

  if (this->hasBuffers)
  {
    glDeleteBuffers(1, this->vboID);
    this->hasBuffers = false;
  }

  if (this->glContext)
  {
    SDL_GL_DeleteContext(this->glContext);
    this->glContext = NULL;
  }

  this->configured = false;
}

void lgr_opengl_deinitialize(void * opaque)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this)
    return;

  if (this->configured)
    lgr_opengl_deconfigure(opaque);

  free(this);
}

bool lgr_opengl_is_compatible(void * opaque, const LG_RendererFormat format)
{
  const struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->configured)
    return false;

  return (memcmp(&this->format, &format, sizeof(LG_RendererFormat)) == 0);
}

void lgr_opengl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->configured)
    return;

  this->window.x  = width;
  this->window.y = height;
  memcpy(&this->destRect, &destRect, sizeof(LG_RendererRect));

  this->resizeWindow = true;
}

bool lgr_opengl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->configured)
    return false;

  this->mouseType = cursor;
  switch(cursor)
  {
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
        GL_RGBA,
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

    case LG_CURSOR_MASKED_COLOR:
    {
      //TODO
      break;
    }
  }

  this->mouseUpdate = true;
  return true;
}

bool lgr_opengl_on_mouse_event(void * opaque, const bool visible, const int x, const int y)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->configured)
    return false;

  if (this->mousePos.x == x && this->mousePos.y == y && this->mouseVisible == visible)
    return true;

  this->mouseVisible = visible;
  this->mousePos.x   = x;
  this->mousePos.y   = y;
  this->mouseUpdate  = true;
  return false;
}

bool lgr_opengl_on_frame_event(void * opaque, const uint8_t * data)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this)
  {
    DEBUG_ERROR("Invalid opaque pointer");
    return false;
  }

  if (!this->configured)
  {
    DEBUG_ERROR("Not configured");
    return false;
  }

  if (this->params.showFPS && this->renderTime > 1e9)
  {
    char str[128];
    const float avgFPS = 1000.0f / (((float)this->renderTime / this->frameCount) / 1e6f);
    snprintf(str, sizeof(str), "FPS: %8.4f", avgFPS);
    SDL_Color color = {0xff, 0xff, 0xff};
    SDL_Surface *textSurface = NULL;
    if (!(textSurface = TTF_RenderText_Blended(this->params.font, str, color)))
    {
      DEBUG_ERROR("Failed to render text");
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

    this->renderTime = 0;
    this->frameCount = 0;
    this->fpsTexture = true;

    glNewList(this->fpsList, GL_COMPILE);
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
    glEndList();
  }

  // bind the texture and update it
  glBindTexture(GL_TEXTURE_2D        , this->textures[FRAME_TEXTURE]);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[0]               );
  glPixelStorei(GL_UNPACK_ALIGNMENT  , 4                            );
  glPixelStorei(GL_UNPACK_ROW_LENGTH , this->format.stride          );

  // copy the buffer to the texture
  memcpySSE(this->texPixels[this->texIndex], data, this->texSize);
  glFlushMappedBufferRange(
    GL_PIXEL_UNPACK_BUFFER,
    this->texSize * this->texIndex,
    this->texSize
  );

  // update the texture
  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    0,
    this->texIndex * this->format.height,
    this->format.width ,
    this->format.height,
    this->vboFormat,
    GL_UNSIGNED_BYTE,
    (void*)(this->texIndex * this->texSize)
  );
  lgr_opengl_check_error("glTexSubImage2D");

  // unbind the buffer
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  const bool mipmap = this->params.resample && (
    (this->format.width  > this->destRect.w) ||
    (this->format.height > this->destRect.h));

  if (this->mipmap != mipmap)
  {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
        mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    this->mipmap = mipmap;
  }

  if (mipmap)
    glGenerateMipmap(GL_TEXTURE_2D);

  glBindTexture(GL_TEXTURE_2D, 0);

  if (++this->texIndex == BUFFER_COUNT)
    this->texIndex = 0;

  this->frameUpdate = true;
  return true;
}

static inline void lgr_opengl_draw_mouse(struct LGR_OpenGL * this)
{
  if (!this->mouseVisible)
    return;

  glPushMatrix();
  glTranslatef(this->mousePos.x, this->mousePos.y, 0.0f);
  glCallList(this->mouseList);
  glPopMatrix();
}

bool lgr_opengl_render(void * opaque)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->configured)
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

    // update the scissor rect to prevent drawing outside of the frame
    glScissor(
      this->destRect.x,
      this->destRect.y,
      this->destRect.w,
      this->destRect.h
    );

    this->resizeWindow = false;
  }


  if (!this->frameUpdate)
  {
    if (!this->mouseUpdate)
      return true;

    // don't update the mouse too fast
    const uint64_t delta = nanotime() - this->lastMouseDraw;
    if (delta < 1e7)
      return true;
  }

  // wait for vsync
  unsigned int count;
  glXWaitVideoSyncSGI(1, 0, &count);

  glDisable(GL_SCISSOR_TEST);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);

  glCallList(this->texList + this->texIndex);
  lgr_opengl_draw_mouse(this);
  if (this->fpsTexture)
    glCallList(this->fpsList);
  glFlush();

  ++this->frameCount;
  const uint64_t t    = nanotime();
  this->renderTime   += t - this->lastFrameTime;
  this->lastFrameTime = t;

  this->frameUpdate   = false;
  this->mouseUpdate   = false;
  this->lastMouseDraw = t;
  return true;
}

const LG_Renderer LGR_OpenGL =
{
  .get_name       = lgr_opengl_get_name,
  .initialize     = lgr_opengl_initialize,
  .configure      = lgr_opengl_configure,
  .deconfigure    = lgr_opengl_deconfigure,
  .deinitialize   = lgr_opengl_deinitialize,
  .is_compatible  = lgr_opengl_is_compatible,
  .on_resize      = lgr_opengl_on_resize,
  .on_mouse_shape = lgr_opengl_on_mouse_shape,
  .on_mouse_event = lgr_opengl_on_mouse_event,
  .on_frame_event = lgr_opengl_on_frame_event,
  .render         = lgr_opengl_render
};