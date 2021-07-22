/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#ifndef _H_I_OVERLAY_
#define _H_I_OVERLAY_

#include <stdbool.h>
#include "common/types.h"

enum LG_OverlayFlags
{
  /* requires mouse interaction */
  LG_OVERLAY_INTERACTIVE = (1U << 1U),
};

struct LG_OverlayOps
{
  /* internal name of the overlay for debugging */
  const char * name;

  /* called when the overlay is registered */
  bool (*init)(void ** udata, void * params);

  /* final free */
  void (*free)(void * udata);

  /* general state flags, may be changed at any time */
  enum LG_OverlayFlags flags;

  /* get the number of windows that will be rendered when `render` is called
   *
   *`interactive` is true if the application is currently in overlay interaction
   * mode
   */
  int (*getWindowCount)(void * udata, bool interactive);

  /* perform the actual drawing/rendering
   *
   * `interactive` is true if the application is currently in overlay interaction
   * mode.
   *
   * The caller provides `windowRects` to be populated by the callee and is sized
   * according to the return value of `getWindowCount`
   */
  void (*render)(void * udata, bool interactive, struct Rect windowRects[]);

  /* TODO: add load/save settings capabillity */
};

#define ASSERT_LG_OVERLAY_VALID(x) \
  assert((x)->name          ); \
  assert((x)->init          ); \
  assert((x)->free          ); \
  assert((x)->getWindowCount); \
  assert((x)->render        );

#endif
