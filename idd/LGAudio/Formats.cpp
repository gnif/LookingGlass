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

#include "Formats.h"

namespace LGAudio
{
  namespace
  {
    constexpr USHORT kExtensibleExtraSize = static_cast<USHORT>(
      sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));

    bool IsAudioPcmWaveFormat(_In_ const KSDATAFORMAT * format)
    {
      return
        IsEqualGUIDAligned(format->MajorFormat,
          KSDATAFORMAT_TYPE_AUDIO) &&
        IsEqualGUIDAligned(format->SubFormat,
          KSDATAFORMAT_SUBTYPE_PCM) &&
        IsEqualGUIDAligned(format->Specifier,
          KSDATAFORMAT_SPECIFIER_WAVEFORMATEX);
    }

    bool WaveFormatCoreMatches(
      _In_ const WAVEFORMATEX & candidate,
      _In_ const WAVEFORMATEX & requested)
    {
      return
        candidate.nChannels == requested.nChannels &&
        candidate.nSamplesPerSec == requested.nSamplesPerSec &&
        candidate.nAvgBytesPerSec == requested.nAvgBytesPerSec &&
        candidate.nBlockAlign == requested.nBlockAlign &&
        candidate.wBitsPerSample == requested.wBitsPerSample;
    }

    bool ExtensibleFormatMatches(
      _In_ const WAVEFORMATEXTENSIBLE & candidate,
      _In_ const WAVEFORMATEXTENSIBLE & requested)
    {
      return
        WaveFormatCoreMatches(candidate.Format, requested.Format) &&
        candidate.Samples.wValidBitsPerSample ==
          requested.Samples.wValidBitsPerSample &&
        candidate.dwChannelMask == requested.dwChannelMask &&
        IsEqualGUIDAligned(candidate.SubFormat, requested.SubFormat);
    }

    void GetFormatList(
      _In_ EndpointKind endpoint,
      _Outptr_result_buffer_(*count)
        KSDATAFORMAT_WAVEFORMATEXTENSIBLE ** formats,
      _Out_ PULONG count)
    {
      if (endpoint == EndpointKind::Render)
      {
        *formats = g_renderFormats;
        *count = kRenderFormatCount;
      }
      else
      {
        *formats = g_captureFormats;
        *count = kCaptureFormatCount;
      }
    }

    void GetDataRangeList(
      _In_ EndpointKind endpoint,
      _Outptr_result_buffer_(*count) KSDATARANGE_AUDIO ** ranges,
      _Out_ PULONG count)
    {
      if (endpoint == EndpointKind::Render)
      {
        *ranges = g_renderDataRanges;
        *count = kRenderFormatCount;
      }
      else
      {
        *ranges = g_captureDataRanges;
        *count = kCaptureFormatCount;
      }
    }

    bool AudioRangeContainsFormat(
      _In_ const KSDATARANGE_AUDIO & range,
      _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE & format)
    {
      const WAVEFORMATEX & wave = format.WaveFormatExt.Format;

      return
        range.MaximumChannels == wave.nChannels &&
        range.MinimumBitsPerSample <= wave.wBitsPerSample &&
        range.MaximumBitsPerSample >= wave.wBitsPerSample &&
        range.MinimumSampleFrequency <= wave.nSamplesPerSec &&
        range.MaximumSampleFrequency >= wave.nSamplesPerSec;
    }

    bool DataRangeIdentifiesFormat(
      _In_ const KSDATARANGE_AUDIO & range,
      _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE & format)
    {
      const WAVEFORMATEX & wave = format.WaveFormatExt.Format;

      return
        range.MaximumChannels == wave.nChannels &&
        range.MinimumBitsPerSample == wave.wBitsPerSample &&
        range.MaximumBitsPerSample == wave.wBitsPerSample &&
        range.MinimumSampleFrequency == wave.nSamplesPerSec &&
        range.MaximumSampleFrequency == wave.nSamplesPerSec;
    }
  }

#define LG_AUDIO_FORMAT(                                                      \
  sampleRate_, channels_, containerBits_, validBits_, mask_)                 \
  {                                                                          \
    {                                                                        \
      sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),                             \
      0,                                                                     \
      0,                                                                     \
      0,                                                                     \
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                 \
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),                                \
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                      \
    },                                                                       \
    {                                                                        \
      {                                                                      \
        WAVE_FORMAT_EXTENSIBLE,                                              \
        channels_,                                                           \
        sampleRate_,                                                         \
        sampleRate_ * channels_ * (containerBits_ / 8),                      \
        channels_ * (containerBits_ / 8),                                    \
        containerBits_,                                                      \
        kExtensibleExtraSize                                                 \
      },                                                                     \
      validBits_,                                                            \
      mask_,                                                                 \
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)                                 \
    }                                                                        \
  }

#define LG_RENDER_FORMATS(sampleRate_)                                       \
  LG_AUDIO_FORMAT(sampleRate_, 2, 16, 16, KSAUDIO_SPEAKER_STEREO),          \
  LG_AUDIO_FORMAT(sampleRate_, 2, 24, 24, KSAUDIO_SPEAKER_STEREO),          \
  LG_AUDIO_FORMAT(sampleRate_, 6, 24, 24,                                   \
    KSAUDIO_SPEAKER_5POINT1_SURROUND),                                      \
  LG_AUDIO_FORMAT(sampleRate_, 8, 24, 24,                                   \
    KSAUDIO_SPEAKER_7POINT1_SURROUND)

#define LG_CAPTURE_FORMATS(sampleRate_)                                      \
  LG_AUDIO_FORMAT(sampleRate_, 1, 16, 16, KSAUDIO_SPEAKER_MONO),            \
  LG_RENDER_FORMATS(sampleRate_)

  KSDATAFORMAT_WAVEFORMATEXTENSIBLE
    g_renderFormats[kRenderFormatCount] =
  {
    LG_RENDER_FORMATS(48000),
    LG_RENDER_FORMATS(44100),
    LG_RENDER_FORMATS(88200),
    LG_RENDER_FORMATS(96000),
    LG_RENDER_FORMATS(176400),
    LG_RENDER_FORMATS(192000)
  };

  KSDATAFORMAT_WAVEFORMATEXTENSIBLE
    g_captureFormats[kCaptureFormatCount] =
  {
    LG_CAPTURE_FORMATS(48000),
    LG_CAPTURE_FORMATS(44100),
    LG_CAPTURE_FORMATS(88200),
    LG_CAPTURE_FORMATS(96000),
    LG_CAPTURE_FORMATS(176400),
    LG_CAPTURE_FORMATS(192000)
  };

#undef LG_CAPTURE_FORMATS
#undef LG_RENDER_FORMATS
#undef LG_AUDIO_FORMAT

#define LG_AUDIO_RANGE(sampleRate_, channels_, containerBits_)               \
  {                                                                          \
    {                                                                        \
      sizeof(KSDATARANGE_AUDIO),                                             \
      0,                                                                     \
      0,                                                                     \
      0,                                                                     \
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                 \
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),                                \
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                      \
    },                                                                       \
    channels_,                                                               \
    containerBits_,                                                          \
    containerBits_,                                                          \
    sampleRate_,                                                             \
    sampleRate_                                                              \
  }

#define LG_RENDER_RANGES(sampleRate_)                                        \
  LG_AUDIO_RANGE(sampleRate_, 2, 16),                                       \
  LG_AUDIO_RANGE(sampleRate_, 2, 24),                                       \
  LG_AUDIO_RANGE(sampleRate_, 6, 24),                                       \
  LG_AUDIO_RANGE(sampleRate_, 8, 24)

#define LG_CAPTURE_RANGES(sampleRate_)                                       \
  LG_AUDIO_RANGE(sampleRate_, 1, 16),                                       \
  LG_RENDER_RANGES(sampleRate_)

  KSDATARANGE_AUDIO g_renderDataRanges[kRenderFormatCount] =
  {
    LG_RENDER_RANGES(48000),
    LG_RENDER_RANGES(44100),
    LG_RENDER_RANGES(88200),
    LG_RENDER_RANGES(96000),
    LG_RENDER_RANGES(176400),
    LG_RENDER_RANGES(192000)
  };

  KSDATARANGE_AUDIO g_captureDataRanges[kCaptureFormatCount] =
  {
    LG_CAPTURE_RANGES(48000),
    LG_CAPTURE_RANGES(44100),
    LG_CAPTURE_RANGES(88200),
    LG_CAPTURE_RANGES(96000),
    LG_CAPTURE_RANGES(176400),
    LG_CAPTURE_RANGES(192000)
  };

#undef LG_CAPTURE_RANGES
#undef LG_RENDER_RANGES
#undef LG_AUDIO_RANGE

#define LG_RENDER_RANGE_POINTERS(base_)                                     \
  (PKSDATARANGE)&g_renderDataRanges[(base_)],                               \
  (PKSDATARANGE)&g_renderDataRanges[(base_) + 1],                           \
  (PKSDATARANGE)&g_renderDataRanges[(base_) + 2],                           \
  (PKSDATARANGE)&g_renderDataRanges[(base_) + 3]

#define LG_CAPTURE_RANGE_POINTERS(base_)                                    \
  (PKSDATARANGE)&g_captureDataRanges[(base_)],                              \
  (PKSDATARANGE)&g_captureDataRanges[(base_) + 1],                          \
  (PKSDATARANGE)&g_captureDataRanges[(base_) + 2],                          \
  (PKSDATARANGE)&g_captureDataRanges[(base_) + 3],                          \
  (PKSDATARANGE)&g_captureDataRanges[(base_) + 4]

  PKSDATARANGE g_renderDataRangePointers[kRenderFormatCount] =
  {
    LG_RENDER_RANGE_POINTERS(0),
    LG_RENDER_RANGE_POINTERS(4),
    LG_RENDER_RANGE_POINTERS(8),
    LG_RENDER_RANGE_POINTERS(12),
    LG_RENDER_RANGE_POINTERS(16),
    LG_RENDER_RANGE_POINTERS(20)
  };

  PKSDATARANGE g_captureDataRangePointers[kCaptureFormatCount] =
  {
    LG_CAPTURE_RANGE_POINTERS(0),
    LG_CAPTURE_RANGE_POINTERS(5),
    LG_CAPTURE_RANGE_POINTERS(10),
    LG_CAPTURE_RANGE_POINTERS(15),
    LG_CAPTURE_RANGE_POINTERS(20),
    LG_CAPTURE_RANGE_POINTERS(25)
  };

#undef LG_CAPTURE_RANGE_POINTERS
#undef LG_RENDER_RANGE_POINTERS

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS ValidateFormat(
    _In_ EndpointKind endpoint,
    _In_ const KSDATAFORMAT * dataFormat,
    _Outptr_opt_result_maybenull_
      const KSDATAFORMAT_WAVEFORMATEXTENSIBLE ** matchedFormat)
  {
    if (matchedFormat)
      *matchedFormat = nullptr;

    if (!dataFormat ||
        dataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsAudioPcmWaveFormat(dataFormat))
      return STATUS_NO_MATCH;

    const WAVEFORMATEX * requested =
      reinterpret_cast<const WAVEFORMATEX *>(dataFormat + 1);

    KSDATAFORMAT_WAVEFORMATEXTENSIBLE * formats;
    ULONG count;
    GetFormatList(endpoint, &formats, &count);

    for (ULONG i = 0; i < count; ++i)
    {
      const KSDATAFORMAT_WAVEFORMATEXTENSIBLE & candidate = formats[i];
      const WAVEFORMATEXTENSIBLE & candidateWave = candidate.WaveFormatExt;

      if (!WaveFormatCoreMatches(candidateWave.Format, *requested))
        continue;

      if (requested->wFormatTag == WAVE_FORMAT_PCM)
      {
        // A stand-alone WAVEFORMATEX is unambiguous only for these legacy
        // mono/stereo, 16-bit formats.
        if (candidateWave.Format.wBitsPerSample != 16 ||
            candidateWave.Samples.wValidBitsPerSample != 16 ||
            candidateWave.Format.nChannels > 2)
          continue;
      }
      else if (requested->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
      {
        if (dataFormat->FormatSize <
              sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
            requested->cbSize < kExtensibleExtraSize)
          continue;

        const WAVEFORMATEXTENSIBLE * requestedExtensible =
          reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(requested);
        if (!ExtensibleFormatMatches(candidateWave, *requestedExtensible))
          continue;
      }
      else
        continue;

      if (matchedFormat)
        *matchedFormat = &candidate;
      return STATUS_SUCCESS;
    }

    return STATUS_NO_MATCH;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS SelectFormatIntersection(
    _In_ EndpointKind endpoint,
    _In_ const KSDATARANGE * clientDataRange,
    _In_ const KSDATARANGE * driverDataRange,
    _In_ ULONG outputBufferLength,
    _Out_writes_bytes_to_opt_(outputBufferLength, *resultantFormatLength)
      PVOID resultantFormat,
    _Out_ PULONG resultantFormatLength)
  {
    if (!clientDataRange || !driverDataRange || !resultantFormatLength)
      return STATUS_INVALID_PARAMETER;

    *resultantFormatLength = 0;

    if (clientDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        driverDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        !IsAudioPcmWaveFormat(clientDataRange) ||
        !IsAudioPcmWaveFormat(driverDataRange))
      return STATUS_NO_MATCH;

    const KSDATARANGE_AUDIO & client =
      *reinterpret_cast<const KSDATARANGE_AUDIO *>(clientDataRange);
    const KSDATARANGE_AUDIO & driver =
      *reinterpret_cast<const KSDATARANGE_AUDIO *>(driverDataRange);

    KSDATAFORMAT_WAVEFORMATEXTENSIBLE * formats;
    KSDATARANGE_AUDIO * ranges;
    ULONG formatCount;
    ULONG rangeCount;
    GetFormatList(endpoint, &formats, &formatCount);
    GetDataRangeList(endpoint, &ranges, &rangeCount);

    if (formatCount != rangeCount)
      return STATUS_INTERNAL_ERROR;

    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE * selected = nullptr;
    for (ULONG i = 0; i < formatCount; ++i)
    {
      if (!DataRangeIdentifiesFormat(driver, formats[i]) ||
          !DataRangeIdentifiesFormat(ranges[i], formats[i]) ||
          !AudioRangeContainsFormat(client, formats[i]))
        continue;

      selected = &formats[i];
      break;
    }

    if (!selected)
      return STATUS_NO_MATCH;

    constexpr ULONG requiredSize =
      sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *resultantFormatLength = requiredSize;

    if (outputBufferLength == 0)
      return STATUS_BUFFER_OVERFLOW;
    if (outputBufferLength < requiredSize)
      return STATUS_BUFFER_TOO_SMALL;
    if (!resultantFormat)
      return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(resultantFormat, selected, requiredSize);
    return STATUS_SUCCESS;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS GetFormatFingerprint(
    _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE * dataFormat,
    _Out_ FormatFingerprint * fingerprint)
  {
    if (!dataFormat || !fingerprint)
      return STATUS_INVALID_PARAMETER;

    const WAVEFORMATEXTENSIBLE & wave = dataFormat->WaveFormatExt;
    if (wave.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        wave.Format.cbSize < kExtensibleExtraSize ||
        wave.Format.nChannels == 0 ||
        wave.Format.nBlockAlign == 0 ||
        wave.Format.wBitsPerSample == 0 ||
        wave.Samples.wValidBitsPerSample == 0 ||
        wave.Samples.wValidBitsPerSample >
          wave.Format.wBitsPerSample ||
        !IsAudioPcmWaveFormat(&dataFormat->DataFormat) ||
        !IsEqualGUIDAligned(wave.SubFormat,
          KSDATAFORMAT_SUBTYPE_PCM))
      return STATUS_INVALID_PARAMETER;

    fingerprint->sampleRate = wave.Format.nSamplesPerSec;
    fingerprint->channelMask = wave.dwChannelMask;
    fingerprint->channels = wave.Format.nChannels;
    fingerprint->containerBits = wave.Format.wBitsPerSample;
    fingerprint->validBits = wave.Samples.wValidBitsPerSample;
    fingerprint->blockAlign = wave.Format.nBlockAlign;
    return STATUS_SUCCESS;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  bool EqualFormatFingerprint(
    _In_ const FormatFingerprint & left,
    _In_ const FormatFingerprint & right)
  {
    return
      left.sampleRate == right.sampleRate &&
      left.channelMask == right.channelMask &&
      left.channels == right.channels &&
      left.containerBits == right.containerBits &&
      left.validBits == right.validBits &&
      left.blockAlign == right.blockAlign;
  }
}
