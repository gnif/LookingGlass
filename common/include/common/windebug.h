/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#include "debug.h"
#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void DebugWinError(const char * file, const unsigned int line, const char * function, const char * desc, HRESULT status);

#define DEBUG_WINERROR(x, y) DebugWinError(STRIPPATH(__FILE__), __LINE__, __FUNCTION__, x, y)

bool IsWindows8();

#ifdef __cplusplus
}
#endif