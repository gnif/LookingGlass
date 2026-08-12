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

#ifndef _H_OVERLAYS_H
#define _H_OVERLAYS_H

#include "interface/overlay.h"
#include "overlay/msg.h"

struct Overlay
{
  const struct LG_OverlayOps * ops;
  const void * params;
  void       * udata;
  int          lastRectCount;
  struct Rect  lastRects[MAX_OVERLAY_RECTS];
};

extern struct LG_OverlayOps LGOverlaySplash;
extern struct LG_OverlayOps LGOverlayAlert;
extern struct LG_OverlayOps LGOverlayFPS;
extern struct LG_OverlayOps LGOverlayGraphs;
extern struct LG_OverlayOps LGOverlayHelp;
extern struct LG_OverlayOps LGOverlayConfig;
extern struct LG_OverlayOps LGOverlayMsg;
extern struct LG_OverlayOps LGOverlayStatus;

void overlayAlert_show(LG_MsgAlert type, const char * fmt, va_list args);

typedef struct OverlayFrameTiming
{
  uint64_t timestamp;
  uint32_t validMask;
  float    capture;
  float    postProcess;
  float    copy;
  float    ready;
  float    hold;
  float    transport;
  float    receive;
  float    providerPrepare;
  float    import;
  float    dispatch;
  float    queue;
  float    prepare;
  float    setup;
  float    effects;
  float    desktop;
  float    compose;
  float    swap;
  float    present;
}
OverlayFrameTiming;

enum OverlayFrameTimingStage
{
  OVERLAY_FRAME_TIMING_CAPTURE,
  OVERLAY_FRAME_TIMING_POST_PROCESS,
  OVERLAY_FRAME_TIMING_COPY,
  OVERLAY_FRAME_TIMING_READY,
  OVERLAY_FRAME_TIMING_HOLD,
  OVERLAY_FRAME_TIMING_TRANSPORT,
  OVERLAY_FRAME_TIMING_RECEIVE,
  OVERLAY_FRAME_TIMING_PROVIDER_PREPARE,
  OVERLAY_FRAME_TIMING_IMPORT,
  OVERLAY_FRAME_TIMING_DISPATCH,
  OVERLAY_FRAME_TIMING_QUEUE,
  OVERLAY_FRAME_TIMING_PREPARE,
  OVERLAY_FRAME_TIMING_SETUP,
  OVERLAY_FRAME_TIMING_EFFECTS,
  OVERLAY_FRAME_TIMING_DESKTOP,
  OVERLAY_FRAME_TIMING_COMPOSE,
  OVERLAY_FRAME_TIMING_SWAP,
  OVERLAY_FRAME_TIMING_PRESENT,
  OVERLAY_FRAME_TIMING_COUNT,
};

#define OVERLAY_FRAME_TIMING_VALID_ALL \
  ((1U << OVERLAY_FRAME_TIMING_COUNT) - 1U)
#define OVERLAY_FRAME_TIMING_VALID_PRODUCER \
  ((1U << OVERLAY_FRAME_TIMING_TRANSPORT) - 1U)
#define OVERLAY_FRAME_TIMING_VALID_TRANSPORT \
  (1U << OVERLAY_FRAME_TIMING_TRANSPORT)
#define OVERLAY_FRAME_TIMING_VALID_PROVIDER \
  ((1U << OVERLAY_FRAME_TIMING_RECEIVE) | \
   (1U << OVERLAY_FRAME_TIMING_PROVIDER_PREPARE))
#define OVERLAY_FRAME_TIMING_VALID_PRESENT \
  (1U << OVERLAY_FRAME_TIMING_PRESENT)

GraphHandle overlayGraph_register(const char * name, RingBuffer buffer,
    float min, float max, GraphFormatFn formatFn);
GraphHandle overlayGraph_registerFrameTiming(const char * name,
    RingBuffer buffer);
void overlayGraph_unregister(GraphHandle handle);
void overlayGraph_setCompact(GraphHandle handle, bool compact);
void overlayGraph_iterate(void (*callback)(GraphHandle handle, const char * name,
    bool * enabled, void * udata), void * udata);
void overlayGraph_invalidate(GraphHandle handle);

void overlayConfig_register(const char * title,
    void (*callback)(void * udata, int * id), void * udata);

void overlayConfig_registerTab(const char * title,
    void (*callback)(void * udata, int * id), void * udata);

typedef enum LG_UserStatus
{
  LG_USER_STATUS_SPICE,
  LG_USER_STATUS_RECORDING,
  LG_USER_STATUS_MAX
}
LGUserStatus;

void overlaySplash_show(bool show);
bool overlaySplash_isFading(void);
void overlayStatus_set(LGUserStatus, bool value);

#endif
