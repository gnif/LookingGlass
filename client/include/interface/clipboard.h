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

#ifndef _H_LG_CLIENT_CLIPBOARD_INTERFACE_
#define _H_LG_CLIENT_CLIPBOARD_INTERFACE_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum LG_ClipboardData
{
  LG_CLIPBOARD_DATA_TEXT = 0,
  LG_CLIPBOARD_DATA_PNG,
  LG_CLIPBOARD_DATA_BMP,
  LG_CLIPBOARD_DATA_TIFF,
  LG_CLIPBOARD_DATA_JPEG,

  LG_CLIPBOARD_DATA_NONE
}
LG_ClipboardData;

typedef void (*LG_ClipboardReplyFn)(void * opaque,
    LG_ClipboardData type, const uint8_t * data, uint32_t size);

typedef uint64_t LG_ClipboardRequest;

#define LG_CLIPBOARD_REQUEST_INVALID UINT64_C(0)

typedef struct LG_ClipboardStatus
{
  bool     available;
  uint32_t generation;
}
LG_ClipboardStatus;

typedef void (*LG_ClipboardStatusFn)(void * opaque,
    const LG_ClipboardStatus * status);

typedef struct LG_ClipboardEventOps
{
  /* The type list and data are borrowed for the duration of the callback.
   * Event callbacks are always invoked after releasing backend locks. */
  void (*notice)(void * opaque, const LG_ClipboardData types[], size_t count);
  void (*data)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type, const void * data, size_t size);
  void (*release)(void * opaque);
  bool (*request)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type);
}
LG_ClipboardEventOps;

typedef struct LG_ClipboardOps
{
  const char * name;

  /* Operations must fail safely if the remote endpoint disappears. */

  /* Registration must synchronously report the current status after releasing
   * any backend locks. Status callbacks must be serialized and generations
   * must advance whenever availability changes. Passing NULL unregisters the
   * listener and synchronously quiesces its callbacks. Status callbacks must
   * not be delivered from another operation or event callback. */
  void (*setStatusListener)(void * opaque, LG_ClipboardStatusFn callback,
      void * callbackOpaque);

  /* Attach begins serialized event delivery and may synchronously replay the
   * current remote notice. Detach synchronously quiesces event callbacks.
   * Other operations must not deliver event callbacks synchronously. */
  bool (*attach)(void * opaque, const LG_ClipboardEventOps * events,
      void * eventOpaque);
  void (*detach)(void * opaque);

  bool (*release)(void * opaque);
  /* All outbound arrays and data are borrowed only until the call returns. */
  bool (*notifyTypes)(void * opaque, const LG_ClipboardData types[],
      size_t count);
  /* Data is a complete response to a remote request. NONE with no payload
   * reports that the request could not be completed. The provider must finish
   * any transport framing before returning. */
  bool (*data)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type, const void * data, size_t size);
  /* A successful request produces exactly one matching data event unless the
   * provider is detached, becomes unavailable, or publishes a newer notice
   * or release first. */
  bool (*request)(void * opaque, LG_ClipboardRequest request,
      LG_ClipboardData type);
}
LG_ClipboardOps;

#endif
