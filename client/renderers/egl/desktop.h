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

#include <stdbool.h>

#include "lg-renderer.h"

typedef struct EGL_Desktop EGL_Desktop;

bool egl_desktop_init(EGL_Desktop ** desktop);
void egl_desktop_free(EGL_Desktop ** desktop);

bool egl_desktop_prepare_update(EGL_Desktop * desktop, const bool sourceChanged, const LG_RendererFormat format, const uint8_t * data);
bool egl_desktop_perform_update(EGL_Desktop * desktop, const bool sourceChanged);
void egl_desktop_render(EGL_Desktop * desktop, const float x, const float y, const float scaleX, const float scaleY);