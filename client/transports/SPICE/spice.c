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
#include "common/option.h"

#if ENABLE_AUDIO
#include "../../src/audio.h"
#endif

#include <purespice.h>

#include <stdlib.h>
#include <string.h>

static void spiceDisconnect(LG_Transport * transport);

static void spiceSetup(void)
{
  const PSInit init =
  {
    .log =
    {
      .info  = debug_info,
      .warn  = debug_warn,
      .error = debug_error,
    },
  };
  purespice_init(&init);
}

static bool spiceCreate(LG_Transport ** result)
{
  if (!result)
    return false;

  LG_Transport * transport = calloc(1, sizeof(*transport));
  if (!transport)
    return false;

  transport->host             = option_get_string("spice", "host");
  transport->port             = option_get_int("spice", "port");
  transport->inputEnabled     = option_get_bool("spice", "input");
  transport->clipboardEnabled = option_get_bool("spice", "clipboard");
  transport->audioEnabled     = option_get_bool("spice", "audio");
  transport->usbAudioEnabled  =
    option_get_bool("spice", "usbAudio");
  transport->audioDebug       = option_get_bool("audio", "debug");

#if ENABLE_AUDIO
  transport->playbackEnabled =
    transport->audioEnabled && lgAudio_supportsPlayback();
  transport->recordEnabled =
    transport->audioEnabled && lgAudio_supportsRecord();
#else
  transport->audioEnabled    = false;
  transport->usbAudioEnabled = false;
#endif

#if !ENABLE_USB_AUDIO
  transport->usbAudioEnabled = false;
#endif

  if (!spiceSurface_init(&transport->surface) ||
      (transport->inputEnabled &&
        !spiceInput_init(&transport->input)) ||
      (transport->clipboardEnabled &&
        !spiceClipboard_init(&transport->clipboard)))
    goto fail;

#if ENABLE_AUDIO
  if (transport->audioEnabled && !spiceAudio_init(&transport->audio))
    goto fail;
#endif

#if ENABLE_USB_AUDIO
  if (transport->audioEnabled && transport->usbAudioEnabled)
  {
    if (!transport->playbackEnabled)
    {
      DEBUG_WARN("USB audio requires a playback backend, using SPICE audio");
      transport->usbAudioEnabled = false;
    }
    else if (!(transport->usbAudio =
          lgaUsb_create(transport->audioDebug)))
    {
      DEBUG_WARN("Failed to initialize USB audio, using SPICE audio");
      transport->usbAudioEnabled = false;
    }
    else
      transport->usbRedir = lgaUsb_redir(transport->usbAudio);
  }
#endif

  transport->connectEvent = lgCreateEvent(false, 0);
  if (!transport->connectEvent)
    goto fail;

  atomic_init(&transport->stop, false);
  atomic_init(&transport->sessionValid, false);

  *result = transport;
  return true;

fail:
  if (transport->connectEvent)
    lgFreeEvent(transport->connectEvent);
#if ENABLE_USB_AUDIO
  lgaUsb_destroy(transport->usbAudio);
#endif
#if ENABLE_AUDIO
  spiceAudio_free(&transport->audio);
#endif
  spiceClipboard_free(&transport->clipboard);
  spiceInput_free(&transport->input);
  spiceSurface_free(&transport->surface);
  free(transport);
  return false;
}

static void spiceDestroy(LG_Transport ** transport)
{
  if (!transport || !*transport)
    return;

  spiceDisconnect(*transport);
  lgFreeEvent((*transport)->connectEvent);
#if ENABLE_USB_AUDIO
  lgaUsb_destroy((*transport)->usbAudio);
#endif
#if ENABLE_AUDIO
  spiceAudio_free(&(*transport)->audio);
#endif
  spiceClipboard_free(&(*transport)->clipboard);
  spiceInput_free(&(*transport)->input);
  spiceSurface_free(&(*transport)->surface);
  free(*transport);
  *transport = NULL;
}

static LG_TransportStatus spiceConnect(LG_Transport * transport,
    LG_TransportSession * session)
{
  if (transport->thread)
    return LG_TRANSPORT_ERROR;

  memset(&transport->session, 0, sizeof(transport->session));
  transport->connectStatus = LG_TRANSPORT_ERROR;
  atomic_store_explicit(&transport->stop, false, memory_order_release);
  atomic_store_explicit(
      &transport->sessionValid, false, memory_order_release);
  lgResetEvent(transport->connectEvent);

  if (!lgCreateThread(
        "spiceProcess", spiceSession_thread, transport, &transport->thread))
    return LG_TRANSPORT_ERROR;

  lgWaitEvent(transport->connectEvent, TIMEOUT_INFINITE);
  if (transport->connectStatus != LG_TRANSPORT_OK)
  {
    lgJoinThread(transport->thread, NULL);
    transport->thread = NULL;
    return transport->connectStatus;
  }

  *session = transport->session;
  return LG_TRANSPORT_OK;
}

static void spiceDisconnect(LG_Transport * transport)
{
  if (!transport->thread)
    return;

  atomic_store_explicit(&transport->stop, true, memory_order_release);
  lgJoinThread(transport->thread, NULL);
  transport->thread = NULL;
}

static bool spiceSessionValid(LG_Transport * transport)
{
  return atomic_load_explicit(
      &transport->sessionValid, memory_order_acquire);
}

static const LG_VideoOps * spiceGetVideoOps(LG_Transport * transport)
{
  (void)transport;
  return spiceSurface_getVideoOps();
}

static const LG_InputOps * spiceGetInputOps(LG_Transport * transport,
    void ** opaque)
{
  *opaque = transport->input;
  return transport->input ? spiceInput_getOps() : NULL;
}

static const LG_AudioOps * spiceGetAudioOps(LG_Transport * transport,
    void ** opaque)
{
#if ENABLE_USB_AUDIO
  if (transport->usbAudio)
  {
    *opaque = transport->usbAudio;
    return &LGA_USB;
  }
#endif

#if ENABLE_AUDIO
  *opaque = transport->audio;
  return transport->audio ? spiceAudio_getOps() : NULL;
#else
  *opaque = NULL;
  return NULL;
#endif
}

static const LG_ClipboardOps * spiceGetClipboardOps(LG_Transport * transport,
    void ** opaque)
{
  *opaque = transport->clipboard;
  return transport->clipboard ? spiceClipboard_getOps() : NULL;
}

static LG_TransportStatus spiceSendControl(LG_Transport * transport,
    const LG_TransportControl * control, LG_TransportControlToken * token)
{
  (void)control;
  (void)token;
  return spiceSessionValid(transport) ?
    LG_TRANSPORT_UNAVAILABLE : LG_TRANSPORT_DISCONNECTED;
}

static LG_TransportStatus spiceControlStatus(LG_Transport * transport,
    LG_TransportControlToken token)
{
  (void)token;
  return spiceSessionValid(transport) ?
    LG_TRANSPORT_UNAVAILABLE : LG_TRANSPORT_DISCONNECTED;
}

const LG_TransportOps LGT_SPICE =
{
  .name            = "spice",
  .setup           = spiceSetup,
  .create          = spiceCreate,
  .destroy         = spiceDestroy,
  .connect         = spiceConnect,
  .disconnect      = spiceDisconnect,
  .sessionValid    = spiceSessionValid,
  .getVideoOps     = spiceGetVideoOps,
  .getInputOps     = spiceGetInputOps,
  .getAudioOps     = spiceGetAudioOps,
  .getClipboardOps = spiceGetClipboardOps,
  .sendControl     = spiceSendControl,
  .controlStatus   = spiceControlStatus,
};
