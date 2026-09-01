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

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "app.h"
#include "common/types.h"
#include "common/framebuffer.h"

#define IS_LG_RENDERER_VALID(x) \
  ((x)->getName             && \
   (x)->create              && \
   (x)->initialize          && \
   (x)->deinitialize        && \
   (x)->onRestart           && \
   (x)->onResize            && \
   (x)->onFontUpdate        && \
   (x)->onMouseShape        && \
   (x)->onMouseEvent        && \
   (x)->renderStartup       && \
   (x)->render              && \
   (x)->createTexture       && \
   (x)->freeTexture         && \
   (x)->swSurfaceConfigure  && \
   (x)->swSurfaceDrawFill   && \
   (x)->swSurfaceDrawBitmap && \
   (x)->swSurfaceShow)

typedef struct LG_RendererParams
{
  bool quickSplash;
}
LG_RendererParams;

typedef enum LG_RendererSupport
{
  LG_SUPPORTS_DMABUF,
  LG_SUPPORTS_HDR_PQ,
  LG_SUPPORTS_HDR_SCRGB
}
LG_RendererSupport;

typedef enum LG_RendererRotate
{
  LG_ROTATE_0,
  LG_ROTATE_90,
  LG_ROTATE_180,
  LG_ROTATE_270
}
LG_RendererRotate;

// kept out of the enum so gcc doesn't warn when it's missing from a switch
// statement.
#define LG_ROTATE_MAX (LG_ROTATE_270+1)

typedef struct LG_RendererFormat
{
  KVMFRFrameType    type;         // frame type
  bool              hdr;          // if the frame is HDR or not
  bool              hdrPQ;        // if the HDR content is PQ mapped
  bool              hdrMetadata;  // if the HDR static metadata is valid
  unsigned int      screenWidth;  // actual width of the host
  unsigned int      screenHeight; // actual height of the host
  unsigned int      dataWidth;    // the width of the packed data
  unsigned int      dataHeight;   // the height of the packed data
  unsigned int      frameWidth;   // width of frame transmitted
  unsigned int      frameHeight;  // height of frame transmitted
  unsigned int      stride;  // scanline width (zero if compresed)
  unsigned int      pitch;   // scanline bytes (or compressed size)
  unsigned int      bpp;     // bits per pixel (zero if compressed)
  LG_RendererRotate rotate;  // guest rotation

  // HDR static metadata, valid when hdrMetadata is true
  uint16_t hdrDisplayPrimary[3][2];
  uint16_t hdrWhitePoint[2];
  uint32_t hdrMaxDisplayLuminance;
  uint32_t hdrMinDisplayLuminance;
  uint32_t hdrMaxContentLightLevel;
  uint32_t hdrMaxFrameAverageLightLevel;
  uint32_t sdrWhiteLevel;
}
LG_RendererFormat;

typedef struct LG_RendererRect
{
  bool valid;
  int  x;
  int  y;
  int  w;
  int  h;
}
LG_RendererRect;

typedef enum LG_RendererCaptureFormat
{
  LG_CAPTURE_RGBA8,
  LG_CAPTURE_RGB10_A2,
  LG_CAPTURE_RGBA32F,
}
LG_RendererCaptureFormat;

typedef struct LG_RendererCapture
{
  unsigned int width;
  unsigned int height;
  size_t       stride;
  size_t       dataSize;
  LG_RendererCaptureFormat format;
  bool         hdr;
  bool         hdrPQ;
  bool         nativeHDR;
  void       * data;
}
LG_RendererCapture;

#define LG_RENDERER_FRAME_TOKEN_NONE 0

typedef uint64_t LG_RendererFrameToken;

/* Renderer timings are client-clock durations in nanoseconds. frameToken is
 * the received frame actually consumed by this render, or
 * LG_RENDERER_FRAME_TOKEN_NONE when the render only refreshed existing
 * content. */
typedef struct LG_RendererFrameTiming
{
  LG_RendererFrameToken frameToken;  /* frame consumed by this render */
  uint64_t              setupTime;   /* render entry to desktop processing */
  uint64_t              effectsTime; /* post-processing work */
  uint64_t              desktopTime; /* desktop work, excluding effects */
  uint64_t              composeTime; /* composition, excluding UI overlay */
  uint64_t              swapTime;    /* actual EGL buffer swap */
  bool                  presentTracked; /* presentation feedback expected */
}
LG_RendererFrameTiming;

typedef enum LG_RendererCursor
{
  LG_CURSOR_COLOR       ,
  LG_CURSOR_MONOCHROME  ,
  LG_CURSOR_MASKED_COLOR
}
LG_RendererCursor;

/* Current producers reserve at most a 512x512 32-bpp cursor payload.
 * Monochrome wire heights contain vertically stacked AND and XOR masks. */
#define LG_CURSOR_MAX_WIDTH     512
#define LG_CURSOR_MAX_HEIGHT    512
#define LG_CURSOR_MAX_DATA_SIZE \
  ((size_t)LG_CURSOR_MAX_WIDTH * LG_CURSOR_MAX_HEIGHT * 4U)

static inline bool lg_rendererCursorValidate(LG_RendererCursor type,
    int width, int height, int pitch, size_t * dataSize)
{
  if (width <= 0 || height <= 0 || pitch <= 0 ||
      width > LG_CURSOR_MAX_WIDTH)
    return false;

  size_t rowBytes;
  int decodedHeight;
  switch (type)
  {
    case LG_CURSOR_COLOR:
    case LG_CURSOR_MASKED_COLOR:
      rowBytes      = (size_t)width * 4U;
      decodedHeight = height;
      break;

    case LG_CURSOR_MONOCHROME:
      if (height & 1)
        return false;
      rowBytes      = ((size_t)width + 7U) / 8U;
      decodedHeight = height / 2;
      break;

    default:
      return false;
  }

  if (decodedHeight <= 0 || decodedHeight > LG_CURSOR_MAX_HEIGHT ||
      (size_t)pitch < rowBytes ||
      (size_t)pitch > LG_CURSOR_MAX_DATA_SIZE / (size_t)height)
    return false;

  if (dataSize)
    *dataSize = (size_t)height * (size_t)pitch;
  return true;
}

typedef struct LG_Renderer LG_Renderer;

typedef enum LG_RendererInteropType
{
  LG_RENDERER_INTEROP_NONE,
  LG_RENDERER_INTEROP_EGL,
}
LG_RendererInteropType;

#define LG_RENDERER_INTEROP_VERSION 1

typedef struct LG_RendererInterop
{
  uint32_t version;
  uint32_t size;
  LG_RendererInteropType type;
  struct
  {
    EGLDisplay display;
    EGLConfig config;
    EGLContext shareContext;
    bool dmaBufImport;
  }
  egl;
}
LG_RendererInterop;

typedef struct LG_RendererOps
{
  /* returns the friendly name of the renderer */
  const char * (*getName)(void);

  /* called pre-creation to allow the renderer to register any options it may
   * have */
  void (*setup)(void);

  /* creates an instance of the renderer
   * Context: lg_run */
  bool (*create)(LG_Renderer ** renderer, const LG_RendererParams params,
      bool * needsOpenGL);

  /* initializes the renderer for use
   * Context: lg_run */
  bool (*initialize)(LG_Renderer * renderer);

  /* deinitializes & frees the renderer
   * Context: lg_run & renderThread */
  void (*deinitialize)(LG_Renderer * renderer);

  /* returns true if the specified feature is supported
   * Context: renderThread */
  bool (*supports)(LG_Renderer * renderer, LG_RendererSupport support);

  /* optionally exposes the initialized renderer device context to a transport.
   * The transport must create its own shared context and release it before the
   * renderer is deinitialized. Context: lg_run */
  bool (*getInterop)(LG_Renderer * renderer, LG_RendererInterop * interop);

  /* called when the renderer is to reset it's state
   * Context: lg_run & frameThread */
  void (*onRestart)(LG_Renderer * renderer);

  /* called when the viewport has been resized
   * Context: renderThrtead */
  void (*onResize)(LG_Renderer * renderer, const int width, const int height,
      const double scale, const LG_RendererRect destRect,
      LG_RendererRotate rotate);

  /* called when the Dear ImGui font atlas texture must be uploaded
   * Context: renderThread */
  bool (*onFontUpdate)(LG_Renderer * renderer);

  /* called when the mouse shape has changed
   * Context: cursorThread */
  bool (*onMouseShape)(LG_Renderer * renderer, const LG_RendererCursor cursor,
      const int width, const int height, const int pitch, const uint8_t * data);

  /* optional display calibration update for hardware cursor composition
   * Context: cursorThread */
  void (*onMouseColorTransform)(LG_Renderer * renderer,
      const KVMFRColorTransform * transform);

  /* updates the cursor-specific SDR white level reported by IddCx
   * Context: cursorThread */
  void (*onMouseWhiteLevel)(LG_Renderer * renderer, uint32_t sdrWhiteLevel);

  /* called when the mouse has moved or changed visibillity
   * Context: cursorThread */
  bool (*onMouseEvent)(LG_Renderer * renderer, const bool visible, int x, int y,
      const int hx, const int hy);

  /* called when the frame format has changed
   * Context: frameThread */
  bool (*onFrameFormat)(LG_Renderer * renderer,
      const LG_RendererFormat format);

  /* called when there is a new frame. frameToken must remain attached to the
   * update if the renderer coalesces it, and must be reported by render only
   * when that update is consumed. A non-NULL releaseFn transfers ownership to
   * the renderer on success and must be called exactly once when the imported
   * frame can no longer be sampled. Context: frameThread */
  bool (*onFrame)(LG_Renderer * renderer, const KVMFRFrameBuffer * frame,
      int dmaFD, const KVMFRFrameDamageRect * damage, int damageCount,
      LG_RendererFrameToken frameToken,
      LG_FrameReleaseFn releaseFn,
      void * releaseOpaque, uint64_t releaseHandle);

  /* optional frame-thread pump for asynchronous imports while no frame is
   * available from the transport */
  void (*onFramePoll)(LG_Renderer * renderer);

  /* called when the rederer is to startup
   * Context: renderThread */
  bool (*renderStartup)(LG_Renderer * renderer, bool useDMA);

  /* called to render the scene. The renderer must not consume a received
   * frame newer than frameTokenLimit, and reports the token it did consume in
   * timing (or LG_RENDERER_FRAME_TOKEN_NONE). Context: renderThread */
  bool (*render)(LG_Renderer * renderer, LG_RendererRotate rotate,
      LG_RendererFrameToken frameTokenLimit, const bool invalidateWindow,
      void (*preSwap)(void * udata), void * udata,
      LG_RendererFrameTiming * timing);

  /* Optional test/diagnostic readback of the fully composed framebuffer.
   * Called on the render thread before swap while the graphics context is
   * current. The caller owns capture->data and must free it. */
  bool (*capture)(LG_Renderer * renderer, LG_RendererCapture * capture);

  /* called to create a texture from the specified 32-bit RGB image data. This
   * method is for use with Dear ImGui
   * Context: renderThread */
  void * (*createTexture)(LG_Renderer * renderer,
      int width, int height, uint8_t * data);

  /* called to free a texture previously created by createTexture. This method
   * is for use with Dear ImGui
   * Context: renderThread */
  void (*freeTexture)(LG_Renderer * renderer, void * texture);

  /* setup the incremental software surface */
  void (*swSurfaceConfigure)(LG_Renderer * renderer,
      int width, int height);

  /* draw a filled rect on the software surface with the specified color */
  void (*swSurfaceDrawFill)(LG_Renderer * renderer,
      int x, int y, int width, int height, uint32_t color);

  /* draw an image on the software surface, data is RGBA32 */
  void (*swSurfaceDrawBitmap)(LG_Renderer * renderer,
      int x, int y, int width, int height, int stride, uint8_t * data,
      bool topDown);

  /* show the incremental software surface */
  void (*swSurfaceShow)(LG_Renderer * renderer, bool show);
}
LG_RendererOps;

typedef struct LG_Renderer
{
  LG_RendererOps ops;
}
LG_Renderer;
