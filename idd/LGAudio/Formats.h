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

#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>

namespace LGAudio
{
  enum class EndpointKind : UCHAR
  {
    Render,
    Capture
  };

  struct FormatFingerprint
  {
    ULONG  sampleRate;
    ULONG  channelMask;
    USHORT channels;
    USHORT containerBits;
    USHORT validBits;
    USHORT blockAlign;
  };

  constexpr ULONG kSampleRate = 48000;
  constexpr ULONG kSampleRateCount = 6;
  constexpr ULONG kRenderFormatsPerRate = 4;
  constexpr ULONG kCaptureFormatsPerRate = 5;
  constexpr ULONG kRenderFormatCount =
    kSampleRateCount * kRenderFormatsPerRate;
  constexpr ULONG kCaptureFormatCount =
    kSampleRateCount * kCaptureFormatsPerRate;
  constexpr ULONG kMaximumSampleRate = 192000;
  constexpr ULONG kMaximumBlockAlign =
    8 * 3;
  constexpr ULONG kMaximumBytesPerSecond =
    kMaximumSampleRate * kMaximumBlockAlign;

  extern KSDATAFORMAT_WAVEFORMATEXTENSIBLE
    g_renderFormats[kRenderFormatCount];
  extern KSDATAFORMAT_WAVEFORMATEXTENSIBLE
    g_captureFormats[kCaptureFormatCount];

  extern KSDATARANGE_AUDIO g_renderDataRanges[kRenderFormatCount];
  extern KSDATARANGE_AUDIO g_captureDataRanges[kCaptureFormatCount];

  extern PKSDATARANGE g_renderDataRangePointers[kRenderFormatCount];
  extern PKSDATARANGE g_captureDataRangePointers[kCaptureFormatCount];

  /**
   * Validate an instantiated WaveRT format against the endpoint's exact
   * discrete format list.
   */
  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS ValidateFormat(
    _In_ EndpointKind endpoint,
    _In_ const KSDATAFORMAT * dataFormat,
    _Outptr_opt_result_maybenull_
      const KSDATAFORMAT_WAVEFORMATEXTENSIBLE ** matchedFormat);

  /**
   * Select the exact WAVEFORMATEXTENSIBLE result for an IMiniport data-range
   * intersection request.
   */
  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS SelectFormatIntersection(
    _In_ EndpointKind endpoint,
    _In_ const KSDATARANGE * clientDataRange,
    _In_ const KSDATARANGE * driverDataRange,
    _In_ ULONG outputBufferLength,
    _Out_writes_bytes_to_opt_(outputBufferLength, *resultantFormatLength)
      PVOID resultantFormat,
    _Out_ PULONG resultantFormatLength);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS GetFormatFingerprint(
    _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE * dataFormat,
    _Out_ FormatFingerprint * fingerprint);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  bool EqualFormatFingerprint(
    _In_ const FormatFingerprint & left,
    _In_ const FormatFingerprint & right);
}

// The rest of the driver uses these as its shared, unqualified audio types.
using LGAudio::EndpointKind;
using LGAudio::FormatFingerprint;
using LGAudio::EqualFormatFingerprint;
using LGAudio::GetFormatFingerprint;
using LGAudio::SelectFormatIntersection;
using LGAudio::ValidateFormat;
using LGAudio::g_captureDataRangePointers;
using LGAudio::g_captureDataRanges;
using LGAudio::g_captureFormats;
using LGAudio::g_renderDataRangePointers;
using LGAudio::g_renderDataRanges;
using LGAudio::g_renderFormats;
using LGAudio::kCaptureFormatCount;
using LGAudio::kMaximumBlockAlign;
using LGAudio::kMaximumBytesPerSecond;
using LGAudio::kMaximumSampleRate;
using LGAudio::kRenderFormatCount;
using LGAudio::kSampleRate;
using LGAudio::kSampleRateCount;
