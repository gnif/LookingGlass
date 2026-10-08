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

#define NETWORK_VIDEO_LEVELS                 5U
#define NETWORK_VIDEO_COMPONENTS             3U
#define NETWORK_VIDEO_BANDS                  4U
#define NETWORK_VIDEO_ALIGNMENT              (1U << NETWORK_VIDEO_LEVELS)
#define NETWORK_VIDEO_MIN_SIZE               (4U << NETWORK_VIDEO_LEVELS)
#define NETWORK_VIDEO_UPLOADS                3U
#define NETWORK_VIDEO_POOL_SIZE              3U
#define NETWORK_VIDEO_DAMAGE_HISTORY         64U
#define NETWORK_VIDEO_POOL_BYTE_LIMIT        \
  (UINT64_C(192) * 1024U * 1024U)
#define NETWORK_VIDEO_PIXEL_BYTES            8U
#define NETWORK_VIDEO_SEQUENCE_BITS          (UINT32_C(7) << 28U)

struct NetworkVideoBlockMeta
{
  uint32_t offset;
  uint32_t stride;
  uint32_t count;
};

struct NetworkVideoPacketState
{
  uint8_t * data;
  uint32_t  length;
};

struct NetworkVideoDirtyGrid
{
  uint8_t * tiles;
  uint32_t  width;
  uint32_t  height;
  uint32_t  count;
};

struct NetworkVideoCurrent
{
  EGLDisplay display;
  EGLSurface draw;
  EGLSurface read;
  EGLContext context;
  EGLenum    api;
};

struct NetworkVideoUpload
{
  GLuint offsetBuffer;
  GLuint payloadBuffer;
  GLsync readySync;
  size_t payloadCapacity;
};

struct NetworkVideoTexture
{
  GLuint   texture;
  GLsync   readySync;
  uint64_t generation;
};

struct NetworkVideoDamageHistory
{
  uint64_t         generation;
  LGNetVideoDamage damage;
};

struct NetworkVideoTexturePool
{
  LG_Lock lock;

  atomic_uint                references;
  EGLDisplay                 display;
  EGLConfig                  eglConfig;
  EGLContext                 shareContext;
  struct NetworkVideoTexture textures[NETWORK_VIDEO_POOL_SIZE];
  unsigned                   textureCount;
  unsigned                   textureLimit;
  uint32_t                   width;
  uint32_t                   height;
};

struct NetworkVideoUniforms
{
  struct
  {
    GLint resolution;
    GLint outputLayer;
    GLint blockOffset;
    GLint blockStride;
    GLint workGroupOffset;
    GLint preserveMissing;
  }
  dequant;

  struct
  {
    GLint coefficients;
    GLint resolution;
    GLint invResolution;
    GLint baseLayer;
    GLint workGroupOffset;
    GLint dcShift;
  }
  idwt;

  struct
  {
    GLint lumaPlane;
    GLint cbPlane;
    GLint crPlane;
    GLint outputSize;
    GLint outputOffset;
    GLint outputExtent;
    GLint bt2020;
    GLint chromaLeft;
    GLint lumaOffset;
    GLint lumaScale;
    GLint chromaMidpoint;
    GLint chromaScale;
  }
  yuv;

  struct
  {
    GLint inputImage;
    GLint outputOffset;
    GLint outputExtent;
  }
  copy;
};

struct NetworkVideoDecoder
{
  LG_Lock lock;

  EGLDisplay display;
  EGLConfig  eglConfig;
  EGLContext shareContext;
  EGLContext context;

  GLuint                           dequantProgram;
  GLuint                           idwtProgram;
  GLuint                           yuvProgram;
  GLuint                           copyProgram;
  GLuint                           coefficientTextures[NETWORK_VIDEO_LEVELS];
  GLuint                           planeTextures      [NETWORK_VIDEO_COMPONENTS];
  GLuint                           canonicalTexture;
  struct NetworkVideoUpload        uploads            [NETWORK_VIDEO_UPLOADS];
  struct NetworkVideoTexturePool * texturePool;
  struct NetworkVideoUniforms      uniforms;
  uint32_t                       * offsets;
  struct NetworkVideoPacketState * packetStates;
  struct NetworkVideoPacketState * pendingPackets;
  uint8_t                        * dirtyBlocks;
  struct NetworkVideoDirtyGrid
    dirty[NETWORK_VIDEO_COMPONENTS][NETWORK_VIDEO_LEVELS];
  struct NetworkVideoDamageHistory
    damageHistory[NETWORK_VIDEO_DAMAGE_HISTORY];

  LGNetVideoStreamConfig config;
  struct NetworkVideoBlockMeta
    blocks[NETWORK_VIDEO_COMPONENTS][NETWORK_VIDEO_LEVELS]
      [NETWORK_VIDEO_BANDS];

  uint32_t alignedWidth;
  uint32_t alignedHeight;
  uint32_t blockCapacity;
  uint64_t referenceFrameID;
  uint64_t outputGeneration;
  unsigned uploadIndex;
  GLint    maxTextureSize;
  GLint    maxArrayLayers;
  GLint    maxWorkGroupCount[2];
  GLint64  maxStorageBlockSize;
  bool     attached;
  bool     configured;
  bool     referenceValid;
};

struct NetworkVideoExternalFrame
{
  LG_ExternalFrame                 base;
  atomic_uint                      references;
  struct NetworkVideoTexturePool * pool;
  GLuint                           texture;
  GLsync                           readySync;
  uint64_t                         generation;
};

static uint32_t networkVideoAlign(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t networkVideoReadU32(const uint8_t * data)
{
  return (uint32_t)data[0]       |
    (uint32_t)data[1] << 8U      |
    (uint32_t)data[2] << 16U     |
    (uint32_t)data[3] << 24U;
}

static void networkVideoWriteU32(uint8_t * data, uint32_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
  data[2] = (uint8_t)(value >> 16U);
  data[3] = (uint8_t)(value >> 24U);
}

static void networkVideoFreePacketStates(
    struct NetworkVideoPacketState * states, uint32_t count)
{
  if (!states)
    return;
  for (uint32_t index = 0; index < count; ++index)
    free(states[index].data);
  free(states);
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

static bool networkVideoTexturePoolMakeCurrent(
    const struct NetworkVideoTexturePool * pool,
    struct NetworkVideoCurrent * previous, EGLContext * temporary)
{
  networkVideoSaveCurrent(previous);
  *temporary = EGL_NO_CONTEXT;
  if (previous->context != EGL_NO_CONTEXT &&
      previous->display == pool->display &&
      previous->context == pool->shareContext)
    return true;

  const EGLint attributes[] =
  {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
  };
  if (eglBindAPI(EGL_OPENGL_ES_API))
    *temporary = eglCreateContext(pool->display, pool->eglConfig,
        pool->shareContext, attributes);
  if (*temporary != EGL_NO_CONTEXT &&
      eglMakeCurrent(pool->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
        *temporary))
    return true;

  if (*temporary != EGL_NO_CONTEXT)
  {
    eglDestroyContext(pool->display, *temporary);
    *temporary = EGL_NO_CONTEXT;
  }
  eglBindAPI(previous->api);
  return false;
}

static void networkVideoTexturePoolRestoreCurrent(
    const struct NetworkVideoTexturePool * pool,
    const struct NetworkVideoCurrent * previous, EGLContext temporary)
{
  if (temporary == EGL_NO_CONTEXT)
    return;

  eglMakeCurrent(pool->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
      EGL_NO_CONTEXT);
  eglDestroyContext(pool->display, temporary);

  if (previous->context != EGL_NO_CONTEXT &&
      previous->display != EGL_NO_DISPLAY)
  {
    eglBindAPI(previous->api);
    eglMakeCurrent(previous->display, previous->draw, previous->read,
        previous->context);
  }
  else
    eglBindAPI(previous->api);
}

static struct NetworkVideoTexturePool * networkVideoTexturePoolCreate(
    const NetworkVideoDecoder * decoder)
{
  struct NetworkVideoTexturePool * pool = calloc(1, sizeof(*pool));
  if (!pool)
    return NULL;

  const uint64_t width        = decoder->config.width;
  const uint64_t height       = decoder->config.height;
  const uint64_t textureBytes = width * height * NETWORK_VIDEO_PIXEL_BYTES;
  const uint64_t textureLimit = textureBytes ?
    NETWORK_VIDEO_POOL_BYTE_LIMIT / textureBytes : 0U;

  LG_LOCK_INIT(pool->lock);
  atomic_init(&pool->references, 1U);
  pool->display      = decoder->display;
  pool->eglConfig    = decoder->eglConfig;
  pool->shareContext = decoder->shareContext;
  pool->width        = decoder->config.width;
  pool->height       = decoder->config.height;
  pool->textureLimit = textureLimit < NETWORK_VIDEO_POOL_SIZE ?
    (unsigned)textureLimit : NETWORK_VIDEO_POOL_SIZE;
  return pool;
}

static void networkVideoTexturePoolRetain(
    struct NetworkVideoTexturePool * pool)
{
  atomic_fetch_add_explicit(&pool->references, 1U, memory_order_relaxed);
}

static void networkVideoTexturePoolRelease(
    struct NetworkVideoTexturePool * pool, bool current)
{
  if (!pool || atomic_fetch_sub_explicit(
        &pool->references, 1U, memory_order_acq_rel) != 1U)
    return;

  if (pool->textureCount)
  {
    struct NetworkVideoCurrent previous;
    EGLContext                 temporary = EGL_NO_CONTEXT;
    bool                       usable    = current;
    if (!usable)
      usable = networkVideoTexturePoolMakeCurrent(
          pool, &previous, &temporary);

    if (usable)
    {
      for (unsigned index = 0; index < pool->textureCount; ++index)
      {
        struct NetworkVideoTexture * texture = &pool->textures[index];
        if (texture->readySync)
          glDeleteSync(texture->readySync);
        if (texture->texture)
          glDeleteTextures(1, &texture->texture);
      }
    }

    if (!current && usable)
      networkVideoTexturePoolRestoreCurrent(pool, &previous, temporary);
  }
  LG_LOCK_FREE(pool->lock);
  free(pool);
}

static bool networkVideoTexturePoolReturn(
    struct NetworkVideoTexturePool * pool, GLuint texture, GLsync readySync,
    uint64_t generation)
{
  if (!pool || !texture)
    return false;

  bool retained = false;
  LG_LOCK(pool->lock);
  if (pool->textureCount < pool->textureLimit)
  {
    struct NetworkVideoTexture * pooled =
      &pool->textures[pool->textureCount++];
    pooled->texture    = texture;
    pooled->readySync  = readySync;
    pooled->generation = generation;
    retained           = true;
  }
  LG_UNLOCK(pool->lock);
  return retained;
}

static void networkVideoTexturePoolRecycleCurrent(
    struct NetworkVideoTexturePool * pool, GLuint texture, GLsync readySync,
    uint64_t generation)
{
  if (networkVideoTexturePoolReturn(
        pool, texture, readySync, generation))
    return;

  if (readySync)
    glDeleteSync(readySync);
  if (texture)
    glDeleteTextures(1, &texture);
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
  if (decoder->canonicalTexture)
    glDeleteTextures(1, &decoder->canonicalTexture);
  for (unsigned index = 0; index < NETWORK_VIDEO_UPLOADS; ++index)
  {
    struct NetworkVideoUpload * upload = &decoder->uploads[index];
    if (upload->readySync)
      glDeleteSync(upload->readySync);
    if (upload->offsetBuffer)
      glDeleteBuffers(1, &upload->offsetBuffer);
    if (upload->payloadBuffer)
      glDeleteBuffers(1, &upload->payloadBuffer);
  }
  networkVideoTexturePoolRelease(decoder->texturePool, true);
  networkVideoFreePacketStates(
      decoder->packetStates, decoder->blockCapacity);
  free(decoder->pendingPackets);
  free(decoder->dirtyBlocks);
  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
    for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
      free(decoder->dirty[component][level].tiles);
  free(decoder->offsets);

  memset(decoder->uploads, 0, sizeof(decoder->uploads));
  memset(decoder->damageHistory, 0, sizeof(decoder->damageHistory));
  decoder->texturePool      = NULL;
  decoder->offsets          = NULL;
  decoder->packetStates     = NULL;
  decoder->pendingPackets   = NULL;
  decoder->dirtyBlocks      = NULL;
  decoder->canonicalTexture = 0;
  decoder->alignedWidth     = 0;
  decoder->alignedHeight    = 0;
  decoder->blockCapacity    = 0;
  decoder->referenceFrameID = 0;
  decoder->outputGeneration = 0;
  decoder->uploadIndex      = 0;
  decoder->configured       = false;
  decoder->referenceValid   = false;
  memset(decoder->coefficientTextures, 0,
      sizeof(decoder->coefficientTextures));
  memset(decoder->planeTextures, 0, sizeof(decoder->planeTextures));
  memset(decoder->dirty, 0, sizeof(decoder->dirty));
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
  if (decoder->copyProgram)
    glDeleteProgram(decoder->copyProgram);
  decoder->dequantProgram = 0;
  decoder->idwtProgram    = 0;
  decoder->yuvProgram     = 0;
  decoder->copyProgram    = 0;
  memset(&decoder->uniforms, 0, sizeof(decoder->uniforms));
}

static bool networkVideoCacheUniformsNL(NetworkVideoDecoder * decoder)
{
  decoder->uniforms.dequant.resolution      = glGetUniformLocation(
      decoder->dequantProgram, "resolution");
  decoder->uniforms.dequant.outputLayer     = glGetUniformLocation(
      decoder->dequantProgram, "outputLayer");
  decoder->uniforms.dequant.blockOffset     = glGetUniformLocation(
      decoder->dequantProgram, "blockOffset");
  decoder->uniforms.dequant.blockStride     = glGetUniformLocation(
      decoder->dequantProgram, "blockStride");
  decoder->uniforms.dequant.workGroupOffset = glGetUniformLocation(
      decoder->dequantProgram, "workGroupOffset");
  decoder->uniforms.dequant.preserveMissing = glGetUniformLocation(
      decoder->dequantProgram, "preserveMissing");

  decoder->uniforms.idwt.coefficients        = glGetUniformLocation(
      decoder->idwtProgram, "coefficients");
  decoder->uniforms.idwt.resolution          = glGetUniformLocation(
      decoder->idwtProgram, "resolution");
  decoder->uniforms.idwt.invResolution       = glGetUniformLocation(
      decoder->idwtProgram, "invResolution");
  decoder->uniforms.idwt.baseLayer           = glGetUniformLocation(
      decoder->idwtProgram, "baseLayer");
  decoder->uniforms.idwt.workGroupOffset     = glGetUniformLocation(
      decoder->idwtProgram, "workGroupOffset");
  decoder->uniforms.idwt.dcShift             = glGetUniformLocation(
      decoder->idwtProgram, "dcShift");

  decoder->uniforms.yuv.lumaPlane      = glGetUniformLocation(
      decoder->yuvProgram, "lumaPlane");
  decoder->uniforms.yuv.cbPlane        = glGetUniformLocation(
      decoder->yuvProgram, "cbPlane");
  decoder->uniforms.yuv.crPlane        = glGetUniformLocation(
      decoder->yuvProgram, "crPlane");
  decoder->uniforms.yuv.outputSize     = glGetUniformLocation(
      decoder->yuvProgram, "outputSize");
  decoder->uniforms.yuv.outputOffset   = glGetUniformLocation(
      decoder->yuvProgram, "outputOffset");
  decoder->uniforms.yuv.outputExtent   = glGetUniformLocation(
      decoder->yuvProgram, "outputExtent");
  decoder->uniforms.yuv.bt2020         = glGetUniformLocation(
      decoder->yuvProgram, "bt2020");
  decoder->uniforms.yuv.chromaLeft     = glGetUniformLocation(
      decoder->yuvProgram, "chromaLeft");
  decoder->uniforms.yuv.lumaOffset     = glGetUniformLocation(
      decoder->yuvProgram, "lumaOffset");
  decoder->uniforms.yuv.lumaScale      = glGetUniformLocation(
      decoder->yuvProgram, "lumaScale");
  decoder->uniforms.yuv.chromaMidpoint = glGetUniformLocation(
      decoder->yuvProgram, "chromaMidpoint");
  decoder->uniforms.yuv.chromaScale    = glGetUniformLocation(
      decoder->yuvProgram, "chromaScale");

  decoder->uniforms.copy.inputImage   = glGetUniformLocation(
      decoder->copyProgram, "inputImage");
  decoder->uniforms.copy.outputOffset = glGetUniformLocation(
      decoder->copyProgram, "outputOffset");
  decoder->uniforms.copy.outputExtent = glGetUniformLocation(
      decoder->copyProgram, "outputExtent");

  const struct NetworkVideoUniforms * uniforms = &decoder->uniforms;
  if (uniforms->dequant.resolution < 0 ||
      uniforms->dequant.outputLayer < 0 ||
      uniforms->dequant.blockOffset < 0 ||
      uniforms->dequant.blockStride < 0 ||
      uniforms->dequant.workGroupOffset < 0 ||
      uniforms->dequant.preserveMissing < 0 ||
      uniforms->idwt.coefficients < 0 ||
      uniforms->idwt.resolution < 0 ||
      uniforms->idwt.invResolution < 0 ||
      uniforms->idwt.baseLayer < 0 ||
      uniforms->idwt.workGroupOffset < 0 ||
      uniforms->idwt.dcShift < 0 ||
      uniforms->yuv.lumaPlane < 0 ||
      uniforms->yuv.cbPlane < 0 || uniforms->yuv.crPlane < 0 ||
      uniforms->yuv.outputSize < 0 || uniforms->yuv.outputOffset < 0 ||
      uniforms->yuv.outputExtent < 0 ||
      uniforms->yuv.bt2020 < 0 ||
      uniforms->yuv.chromaLeft < 0 || uniforms->yuv.lumaOffset < 0 ||
      uniforms->yuv.lumaScale < 0 ||
      uniforms->yuv.chromaMidpoint < 0 ||
      uniforms->yuv.chromaScale < 0 ||
      uniforms->copy.inputImage < 0 ||
      uniforms->copy.outputOffset < 0 ||
      uniforms->copy.outputExtent < 0)
    return false;

  glUseProgram(decoder->idwtProgram);
  glUniform1i(uniforms->idwt.coefficients, 0);
  glUseProgram(decoder->yuvProgram);
  glUniform1i(uniforms->yuv.lumaPlane, 0);
  glUniform1i(uniforms->yuv.cbPlane, 1);
  glUniform1i(uniforms->yuv.crPlane, 2);
  glUseProgram(decoder->copyProgram);
  glUniform1i(uniforms->copy.inputImage, 0);
  return glGetError() == GL_NO_ERROR;
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

static struct NetworkVideoTexture networkVideoTexturePoolAcquire(
    struct NetworkVideoTexturePool * pool)
{
  struct NetworkVideoTexture pooled = { 0 };
  LG_LOCK(pool->lock);
  if (pool->textureCount)
  {
    pooled = pool->textures[--pool->textureCount];
    memset(&pool->textures[pool->textureCount], 0,
        sizeof(pool->textures[pool->textureCount]));
  }
  LG_UNLOCK(pool->lock);
  if (pooled.readySync)
    glDeleteSync(pooled.readySync);
  if (pooled.texture)
    return pooled;

  if (!networkVideoCreateTexture2D(&pooled.texture, GL_RGBA16F,
        pool->width, pool->height, GL_LINEAR))
    pooled.texture = 0;
  return pooled;
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

  if (!networkVideoCreateTexture2D(&decoder->canonicalTexture, GL_RGBA16F,
        decoder->config.width, decoder->config.height, GL_LINEAR))
    return false;

  decoder->offsets        = malloc(
      (size_t)decoder->blockCapacity * sizeof(*decoder->offsets));
  decoder->packetStates   = calloc(
      decoder->blockCapacity, sizeof(*decoder->packetStates));
  decoder->pendingPackets = calloc(
      decoder->blockCapacity, sizeof(*decoder->pendingPackets));
  decoder->dirtyBlocks    = calloc(
      decoder->blockCapacity, sizeof(*decoder->dirtyBlocks));
  decoder->texturePool    = networkVideoTexturePoolCreate(decoder);
  if (!decoder->offsets || !decoder->packetStates ||
      !decoder->pendingPackets || !decoder->dirtyBlocks ||
      !decoder->texturePool)
    return false;

  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
  {
    for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
    {
      const uint32_t inputWidth  = decoder->alignedWidth >> (level + 1U);
      const uint32_t inputHeight = decoder->alignedHeight >> (level + 1U);
      struct NetworkVideoDirtyGrid * grid =
        &decoder->dirty[component][level];
      grid->width  = (inputWidth + 15U) / 16U;
      grid->height = (inputHeight + 15U) / 16U;
      grid->tiles  = calloc(
          (size_t)grid->width * grid->height, sizeof(*grid->tiles));
      if (!grid->tiles)
        return false;
    }
  }

  for (unsigned index = 0; index < NETWORK_VIDEO_UPLOADS; ++index)
  {
    struct NetworkVideoUpload * upload = &decoder->uploads[index];
    glGenBuffers(1, &upload->offsetBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, upload->offsetBuffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)decoder->blockCapacity * sizeof(*decoder->offsets),
        NULL, GL_STREAM_DRAW);
    glGenBuffers(1, &upload->payloadBuffer);
    if (!upload->offsetBuffer || !upload->payloadBuffer)
      return false;
  }

  return glGetError() == GL_NO_ERROR;
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
  struct NetworkVideoTexturePool * pool      = frame->pool;
  const bool                       retained  = networkVideoTexturePoolReturn(
      pool, frame->texture, frame->readySync, frame->generation);
  if (!retained)
  {
    struct NetworkVideoCurrent previous;
    EGLContext                 temporary = EGL_NO_CONTEXT;
    const bool                 usable    = networkVideoTexturePoolMakeCurrent(
        pool, &previous, &temporary);
    if (usable)
    {
      if (frame->readySync)
        glDeleteSync(frame->readySync);
      if (frame->texture)
        glDeleteTextures(1, &frame->texture);
      networkVideoTexturePoolRestoreCurrent(pool, &previous, temporary);
    }
  }
  networkVideoTexturePoolRelease(pool, false);
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
  decoder->referenceFrameID     = 0;
  decoder->attached             = false;
  decoder->referenceValid       = false;
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
  decoder->referenceFrameID = 0;
  decoder->referenceValid   = false;
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
    EGL_CONTEXT_MAJOR_VERSION, 3,
    EGL_CONTEXT_MINOR_VERSION, 1,
    EGL_NONE,
  };
  if (!eglBindAPI(EGL_OPENGL_ES_API))
    goto fail;
  decoder->context = eglCreateContext(decoder->display, decoder->eglConfig,
      decoder->shareContext, attributes);
  if (decoder->context == EGL_NO_CONTEXT)
  {
    DEBUG_ERROR("Failed to create the required OpenGL ES 3.1 network "
        "video context (eglError: 0x%x)", eglGetError());
    goto fail;
  }

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
  decoder->copyProgram    = networkVideoCreateProgram(
      NETWORK_VIDEO_COPY_SOURCES,
      sizeof(NETWORK_VIDEO_COPY_SOURCES) /
        sizeof(NETWORK_VIDEO_COPY_SOURCES[0]));
  const bool programsReady = decoder->dequantProgram &&
    decoder->idwtProgram && decoder->yuvProgram && decoder->copyProgram &&
    networkVideoCacheUniformsNL(decoder);
  networkVideoRestoreCurrent(decoder, &previous);
  if (!programsReady)
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

static bool networkVideoPacketMatches(
    const struct NetworkVideoPacketState * state,
    const uint8_t * packet, uint32_t length)
{
  if (!state->data || state->length != length || length < 8U)
    return !state->data && !packet;
  if (!packet ||
      networkVideoReadU32(state->data) !=
        (networkVideoReadU32(packet) & ~NETWORK_VIDEO_SEQUENCE_BITS))
    return false;
  return memcmp(state->data + sizeof(uint32_t),
      packet + sizeof(uint32_t), length - sizeof(uint32_t)) == 0;
}

static void networkVideoDiscardPendingPacketsNL(
    NetworkVideoDecoder * decoder)
{
  for (uint32_t index = 0; index < decoder->blockCapacity; ++index)
    if (decoder->dirtyBlocks[index] &&
        decoder->pendingPackets[index].data !=
          decoder->packetStates[index].data)
      free(decoder->pendingPackets[index].data);
}

static void networkVideoCommitPendingPacketsNL(
    NetworkVideoDecoder * decoder)
{
  for (uint32_t index = 0; index < decoder->blockCapacity; ++index)
  {
    if (!decoder->dirtyBlocks[index])
      continue;
    free(decoder->packetStates[index].data);
    decoder->packetStates[index] = decoder->pendingPackets[index];
  }
}

static bool networkVideoStagePacketsNL(NetworkVideoDecoder * decoder,
    const void * data, size_t size, bool update, uint32_t * changedCount)
{
  const uint8_t * bytes = data;
  memcpy(decoder->pendingPackets, decoder->packetStates,
      (size_t)decoder->blockCapacity * sizeof(*decoder->pendingPackets));
  memset(decoder->dirtyBlocks, 0, decoder->blockCapacity);
  *changedCount = 0;

  for (uint32_t index = 0; index < decoder->blockCapacity; ++index)
  {
    const bool present = decoder->offsets[index] != UINT32_MAX;
    if (update && !present)
      continue;

    const uint8_t * packet = NULL;
    uint32_t        length = 0;
    if (present)
    {
      const size_t   offset =
        (size_t)decoder->offsets[index] * sizeof(uint32_t);
      if (offset > size - 8U)
        goto fail;
      const uint32_t first  = networkVideoReadU32(bytes + offset);
      length = ((first >> 16U) & UINT32_C(0x00000fff)) *
        sizeof(uint32_t);
      if (length < 8U || length > size - offset)
        goto fail;
      if (first & UINT32_C(0x0000ffff))
        packet = bytes + offset;
    }

    if (networkVideoPacketMatches(
          &decoder->packetStates[index], packet, length))
    {
      if (update)
        decoder->offsets[index] = UINT32_MAX;
      continue;
    }

    struct NetworkVideoPacketState pending = { 0 };
    if (packet)
    {
      pending.data = malloc(length);
      if (!pending.data)
        goto fail;
      memcpy(pending.data, packet, length);
      networkVideoWriteU32(pending.data,
          networkVideoReadU32(pending.data) & ~NETWORK_VIDEO_SEQUENCE_BITS);
      pending.length = length;
    }
    decoder->pendingPackets[index] = pending;
    decoder->dirtyBlocks[index]    = 1U;
    ++*changedCount;
  }
  return true;

fail:
  networkVideoDiscardPendingPacketsNL(decoder);
  return false;
}

static void networkVideoClearDirtyNL(NetworkVideoDecoder * decoder)
{
  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
  {
    for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
    {
      struct NetworkVideoDirtyGrid * grid =
        &decoder->dirty[component][level];
      memset(grid->tiles, 0, (size_t)grid->width * grid->height);
      grid->count = 0;
    }
  }
}

static uint32_t networkVideoMirrorTexel(
    int32_t coordinate, uint32_t length)
{
  const int32_t period  = (int32_t)(2U * length);
  int32_t       wrapped = coordinate % period;
  if (wrapped < 0)
    wrapped += period;
  return (uint32_t)wrapped < length ? (uint32_t)wrapped :
    (uint32_t)(period - wrapped - 1);
}

static bool networkVideoIDWTGroupReads(uint32_t group, uint32_t length,
    uint32_t begin, uint32_t end)
{
  const int32_t base  = (int32_t)(group * 16U) - 2;
  const int32_t limit = (int32_t)length;
  for (int32_t local = 0; local <= 18; local += 2)
  {
    const int32_t coordinate = base + local;
    for (unsigned parity = 0; parity < 2U; ++parity)
    {
      int32_t first = coordinate;
      if (parity && coordinate < 0)
        --first;
      else if (!parity && coordinate + 1 >= limit)
        ++first;
      for (int32_t sample = 0; sample < 2; ++sample)
      {
        const uint32_t texel = networkVideoMirrorTexel(
            first + sample, length);
        if (texel >= begin && texel < end)
          return true;
      }
    }
  }
  return false;
}

static void networkVideoMarkGridRect(struct NetworkVideoDirtyGrid * grid,
    uint32_t inputWidth, uint32_t inputHeight,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  if (!width || !height)
    return;

  const uint32_t endX      = x + width;
  const uint32_t endY      = y + height;
  const uint32_t startX    = x < 18U ? 0U : (x - 18U) / 16U + 1U;
  const uint32_t startY    = y < 18U ? 0U : (y - 18U) / 16U + 1U;
  uint32_t       groupEndX = (endX + 17U) / 16U < grid->width ?
    (endX + 17U) / 16U : grid->width;
  uint32_t       groupEndY = (endY + 17U) / 16U < grid->height ?
    (endY + 17U) / 16U : grid->height;
  if (groupEndX < grid->width && networkVideoIDWTGroupReads(
        grid->width - 1U, inputWidth, x, endX))
    groupEndX = grid->width;
  if (groupEndY < grid->height && networkVideoIDWTGroupReads(
        grid->height - 1U, inputHeight, y, endY))
    groupEndY = grid->height;

  for (uint32_t groupY = startY; groupY < groupEndY; ++groupY)
  {
    for (uint32_t groupX = startX; groupX < groupEndX; ++groupX)
    {
      uint8_t * dirty = &grid->tiles[
        (size_t)groupY * grid->width + groupX];
      if (!*dirty)
      {
        *dirty = 1U;
        ++grid->count;
      }
    }
  }
}

static void networkVideoPrepareDirtyNL(NetworkVideoDecoder * decoder)
{
  networkVideoClearDirtyNL(decoder);
  for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
  {
    const uint32_t width  = decoder->alignedWidth >> (level + 1U);
    const uint32_t height = decoder->alignedHeight >> (level + 1U);
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
        for (uint32_t local = 0; local < meta->count; ++local)
        {
          if (!decoder->dirtyBlocks[meta->offset + local])
            continue;
          const uint32_t x = local % meta->stride * 32U;
          const uint32_t y = local / meta->stride * 32U;
          networkVideoMarkGridRect(&decoder->dirty[component][level],
              width, height, x, y,
              x < width ? (width - x < 32U ? width - x : 32U) : 0U,
              y < height ? (height - y < 32U ? height - y : 32U) : 0U);
        }
      }
    }
  }
}

static size_t networkVideoPayloadCapacity(size_t size)
{
  size_t capacity = 64U * 1024U;
  while (capacity < size)
    capacity *= 2U;
  return capacity;
}

static struct NetworkVideoUpload * networkVideoUploadNL(
    NetworkVideoDecoder * decoder, const void * data, size_t size)
{
  static const uint64_t padding = 0;

  struct NetworkVideoUpload * upload =
    &decoder->uploads[decoder->uploadIndex++ % NETWORK_VIDEO_UPLOADS];
  if (upload->readySync)
  {
    const GLenum wait = glClientWaitSync(upload->readySync,
        GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    if (wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED)
      return NULL;
    glDeleteSync(upload->readySync);
    upload->readySync = 0;
  }

  glBindBuffer(GL_SHADER_STORAGE_BUFFER, upload->offsetBuffer);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
      (GLsizeiptr)decoder->blockCapacity * sizeof(*decoder->offsets),
      decoder->offsets);

  glBindBuffer(GL_SHADER_STORAGE_BUFFER, upload->payloadBuffer);
  const size_t required = size + sizeof(padding);
  if (upload->payloadCapacity < required)
  {
    upload->payloadCapacity = networkVideoPayloadCapacity(required);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        (GLsizeiptr)upload->payloadCapacity,
        NULL, GL_STREAM_DRAW);
  }
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size, data);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)size,
      sizeof(padding), &padding);
  if (glGetError() != GL_NO_ERROR)
    return NULL;
  return upload;
}

static void networkVideoDequantNL(NetworkVideoDecoder * decoder,
    const struct NetworkVideoUpload * upload, bool update)
{
  glUseProgram(decoder->dequantProgram);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, upload->offsetBuffer);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, upload->payloadBuffer);
  glUniform1i(decoder->uniforms.dequant.preserveMissing, update);

  for (unsigned level = 0; level < NETWORK_VIDEO_LEVELS; ++level)
  {
    const uint32_t width  = decoder->alignedWidth  >> (level + 1U);
    const uint32_t height = decoder->alignedHeight >> (level + 1U);
    glBindImageTexture(0, decoder->coefficientTextures[level], 0,
        GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
    glUniform2i(decoder->uniforms.dequant.resolution, width, height);

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
        glUniform1i(decoder->uniforms.dequant.outputLayer,
            component * NETWORK_VIDEO_BANDS + band);
        glUniform1i(decoder->uniforms.dequant.blockOffset, meta->offset);
        glUniform1i(decoder->uniforms.dequant.blockStride, meta->stride);
        if (!update)
        {
          glUniform2i(decoder->uniforms.dequant.workGroupOffset, 0, 0);
          glDispatchCompute(
              (width + 31U) / 32U, (height + 31U) / 32U, 1);
          continue;
        }

        const uint32_t rows = meta->count / meta->stride;
        for (uint32_t y = 0; y < rows; ++y)
        {
          uint32_t x = 0;
          while (x < meta->stride)
          {
            const size_t row = (size_t)meta->offset +
              (size_t)y * meta->stride;
            while (x < meta->stride && !decoder->dirtyBlocks[row + x])
              ++x;
            const uint32_t start = x;
            while (x < meta->stride && decoder->dirtyBlocks[row + x])
              ++x;
            if (start == x)
              continue;
            glUniform2i(decoder->uniforms.dequant.workGroupOffset,
                (GLint)start, (GLint)y);
            glDispatchCompute(x - start, 1, 1);
          }
        }
      }
    }
  }
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
      GL_TEXTURE_FETCH_BARRIER_BIT);
}

static void networkVideoPropagateDirtyNL(NetworkVideoDecoder * decoder,
    unsigned component, unsigned level, bool final)
{
  if (final)
    return;

  const struct NetworkVideoDirtyGrid * grid =
    &decoder->dirty[component][level];
  const uint32_t outputWidth  = decoder->alignedWidth >> level;
  const uint32_t outputHeight = decoder->alignedHeight >> level;
  for (uint32_t y = 0; y < grid->height; ++y)
  {
    for (uint32_t x = 0; x < grid->width; ++x)
    {
      if (!grid->tiles[(size_t)y * grid->width + x])
        continue;
      const uint32_t outputX = x * 32U;
      const uint32_t outputY = y * 32U;
      const uint32_t width   = outputX < outputWidth ?
        (outputWidth - outputX < 32U ? outputWidth - outputX : 32U) : 0U;
      const uint32_t height  = outputY < outputHeight ?
        (outputHeight - outputY < 32U ? outputHeight - outputY : 32U) : 0U;
      networkVideoMarkGridRect(&decoder->dirty[component][level - 1U],
          outputWidth, outputHeight,
          outputX, outputY, width, height);
    }
  }
}

static void networkVideoIDWTNL(NetworkVideoDecoder * decoder, bool update)
{
  glUseProgram(decoder->idwtProgram);
  glActiveTexture(GL_TEXTURE0);

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
      glUniform2i(decoder->uniforms.idwt.resolution,
          inputHeight, inputWidth);
      glUniform2f(decoder->uniforms.idwt.invResolution,
          1.0f / inputHeight, 1.0f / inputWidth);
      glUniform1i(decoder->uniforms.idwt.baseLayer,
          component * NETWORK_VIDEO_BANDS);
      const bool final = level == (int)finalLevel;
      glUniform1i(decoder->uniforms.idwt.dcShift,
          final ? GL_TRUE : GL_FALSE);

      struct NetworkVideoDirtyGrid * grid =
        &decoder->dirty[component][level];
      if (update && !grid->count)
        continue;

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
      if (!update)
      {
        glUniform2i(decoder->uniforms.idwt.workGroupOffset, 0, 0);
        glDispatchCompute(
            (inputHeight + 15U) / 16U, (inputWidth + 15U) / 16U, 1);
      }
      else
      {
        for (uint32_t y = 0; y < grid->height; ++y)
        {
          uint32_t x = 0;
          while (x < grid->width)
          {
            const size_t row = (size_t)y * grid->width;
            while (x < grid->width && !grid->tiles[row + x])
              ++x;
            const uint32_t start = x;
            while (x < grid->width && grid->tiles[row + x])
              ++x;
            if (start == x)
              continue;
            glUniform2i(decoder->uniforms.idwt.workGroupOffset,
                (GLint)y, (GLint)start);
            glDispatchCompute(1, x - start, 1);
          }
        }
        networkVideoPropagateDirtyNL(
            decoder, component, (unsigned)level, final);
      }
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

static bool networkVideoDamageFull(const LGNetVideoDamage * damage)
{
  return (damage->flags & LG_NET_VIDEO_DAMAGE_FULL) != 0;
}

static void networkVideoDispatchRect(GLint offsetUniform,
    GLint extentUniform, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
  glUniform2i(offsetUniform, (GLint)x, (GLint)y);
  glUniform2i(extentUniform, (GLint)width, (GLint)height);
  glDispatchCompute((width + 7U) / 8U, (height + 7U) / 8U, 1);
}

static void networkVideoDispatchDamageNL(NetworkVideoDecoder * decoder,
    GLint offsetUniform, GLint extentUniform,
    const LGNetVideoDamage * damage)
{
  if (networkVideoDamageFull(damage))
  {
    networkVideoDispatchRect(offsetUniform, extentUniform, 0, 0,
        decoder->config.width, decoder->config.height);
    return;
  }

  for (uint16_t index = 0; index < damage->count; ++index)
  {
    const LGNetVideoDamageRect * rect = &damage->rects[index];
    networkVideoDispatchRect(offsetUniform, extentUniform,
        rect->x, rect->y, rect->width, rect->height);
  }
}

static const LGNetVideoDamage * networkVideoFindDamageNL(
    const NetworkVideoDecoder * decoder, uint64_t generation)
{
  const struct NetworkVideoDamageHistory * history =
    &decoder->damageHistory[generation % NETWORK_VIDEO_DAMAGE_HISTORY];
  return history->generation == generation ? &history->damage : NULL;
}

static bool networkVideoCopyNeedsFullNL(
    const NetworkVideoDecoder * decoder, uint64_t textureGeneration,
    uint64_t generation, const LGNetVideoDamage * damage)
{
  if (!textureGeneration || textureGeneration >= generation ||
      generation - textureGeneration > NETWORK_VIDEO_DAMAGE_HISTORY ||
      networkVideoDamageFull(damage))
    return true;

  for (uint64_t previous = textureGeneration + 1U;
       previous < generation; ++previous)
  {
    const LGNetVideoDamage * prior =
      networkVideoFindDamageNL(decoder, previous);
    if (!prior || networkVideoDamageFull(prior))
      return true;
  }
  return false;
}

static void networkVideoCopyCanonicalNL(NetworkVideoDecoder * decoder,
    GLuint texture, uint64_t textureGeneration, uint64_t generation,
    const LGNetVideoDamage * damage)
{
  glUseProgram(decoder->copyProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, decoder->canonicalTexture);
  glBindImageTexture(0, texture, 0, GL_FALSE, 0,
      GL_WRITE_ONLY, GL_RGBA16F);

  if (networkVideoCopyNeedsFullNL(
        decoder, textureGeneration, generation, damage))
  {
    networkVideoDispatchRect(decoder->uniforms.copy.outputOffset,
        decoder->uniforms.copy.outputExtent, 0, 0,
        decoder->config.width, decoder->config.height);
  }
  else
  {
    for (uint64_t previous = textureGeneration + 1U;
         previous < generation; ++previous)
      networkVideoDispatchDamageNL(decoder,
          decoder->uniforms.copy.outputOffset,
          decoder->uniforms.copy.outputExtent,
          networkVideoFindDamageNL(decoder, previous));
    networkVideoDispatchDamageNL(decoder,
        decoder->uniforms.copy.outputOffset,
        decoder->uniforms.copy.outputExtent, damage);
  }

  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
      GL_TEXTURE_FETCH_BARRIER_BIT);
}

static void networkVideoBuildDamage(const LGNetVideoDamage * damage,
    NetworkVideoDecodedFrame * result)
{
  if (networkVideoDamageFull(damage))
    return;

  for (uint16_t index = 0; index < damage->count; ++index)
  {
    const LGNetVideoDamageRect * source = &damage->rects[index];
    result->damageRects[index] = (KVMFRFrameDamageRect)
    {
      .x      = source->x,
      .y      = source->y,
      .width  = source->width,
      .height = source->height,
    };
  }
  result->damageRectsCount = damage->count;
}

static struct NetworkVideoExternalFrame * networkVideoConvertNL(
    NetworkVideoDecoder * decoder, bool chromaLeft, uint64_t generation,
    const LGNetVideoDamage * damage)
{
  glUseProgram(decoder->yuvProgram);
  for (unsigned component = 0;
       component < NETWORK_VIDEO_COMPONENTS; ++component)
  {
    glActiveTexture(GL_TEXTURE0 + component);
    glBindTexture(GL_TEXTURE_2D, decoder->planeTextures[component]);
  }
  glUniform2i(decoder->uniforms.yuv.outputSize,
      decoder->config.width, decoder->config.height);
  glUniform1i(decoder->uniforms.yuv.bt2020,
      decoder->config.colorMatrix == LG_NET_COLOR_MATRIX_BT2020);
  glUniform1i(decoder->uniforms.yuv.chromaLeft, chromaLeft);
  float lumaOffset;
  float lumaScale;
  float chromaMidpoint;
  float chromaScale;
  networkVideoColorRange(&decoder->config,
      &lumaOffset, &lumaScale, &chromaMidpoint, &chromaScale);
  glUniform1f(decoder->uniforms.yuv.lumaOffset, lumaOffset);
  glUniform1f(decoder->uniforms.yuv.lumaScale, lumaScale);
  glUniform1f(decoder->uniforms.yuv.chromaMidpoint, chromaMidpoint);
  glUniform1f(decoder->uniforms.yuv.chromaScale, chromaScale);
  glBindImageTexture(0, decoder->canonicalTexture, 0, GL_FALSE, 0,
      GL_WRITE_ONLY, GL_RGBA16F);
  networkVideoDispatchDamageNL(decoder,
      decoder->uniforms.yuv.outputOffset,
      decoder->uniforms.yuv.outputExtent, damage);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
      GL_TEXTURE_FETCH_BARRIER_BIT);

  struct NetworkVideoTexture pooled =
    networkVideoTexturePoolAcquire(decoder->texturePool);
  if (!pooled.texture)
    return NULL;
  networkVideoCopyCanonicalNL(decoder, pooled.texture,
      pooled.generation, generation, damage);

  GLsync readySync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  if (!readySync || glGetError() != GL_NO_ERROR)
  {
    networkVideoTexturePoolRecycleCurrent(
        decoder->texturePool, pooled.texture, readySync, 0);
    return NULL;
  }

  struct NetworkVideoExternalFrame * frame = calloc(1, sizeof(*frame));
  if (!frame)
  {
    networkVideoTexturePoolRecycleCurrent(
        decoder->texturePool, pooled.texture, readySync, 0);
    return NULL;
  }
  frame->base.ops      = &NETWORK_VIDEO_EXTERNAL_OPS;
  frame->pool          = decoder->texturePool;
  frame->texture       = pooled.texture;
  frame->readySync     = readySync;
  frame->generation    = generation;
  networkVideoTexturePoolRetain(frame->pool);
  atomic_init(&frame->references, 1U);
  return frame;
}

NetworkVideoDecodeResult networkVideoDecoderDecode(
    NetworkVideoDecoder * decoder, const LGNetVideoStreamConfig * config,
    const void * data, size_t size, uint32_t blockCount,
    bool update, bool complete, uint64_t baseFrameID, uint64_t frameID,
    NetworkVideoDecodedFrame * result)
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

  if ((!update && baseFrameID) || (update && !complete) ||
      (update && (!decoder->referenceValid ||
        !baseFrameID || baseFrameID > decoder->referenceFrameID ||
        frameID <= decoder->referenceFrameID)))
  {
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_CORRUPT;
  }

  NetworkPyroWaveLayout       layout;
  const NetworkPyroWaveResult parse = networkPyroWaveParse(
      config, data, size, blockCount, decoder->offsets,
      decoder->blockCapacity, &layout);
  if (parse != NETWORK_PYROWAVE_OK || layout.delta != update)
  {
    LG_UNLOCK(decoder->lock);
    return parse == NETWORK_PYROWAVE_UNSUPPORTED ?
      NETWORK_VIDEO_DECODE_UNSUPPORTED : NETWORK_VIDEO_DECODE_CORRUPT;
  }

  LGNetVideoDamage damage = layout.damage;
  if (!update)
  {
    damage = (LGNetVideoDamage)
    {
      .flags = (LGNetVideoDamageFlags)LG_NET_VIDEO_DAMAGE_FULL,
    };
  }
  const bool hasPresentationDamage =
    networkVideoDamageFull(&damage) || damage.count;

  uint32_t changedCount = 0;
  if (!networkVideoStagePacketsNL(
        decoder, data, size, update, &changedCount))
  {
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }
  if (update && !changedCount)
  {
    networkVideoCommitPendingPacketsNL(decoder);
    decoder->referenceFrameID = frameID;
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_NO_FRAME;
  }

  if (update)
    networkVideoPrepareDirtyNL(decoder);
  else
    networkVideoClearDirtyNL(decoder);

  struct NetworkVideoCurrent previous;
  if (!networkVideoMakeCurrent(decoder, &previous))
  {
    networkVideoDiscardPendingPacketsNL(decoder);
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }

  while (glGetError() != GL_NO_ERROR)
  {
  }
  struct NetworkVideoUpload * upload = networkVideoUploadNL(
      decoder, data, size);
  if (!upload)
  {
    networkVideoDiscardPendingPacketsNL(decoder);
    networkVideoRestoreCurrent(decoder, &previous);
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }

  const uint64_t decodeStart = nanotime();
  uint64_t       generation  = decoder->outputGeneration + 1U;
  if (!generation)
    generation = 1U;
  networkVideoDequantNL(decoder, upload, update);
  upload->readySync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  networkVideoIDWTNL(decoder, update);
  struct NetworkVideoExternalFrame * external = NULL;
  if (hasPresentationDamage)
    external = networkVideoConvertNL(decoder,
        layout.chromaLeft, generation, &damage);
  else
    glFlush();
  const uint64_t decodeTime = nanotime() - decodeStart;
  const bool     success    =
    (!hasPresentationDamage || external) && glGetError() == GL_NO_ERROR;
  networkVideoRestoreCurrent(decoder, &previous);
  if (!success)
  {
    networkVideoDiscardPendingPacketsNL(decoder);
    decoder->referenceFrameID = 0;
    decoder->referenceValid   = false;
    if (external)
      networkVideoExternalRelease(&external->base);
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_ERROR;
  }

  networkVideoCommitPendingPacketsNL(decoder);
  decoder->referenceFrameID = complete ? frameID : 0;
  decoder->referenceValid   = complete;
  if (!hasPresentationDamage)
  {
    LG_UNLOCK(decoder->lock);
    return NETWORK_VIDEO_DECODE_NO_FRAME;
  }

  struct NetworkVideoDamageHistory * history =
    &decoder->damageHistory[generation % NETWORK_VIDEO_DAMAGE_HISTORY];
  history->generation       = generation;
  history->damage           = damage;
  decoder->outputGeneration = generation;

  uint32_t formatVersion = (uint32_t)config->configEpoch;
  if (!formatVersion)
    formatVersion = 1;

  result->externalFrame = &external->base;
  result->decodeTimeNs  = decodeTime;
  if (update)
    networkVideoBuildDamage(&damage, result);
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
