/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "interface/clipboard.h"
#include "common/debug.h"

struct state
{
  LG_ClipboardReleaseFn releaseFn;
  LG_ClipboardRequestFn requestFn;
  LG_ClipboardNotifyFn  notifyFn;
  LG_ClipboardDataFn    dataFn;
  LG_ClipboardData      type;
};

static struct state * this = NULL;

static const char * wayland_cb_getName()
{
  return "Wayland";
}

static bool wayland_cb_init(
    SDL_SysWMinfo         * wminfo,
    LG_ClipboardReleaseFn   releaseFn,
    LG_ClipboardNotifyFn    notifyFn,
    LG_ClipboardDataFn      dataFn)
{
  if (wminfo->subsystem != SDL_SYSWM_WAYLAND)
  {
    DEBUG_ERROR("wrong subsystem");
    return false;
  }

  this = (struct state *)malloc(sizeof(struct state));
  memset(this, 0, sizeof(struct state));

  this->releaseFn = releaseFn;
  this->notifyFn  = notifyFn;
  this->dataFn    = dataFn;
  return true;
}

static void wayland_cb_free()
{
  free(this);
  this = NULL;
}

static void wayland_cb_wmevent(SDL_SysWMmsg * msg)
{
}

static void wayland_cb_notice(LG_ClipboardRequestFn requestFn, LG_ClipboardData type)
{
}

static void wayland_cb_release()
{
  this->requestFn = NULL;
}

static void wayland_cb_request(LG_ClipboardData type)
{
}

const LG_Clipboard LGC_Wayland =
{
  .getName = wayland_cb_getName,
  .init    = wayland_cb_init,
  .free    = wayland_cb_free,
  .wmevent = wayland_cb_wmevent,
  .notice  = wayland_cb_notice,
  .release = wayland_cb_release,
  .request = wayland_cb_request
};
