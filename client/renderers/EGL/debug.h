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

#include <common/debug.h>

#define EGL_DEBUG_PRINT(type, fmt, ...) do {egl_debug_printf(type " %20s:%-4u | %-30s | " fmt, STRIPPATH(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__);} while (0)
#define EGL_ERROR(fmt, ...) EGL_DEBUG_PRINT("[E]", fmt, ##__VA_ARGS__)

void egl_debug_printf(char * format, ...);