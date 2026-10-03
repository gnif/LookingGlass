/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include <initguid.h>

DEFINE_GUID(CLSID_MMDeviceEnumerator,
    0xbcde0395, 0xe52f, 0x467c,
    0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);

DEFINE_GUID(IID_IMMDeviceEnumerator,
    0xa95664d2, 0x9614, 0x4f35,
    0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);

DEFINE_GUID(IID_IAudioClient,
    0x1cb9ad4c, 0xdbfa, 0x4c32,
    0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);

DEFINE_GUID(IID_IAudioRenderClient,
    0xf294acfc, 0x3146, 0x4483,
    0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);

DEFINE_GUID(IID_IAudioCaptureClient,
    0xc8adbd64, 0xe71e, 0x48a0,
    0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17);

DEFINE_GUID(IID_IChannelAudioVolume,
    0x1c158861, 0xb533, 0x4b30,
    0xb1, 0xcf, 0xe8, 0x53, 0xe5, 0x1c, 0x59, 0xb8);

DEFINE_GUID(IID_ISimpleAudioVolume,
    0x87ce5498, 0x68d6, 0x44e5,
    0x92, 0x15, 0x6d, 0xa4, 0x7e, 0xf8, 0x83, 0xd8);
