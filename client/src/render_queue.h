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

#include "interface/renderer.h"

#include <stdatomic.h>

typedef enum RenderQueueSource
{
  RENDER_QUEUE_SOURCE_NONE,
  RENDER_QUEUE_SOURCE_PRIMARY,
  RENDER_QUEUE_SOURCE_FALLBACK,
  RENDER_QUEUE_SOURCE_COUNT,
}
RenderQueueSource;

typedef void (*RenderQueueSourceAppliedFn)(void * opaque,
    RenderQueueSource source, uint64_t generation, uint64_t serial,
    bool swSurface);
typedef bool (*RenderQueueSourcePrepareFn)(void * opaque,
    RenderQueueSource source, uint64_t generation, uint64_t serial);

void renderQueue_init(void);
void renderQueue_free(void);
void renderQueue_clear(void);
void renderQueue_process(void);
void renderQueue_presented(void);

void renderQueue_setSourceFns(RenderQueueSourcePrepareFn prepare,
    RenderQueueSourceAppliedFn applied, void * opaque);

/* Starting a source lifecycle makes commands from its prior generation stale.
 * After a successful render, call renderQueue_presented on the render thread
 * to acknowledge the newest applied transition. */
uint64_t renderQueue_sourceBegin(RenderQueueSource source);
void renderQueue_sourceInvalidate(RenderQueueSource source,
    uint64_t generation);
void renderQueue_sourceClearCursor(RenderQueueSource source);
uint64_t renderQueue_sourceTransition(RenderQueueSource source,
    uint64_t generation, bool swSurface,
    atomic_uint_least64_t * publishedSerial);

uint64_t renderQueue_sourceSwSurfaceConfigureTransition(
    RenderQueueSource source, uint64_t generation, int width, int height,
    atomic_uint_least64_t * publishedSerial);

void renderQueue_sourceSwSurfaceDrawFill(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height,
    uint32_t color);

void renderQueue_sourceSwSurfaceDrawBitmap(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height, int stride,
    const void * data, bool topDown);

void renderQueue_sourceSurfaceFormat(RenderQueueSource source,
    uint64_t generation, const LG_RendererFormat format,
    bool rendererSupportsNativeHDR);

void renderQueue_sourceCursorState(RenderQueueSource source,
    uint64_t generation, bool visible, int x, int y, int hx, int hy);

/* Cursor payloads are copied before these calls return. */
void renderQueue_sourceCursorImage(RenderQueueSource source,
    uint64_t generation, LG_RendererCursor type,
    int width, int height, int pitch, const void * data);

void renderQueue_sourceCursorColorTransform(RenderQueueSource source,
    uint64_t generation, const LGColorTransform * transform);

void renderQueue_sourceCursorWhiteLevel(RenderQueueSource source,
    uint64_t generation, uint32_t sdrWhiteLevel);
