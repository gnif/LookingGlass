/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

/*
 * The PyroWave block layout, inverse CDF 9/7 orchestration, and
 * dequantization logic in this file derive from PyroWave at commit
 * 89f7e47d4abbf650c91fae766728af866c5e32a0.
 *
 * Copyright (c) 2025 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "decoder.h"

#include "pyrowave.h"
#include "shaders.h"

#include "common/debug.h"
#include "common/locking.h"
#include "common/time.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_VIDEO_LEVELS     5U
#define NETWORK_VIDEO_COMPONENTS 3U
#define NETWORK_VIDEO_BANDS      4U
#define NETWORK_VIDEO_ALIGNMENT  (1U << NETWORK_VIDEO_LEVELS)
#define NETWORK_VIDEO_MIN_SIZE   (4U << NETWORK_VIDEO_LEVELS)

struct NetworkVideoBlockMeta
{
  uint32_t offset;
  uint32_t stride;
  uint32_t count;
};

struct NetworkVideoCurrent
{
  EGLDisplay display;
  EGLSurface draw;
  EGLSurface read;
  EGLContext context;
  EGLenum    api;
};

struct NetworkVideoDecoder
{
  LG_Lock lock;

  EGLDisplay display;
  EGLConfig  eglConfig;
  EGLContext shareContext;
  EGLContext context;

  GLuint dequantProgram;
  GLuint idwtProgram;
  GLuint yuvProgram;
  GLuint coefficientTextures[NETWORK_VIDEO_LEVELS];
  GLuint planeTextures      [NETWORK_VIDEO_COMPONENTS];
  GLuint offsetBuffer;
  GLuint payloadBuffer;

  LGNetVideoStreamConfig config;
  struct NetworkVideoBlockMeta
    blocks[NETWORK_VIDEO_COMPONENTS][NETWORK_VIDEO_LEVELS]
      [NETWORK_VIDEO_BANDS];

  uint32_t alignedWidth;
  uint32_t alignedHeight;
  uint32_t blockCapacity;
  GLint    maxTextureSize;
  GLint    maxArrayLayers;
  GLint    maxWorkGroupCount[2];
  GLint64  maxStorageBlockSize;
  bool     attached;
  bool     configured;
};

struct NetworkVideoExternalFrame
{
  LG_ExternalFrame base;
  atomic_uint      references;
  EGLDisplay       display;
  EGLConfig        eglConfig;
  EGLContext       shareContext;
  GLuint           texture;
  GLsync           readySync;
};

static uint32_t networkVideoReadU32(const uint8_t * data)
{
  return (uint32_t)data[0]        |
    (uint32_t)data[1] << 8        |
    (uint32_t)data[2] << 16       |
    (uint32_t)data[3] << 24;
}

static uint32_t networkVideoAlign(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1U) & ~(alignment - 1U);
}

static void networkVideoSaveCurrent(struct NetworkVideoCurrent * current)
{
  current->display = eglGetCurrentDisplay();
  current->draw    = eglGetCurrentSurface(EGL_DRAW);
  current->read    = eglGetCurrentSurface(EGL_READ);
  current->context = eglGetCurrentContext();
  current->api     = eglQueryAPI();
}

static bool networkVideoMakeCurrent(NetworkVideoDecoder * decoder,
    struct NetworkVideoCurrent * previous)
{
  networkVideoSaveCurrent(previous);
  if (eglBindAPI(EGL_OPENGL_ES_API) &&
      eglMakeCurrent(decoder->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
        decoder->context))
    return true;

  eglBindAPI(previous->api);
  return false;
}

static void networkVideoRestoreCurrent(NetworkVideoDecoder * decoder,
    const struct NetworkVideoCurrent * previous)
{
  if (previous->context != EGL_NO_CONTEXT &&
      previous->display != EGL_NO_DISPLAY)
  {
    eglBindAPI(previous->api);
    eglMakeCurrent(previous->display, previous->draw, previous->read,
        previous->context);
    return;
  }

  eglMakeCurrent(decoder->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
      EGL_NO_CONTEXT);
  eglBindAPI(previous->api);
}

static GLuint networkVideoCompileShader(
    const char * const * sources, GLsizei count)
{
  const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
  if (!shader)
    return 0;

  glShaderSource(shader, count, sources, NULL);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE)
    return shader;

  char  log[4096];
  GLint length = 0;
  glGetShaderInfoLog(shader, sizeof(log), &length, log);
  DEBUG_ERROR("Network video compute shader failed to compile: %.*s",
      length, log);
  glDeleteShader(shader);
  return 0;
}

static GLuint networkVideoCreateProgram(
    const char * const * sources, GLsizei count)
{
  const GLuint shader = networkVideoCompileShader(sources, count);
  if (!shader)
    return 0;

  const GLuint program = glCreateProgram();
  if (!program)
  {
    glDeleteShader(shader);
    return 0;
  }

  glAttachShader(program, shader);
  glLinkProgram(program);
  glDeleteShader(shader);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE)
    return program;

  char  log[4096];
  GLint length = 0;
  glGetProgramInfoLog(program, sizeof(log), &length, log);
  DEBUG_ERROR("Network video compute program failed to link: %.*s",
      length, log);
  glDeleteProgram(program);
  return 0;
}

static void networkVideoDeleteConfigurationNL(NetworkVideoDecoder * decoder)
{
  glDeleteTextures(NETWORK_VIDEO_LEVELS, decoder->coefficientTextures);
  glDeleteTextures(NETWORK_VIDEO_COMPONENTS, decoder->planeTextures);
  if (decoder->offsetBuffer)
    glDeleteBuffers(1, &decoder->offsetBuffer);
  if (decoder->payloadBuffer)
    glDeleteBuffers(1, &decoder->payloadBuffer);

  decoder->offsetBuffer  = 0;
  decoder->payloadBuffer = 0;
  decoder->alignedWidth  = 0;
  decoder->alignedHeight = 0;
  decoder->blockCapacity = 0;
  decoder->configured    = false;
  memset(decoder->coefficientTextures, 0,
      sizeof(decoder->coefficientTextures));
  memset(decoder->planeTextures, 0, sizeof(decoder->planeTextures));
  memset(&decoder->config, 0, sizeof(decoder->config));
  memset(decoder->blocks, 0, sizeof(decoder->blocks));
}

static void networkVideoDeleteProgramsNL(NetworkVideoDecoder * decoder)
{
  if (decoder->dequantProgram)
    glDeleteProgram(decoder->dequantProgram);
  if (decoder->idwtProgram)
    glDeleteProgram(decoder->idwtProgram);
  if (decoder->yuvProgram)
    glDeleteProgram(decoder->yuvProgram);
  decoder->dequantProgram = 0;
  decoder->idwtProgram    = 0;
  decoder->yuvProgram     = 0;
}

static bool networkVideoBuildLayoutNL(NetworkVideoDecoder * decoder,
    const LGNetVideoStreamConfig * config)
{
  decoder->alignedWidth  = networkVideoAlign(
      config->width, NETWORK_VIDEO_ALIGNMENT);
  decoder->alignedHeight = networkVideoAlign(
      config->height, NETWORK_VIDEO_ALIGNMENT);
  if (decoder->alignedWidth < NETWORK_VIDEO_MIN_SIZE)
    decoder->alignedWidth = NETWORK_VIDEO_MIN_SIZE;
  if (decoder->alignedHeight < NETWORK_VIDEO_MIN_SIZE)
    decoder->alignedHeight = NETWORK_VIDEO_MIN_SIZE;

  uint64_t blockOffset = 0;
  for (int level = NETWORK_VIDEO_LEVELS - 1; level >= 0; --level)
  {
    const uint32_t width       = decoder->alignedWidth >> (level + 1);
    const uint32_t height      = decoder->alignedHeight >> (level + 1);
    const uint32_t blockStride = (width + 31U) / 32U;
    const uint32_t blockRows   = (height + 31U) / 32U;
    for (unsigned component = 0;
         component < NETWORK_VIDEO_COMPONENTS; ++component)
    {
      if (!level && component &&
          config->chromaSubsampling == LG_NET_VIDEO_CHROMA_420)
        continue;

      const unsigned firstBand =
        level == NETWORK_VIDEO_LEVELS - 1 ? 0U : 1U;
      for (unsigned band = firstBand; band < NETWORK_VIDEO_BANDS; ++band)
      {
        struct NetworkVideoBlockMeta * meta =
          &decoder->blocks[component][level][band];
        meta->offset = (uint32_t)blockOffset;
        meta->stride = blockStride;
        meta->count  = blockStride * blockRows;

        blockOffset += meta->count;
      }
    }
  }

  if (!blockOffset || blockOffset > LG_NET_VIDEO_MAX_BLOCKS)
    return false;
  decoder->blockCapacity = (uint32_t)blockOffset;
  return true;
}

static bool networkVideoResourcesSupportedNL(
    const NetworkVideoDecoder * decoder,
    const LGNetVideoStreamConfig * config)
{
  const uint64_t offsetBytes    =
    (uint64_t)decoder->blockCapacity * sizeof(uint32_t);
  const uint64_t payloadBytes   = (uint64_t)config->maxFrameLength + 8U;
  const uint32_t dequantGroupsX =
    (decoder->alignedWidth / 2U + 31U) / 32U;
  const uint32_t dequantGroupsY =
    (decoder->alignedHeight / 2U + 31U) / 32U;
  const uint32_t idwtGroupsX    =
    (decoder->alignedHeight / 2U + 15U) / 16U;
  const uint32_t idwtGroupsY    =
    (decoder->alignedWidth / 2U + 15U) / 16U;

  return decoder->maxTextureSize > 0 &&
    decoder->maxArrayLayers >=
      (GLint)(NETWORK_VIDEO_COMPONENTS * NETWORK_VIDEO_BANDS) &&
    decoder->alignedWidth <= (uint32_t)decoder->maxTextureSize &&
    decoder->alignedHeight <= (uint32_t)decoder->maxTextureSize &&
    decoder->maxStorageBlockSize > 0 &&
    offsetBytes <= (uint64_t)decoder->maxStorageBlockSize &&
    payloadBytes <= (uint64_t)decoder->maxStorageBlockSize &&
    dequantGroupsX <= (uint32_t)decoder->maxWorkGroupCount[0] &&
    dequantGroupsY <= (uint32_t)decoder->maxWorkGroupCount[1] &&
    idwtGroupsX <= (uint32_t)decoder->maxWorkGroupCount[0] &&
    idwtGroupsY <= (uint32_t)decoder->maxWorkGroupCount[1];
}

static bool networkVideoCreateTexture2D(GLuint * texture, GLenum format,
    uint32_t width, uint32_t height, GLenum filter)
{
  glGenTextures(1, texture);
  if (!*texture)
    return false;
  glBindTexture(GL_TEXTURE_2D, *texture);
  glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (glGetError() == GL_NO_ERROR)
    return true;

  glDeleteTextures(1, texture);
  *texture = 0;
  return false;
}

static bool networkVideoAllocateNL(NetworkVideoDecoder * decoder)
{
  while (glGetError() != GL_NO_ERROR)
  {
  }

  glGenTextures(NETWORK_VIDEO_LEVELS, decoder->coefficientTextures);
  for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
  {
    if (!decoder->coefficientTextures[level])
      return false;

    const uint32_t width  = decoder->alignedWidth  >> (level + 1U);
    const uint32_t height = decoder->alignedHeight >> (level + 1U);
    glBindTexture(GL_TEXTURE_2D_ARRAY,
        decoder->coefficientTextures[level]);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R32F, width, height,
        NETWORK_VIDEO_COMPONENTS * NETWORK_VIDEO_BANDS);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,
        GL_MIRRORED_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,
        GL_MIRRORED_REPEAT);
  }

  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
  {
    const bool chroma = component != 0 &&
      decoder->config.chromaSubsampling == LG_NET_VIDEO_CHROMA_420;
    const uint32_t width  = chroma ?
      decoder->alignedWidth / 2U : decoder->alignedWidth;
    const uint32_t height = chroma ?
      decoder->alignedHeight / 2U : decoder->alignedHeight;
    if (!networkVideoCreateTexture2D(&decoder->planeTextures[component],
          GL_R32F, width, height, GL_LINEAR))
      return false;
  }

  glGenBuffers(1, &decoder->offsetBuffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, decoder->offsetBuffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
      (GLsizeiptr)decoder->blockCapacity * sizeof(uint32_t),
      NULL, GL_STREAM_DRAW);
  glGenBuffers(1, &decoder->payloadBuffer);
  return decoder->offsetBuffer && decoder->payloadBuffer &&
    glGetError() == GL_NO_ERROR;
}

static void networkVideoExternalRetain(LG_ExternalFrame * base)
{
  struct NetworkVideoExternalFrame * frame =
    (struct NetworkVideoExternalFrame *)base;
  atomic_fetch_add_explicit(
      &frame->references, 1U, memory_order_relaxed);
}

static void networkVideoExternalDelete(
    struct NetworkVideoExternalFrame * frame)
{
  struct NetworkVideoCurrent previous;
  networkVideoSaveCurrent(&previous);
  EGLContext temporary = EGL_NO_CONTEXT;
  bool       changed   = false;
  bool       usable    = previous.context != EGL_NO_CONTEXT &&
    previous.context == frame->shareContext &&
    previous.display == frame->display;
  if (!usable)
  {
    const EGLint attributes[] =
    {
      EGL_CONTEXT_CLIENT_VERSION, 3,
      EGL_NONE,
    };
    if (eglBindAPI(EGL_OPENGL_ES_API))
      temporary = eglCreateContext(frame->display, frame->eglConfig,
          frame->shareContext, attributes);
    if (temporary != EGL_NO_CONTEXT &&
        eglMakeCurrent(frame->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
          temporary))
    {
      changed = true;
      usable  = true;
    }
  }

  if (usable)
  {
    if (frame->readySync)
      glDeleteSync(frame->readySync);
    if (frame->texture)
      glDeleteTextures(1, &frame->texture);
  }

  if (temporary != EGL_NO_CONTEXT)
  {
    if (changed)
      eglMakeCurrent(frame->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
          EGL_NO_CONTEXT);
    eglDestroyContext(frame->display, temporary);
  }
  if (previous.context != EGL_NO_CONTEXT &&
      previous.display != EGL_NO_DISPLAY)
  {
    eglBindAPI(previous.api);
    eglMakeCurrent(previous.display, previous.draw, previous.read,
        previous.context);
  }
  else
    eglBindAPI(previous.api);
}

static void networkVideoExternalRelease(LG_ExternalFrame * base)
{
  struct NetworkVideoExternalFrame * frame =
    (struct NetworkVideoExternalFrame *)base;
  if (atomic_fetch_sub_explicit(
        &frame->references, 1U, memory_order_acq_rel) != 1U)
    return;

  networkVideoExternalDelete(frame);
  free(frame);
}

static bool networkVideoExternalQuery(const LG_ExternalFrame * base,
    void * result, size_t resultSize)
{
  const struct NetworkVideoExternalFrame * frame =
    (const struct NetworkVideoExternalFrame *)base;
  if (!result || resultSize < sizeof(LG_ExternalFrameEGLTexture))
    return false;

  LG_ExternalFrameEGLTexture descriptor =
  {
    .version   = LG_EXTERNAL_FRAME_EGL_TEXTURE_VERSION,
    .size      = sizeof(descriptor),
    .texture   = frame->texture,
    .target    = GL_TEXTURE_2D,
    .readySync = (uintptr_t)frame->readySync,
  };
  memcpy(result, &descriptor, sizeof(descriptor));
  return true;
}

static const LG_ExternalFrameOps NETWORK_VIDEO_EXTERNAL_OPS =
{
  .type    = LG_EXTERNAL_FRAME_EGL_TEXTURE,
  .retain  = networkVideoExternalRetain,
  .release = networkVideoExternalRelease,
  .query   = networkVideoExternalQuery,
};

bool networkVideoDecoderCreate(NetworkVideoDecoder ** result)
{
  if (!result)
    return false;

  NetworkVideoDecoder * decoder = calloc(1, sizeof(*decoder));
  if (!decoder)
    return false;
  LG_LOCK_INIT(decoder->lock);
  *result = decoder;
  return true;
}

void networkVideoDecoderDetach(NetworkVideoDecoder * decoder)
{
  if (!decoder)
    return;

  LG_LOCK(decoder->lock);
  if (decoder->context != EGL_NO_CONTEXT)
  {
    struct NetworkVideoCurrent previous;
    if (networkVideoMakeCurrent(decoder, &previous))
    {
      networkVideoDeleteConfigurationNL(decoder);
      networkVideoDeleteProgramsNL(decoder);
      networkVideoRestoreCurrent(decoder, &previous);
    }
    eglDestroyContext(decoder->display, decoder->context);
  }
  decoder->display              = EGL_NO_DISPLAY;
  decoder->eglConfig            = NULL;
  decoder->shareContext         = EGL_NO_CONTEXT;
  decoder->context              = EGL_NO_CONTEXT;
  decoder->maxTextureSize       = 0;
  decoder->maxArrayLayers       = 0;
  decoder->maxWorkGroupCount[0] = 0;
  decoder->maxWorkGroupCount[1] = 0;
  decoder->maxStorageBlockSize  = 0;
  decoder->attached             = false;
  LG_UNLOCK(decoder->lock);
}

void networkVideoDecoderDestroy(NetworkVideoDecoder ** result)
{
  if (!result || !*result)
    return;
  NetworkVideoDecoder * decoder = *result;
  networkVideoDecoderDetach(decoder);
  LG_LOCK_FREE(decoder->lock);
  free(decoder);
  *result = NULL;
}

void networkVideoDecoderReset(NetworkVideoDecoder * decoder)
{
  if (!decoder)
    return;

  LG_LOCK(decoder->lock);
  if (decoder->context != EGL_NO_CONTEXT)
  {
    struct NetworkVideoCurrent previous;
    if (networkVideoMakeCurrent(decoder, &previous))
    {
      networkVideoDeleteConfigurationNL(decoder);
      networkVideoRestoreCurrent(decoder, &previous);
    }
  }
  LG_UNLOCK(decoder->lock);
}

bool networkVideoDecoderAttach(NetworkVideoDecoder * decoder,
    const LG_RendererInterop * interop)
{
  const size_t requiredSize = offsetof(LG_RendererInterop,
      egl.sharedTextureImport) + sizeof(interop->egl.sharedTextureImport);
  if (!decoder || !interop ||
      interop->version < LG_RENDERER_INTEROP_EGL_TEXTURE_VERSION ||
      interop->size < requiredSize ||
      interop->type != LG_RENDERER_INTEROP_EGL ||
      !interop->egl.display || !interop->egl.config ||
      !interop->egl.shareContext || !interop->egl.sharedTextureImport)
    return false;

  networkVideoDecoderDetach(decoder);
  LG_LOCK(decoder->lock);
  decoder->display      = interop->egl.display;
  decoder->eglConfig    = interop->egl.config;
  decoder->shareContext = interop->egl.shareContext;
  const EGLint attributes[] =
  {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
  };
  if (!eglBindAPI(EGL_OPENGL_ES_API))
    goto fail;
  decoder->context = eglCreateContext(decoder->display, decoder->eglConfig,
      decoder->shareContext, attributes);
  if (decoder->context == EGL_NO_CONTEXT)
    goto fail;

  struct NetworkVideoCurrent previous;
  if (!networkVideoMakeCurrent(decoder, &previous))
    goto fail;

  GLint major = 0;
  GLint minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (major < 3 || (major == 3 && minor < 1))
  {
    DEBUG_ERROR("Network video requires OpenGL ES 3.1, found %d.%d",
        major, minor);
    networkVideoRestoreCurrent(decoder, &previous);
    goto fail;
  }

  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &decoder->maxTextureSize);
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &decoder->maxArrayLayers);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0,
      &decoder->maxWorkGroupCount[0]);
  glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1,
      &decoder->maxWorkGroupCount[1]);
  glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE,
      &decoder->maxStorageBlockSize);

  decoder->dequantProgram = networkVideoCreateProgram(
      NETWORK_VIDEO_DEQUANT_SOURCES,
      sizeof(NETWORK_VIDEO_DEQUANT_SOURCES) /
        sizeof(NETWORK_VIDEO_DEQUANT_SOURCES[0]));
  decoder->idwtProgram    = networkVideoCreateProgram(
      NETWORK_VIDEO_IDWT_SOURCES,
      sizeof(NETWORK_VIDEO_IDWT_SOURCES) /
        sizeof(NETWORK_VIDEO_IDWT_SOURCES[0]));
  decoder->yuvProgram     = networkVideoCreateProgram(
      NETWORK_VIDEO_YUV_SOURCES,
      sizeof(NETWORK_VIDEO_YUV_SOURCES) /
        sizeof(NETWORK_VIDEO_YUV_SOURCES[0]));
  networkVideoRestoreCurrent(decoder, &previous);
  if (!decoder->dequantProgram || !decoder->idwtProgram ||
      !decoder->yuvProgram)
    goto fail;

  decoder->attached = true;
  LG_UNLOCK(decoder->lock);
  return true;

fail:
  if (decoder->context != EGL_NO_CONTEXT)
  {
    struct NetworkVideoCurrent cleanup;
    if (networkVideoMakeCurrent(decoder, &cleanup))
    {
      networkVideoDeleteProgramsNL(decoder);
      networkVideoRestoreCurrent(decoder, &cleanup);
    }
    eglDestroyContext(decoder->display, decoder->context);
  }
  decoder->context              = EGL_NO_CONTEXT;
  decoder->display              = EGL_NO_DISPLAY;
  decoder->eglConfig            = NULL;
  decoder->shareContext         = EGL_NO_CONTEXT;
  decoder->maxTextureSize       = 0;
  decoder->maxArrayLayers       = 0;
  decoder->maxWorkGroupCount[0] = 0;
  decoder->maxWorkGroupCount[1] = 0;
  decoder->maxStorageBlockSize  = 0;
  LG_UNLOCK(decoder->lock);
  return false;
}

NetworkVideoDecodeResult networkVideoDecoderConfigure(
    NetworkVideoDecoder * decoder, const LGNetVideoStreamConfig * config)
{
  if (!decoder || !networkPyroWaveProfileValid(config))
    return NETWORK_VIDEO_DECODE_UNSUPPORTED;

  LG_LOCK(decoder->lock);
  if (!decoder->attached)
  {
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_UNSUPPORTED;
  }

  struct NetworkVideoCurrent previous;
  if (!networkVideoMakeCurrent(decoder, &previous))
  {
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }

  networkVideoDeleteConfigurationNL(decoder);
  decoder->config = *config;

  const bool valid = networkVideoBuildLayoutNL(decoder, config) &&
    networkVideoResourcesSupportedNL(decoder, config) &&
    networkVideoAllocateNL(decoder);
  if (!valid)
    networkVideoDeleteConfigurationNL(decoder);
  else
    decoder->configured = true;
  networkVideoRestoreCurrent(decoder, &previous);
  LG_UNLOCK(decoder->lock);
  return valid ? NETWORK_VIDEO_DECODE_OK : NETWORK_VIDEO_DECODE_ERROR;
}

static bool networkVideoConfigMatches(
    const LGNetVideoStreamConfig * left,
    const LGNetVideoStreamConfig * right)
{
  return left->streamID == right->streamID &&
    left->configEpoch == right->configEpoch &&
    left->codec == right->codec &&
    left->codecVersion == right->codecVersion &&
    left->width == right->width && left->height == right->height &&
    left->pixelFormat == right->pixelFormat &&
    left->chromaSubsampling == right->chromaSubsampling &&
    left->colorPrimaries == right->colorPrimaries &&
    left->colorTransfer == right->colorTransfer &&
    left->colorMatrix == right->colorMatrix &&
    left->colorRange == right->colorRange &&
    left->bitDepth == right->bitDepth && left->flags == right->flags;
}

static bool networkVideoPayloadOffsetsNL(NetworkVideoDecoder * decoder,
    const uint8_t * data, size_t size, uint32_t * offsets,
    bool * chromaLeft)
{
  for (uint32_t index = 0; index < decoder->blockCapacity; ++index)
    offsets[index] = UINT32_MAX;

  size_t cursor = 0;
  bool   header = false;
  while (cursor + 8U <= size)
  {
    const uint32_t first  = networkVideoReadU32(data + cursor);
    const uint32_t second = networkVideoReadU32(data + cursor + 4U);
    if (first >> 31)
    {
      *chromaLeft = (second >> 31) != 0;
      header      = true;
      cursor      += 8U;
      continue;
    }

    const uint32_t words = (first >> 16) & UINT32_C(0x00000FFF);
    const uint32_t block = second >> 8;
    const size_t   bytes = (size_t)words * sizeof(uint32_t);
    if (!words || block >= decoder->blockCapacity || bytes > size - cursor)
      return false;
    if (offsets[block] == UINT32_MAX)
      offsets[block] = (uint32_t)(cursor / sizeof(uint32_t));
    cursor += bytes;
  }
  return header && cursor == size;
}

static void networkVideoSetUniform2i(
    GLuint program, const char * name, GLint x, GLint y)
{
  glUniform2i(glGetUniformLocation(program, name), x, y);
}

static void networkVideoDequantNL(NetworkVideoDecoder * decoder)
{
  glUseProgram(decoder->dequantProgram);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, decoder->offsetBuffer);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, decoder->payloadBuffer);

  for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
  {
    const uint32_t width  = decoder->alignedWidth  >> (level + 1U);
    const uint32_t height = decoder->alignedHeight >> (level + 1U);
    glBindImageTexture(0, decoder->coefficientTextures[level], 0,
        GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
    networkVideoSetUniform2i(decoder->dequantProgram,
        "resolution", width, height);

    for (unsigned component = 0;
         component < NETWORK_VIDEO_COMPONENTS; ++component)
    {
      if (!level && component &&
          decoder->config.chromaSubsampling == LG_NET_VIDEO_CHROMA_420)
        continue;
      const unsigned firstBand =
        level == NETWORK_VIDEO_LEVELS - 1U ? 0U : 1U;
      for (unsigned band = firstBand; band < NETWORK_VIDEO_BANDS; ++band)
      {
        const struct NetworkVideoBlockMeta * meta =
          &decoder->blocks[component][level][band];
        glUniform1i(glGetUniformLocation(decoder->dequantProgram,
            "outputLayer"), component * NETWORK_VIDEO_BANDS + band);
        glUniform1i(glGetUniformLocation(decoder->dequantProgram,
            "blockOffset"), meta->offset);
        glUniform1i(glGetUniformLocation(decoder->dequantProgram,
            "blockStride"), meta->stride);
        glDispatchCompute((width + 31U) / 32U, (height + 31U) / 32U, 1);
      }
    }
  }
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
      GL_TEXTURE_FETCH_BARRIER_BIT);
}

static void networkVideoIDWTNL(NetworkVideoDecoder * decoder)
{
  glUseProgram(decoder->idwtProgram);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(decoder->idwtProgram,
      "coefficients"), 0);

  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
  {
    const unsigned finalLevel = component &&
      decoder->config.chromaSubsampling == LG_NET_VIDEO_CHROMA_420 ? 1U : 0U;
    for (int level = NETWORK_VIDEO_LEVELS - 1;
         level >= (int)finalLevel; --level)
    {
      glBindTexture(GL_TEXTURE_2D_ARRAY,
          decoder->coefficientTextures[level]);
      const uint32_t inputWidth  = decoder->alignedWidth >> (level + 1);
      const uint32_t inputHeight = decoder->alignedHeight >> (level + 1);
      networkVideoSetUniform2i(decoder->idwtProgram,
          "resolution", inputHeight, inputWidth);
      glUniform2f(glGetUniformLocation(decoder->idwtProgram,
          "invResolution"), 1.0f / inputHeight, 1.0f / inputWidth);
      glUniform1i(glGetUniformLocation(decoder->idwtProgram,
          "inputLevel"), level);
      glUniform1i(glGetUniformLocation(decoder->idwtProgram,
          "baseLayer"), component * NETWORK_VIDEO_BANDS);
      const bool final = level == (int)finalLevel;
      glUniform1i(glGetUniformLocation(decoder->idwtProgram,
          "dcShift"), final ? GL_TRUE : GL_FALSE);

      if (final)
      {
        glBindImageTexture(1, decoder->planeTextures[component], 0,
            GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
      }
      else
      {
        glBindImageTexture(1, decoder->coefficientTextures[level - 1], 0,
            GL_FALSE, component * NETWORK_VIDEO_BANDS,
            GL_WRITE_ONLY, GL_R32F);
      }
      glDispatchCompute(
          (inputHeight + 15U) / 16U, (inputWidth + 15U) / 16U, 1);
      glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
          GL_TEXTURE_FETCH_BARRIER_BIT);
    }
  }
}

static void networkVideoColorRange(
    const LGNetVideoStreamConfig * config,
    float * lumaOffset, float * lumaScale,
    float * chromaMidpoint, float * chromaScale)
{
  float maximum;
  float shift;
  if (config->pixelFormat == LG_NET_VIDEO_PIXEL_FORMAT_YUV444P16F)
  {
    maximum         = 255.0f;
    shift           = 1.0f;
    *chromaMidpoint = 0.5f;
  }
  else
  {
    maximum         = (float)((UINT32_C(1) << config->bitDepth) - 1U);
    shift           = (float)(UINT32_C(1) << (config->bitDepth - 8U));
    *chromaMidpoint = 128.0f * shift / maximum;
  }

  if (config->colorRange == LG_NET_COLOR_RANGE_LIMITED)
  {
    *lumaOffset  = 16.0f * shift / maximum;
    *lumaScale   = maximum / (219.0f * shift);
    *chromaScale = maximum / (224.0f * shift);
  }
  else
  {
    *lumaOffset  = 0.0f;
    *lumaScale   = 1.0f;
    *chromaScale = 1.0f;
  }
}

static struct NetworkVideoExternalFrame * networkVideoConvertNL(
    NetworkVideoDecoder * decoder, bool chromaLeft)
{
  GLuint texture = 0;
  if (!networkVideoCreateTexture2D(&texture, GL_RGBA16F,
        decoder->config.width, decoder->config.height, GL_LINEAR))
    return NULL;

  glUseProgram(decoder->yuvProgram);
  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
  {
    glActiveTexture(GL_TEXTURE0 + component);
    glBindTexture(GL_TEXTURE_2D, decoder->planeTextures[component]);
  }
  glUniform1i(glGetUniformLocation(decoder->yuvProgram, "lumaPlane"), 0);
  glUniform1i(glGetUniformLocation(decoder->yuvProgram, "cbPlane"), 1);
  glUniform1i(glGetUniformLocation(decoder->yuvProgram, "crPlane"), 2);
  networkVideoSetUniform2i(decoder->yuvProgram, "outputSize",
      decoder->config.width, decoder->config.height);
  glUniform1i(glGetUniformLocation(decoder->yuvProgram, "bt2020"),
      decoder->config.colorMatrix == LG_NET_COLOR_MATRIX_BT2020);
  glUniform1i(glGetUniformLocation(decoder->yuvProgram, "chromaLeft"),
      chromaLeft);
  float lumaOffset;
  float lumaScale;
  float chromaMidpoint;
  float chromaScale;
  networkVideoColorRange(&decoder->config,
      &lumaOffset, &lumaScale, &chromaMidpoint, &chromaScale);
  glUniform1f(glGetUniformLocation(decoder->yuvProgram, "lumaOffset"),
      lumaOffset);
  glUniform1f(glGetUniformLocation(decoder->yuvProgram, "lumaScale"),
      lumaScale);
  glUniform1f(glGetUniformLocation(decoder->yuvProgram, "chromaMidpoint"),
      chromaMidpoint);
  glUniform1f(glGetUniformLocation(decoder->yuvProgram, "chromaScale"),
      chromaScale);
  glBindImageTexture(0, texture, 0, GL_FALSE, 0,
      GL_WRITE_ONLY, GL_RGBA16F);
  glDispatchCompute((decoder->config.width + 7U) / 8U,
      (decoder->config.height + 7U) / 8U, 1);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
      GL_TEXTURE_FETCH_BARRIER_BIT);

  GLsync readySync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  if (!readySync || glGetError() != GL_NO_ERROR)
  {
    if (readySync)
      glDeleteSync(readySync);
    glDeleteTextures(1, &texture);
    return NULL;
  }

  struct NetworkVideoExternalFrame * frame = calloc(1, sizeof(*frame));
  if (!frame)
  {
    glDeleteSync(readySync);
    glDeleteTextures(1, &texture);
    return NULL;
  }
  frame->base.ops     = &NETWORK_VIDEO_EXTERNAL_OPS;
  frame->display      = decoder->display;
  frame->eglConfig    = decoder->eglConfig;
  frame->shareContext = decoder->shareContext;
  frame->texture      = texture;
  frame->readySync    = readySync;
  atomic_init(&frame->references, 1U);
  return frame;
}

NetworkVideoDecodeResult networkVideoDecoderDecode(
    NetworkVideoDecoder * decoder, const LGNetVideoStreamConfig * config,
    const void * data, size_t size, NetworkVideoDecodedFrame * result)
{
  if (!decoder || !config || !data || !size || !result)
    return NETWORK_VIDEO_DECODE_CORRUPT;
  memset(result, 0, sizeof(*result));

  LG_LOCK(decoder->lock);
  if (!decoder->configured ||
      !networkVideoConfigMatches(&decoder->config, config))
  {
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_UNSUPPORTED;
  }

  uint32_t * offsets    = malloc(
      (size_t)decoder->blockCapacity * sizeof(*offsets));
  bool       chromaLeft = false;
  if (!offsets || !networkVideoPayloadOffsetsNL(decoder,
        data, size, offsets, &chromaLeft))
  {
    free(offsets);
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_CORRUPT;
  }

  struct NetworkVideoCurrent previous;
  if (!networkVideoMakeCurrent(decoder, &previous))
  {
    free(offsets);
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }

  while (glGetError() != GL_NO_ERROR)
  {
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, decoder->offsetBuffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
      (GLsizeiptr)decoder->blockCapacity * sizeof(*offsets),
      offsets, GL_STREAM_DRAW);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, decoder->payloadBuffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, size + 8U, NULL, GL_STREAM_DRAW);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, size, data);
  free(offsets);

  const uint64_t decodeStart = nanotime();
  networkVideoDequantNL(decoder);
  networkVideoIDWTNL(decoder);
  struct NetworkVideoExternalFrame * external =
    networkVideoConvertNL(decoder, chromaLeft);
  const uint64_t decodeTime = nanotime() - decodeStart;
  const bool     success    = external && glGetError() == GL_NO_ERROR;
  networkVideoRestoreCurrent(decoder, &previous);
  if (!success)
  {
    if (external)
      networkVideoExternalRelease(&external->base);
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }

  uint32_t formatVersion = (uint32_t)config->configEpoch;
  if (!formatVersion)
    formatVersion = 1;

  result->externalFrame = &external->base;
  result->decodeTimeNs  = decodeTime;
  result->format        = (LG_TransportFrameFormat)
  {
    .version       = formatVersion,
    .type          = FRAME_TYPE_RGBA16F,
    .screenWidth   = config->width,
    .screenHeight  = config->height,
    .dataWidth     = config->width,
    .dataHeight    = config->height,
    .frameWidth    = config->width,
    .frameHeight   = config->height,
    .rotation      = FRAME_ROT_0,
    .stride        = config->width,
    .pitch         = config->width * 8U,
    .hdr           = (config->flags & LG_NET_VIDEO_STREAM_HDR) != 0,
    .hdrPQ         = config->colorTransfer == LG_NET_COLOR_TRANSFER_PQ,
    .hdrMetadata   = false,
    .sdrWhiteLevel = LG_SDR_WHITE_LEVEL_DEFAULT,
  };
  result->timing.providerValid = true;
  result->timing.prepareTime   = decodeTime;
  LG_UNLOCK(decoder->lock);
  return NETWORK_VIDEO_DECODE_OK;
}

void networkVideoDecoderRelease(NetworkVideoDecodedFrame * frame)
{
  if (!frame)
    return;
  lgExternalFrameRelease(frame->externalFrame);
  memset(frame, 0, sizeof(*frame));
}
