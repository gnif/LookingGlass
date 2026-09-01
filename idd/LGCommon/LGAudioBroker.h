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
// The transfer limits in LGAudioBrokerCaps exclude the packet header.
static constexpr UINT32 LGAUDIO_BROKER_MAX_PAYLOAD_BYTES = 64U * 1024U;

enum LGAudioBrokerCapability : UINT32
{
  LGAUDIO_BROKER_CAP_CONTROL = 1U << 0,
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

enum LGAudioBrokerStreamFlag : UINT32
{
  LGAUDIO_BROKER_STREAM_FORMAT_VALID = 1U << 0
};

enum LGAudioBrokerPacketDirection : UINT32
{
  LGAUDIO_BROKER_PACKET_RENDER = 1,
  LGAUDIO_BROKER_PACKET_CAPTURE = 2
};

enum LGAudioBrokerPacketFlag : UINT32
{
  LGAUDIO_BROKER_PACKET_DISCONTINUITY = 1U << 0
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
  // Changes on format configuration and each transition to STOP.
  UINT64 generation;
  UINT32 state;
  UINT32 flags;
  LGAudioBrokerFormat format;
};

struct LGAudioBrokerPacketHeader
{
  // READ returns this header followed by render payload. WRITE accepts this
  // header followed by capture payload. size is always the header size.
  UINT32 size;
  UINT32 version;
  UINT32 flags;
  UINT32 direction;
  UINT64 adapterGeneration;
  UINT64 bindingGeneration;
  UINT64 streamGeneration;
  // Render sequence is monotonic for the driver lifetime. Capture starts at
  // zero for each binding/stream-generation pair and must be contiguous.
  UINT64 sequence;
  LGAudioBrokerFormat format;
  UINT32 frameCount;
  UINT32 payloadBytes;
  UINT32 reserved[2];
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
  UINT64 renderPacketCount;
  UINT64 renderByteCount;
  UINT64 renderDropBytes;
  UINT64 capturePacketCount;
  UINT64 captureByteCount;
  UINT64 captureDropBytes;
  UINT64 captureUnderrunBytes;
  UINT64 cancelledReadCount;
  UINT64 reserved[4];
};

#pragma pack(pop)

static_assert(sizeof(LGAudioBrokerHeader) == 8,
  "LGAudio broker header layout changed");
static_assert(sizeof(LGAudioBrokerFormat) == 16,
  "LGAudio broker format layout changed");
static_assert(sizeof(LGAudioBrokerStreamState) == 32,
  "LGAudio broker stream-state layout changed");
static_assert(sizeof(LGAudioBrokerPacketHeader) == 80,
  "LGAudio broker packet layout changed");
static_assert(sizeof(LGAudioBrokerCaps) == 64,
  "LGAudio broker capabilities layout changed");
static_assert(sizeof(LGAudioBrokerState) == 104,
  "LGAudio broker state layout changed");
static_assert(sizeof(LGAudioBrokerBind) == 48,
  "LGAudio broker binding layout changed");
static_assert(sizeof(LGAudioBrokerStats) == 160,
  "LGAudio broker statistics layout changed");
static_assert(LGAUDIO_BROKER_MAX_PAYLOAD_BYTES >=
    (192000U / 100U) * 8U * 3U,
  "LGAudio broker payload cannot hold 10 ms at the maximum format");
