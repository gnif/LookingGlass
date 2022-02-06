/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

bool waylandActivationInit(void)
{
  if (!wlWm.xdgActivation)
    DEBUG_WARN("xdg_activation_v1 not exported by compositor, will not be able "
               "to request host focus on behalf of guest applications");
  return true;
}

void waylandActivationFree(void)
{
  if (wlWm.xdgActivation)
  {
    xdg_activation_v1_destroy(wlWm.xdgActivation);
  }
}

static void activationTokenDone(void * data,
    struct xdg_activation_token_v1 * xdgToken, const char * token)
{
  xdg_activation_v1_activate(wlWm.xdgActivation, token, wlWm.surface);
  xdg_activation_token_v1_destroy(xdgToken);
}

static const struct xdg_activation_token_v1_listener activationTokenListener = {
  .done = &activationTokenDone,
};

void waylandActivationRequestActivation(void)
{
  if (!wlWm.xdgActivation) return;

  struct xdg_activation_token_v1 * token =
    xdg_activation_v1_get_activation_token(wlWm.xdgActivation);

  if (!token)
  {
    DEBUG_ERROR("failed to retrieve XDG activation token");
    return;
  }

  xdg_activation_token_v1_add_listener(token, &activationTokenListener, NULL);
  xdg_activation_token_v1_set_surface(token, wlWm.surface);
  xdg_activation_token_v1_commit(token);
}
