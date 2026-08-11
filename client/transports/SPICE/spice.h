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

#ifndef _H_LG_CLIENT_TRANSPORT_SPICE_
#define _H_LG_CLIENT_TRANSPORT_SPICE_

#include "interface/transport.h"

#include "clipboard.h"
#include "input.h"
#include "surface.h"

#if ENABLE_AUDIO
#include "audio.h"
#endif

#if ENABLE_USB_AUDIO
#include "audio_usb.h"
#endif

#include "common/event.h"
#include "common/thread.h"

#include <stdatomic.h>

struct LG_Transport
{
  const char * host;
  unsigned int port;
  bool inputEnabled;
  bool clipboardEnabled;
  bool audioEnabled;
  bool usbAudioEnabled;
  bool playbackEnabled;
  bool recordEnabled;
  bool audioDebug;

  SpiceInput     * input;
  SpiceClipboard * clipboard;
  SpiceSurface   * surface;
#if ENABLE_AUDIO
  SpiceAudio     * audio;
#endif
#if ENABLE_USB_AUDIO
  LGA_USBState   * usbAudio;
  LG_USBRedir    * usbRedir;
#endif

  LGThread * thread;
  LGEvent  * connectEvent;
  atomic_bool stop;
  atomic_bool sessionValid;
  bool connected;
  LG_TransportStatus connectStatus;
  LG_TransportSession session;
};

int spiceSession_thread(void * opaque);

#endif
