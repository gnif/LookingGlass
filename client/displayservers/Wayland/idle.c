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

#include "wayland.h"

#include <stdbool.h>
#include <wayland-client.h>

#include "common/debug.h"

bool waylandIdleInit(void)
{
  if (!wlWm.idleInhibitManager)
    DEBUG_WARN("zwp_idle_inhibit_manager_v1 not exported by compositor, will "
               "not be able to suppress idle states");
  return true;
}

void waylandIdleFree(void)
{
  if (wlWm.idleInhibitManager)
  {
    waylandUninhibitIdle();
    zwp_idle_inhibit_manager_v1_destroy(wlWm.idleInhibitManager);
  }
}

void waylandInhibitIdle(void)
{
  if (wlWm.idleInhibitManager && !wlWm.idleInhibitor)
    wlWm.idleInhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
        wlWm.idleInhibitManager, wlWm.surface);
}

void waylandUninhibitIdle(void)
{
  if (wlWm.idleInhibitor)
  {
    zwp_idle_inhibitor_v1_destroy(wlWm.idleInhibitor);
    wlWm.idleInhibitor = NULL;
  }
}
