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

    bool IsSupportedSubFormat(_In_ const GUID & subFormat)
    {
      return
        IsEqualGUIDAligned(subFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        IsEqualGUIDAligned(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    bool IsAudioWaveFormat(_In_ const KSDATAFORMAT * format)
    {
      return
        IsEqualGUIDAligned(format->MajorFormat,
          KSDATAFORMAT_TYPE_AUDIO) &&
        IsSupportedSubFormat(format->SubFormat) &&
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

    bool AudioRangeContainsFormat(
      _In_ const KSDATARANGE_AUDIO & range,
      _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE & format)
    {
      const WAVEFORMATEX & wave = format.WaveFormatExt.Format;

      return
        IsEqualGUIDAligned(range.DataRange.SubFormat,
          format.DataFormat.SubFormat) &&
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
        IsEqualGUIDAligned(range.DataRange.SubFormat,
          format.DataFormat.SubFormat) &&
        range.MaximumChannels == wave.nChannels &&
        range.MinimumBitsPerSample == wave.wBitsPerSample &&
        range.MaximumBitsPerSample == wave.wBitsPerSample &&
        range.MinimumSampleFrequency == wave.nSamplesPerSec &&
        range.MaximumSampleFrequency == wave.nSamplesPerSec;
    }

    SampleFormat GetSampleFormat(
      _In_ const WAVEFORMATEXTENSIBLE & wave)
    {
      if (IsEqualGUIDAligned(wave.SubFormat,
          KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
      {
        return wave.Format.wBitsPerSample == 32 &&
          wave.Samples.wValidBitsPerSample == 32 ?
          SampleFormat::F32LE : SampleFormat::Unknown;
      }

      if (!IsEqualGUIDAligned(wave.SubFormat,
          KSDATAFORMAT_SUBTYPE_PCM) ||
          wave.Format.wBitsPerSample !=
            wave.Samples.wValidBitsPerSample)
        return SampleFormat::Unknown;

      switch (wave.Format.wBitsPerSample)
      {
        case 16:
          return SampleFormat::S16LE;
        case 24:
          return SampleFormat::S24LE;
        case 32:
          return SampleFormat::S32LE;
        default:
          return SampleFormat::Unknown;
      }
    }
  }

#define LG_AUDIO_FORMAT(                                                      \
  sampleRate_, channels_, bits_, mask_, subType_)                            \
  {                                                                          \
    {                                                                        \
      sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),                             \
      0,                                                                     \
      0,                                                                     \
      0,                                                                     \
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                 \
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_##subType_),                        \
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                      \
    },                                                                       \
    {                                                                        \
      {                                                                      \
        WAVE_FORMAT_EXTENSIBLE,                                              \
        channels_,                                                           \
        sampleRate_,                                                         \
        sampleRate_ * channels_ * (bits_ / 8),                               \
        channels_ * (bits_ / 8),                                             \
        bits_,                                                               \
        kExtensibleExtraSize                                                 \
      },                                                                     \
      bits_,                                                                 \
      mask_,                                                                 \
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_##subType_)                         \
    }                                                                        \
  }

#define LG_RENDER_LAYOUTS(sampleRate_, bits_, subType_)                      \
  LG_AUDIO_FORMAT(sampleRate_, 2, bits_, KSAUDIO_SPEAKER_STEREO,            \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 1, bits_, KSAUDIO_SPEAKER_MONO,              \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 4, bits_, KSAUDIO_SPEAKER_QUAD,              \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 4, bits_, KSAUDIO_SPEAKER_SURROUND,          \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 6, bits_,                                    \
    KSAUDIO_SPEAKER_5POINT1_SURROUND, subType_),                            \
  LG_AUDIO_FORMAT(sampleRate_, 8, bits_,                                    \
    KSAUDIO_SPEAKER_7POINT1_SURROUND, subType_)

#define LG_CAPTURE_LAYOUTS(sampleRate_, bits_, subType_)                     \
  LG_AUDIO_FORMAT(sampleRate_, 1, bits_, KSAUDIO_SPEAKER_MONO,              \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 2, bits_, KSAUDIO_SPEAKER_STEREO,            \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 4, bits_, KSAUDIO_SPEAKER_QUAD,              \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 4, bits_, KSAUDIO_SPEAKER_SURROUND,          \
    subType_),                                                               \
  LG_AUDIO_FORMAT(sampleRate_, 6, bits_,                                    \
    KSAUDIO_SPEAKER_5POINT1_SURROUND, subType_),                            \
  LG_AUDIO_FORMAT(sampleRate_, 8, bits_,                                    \
    KSAUDIO_SPEAKER_7POINT1_SURROUND, subType_)

#define LG_RENDER_FORMATS(sampleRate_)                                       \
  LG_RENDER_LAYOUTS(sampleRate_, 16, PCM),                                  \
  LG_RENDER_LAYOUTS(sampleRate_, 24, PCM),                                  \
  LG_RENDER_LAYOUTS(sampleRate_, 32, PCM),                                  \
  LG_RENDER_LAYOUTS(sampleRate_, 32, IEEE_FLOAT)

#define LG_CAPTURE_FORMATS(sampleRate_)                                      \
  LG_CAPTURE_LAYOUTS(sampleRate_, 16, PCM),                                 \
  LG_CAPTURE_LAYOUTS(sampleRate_, 24, PCM),                                 \
  LG_CAPTURE_LAYOUTS(sampleRate_, 32, PCM),                                 \
  LG_CAPTURE_LAYOUTS(sampleRate_, 32, IEEE_FLOAT)

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
#undef LG_CAPTURE_LAYOUTS
#undef LG_RENDER_LAYOUTS
#undef LG_AUDIO_FORMAT

#define LG_AUDIO_RANGE(sampleRate_, channels_, bits_, subType_)              \
  {                                                                          \
    {                                                                        \
      sizeof(KSDATARANGE_AUDIO),                                             \
      0,                                                                     \
      0,                                                                     \
      0,                                                                     \
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                 \
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_##subType_),                        \
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                      \
    },                                                                       \
    channels_,                                                               \
    bits_,                                                                   \
    bits_,                                                                   \
    sampleRate_,                                                             \
    sampleRate_                                                              \
  }

#define LG_RENDER_LAYOUT_RANGES(sampleRate_, bits_, subType_)                \
  LG_AUDIO_RANGE(sampleRate_, 2, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 1, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 4, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 6, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 8, bits_, subType_)

#define LG_CAPTURE_LAYOUT_RANGES(sampleRate_, bits_, subType_)               \
  LG_AUDIO_RANGE(sampleRate_, 1, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 2, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 4, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 6, bits_, subType_),                          \
  LG_AUDIO_RANGE(sampleRate_, 8, bits_, subType_)

#define LG_RENDER_RANGES(sampleRate_)                                        \
  LG_RENDER_LAYOUT_RANGES(sampleRate_, 16, PCM),                            \
  LG_RENDER_LAYOUT_RANGES(sampleRate_, 24, PCM),                            \
  LG_RENDER_LAYOUT_RANGES(sampleRate_, 32, PCM),                            \
  LG_RENDER_LAYOUT_RANGES(sampleRate_, 32, IEEE_FLOAT)

#define LG_CAPTURE_RANGES(sampleRate_)                                       \
  LG_CAPTURE_LAYOUT_RANGES(sampleRate_, 16, PCM),                           \
  LG_CAPTURE_LAYOUT_RANGES(sampleRate_, 24, PCM),                           \
  LG_CAPTURE_LAYOUT_RANGES(sampleRate_, 32, PCM),                           \
  LG_CAPTURE_LAYOUT_RANGES(sampleRate_, 32, IEEE_FLOAT)

  KSDATARANGE_AUDIO g_renderDataRanges[kRenderDataRangeCount] =
  {
    LG_RENDER_RANGES(48000),
    LG_RENDER_RANGES(44100),
    LG_RENDER_RANGES(88200),
    LG_RENDER_RANGES(96000),
    LG_RENDER_RANGES(176400),
    LG_RENDER_RANGES(192000)
  };

  KSDATARANGE_AUDIO g_captureDataRanges[kCaptureDataRangeCount] =
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
#undef LG_CAPTURE_LAYOUT_RANGES
#undef LG_RENDER_LAYOUT_RANGES
#undef LG_AUDIO_RANGE

#define LG_RANGE_POINTERS_5(ranges_, base_)                                 \
  (PKSDATARANGE)&ranges_[(base_)],                                          \
  (PKSDATARANGE)&ranges_[(base_) + 1],                                      \
  (PKSDATARANGE)&ranges_[(base_) + 2],                                      \
  (PKSDATARANGE)&ranges_[(base_) + 3],                                      \
  (PKSDATARANGE)&ranges_[(base_) + 4]

#define LG_RANGE_POINTERS(ranges_, base_)                                   \
  LG_RANGE_POINTERS_5(ranges_, base_),                                      \
  LG_RANGE_POINTERS_5(ranges_, (base_) + 5),                                \
  LG_RANGE_POINTERS_5(ranges_, (base_) + 10),                               \
  LG_RANGE_POINTERS_5(ranges_, (base_) + 15)

  PKSDATARANGE g_renderDataRangePointers[kRenderDataRangeCount] =
  {
    LG_RANGE_POINTERS(g_renderDataRanges, 0),
    LG_RANGE_POINTERS(g_renderDataRanges, 20),
    LG_RANGE_POINTERS(g_renderDataRanges, 40),
    LG_RANGE_POINTERS(g_renderDataRanges, 60),
    LG_RANGE_POINTERS(g_renderDataRanges, 80),
    LG_RANGE_POINTERS(g_renderDataRanges, 100)
  };

  PKSDATARANGE g_captureDataRangePointers[kCaptureDataRangeCount] =
  {
    LG_RANGE_POINTERS(g_captureDataRanges, 0),
    LG_RANGE_POINTERS(g_captureDataRanges, 20),
    LG_RANGE_POINTERS(g_captureDataRanges, 40),
    LG_RANGE_POINTERS(g_captureDataRanges, 60),
    LG_RANGE_POINTERS(g_captureDataRanges, 80),
    LG_RANGE_POINTERS(g_captureDataRanges, 100)
  };

#undef LG_RANGE_POINTERS
#undef LG_RANGE_POINTERS_5

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
        !IsAudioWaveFormat(dataFormat))
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

      if (!IsEqualGUIDAligned(candidate.DataFormat.SubFormat,
            dataFormat->SubFormat) ||
          !WaveFormatCoreMatches(candidateWave.Format, *requested))
        continue;

      if (requested->wFormatTag == WAVE_FORMAT_PCM)
      {
        if (!IsEqualGUIDAligned(candidateWave.SubFormat,
              KSDATAFORMAT_SUBTYPE_PCM) ||
            candidateWave.Format.wBitsPerSample != 16 ||
            candidateWave.Samples.wValidBitsPerSample != 16 ||
            candidateWave.Format.nChannels > 2)
          continue;
      }
      else if (requested->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
      {
        if (!IsEqualGUIDAligned(candidateWave.SubFormat,
              KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ||
            candidateWave.Format.wBitsPerSample != 32 ||
            candidateWave.Samples.wValidBitsPerSample != 32 ||
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
        if (!IsEqualGUIDAligned(dataFormat->SubFormat,
              requestedExtensible->SubFormat) ||
            !ExtensibleFormatMatches(candidateWave, *requestedExtensible))
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
        !IsAudioWaveFormat(clientDataRange) ||
        !IsAudioWaveFormat(driverDataRange))
      return STATUS_NO_MATCH;

    const KSDATARANGE_AUDIO & client =
      *reinterpret_cast<const KSDATARANGE_AUDIO *>(clientDataRange);
    const KSDATARANGE_AUDIO & driver =
      *reinterpret_cast<const KSDATARANGE_AUDIO *>(driverDataRange);

    KSDATAFORMAT_WAVEFORMATEXTENSIBLE * formats;
    ULONG formatCount;
    GetFormatList(endpoint, &formats, &formatCount);

    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE * selected = nullptr;
    for (ULONG i = 0; i < formatCount; ++i)
    {
      if (!DataRangeIdentifiesFormat(driver, formats[i]) ||
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
    const SampleFormat sampleFormat = GetSampleFormat(wave);
    if (wave.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        wave.Format.cbSize < kExtensibleExtraSize ||
        wave.Format.nChannels == 0 ||
        wave.Format.nBlockAlign == 0 ||
        wave.Format.wBitsPerSample == 0 ||
        wave.Samples.wValidBitsPerSample == 0 ||
        wave.Samples.wValidBitsPerSample >
          wave.Format.wBitsPerSample ||
        !IsAudioWaveFormat(&dataFormat->DataFormat) ||
        !IsEqualGUIDAligned(dataFormat->DataFormat.SubFormat,
          wave.SubFormat) ||
        sampleFormat == SampleFormat::Unknown)
      return STATUS_INVALID_PARAMETER;

    fingerprint->sampleFormat = sampleFormat;
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
      left.sampleFormat == right.sampleFormat &&
      left.sampleRate == right.sampleRate &&
      left.channelMask == right.channelMask &&
      left.channels == right.channels &&
      left.containerBits == right.containerBits &&
      left.validBits == right.validBits &&
      left.blockAlign == right.blockAlign;
  }
}
