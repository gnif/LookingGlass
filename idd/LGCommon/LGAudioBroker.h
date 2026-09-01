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

#if defined(_KERNEL_MODE) || defined(_NTDDK_) || defined(_WDMDDK_)
extern "C"
{
#include <ntddk.h>
}
#else
#include <Windows.h>
#include <winioctl.h>
#endif

static constexpr wchar_t LGAUDIO_BROKER_NT_DEVICE_NAME[] =
  L"\\Device\\LookingGlassAudioBroker";
static constexpr wchar_t LGAUDIO_BROKER_DOS_DEVICE_NAME[] =
  L"\\DosDevices\\LookingGlassAudioBroker";
static constexpr wchar_t LGAUDIO_BROKER_USER_DEVICE_PATH[] =
  L"\\\\.\\LookingGlassAudioBroker";

// This GUID identifies the security policy for the named control device. It
// is not a PnP device-interface class.
static const GUID LGAUDIO_BROKER_DEVICE_CLASS_GUID =
  { 0x98f877f1, 0x73d6, 0x4429,
    { 0xb4, 0x90, 0x28, 0xc5, 0x30, 0xd2, 0x38, 0xcd } };

static constexpr UINT32 LGAUDIO_BROKER_ABI_VERSION = 1U;

enum LGAudioBrokerCapability : UINT32
{
  LGAUDIO_BROKER_CAP_CONTROL = 1U << 0,
  // These remain clear until the corresponding data plane is implemented.
  LGAUDIO_BROKER_CAP_RENDER_READ = 1U << 1,
  LGAUDIO_BROKER_CAP_CAPTURE_WRITE = 1U << 2,
  LGAUDIO_BROKER_CAP_CLOCK_CONTROL = 1U << 3
};

enum LGAudioBrokerStateFlag : UINT32
{
  LGAUDIO_BROKER_STATE_ADAPTER_PRESENT = 1U << 0,
  LGAUDIO_BROKER_STATE_BOUND = 1U << 1,
  LGAUDIO_BROKER_STATE_DATA_PLANE_ACTIVE = 1U << 2
};

enum LGAudioBrokerStreamStateValue : UINT32
{
  LGAUDIO_BROKER_STREAM_UNAVAILABLE = 0,
  LGAUDIO_BROKER_STREAM_STOPPED = 1,
  LGAUDIO_BROKER_STREAM_ACQUIRED = 2,
  LGAUDIO_BROKER_STREAM_PAUSED = 3,
  LGAUDIO_BROKER_STREAM_RUNNING = 4
};

static constexpr UINT32 IOCTL_LGAUDIO_BROKER_QUERY_CAPS =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);
static constexpr UINT32 IOCTL_LGAUDIO_BROKER_QUERY_STATE =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);
static constexpr UINT32 IOCTL_LGAUDIO_BROKER_BIND =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);
static constexpr UINT32 IOCTL_LGAUDIO_BROKER_UNBIND =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);
static constexpr UINT32 IOCTL_LGAUDIO_BROKER_QUERY_STATS =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);

#pragma pack(push, 8)

struct LGAudioBrokerHeader
{
  UINT32 size;
  UINT32 version;
};

struct LGAudioBrokerFormat
{
  UINT32 sampleRate;
  UINT32 channelMask;
  UINT16 channels;
  UINT16 containerBits;
  UINT16 validBits;
  UINT16 blockAlign;
};

struct LGAudioBrokerStreamState
{
  UINT64 generation;
  UINT32 state;
  UINT32 flags;
  LGAudioBrokerFormat format;
};

struct LGAudioBrokerCaps
{
  UINT32 size;
  UINT32 version;
  UINT32 minimumVersion;
  UINT32 maximumVersion;
  UINT32 capabilities;
  UINT32 maximumRenderTransfer;
  UINT32 maximumCaptureTransfer;
  UINT32 reserved0;
  UINT64 reserved[4];
};

struct LGAudioBrokerState
{
  UINT32 size;
  UINT32 version;
  UINT32 flags;
  UINT32 reserved0;
  UINT64 adapterGeneration;
  UINT64 eventSequence;
  UINT64 bindingGeneration;
  LGAudioBrokerStreamState render;
  LGAudioBrokerStreamState capture;
};

struct LGAudioBrokerBind
{
  UINT32 size;
  UINT32 version;
  UINT32 flags;
  UINT32 reserved0;
  UINT64 adapterGeneration;
  UINT64 bindingGeneration;
  UINT64 reserved[2];
};

struct LGAudioBrokerStats
{
  UINT32 size;
  UINT32 version;
  UINT64 adapterGeneration;
  UINT64 eventSequence;
  UINT64 bindingGeneration;
  UINT64 bindCount;
  UINT64 unbindCount;
  UINT64 cleanupCount;
  UINT64 rejectedRequestCount;
  UINT64 reserved[2];
};

#pragma pack(pop)

static_assert(sizeof(LGAudioBrokerHeader) == 8,
  "LGAudio broker header layout changed");
static_assert(sizeof(LGAudioBrokerFormat) == 16,
  "LGAudio broker format layout changed");
static_assert(sizeof(LGAudioBrokerStreamState) == 32,
  "LGAudio broker stream-state layout changed");
static_assert(sizeof(LGAudioBrokerCaps) == 64,
  "LGAudio broker capabilities layout changed");
static_assert(sizeof(LGAudioBrokerState) == 104,
  "LGAudio broker state layout changed");
static_assert(sizeof(LGAudioBrokerBind) == 48,
  "LGAudio broker binding layout changed");
static_assert(sizeof(LGAudioBrokerStats) == 80,
  "LGAudio broker statistics layout changed");
