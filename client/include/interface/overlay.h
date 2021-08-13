/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

struct LG_OverlayOps
{
  /* internal name of the overlay for debugging */
  const char * name;

  /* called very early to allow for option registration, optional */
  void (*earlyInit)(void);

  /* called when the overlay is registered */
  bool (*init)(void ** udata, const void * params);

  /* final free */
  void (*free)(void * udata);

  /* return true if realtime rendering is required when in jitRender mode
   * optional, if omitted assumes false */
  bool (*needs_render)(void * udata, bool interactive);

  /* perform the actual drawing/rendering
   *
   * `interactive` is true if the application is currently in overlay interaction
   * mode.
   *
   * `windowRects` is an array of window rects that were rendered using screen
   * coordinates. Will be `NULL` if the information is not required.
   *
   * `maxRects` is the length of `windowRects`, or 0 if `windowRects` is `NULL`
   *
   * returns the number of rects written to `windowRects`, or -1 if there is not
   * enough room left.
   */
  int (*render)(void * udata, bool interactive, struct Rect * windowRects,
      int maxRects);

  /* TODO: add load/save settings capabillity */
};

#define ASSERT_LG_OVERLAY_VALID(x) \
  DEBUG_ASSERT((x)->name  ); \
  DEBUG_ASSERT((x)->init  ); \
  DEBUG_ASSERT((x)->free  ); \
  DEBUG_ASSERT((x)->render);

#endif
