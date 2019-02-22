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

#include "lg-clipboard.h"
#include <stdbool.h>

static const char * x11_cb_getName()
{
  return "X11";
}

static bool x11_cb_init()
{
  return true;
}

static void x11_cb_free()
{
}

const LG_Clipboard LGC_X11 =
{
  .getName = x11_cb_getName,
  .init    = x11_cb_init,
  .free    = x11_cb_free
};