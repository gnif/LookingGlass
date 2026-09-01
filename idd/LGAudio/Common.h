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

#pragma once

extern "C"
{
#include <ntddk.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>
}

#define LGAUDIO_POOL_TAG 'uAGL'
#define LGAUDIO_RING_TAG 'rAGL'

#define LGAUDIO_TRACE_ERROR(format, ...)                                      \
  KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,                         \
    "LGAudio: " format "\n", __VA_ARGS__))

#define LGAUDIO_TRACE_INFO(format, ...)                                       \
  KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,                          \
    "LGAudio: " format "\n", __VA_ARGS__))

void * __cdecl operator new(size_t size, POOL_TYPE poolType, ULONG tag);
void * __cdecl operator new(size_t size, POOL_FLAGS poolFlags, ULONG tag);
void __cdecl operator delete(void *memory) noexcept;
void __cdecl operator delete(void *memory, size_t size) noexcept;
void __cdecl operator delete(void *memory, POOL_TYPE poolType,
  ULONG tag) noexcept;
void __cdecl operator delete(void *memory, POOL_FLAGS poolFlags,
  ULONG tag) noexcept;

template<typename T>
inline void SafeRelease(T *&object)
{
  if (object)
  {
    object->Release();
    object = nullptr;
  }
}

inline bool GuidEqual(REFGUID left, REFGUID right)
{
  return !!IsEqualGUIDAligned(left, right);
}

constexpr ULONG kRenderWaveHostPin = 0;
constexpr ULONG kRenderWaveBridgePin = 1;
constexpr ULONG kRenderTopologyBridgePin = 0;
constexpr ULONG kRenderTopologyEndpointPin = 1;

constexpr ULONG kCaptureWaveBridgePin = 0;
constexpr ULONG kCaptureWaveHostPin = 1;
constexpr ULONG kCaptureTopologyEndpointPin = 0;
constexpr ULONG kCaptureTopologyBridgePin = 1;

constexpr wchar_t kWaveRenderName[] = L"WaveRender";
constexpr wchar_t kTopologyRenderName[] = L"TopologyRender";
constexpr wchar_t kWaveCaptureName[] = L"WaveCapture";
constexpr wchar_t kTopologyCaptureName[] = L"TopologyCapture";
