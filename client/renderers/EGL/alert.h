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

#pragma once

#include <stdbool.h>

#include "interface/font.h"

typedef struct EGL_Alert EGL_Alert;

bool egl_alert_init(EGL_Alert ** alert, const LG_Font * font, LG_FontObj fontObj);
void egl_alert_free(EGL_Alert ** alert);

void egl_alert_set_color(EGL_Alert * alert, const uint32_t color);
void egl_alert_set_text (EGL_Alert * alert, const char * str);
void egl_alert_render   (EGL_Alert * alert, const float scaleX, const float scaleY);