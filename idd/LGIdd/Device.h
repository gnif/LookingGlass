/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#pragma once

#include "public.h"

#include <Windows.h>

#if 1
const DWORD DisplayModes[][3] =
{
  {7680, 4800, 120}, {7680, 4320, 120}, {6016, 3384, 120}, {5760, 3600, 120},
  {5760, 3240, 120}, {5120, 2800, 120}, {4096, 2560, 120}, {4096, 2304, 120},
  {3840, 2400, 120}, {3840, 2160, 120}, {3200, 2400, 120}, {3200, 1800, 120},
  {3008, 1692, 120}, {2880, 1800, 120}, {2880, 1620, 120}, {2560, 1600, 120},
  {2560, 1440, 120}, {1920, 1440, 120}, {1920, 1200, 120}, {1920, 1080, 120},
  {1600, 1200, 120}, {1600, 1024, 120}, {1600, 1050, 120}, {1600, 900 , 120},
  {1440, 900 , 120}, {1400, 1050, 120}, {1366, 768 , 120}, {1360, 768 , 120},
  {1280, 1024, 120}, {1280, 960 , 120}, {1280, 800 , 120}, {1280, 768 , 120},
  {1280, 720 , 120}, {1280, 600 , 120}, {1152, 864 , 120}, {1024, 768 , 120},
  {800 , 600 , 120}, {640 , 480 , 120}
};

const DWORD PreferredDisplayMode = 19;
#else
const DWORD DisplayModes[][3] =
{
  { 2560, 1440, 144 },
  { 1920, 1080,  60 },
  { 1024,  768,  60 },
};

const DWORD PreferredDisplayMode = 0;
#endif

typedef struct _DEVICE_CONTEXT
{
  ULONG PrivateDeviceData;  // just a placeholder
}
DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

NTSTATUS LGIddCreateDevice(_Inout_ PWDFDEVICE_INIT deviceInit);
