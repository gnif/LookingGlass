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

#include "surface.h"

#include "spice.h"

#include "common/debug.h"
#include "common/locking.h"

#include <stdlib.h>
#include <string.h>

struct SpiceSurface
{
  LG_RWLock eventsLock;
  LG_Lock   activationLock;

  const LG_SwSurfaceEventOps * events;
  void                       * eventOpaque;
  bool                         active;
  bool                         primaryValid;
  int                          hotX;
  int                          hotY;
};

static bool surfaceAttach(LG_Transport * transport,
    const LG_SwSurfaceEventOps * events, void * opaque)
{
  if (!events || !events->configure || !events->destroy ||
      !events->drawFill || !events->drawBitmap || !events->pointer)
    return false;

  SpiceSurface * surface = transport->surface;
  LG_LOCK_EXCLUSIVE(surface->eventsLock);
  surface->events      = events;
  surface->eventOpaque = opaque;
  LG_UNLOCK_EXCLUSIVE(surface->eventsLock);
  return true;
}

static void surfaceDetach(LG_Transport * transport)
{
  SpiceSurface * surface = transport->surface;
  LG_LOCK_EXCLUSIVE(surface->eventsLock);
  surface->events      = NULL;
  surface->eventOpaque = NULL;
  LG_UNLOCK_EXCLUSIVE(surface->eventsLock);
}

static bool surfaceSetActive(LG_Transport * transport, bool active)
{
  SpiceSurface * surface = transport->surface;
  LG_LOCK(surface->activationLock);

  if (surface->active == active)
  {
    LG_UNLOCK(surface->activationLock);
    return true;
  }

  bool result = false;
  if (active)
  {
    LG_LOCK_SHARED(surface->eventsLock);
    const bool attached = surface->events != NULL;
    LG_UNLOCK_SHARED(surface->eventsLock);
    if (!attached)
      goto done;

    if (!atomic_load_explicit(
          &transport->sessionValid, memory_order_acquire) ||
        !purespice_hasChannel(PS_CHANNEL_DISPLAY) ||
        !purespice_hasChannel(PS_CHANNEL_CURSOR) ||
        !purespice_connectChannel(PS_CHANNEL_DISPLAY))
      goto done;

    if (!purespice_connectChannel(PS_CHANNEL_CURSOR))
    {
      purespice_disconnectChannel(PS_CHANNEL_DISPLAY);
      goto done;
    }
  }
  else
  {
    if (!purespice_disconnectChannel(PS_CHANNEL_DISPLAY))
      goto done;

    if (!purespice_disconnectChannel(PS_CHANNEL_CURSOR))
    {
      purespice_connectChannel(PS_CHANNEL_DISPLAY);
      goto done;
    }
  }

  surface->active = active;
  result = true;

done:
  LG_UNLOCK(surface->activationLock);
  return result;
}

static const LG_SwSurfaceOps swSurfaceOps =
{
  .attach    = surfaceAttach,
  .detach    = surfaceDetach,
  .setActive = surfaceSetActive,
};

static const LG_VideoOps videoOps =
{
  .name      = "SPICE",
  .type      = LG_VIDEO_TYPE_SW_SURFACE,
  .swSurface = &swSurfaceOps,
};

bool spiceSurface_init(SpiceSurface ** result)
{
  if (!result)
    return false;

  SpiceSurface * surface = calloc(1, sizeof(*surface));
  if (!surface)
    return false;

  LG_RWLOCK_INIT(surface->eventsLock);
  LG_LOCK_INIT(surface->activationLock);
  *result = surface;
  return true;
}

void spiceSurface_free(SpiceSurface ** surface)
{
  if (!surface || !*surface)
    return;

  LG_LOCK_FREE((*surface)->activationLock);
  LG_RWLOCK_FREE((*surface)->eventsLock);
  free(*surface);
  *surface = NULL;
}

const LG_VideoOps * spiceSurface_getVideoOps(void)
{
  return &videoOps;
}

void spiceSurface_sessionStopped(SpiceSurface * surface)
{
  LG_LOCK(surface->activationLock);
  surface->active       = false;
  surface->primaryValid = false;
  LG_UNLOCK(surface->activationLock);
}

void spiceSurface_create(SpiceSurface * surface, unsigned int surfaceId,
    PSSurfaceFormat format, unsigned int width, unsigned int height)
{
  if (surfaceId != 0)
  {
    DEBUG_INFO("Ignoring secondary SPICE surface: id: %u, size: %ux%u",
        surfaceId, width, height);
    return;
  }

  switch (format)
  {
    case PS_SURFACE_FMT_32_xRGB:
    case PS_SURFACE_FMT_32_ARGB:
      break;

    default:
      DEBUG_ERROR("Unsupported primary SPICE surface format: %d", format);
      surface->primaryValid = false;
      return;
  }

  DEBUG_INFO("Create primary SPICE surface: id: %u, size: %ux%u",
      surfaceId, width, height);
  surface->primaryValid = true;

  LG_LOCK_SHARED(surface->eventsLock);
  if (surface->events)
  {
    surface->events->configure(surface->eventOpaque, width, height);
    surface->events->drawFill(
        surface->eventOpaque, 0, 0, width, height, 0);
  }
  LG_UNLOCK_SHARED(surface->eventsLock);
}

void spiceSurface_destroy(SpiceSurface * surface, unsigned int surfaceId)
{
  if (!surface->primaryValid || surfaceId != 0)
  {
    DEBUG_INFO("Ignoring destruction of inactive or secondary SPICE surface %u",
        surfaceId);
    return;
  }

  DEBUG_INFO("Destroy primary SPICE surface %u", surfaceId);
  surface->primaryValid = false;

  LG_LOCK_SHARED(surface->eventsLock);
  if (surface->events)
    surface->events->destroy(surface->eventOpaque);
  LG_UNLOCK_SHARED(surface->eventsLock);
}

void spiceSurface_drawFill(SpiceSurface * surface, unsigned int surfaceId,
    int x, int y, int width, int height, uint32_t color)
{
  if (!surface->primaryValid || surfaceId != 0)
    return;

  LG_LOCK_SHARED(surface->eventsLock);
  if (surface->events)
    surface->events->drawFill(
        surface->eventOpaque, x, y, width, height, color);
  LG_UNLOCK_SHARED(surface->eventsLock);
}

void spiceSurface_drawBitmap(SpiceSurface * surface, unsigned int surfaceId,
    PSBitmapFormat format, bool topDown, int x, int y,
    int width, int height, int stride, void * data)
{
  if (!surface->primaryValid || surfaceId != 0)
    return;

  switch (format)
  {
    case PS_BITMAP_FMT_32BIT:
    case PS_BITMAP_FMT_RGBA:
      break;

    default:
      DEBUG_ERROR("Unsupported SPICE bitmap format: %d", format);
      return;
  }

  LG_LOCK_SHARED(surface->eventsLock);
  if (surface->events)
    surface->events->drawBitmap(surface->eventOpaque, topDown,
        x, y, width, height, stride, data);
  LG_UNLOCK_SHARED(surface->eventsLock);
}

static void pointerEvent(SpiceSurface * surface,
    const LG_TransportPointer * pointer)
{
  LG_LOCK_SHARED(surface->eventsLock);
  if (surface->events)
    surface->events->pointer(surface->eventOpaque, pointer);
  LG_UNLOCK_SHARED(surface->eventsLock);
}

void spiceSurface_setRGBAImage(SpiceSurface * surface,
    int width, int height, int hx, int hy, const void * data)
{
  surface->hotX = hx;
  surface->hotY = hy;

  const uint8_t * rgba = data;
  uint8_t * bgra = malloc((size_t)width * height * 4);
  if (!bgra)
    return;

  for (int i = 0; i < width * height; ++i)
  {
    bgra[i * 4 + 0] = rgba[i * 4 + 2];
    bgra[i * 4 + 1] = rgba[i * 4 + 1];
    bgra[i * 4 + 2] = rgba[i * 4 + 0];
    bgra[i * 4 + 3] = rgba[i * 4 + 3];
  }

  const LG_TransportPointer pointer =
  {
    .flags  = LG_TRANSPORT_POINTER_SHAPE,
    .type   = CURSOR_TYPE_COLOR,
    .hx     = hx,
    .hy     = hy,
    .width  = width,
    .height = height,
    .pitch  = width * 4,
    .shape  = bgra,
  };
  pointerEvent(surface, &pointer);
  free(bgra);
}

void spiceSurface_setMonoImage(SpiceSurface * surface,
    int width, int height, int hx, int hy,
    const void * xorMask, const void * andMask)
{
  surface->hotX = hx;
  surface->hotY = hy;

  const int stride = (width + 7) / 8;
  uint8_t * buffer = malloc((size_t)stride * height * 2);
  if (!buffer)
    return;

  memcpy(buffer, andMask, (size_t)stride * height);
  memcpy(buffer + (size_t)stride * height,
      xorMask, (size_t)stride * height);

  const LG_TransportPointer pointer =
  {
    .flags  = LG_TRANSPORT_POINTER_SHAPE,
    .type   = CURSOR_TYPE_MONOCHROME,
    .hx     = hx,
    .hy     = hy,
    .width  = width,
    .height = height * 2,
    .pitch  = stride,
    .shape  = buffer,
  };
  pointerEvent(surface, &pointer);
  free(buffer);
}

void spiceSurface_setColorImage(SpiceSurface * surface,
    int width, int height, int hx, int hy,
    const void * data, const void * maskData)
{
  surface->hotX = hx;
  surface->hotY = hy;

  const uint8_t * rgba    = data;
  const uint8_t * andMask = maskData;
  const int maskPitch     = (width + 7) / 8;
  uint8_t * bgra          = malloc((size_t)width * height * 4);
  if (!bgra)
    return;

  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
    {
      const int i = y * width + x;
      bgra[i * 4 + 0] = rgba[i * 4 + 2];
      bgra[i * 4 + 1] = rgba[i * 4 + 1];
      bgra[i * 4 + 2] = rgba[i * 4 + 0];
      bgra[i * 4 + 3] = andMask[y * maskPitch + x / 8] &
        (0x80U >> (x % 8)) ? 255 : 0;
    }

  const LG_TransportPointer pointer =
  {
    .flags  = LG_TRANSPORT_POINTER_SHAPE,
    .type   = CURSOR_TYPE_MASKED_COLOR,
    .hx     = hx,
    .hy     = hy,
    .width  = width,
    .height = height,
    .pitch  = width * 4,
    .shape  = bgra,
  };
  pointerEvent(surface, &pointer);
  free(bgra);
}

void spiceSurface_setPointerState(SpiceSurface * surface,
    bool visible, int x, int y)
{
  const LG_TransportPointer pointer =
  {
    .flags = LG_TRANSPORT_POINTER_POSITION |
      LG_TRANSPORT_POINTER_VISIBLE_VALID |
      (visible ? LG_TRANSPORT_POINTER_VISIBLE : 0),
    .x     = x,
    .y     = y,
    .hx    = surface->hotX,
    .hy    = surface->hotY,
  };
  pointerEvent(surface, &pointer);
}
