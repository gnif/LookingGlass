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

#include "spice.h"

#include "common/debug.h"
#include "common/time.h"

#include <purespice.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define USB_REDIR_DISCONNECT_TIMEOUT_US UINT64_C(500000)

static _Atomic(LG_Transport *) l_callbackTarget;

static LG_Transport * callbackTarget(void)
{
  return atomic_load_explicit(&l_callbackTarget, memory_order_acquire);
}

static void ready(void)
{
  LG_Transport * transport = callbackTarget();
  if (!transport)
    return;

  snprintf(transport->session.version,
      sizeof(transport->session.version), "SPICE");
  transport->session.os = LG_TRANSPORT_OS_OTHER;

  purespice_mouseMode(true);

  PSServerInfo info;
  if (purespice_getServerInfo(&info))
  {
    if (info.name)
      snprintf(transport->session.name,
          sizeof(transport->session.name), "%s", info.name);

    for (unsigned int i = 0; i < sizeof(info.uuid); ++i)
      if (info.uuid[i])
      {
        transport->session.uuidValid = true;
        memcpy(transport->session.uuid, info.uuid, sizeof(info.uuid));
        break;
      }

    purespice_freeServerInfo(&info);
  }
  else
    DEBUG_WARN("Failed to obtain SPICE server information");

  if (transport->input)
    spiceInput_setAvailable(transport->input, true);
#if ENABLE_AUDIO
  if (transport->audio && !transport->usbAudioEnabled)
    spiceAudio_setAvailable(transport->audio, true);
#endif

  transport->connectStatus = LG_TRANSPORT_OK;
  atomic_store_explicit(
      &transport->sessionValid, true, memory_order_release);
  lgSignalEvent(transport->connectEvent);
}

static void surfaceCreate(unsigned int surfaceId, PSSurfaceFormat format,
    unsigned int width, unsigned int height)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_create(
        transport->surface, surfaceId, format, width, height);
}

static void surfaceDestroy(unsigned int surfaceId)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_destroy(transport->surface, surfaceId);
}

static void drawFill(unsigned int surfaceId, int x, int y,
    int width, int height, uint32_t color)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_drawFill(
        transport->surface, surfaceId, x, y, width, height, color);
}

static void drawBitmap(unsigned int surfaceId, PSBitmapFormat format,
    bool topDown, int x, int y, int width, int height, int stride, void * data)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_drawBitmap(transport->surface, surfaceId, format,
        topDown, x, y, width, height, stride, data);
}

static void setRGBAImage(int width, int height, int hx, int hy,
    const void * data)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_setRGBAImage(
        transport->surface, width, height, hx, hy, data);
}

static void setMonoImage(int width, int height, int hx, int hy,
    const void * xorMask, const void * andMask)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_setMonoImage(transport->surface,
        width, height, hx, hy, xorMask, andMask);
}

static void setColorImage(int width, int height, int hx, int hy,
    const void * data, const void * maskData)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_setColorImage(transport->surface,
        width, height, hx, hy, data, maskData);
}

static void setPointerState(bool visible, int x, int y)
{
  LG_Transport * transport = callbackTarget();
  if (transport)
    spiceSurface_setPointerState(transport->surface, visible, x, y);
}

#if ENABLE_AUDIO
static void playbackStart(int channels, int sampleRate,
    PSAudioFormat format, uint32_t time)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_playbackStart(
        transport->audio, channels, sampleRate, format, time);
}

static void playbackVolume(int channels, const uint16_t volume[])
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_playbackVolume(transport->audio, channels, volume);
}

static void playbackMute(bool mute)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_playbackMute(transport->audio, mute);
}

static void playbackStop(void)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_playbackStop(transport->audio);
}

static void playbackData(uint8_t * data, size_t size, uint32_t time)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_playbackData(transport->audio, data, size, time);
}

static void recordStart(int channels, int sampleRate, PSAudioFormat format)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_recordStart(
        transport->audio, channels, sampleRate, format);
}

static void recordVolume(int channels, const uint16_t volume[])
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_recordVolume(transport->audio, channels, volume);
}

static void recordMute(bool mute)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_recordMute(transport->audio, mute);
}

static void recordStop(void)
{
  LG_Transport * transport = callbackTarget();
  if (transport && transport->audio)
    spiceAudio_recordStop(transport->audio);
}
#endif

static void disconnectProviders(LG_Transport * transport)
{
  atomic_store_explicit(
      &transport->sessionValid, false, memory_order_release);

  if (transport->input)
    spiceInput_setAvailable(transport->input, false);
#if ENABLE_AUDIO
  if (transport->audio)
    spiceAudio_setAvailable(transport->audio, false);
#endif
  if (transport->clipboard)
    spiceClipboard_setAvailable(transport->clipboard, false);
  spiceSurface_sessionStopped(transport->surface);
}

int spiceSession_thread(void * opaque)
{
  LG_Transport * transport = opaque;
  LG_Transport * expected  = NULL;
  if (!atomic_compare_exchange_strong_explicit(&l_callbackTarget,
        &expected, transport, memory_order_acq_rel, memory_order_acquire))
  {
    DEBUG_ERROR("A SPICE session is already active");
    lgSignalEvent(transport->connectEvent);
    return 0;
  }

  if (transport->clipboard)
    spiceClipboard_setCallbackTarget(transport->clipboard);

  const PSConfig config =
  {
    .host     = transport->host,
    .port     = transport->port,
    .password = "",
    .ready    = ready,
    .inputs =
    {
      .enable      = transport->inputEnabled,
      .autoConnect = true,
    },
    .clipboard =
    {
      .enable  = transport->clipboardEnabled,
      .notice  = spiceClipboard_notice,
      .data    = spiceClipboard_data,
      .release = spiceClipboard_release,
      .request = spiceClipboard_request,
      .status  = spiceClipboard_status,
    },
    .display =
    {
      .enable         = true,
      .autoConnect    = false,
      .surfaceCreate  = surfaceCreate,
      .surfaceDestroy = surfaceDestroy,
      .drawFill       = drawFill,
      .drawBitmap     = drawBitmap,
    },
    .cursor =
    {
      .enable        = true,
      .autoConnect   = false,
      .setRGBAImage  = setRGBAImage,
      .setMonoImage  = setMonoImage,
      .setColorImage = setColorImage,
      .setState      = setPointerState,
    },
#if ENABLE_AUDIO
    .playback =
    {
      .enable      = transport->playbackEnabled &&
        !transport->usbAudioEnabled,
      .autoConnect = true,
      .start       = playbackStart,
      .volume      = playbackVolume,
      .mute        = playbackMute,
      .stop        = playbackStop,
      .data        = playbackData,
    },
    .record =
    {
      .enable      = transport->recordEnabled &&
        !transport->usbAudioEnabled,
      .autoConnect = true,
      .start       = recordStart,
      .volume      = recordVolume,
      .mute        = recordMute,
      .stop        = recordStop,
    },
#endif
#if ENABLE_USB_AUDIO
    .usbRedir =
    {
      .enable      = transport->usbAudio != NULL,
      .autoConnect = false,
      .opaque      = transport->usbRedir,
      .state       = lgUsbRedir_state,
      .data        = lgUsbRedir_data,
    },
#endif
  };

  PSStatus status = PS_STATUS_SHUTDOWN;
  if (!purespice_connect(&config))
  {
    DEBUG_ERROR("Failed to connect to SPICE server");
    goto done;
  }

  transport->connected = true;
  status = PS_STATUS_RUN;
  int processTimeout = 100;

  while (!atomic_load_explicit(&transport->stop, memory_order_acquire))
  {
#if ENABLE_USB_AUDIO
    if (transport->usbRedir && !lgUsbRedir_process(transport->usbRedir))
      DEBUG_WARN("Failed to process USB audio redirection");
    if (transport->usbAudio)
    {
      const uint64_t delay = lgaUsb_processDelayNs(transport->usbAudio);
      if (delay == UINT64_MAX)
        processTimeout = 10;
      else
      {
        const uint64_t timeout = delay / UINT64_C(1000000) +
          (delay % UINT64_C(1000000) != 0);
        processTimeout = (int)min(timeout, UINT64_C(10));
      }
    }
#endif

    status = purespice_process(processTimeout);
    if (status != PS_STATUS_RUN)
    {
      if (status != PS_STATUS_SHUTDOWN)
        DEBUG_ERROR("Failed to process SPICE messages");
      break;
    }
  }

  disconnectProviders(transport);

#if ENABLE_USB_AUDIO
  if (status == PS_STATUS_RUN && transport->usbRedir)
  {
    if (!lgUsbRedir_process(transport->usbRedir))
      DEBUG_WARN("Failed to disconnect USB audio device");
    else
    {
      const uint64_t deadline =
        microtime() + USB_REDIR_DISCONNECT_TIMEOUT_US;
      while (lgUsbRedir_disconnectPending(transport->usbRedir) &&
          microtime() < deadline)
      {
        if (!lgUsbRedir_process(transport->usbRedir))
        {
          DEBUG_WARN("Failed to process USB audio device removal");
          break;
        }
        status = purespice_process(10);
        if (status != PS_STATUS_RUN)
          break;
      }

      if (status == PS_STATUS_RUN &&
          lgUsbRedir_disconnectPending(transport->usbRedir))
        DEBUG_WARN("Timed out disconnecting USB audio device");
    }
  }
#endif

  purespice_disconnect();
  transport->connected = false;

done:
  disconnectProviders(transport);
  if (transport->clipboard)
    spiceClipboard_setCallbackTarget(NULL);

  expected = transport;
  atomic_compare_exchange_strong_explicit(&l_callbackTarget,
      &expected, NULL, memory_order_acq_rel, memory_order_acquire);
  lgSignalEvent(transport->connectEvent);
  return 0;
}
