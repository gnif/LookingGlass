#include "lg-renderer.h"
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include <SDL_ttf.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>

#include "debug.h"
#include "memcpySSE.h"
#include "utils.h"

#define VBO_BUFFERS 2

#define FPS_TEXTURE        (VBO_BUFFERS  )
#define MOUSE_TEXTURE      (VBO_BUFFERS+1)
#define TEXTURE_COUNT      (VBO_BUFFERS+2)

static PFNGLXGETVIDEOSYNCSGIPROC  glXGetVideoSyncSGI  = NULL;
static PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI = NULL;

struct LGR_OpenGL
{
  LG_RendererParams params;
  bool              initialized;
  SDL_GLContext     glContext;
  bool              resizeWindow;
  bool              mouseUpdate;
  bool              frameUpdate;

  LG_RendererFormat format;
  GLuint            intFormat;
  GLuint            vboFormat;
  size_t            texSize;

  uint64_t          drawStart;
  bool              hasBuffers;
  GLuint            vboID[VBO_BUFFERS];
  uint8_t         * texPixels[VBO_BUFFERS];
  int               texIndex;
  int               texList;
  int               fpsList;
  LG_RendererRect   destRect;

  bool              hasTextures;
  GLuint            textures[TEXTURE_COUNT];

  uint              gpuFrameCount;
  bool              fpsTexture;
  uint64_t          lastFrameTime;
  uint64_t          renderTime;
  uint64_t          frameCount;
  SDL_Rect          fpsRect;

  LG_RendererCursor mouseType;
  bool              mouseRepair;
  SDL_Rect          mouseRepairPos;
  bool              mouseVisible;
  SDL_Rect          mousePos;
};

void lgr_opengl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect);

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

bool lgr_opengl_initialize(void ** opaque, const LG_RendererParams params, const LG_RendererFormat format)
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

  this->glContext = SDL_GL_CreateContext(params.window);
  if (!this->glContext)
  {
    DEBUG_ERROR("Failed to create the OpenGL context");
    return false;
  }

  if (SDL_GL_MakeCurrent(params.window, this->glContext) != 0)
  {
    DEBUG_ERROR("Failed to make the GL context current");
    return false;
  }

  DEBUG_INFO("Vendor  : %s", glGetString(GL_VENDOR  ));
  DEBUG_INFO("Renderer: %s", glGetString(GL_RENDERER));
  DEBUG_INFO("Version : %s", glGetString(GL_VERSION ));

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

  SDL_GL_SetSwapInterval(1);

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
  this->texList = glGenLists(2);
  this->fpsList = glGenLists(1);

  // generate the pixel unpack buffers
  glGenBuffers(VBO_BUFFERS, this->vboID);
  if (lgr_opengl_check_error("glGenBuffers"))
    return false;
  this->hasBuffers = true;

  // persistant bind the buffers
  for (int i = 0; i < VBO_BUFFERS; ++i)
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[i]);
    if (lgr_opengl_check_error("glBindBuffer"))
      return false;

    glBufferStorage(GL_PIXEL_UNPACK_BUFFER, this->texSize, 0, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);
    if (lgr_opengl_check_error("glBufferStorage"))
      return false;

    this->texPixels[i] = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, this->texSize,
        GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_FLUSH_EXPLICIT_BIT);
    if (lgr_opengl_check_error("glMapBufferRange"))
      return false;

    if (!this->texPixels[i])
    {
      DEBUG_ERROR("Failed to map the buffer range");
      return false;
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  // create the textures
  glGenTextures(TEXTURE_COUNT, this->textures);
  if (lgr_opengl_check_error("glGenTextures"))
    return false;
  this->hasTextures = true;

  // bind the textures to the unpack buffers
  for (int i = 0; i < VBO_BUFFERS; ++i)
  {
    glBindTexture(GL_TEXTURE_2D, this->textures[i]);
    if (lgr_opengl_check_error("glBindTexture"))
      return false;

    glTexImage2D(
      GL_TEXTURE_2D,
      0,
      this->intFormat,
      format.width, format.height,
      0,
      this->vboFormat,
      GL_UNSIGNED_BYTE,
      (void*)0
    );

    if (lgr_opengl_check_error("glTexImage2D"))
      return false;
  }

  glBindTexture(GL_TEXTURE_2D, 0);

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
  this->initialized = true;
  return true;
}

void lgr_opengl_deinitialize(void * opaque)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this)
    return;

  if (this->hasTextures)
    glDeleteTextures(VBO_BUFFERS, this->textures);

  if (this->hasBuffers)
    glDeleteBuffers(VBO_BUFFERS, this->vboID);

  if (this->glContext)
    SDL_GL_DeleteContext(this->glContext);

  free(this);
}

bool lgr_opengl_is_compatible(void * opaque, const LG_RendererFormat format)
{
  const struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  return (memcmp(&this->format, &format, sizeof(LG_RendererFormat)) == 0);
}

void lgr_opengl_on_resize(void * opaque, const int width, const int height, const LG_RendererRect destRect)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return;

  this->params.width  = width;
  this->params.height = height;
  memcpy(&this->destRect, &destRect, sizeof(LG_RendererRect));

  this->resizeWindow  = true;
}

bool lgr_opengl_on_mouse_shape(void * opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  if (SDL_GL_MakeCurrent(this->params.window, this->glContext) != 0)
  {
    DEBUG_ERROR("Failed to make the GL context current");
    return false;
  }

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
      break;
    }

    case LG_CURSOR_MONOCHROME:
    {
      uint32_t d[width * height];
      for(int y = 0; y < height / 2; ++y)
        for(int x = 0; x < width; ++x)
        {
          const uint8_t  * srcAnd  = data + (pitch * y) + (x / 8);
          const uint8_t  * srcXor  = srcAnd + pitch * (height / 2);
          const uint8_t    mask    = 0x80 >> (x % 8);
          const uint32_t   andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
          const uint32_t   xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;

          d[y * width + x                       ] = andMask;
          d[y * width + x + width * (height / 2)] = xorMask;
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
      this->mousePos.h = height / 2;
      break;
    }

    case LG_CURSOR_MASKED_COLOR:
    {
      break;
    }
  }

  return true;
}

bool lgr_opengl_on_mouse_event(void * opaque, const bool visible, const int x, const int y)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  if (this->mousePos.x == x && this->mousePos.y == y)
    return true;

  this->mouseUpdate  = true;
  this->mouseVisible = visible;
  this->mousePos.x   = x;
  this->mousePos.y   = y;

  return false;
}

bool lgr_opengl_on_frame_event(void * opaque, const uint8_t * data, bool resample)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  if (SDL_GL_MakeCurrent(this->params.window, this->glContext) != 0)
  {
    DEBUG_ERROR("Failed to make the GL context current");
    return false;
  }

  if (++this->texIndex == VBO_BUFFERS)
    this->texIndex = 0;

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

    glBindTexture(GL_TEXTURE_2D       , this->textures[VBO_BUFFERS]);
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

      glBindTexture(GL_TEXTURE_2D, this->textures[VBO_BUFFERS]);
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

  // copy the buffer to the texture
  memcpySSE(this->texPixels[this->texIndex], data, this->texSize);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[this->texIndex]);
  glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, this->texSize);

  // bind the texture and update it
  glBindTexture(GL_TEXTURE_2D         , this->textures[this->texIndex]);
  glPixelStorei(GL_UNPACK_ALIGNMENT   , 4                             );
  glPixelStorei(GL_UNPACK_ROW_LENGTH  , this->format.width            );

  // update the texture
  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    0, 0,
    this->format.width ,
    this->format.height,
    this->vboFormat,
    GL_UNSIGNED_BYTE,
    (void*)0
  );

  const bool mipmap = resample && (
    (this->format.width  > this->destRect.w) ||
    (this->format.height > this->destRect.h));

  // unbind the buffer
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  // configure the texture
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

  this->frameUpdate = true;
  return true;
}

void lgr_opengl_draw_mouse(struct LGR_OpenGL * this)
{
  if (this->mouseRepair)
  {
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glScalef(1.0f / (float)this->format.width, 1.0f / (float)this->format.height, 1.0f);
    glTranslatef(this->mouseRepairPos.x, this->mouseRepairPos.y, 0.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(this->mouseRepairPos.x, this->mouseRepairPos.y, 0.0f);

    // repair the damage from the cursor's last position
    glBindTexture(GL_TEXTURE_2D, this->textures[this->texIndex]);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLE_STRIP);
      glTexCoord2f(0                     , 0                     ); glVertex2i(0                     , 0                     );
      glTexCoord2f(this->mouseRepairPos.w, 0                     ); glVertex2i(this->mouseRepairPos.w, 0                     );
      glTexCoord2f(0                     , this->mouseRepairPos.h); glVertex2i(0                     , this->mouseRepairPos.h);
      glTexCoord2f(this->mouseRepairPos.w, this->mouseRepairPos.h); glVertex2i(this->mouseRepairPos.w, this->mouseRepairPos.h);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    this->mouseRepair = false;

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }

  if (!this->mouseVisible)
    return;

  memcpy(&this->mouseRepairPos, &this->mousePos, sizeof(SDL_Rect));
  this->mouseRepair = true;

  glPushMatrix();
  glTranslatef(this->mouseRepairPos.x, this->mouseRepairPos.y, 0.0f);

  switch(this->mouseType)
  {
    case LG_CURSOR_COLOR:
    {
      glEnable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, this->textures[MOUSE_TEXTURE]);
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2i(0               , 0               );
        glTexCoord2f(1.0f, 0.0f); glVertex2i(this->mousePos.w, 0               );
        glTexCoord2f(0.0f, 1.0f); glVertex2i(0               , this->mousePos.h);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(this->mousePos.w, this->mousePos.h);
      glEnd();
      glBindTexture(GL_TEXTURE_2D, 0);
      glDisable(GL_BLEND);
      break;
    }

    case LG_CURSOR_MONOCHROME:
    {
      glEnable(GL_COLOR_LOGIC_OP);
      glBindTexture(GL_TEXTURE_2D, this->textures[MOUSE_TEXTURE]);
      glLogicOp(GL_AND);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2i(0               , 0               );
        glTexCoord2f(1.0f, 0.0f); glVertex2i(this->mousePos.w, 0               );
        glTexCoord2f(0.0f, 0.5f); glVertex2i(0               , this->mousePos.h);
        glTexCoord2f(1.0f, 0.5f); glVertex2i(this->mousePos.w, this->mousePos.h);
      glEnd();
      glLogicOp(GL_XOR);
      glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.5f); glVertex2i(0               , 0               );
        glTexCoord2f(1.0f, 0.5f); glVertex2i(this->mousePos.w, 0               );
        glTexCoord2f(0.0f, 1.0f); glVertex2i(0               , this->mousePos.h);
        glTexCoord2f(1.0f, 1.0f); glVertex2i(this->mousePos.w, this->mousePos.h);
      glEnd();
      glDisable(GL_COLOR_LOGIC_OP);
      glBindTexture(GL_TEXTURE_2D, 0);
      break;
    }

    case LG_CURSOR_MASKED_COLOR:
      // TODO
      break;
  }

  glPopMatrix();
}

bool lgr_opengl_render(void * opaque)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  if (SDL_GL_MakeCurrent(this->params.window, this->glContext) != 0)
  {
    DEBUG_ERROR("Failed to make the GL context current");
    return false;
  }

  if (this->frameUpdate)
  {
    if (this->resizeWindow)
    {
      // setup the projection matrix
      glViewport(0, 0, this->params.width, this->params.height);
      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      gluOrtho2D(0, this->params.width, this->params.height, 0);

      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glTranslatef(this->destRect.x, this->destRect.y, 0.0f);
      glScalef(
        (float)this->destRect.w / (float)this->format.width,
        (float)this->destRect.h / (float)this->format.height,
        1.0f
      );

      // update the display lists
      for(int i = 0; i < VBO_BUFFERS; ++i)
      {
        glNewList(this->texList + i, GL_COMPILE);
          glBindTexture(GL_TEXTURE_2D, this->textures[i]);
          glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
          glBegin(GL_TRIANGLE_STRIP);
            glTexCoord2f(0.0f, 0.0f); glVertex2i(0                 , 0                  );
            glTexCoord2f(1.0f, 0.0f); glVertex2i(this->format.width, 0                  );
            glTexCoord2f(0.0f, 1.0f); glVertex2i(0                 , this->format.height);
            glTexCoord2f(1.0f, 1.0f); glVertex2i(this->format.width, this->format.height);
         glEnd();
        glEndList();
      }

      // update the scissor rect to prevent drawing outside of the frame
      glScissor(
        this->destRect.x,
        this->destRect.y,
        this->destRect.w,
        this->destRect.h
      );

      this->resizeWindow = false;
      glDisable(GL_SCISSOR_TEST);
      glClear(GL_COLOR_BUFFER_BIT);
      glEnable(GL_SCISSOR_TEST);
    }

    glCallList(this->texList + this->texIndex);
    this->mouseRepair = false;
    lgr_opengl_draw_mouse(this);
    if (this->fpsTexture)
      glCallList(this->fpsList);

    glXGetVideoSyncSGI(&this->gpuFrameCount);
    SDL_GL_SwapWindow(this->params.window);

    unsigned int count;
    glXGetVideoSyncSGI(&count);
    while(count == this->gpuFrameCount)
    {
      unsigned int remainder;
      glXWaitVideoSyncSGI(1, 0, &remainder);
      glXGetVideoSyncSGI(&count);
      glFinish();
    }

    ++this->frameCount;
    const uint64_t t    = nanotime();
    this->renderTime   += t - this->lastFrameTime;
    this->lastFrameTime = t;
  }
  else
    if (this->mouseUpdate)
    {
      glDrawBuffer(GL_FRONT);
      lgr_opengl_draw_mouse(this);
      glFlush();
      glDrawBuffer(GL_BACK);
    }


  this->frameUpdate = false;
  this->mouseUpdate = false;
  return true;
}

const LG_Renderer LGR_OpenGL =
{
  .get_name       = lgr_opengl_get_name,
  .initialize     = lgr_opengl_initialize,
  .deinitialize   = lgr_opengl_deinitialize,
  .is_compatible  = lgr_opengl_is_compatible,
  .on_resize      = lgr_opengl_on_resize,
  .on_mouse_shape = lgr_opengl_on_mouse_shape,
  .on_mouse_event = lgr_opengl_on_mouse_event,
  .on_frame_event = lgr_opengl_on_frame_event,
  .render         = lgr_opengl_render
};