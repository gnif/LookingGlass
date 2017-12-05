#include "lg-renderer.h"
#include <stdint.h>
#include <stdbool.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glu.h>

#include "debug.h"
#include "memcpySSE.h"

#define VBO_BUFFERS 2

struct LGR_OpenGL
{
  bool              initialized;
  LG_RendererFormat format;
  GLuint            intFormat;
  GLuint            vboFormat;
  size_t            texSize;

  bool              hasBuffers;
  GLuint            vboID[VBO_BUFFERS];
  uint8_t         * texPixels[VBO_BUFFERS];
  int               texIndex;

  bool              hasTextures;
  GLuint            vboTex[VBO_BUFFERS];
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

bool lgr_opengl_initialize(void ** opaque, const LG_RendererParams params, const LG_RendererFormat format)
{
  // check if the GPU supports GL_ARB_buffer_storage first
  // there is no advantage to this renderer if it is not present.
  const GLubyte * extensions = glGetString(GL_EXTENSIONS);
  if (!gluCheckExtension((const GLubyte *)"GL_ARB_buffer_storage", extensions))
  {
    DEBUG_INFO("The GPU doesn't support GL_ARB_buffer_storage");
    return false;
  }

  // create our local storage
  *opaque = malloc(sizeof(struct LGR_OpenGL));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct LGR_OpenGL));
    return false;
  }
  memset(*opaque, 0, sizeof(struct LGR_OpenGL));
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)*opaque;

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
  glGenTextures(VBO_BUFFERS, this->vboTex);
  if (lgr_opengl_check_error("glGenTextures"))
    return false;
  this->hasTextures = true;

  // bind the textures to the unpack buffers
  for (int i = 0; i < VBO_BUFFERS; ++i)
  {
    glBindTexture(GL_TEXTURE_2D, this->vboTex[i]);
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

  glEnable(GL_TEXTURE_2D);

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
    glDeleteTextures(VBO_BUFFERS, this->vboTex);

  if (this->hasBuffers)
    glDeleteBuffers(VBO_BUFFERS, this->vboID);

  free(this);
}

bool lgr_opengl_is_compatible(void * opaque, const LG_RendererFormat format)
{
  const struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  return (memcmp(&this->format, &format, sizeof(LG_RendererFormat)) == 0);
}

bool lgr_opengl_render(void * opaque, const LG_RendererRect destRect, const uint8_t * data, bool resample)
{
  struct LGR_OpenGL * this = (struct LGR_OpenGL *)opaque;
  if (!this || !this->initialized)
    return false;

  glClear(GL_COLOR_BUFFER_BIT);

  // copy the buffer to the texture
  memcpySSE(this->texPixels[this->texIndex], data, this->texSize);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->vboID[this->texIndex]);
  glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, this->texSize);

  // bind the texture and update it
  glBindTexture(GL_TEXTURE_2D         , this->vboTex[this->texIndex]);
  glPixelStorei(GL_UNPACK_ALIGNMENT   , 4                           );
  glPixelStorei(GL_UNPACK_ROW_LENGTH  , this->format.width          );
  glTexSubImage2D(
    GL_TEXTURE_2D,
    0,
    0, 0,
    this->format.width,
    this->format.height,
    this->vboFormat,
    GL_UNSIGNED_BYTE,
    (void*)0
  );

  const bool mipmap = resample && (
    (this->format.width  != destRect.w) ||
    (this->format.height != destRect.h));

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

  // draw the screen
  glEnable(GL_TEXTURE_2D);
  glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.0f, 0.0f); glVertex2i(destRect.x             , destRect.y             );
    glTexCoord2f(1.0f, 0.0f); glVertex2i(destRect.x + destRect.w, destRect.y             );
    glTexCoord2f(0.0f, 1.0f); glVertex2i(destRect.x             , destRect.y + destRect.h);
    glTexCoord2f(1.0f, 1.0f); glVertex2i(destRect.x + destRect.w, destRect.y + destRect.h);
  glEnd();
  glDisable(GL_TEXTURE_2D);

  glBindTexture(GL_TEXTURE_2D, 0);

  if (++this->texIndex == VBO_BUFFERS)
    this->texIndex = 0;

  return true;
}

const LG_Renderer LGR_OpenGL =
{
  .get_name      = lgr_opengl_get_name,
  .initialize    = lgr_opengl_initialize,
  .deinitialize  = lgr_opengl_deinitialize,
  .is_compatible = lgr_opengl_is_compatible,
  .render        = lgr_opengl_render
};