/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#pragma once
#include "lg-renderer.h"

extern const LG_Renderer LGR_EGL;
extern const LG_Renderer LGR_OpenGL;

const LG_Renderer * LG_Renderers[] =
{
  &LGR_EGL,
  &LGR_OpenGL,
  NULL // end of array sentinal
};

#define LG_RENDERER_COUNT ((sizeof(LG_Renderers) / sizeof(LG_Renderer *)) - 1)