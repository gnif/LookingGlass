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

#include "format.h"

#include <limits.h>

#define WASAPI_TICKS_PER_SECOND UINT64_C(10000000)

#define WASAPI_SPEAKER_FRONT_LEFT             UINT32_C(0x00000001)
#define WASAPI_SPEAKER_FRONT_RIGHT            UINT32_C(0x00000002)
#define WASAPI_SPEAKER_FRONT_CENTER           UINT32_C(0x00000004)
#define WASAPI_SPEAKER_LOW_FREQUENCY          UINT32_C(0x00000008)
#define WASAPI_SPEAKER_BACK_LEFT              UINT32_C(0x00000010)
#define WASAPI_SPEAKER_BACK_RIGHT             UINT32_C(0x00000020)
#define WASAPI_SPEAKER_FRONT_LEFT_OF_CENTER   UINT32_C(0x00000040)
#define WASAPI_SPEAKER_FRONT_RIGHT_OF_CENTER  UINT32_C(0x00000080)
#define WASAPI_SPEAKER_BACK_CENTER            UINT32_C(0x00000100)
#define WASAPI_SPEAKER_SIDE_LEFT              UINT32_C(0x00000200)
#define WASAPI_SPEAKER_SIDE_RIGHT             UINT32_C(0x00000400)
#define WASAPI_SPEAKER_TOP_CENTER             UINT32_C(0x00000800)
#define WASAPI_SPEAKER_TOP_FRONT_LEFT         UINT32_C(0x00001000)
#define WASAPI_SPEAKER_TOP_FRONT_CENTER       UINT32_C(0x00002000)
#define WASAPI_SPEAKER_TOP_FRONT_RIGHT        UINT32_C(0x00004000)
#define WASAPI_SPEAKER_TOP_BACK_LEFT          UINT32_C(0x00008000)
#define WASAPI_SPEAKER_TOP_BACK_CENTER        UINT32_C(0x00010000)
#define WASAPI_SPEAKER_TOP_BACK_RIGHT         UINT32_C(0x00020000)

void wasapi_controlPendingReset(WasapiControlPending * state)
{
  *state = (WasapiControlPending) { 0 };
}

WasapiControlPending wasapi_controlPendingTake(WasapiControlPending * state)
{
  const WasapiControlPending pending = *state;
  wasapi_controlPendingReset(state);
  return pending;
}

int wasapi_sampleSize(LG_AudioSampleFormat format)
{
  switch (format)
  {
    case LG_AUDIO_FMT_U8     : return 1;
    case LG_AUDIO_FMT_S16_LE : return 2;
    case LG_AUDIO_FMT_S24_LE : return 3;
    case LG_AUDIO_FMT_S32_LE :
    case LG_AUDIO_FMT_F32_LE :
    case LG_AUDIO_FMT_F32_NE : return 4;
    case LG_AUDIO_FMT_F64_LE : return 8;
  }

  return 0;
}

static uint32_t wasapi_channelBit(LG_AudioChannel channel)
{
  switch (channel)
  {
    case LG_AUDIO_CH_UNKNOWN            : return 0;
    case LG_AUDIO_CH_MONO               :
    case LG_AUDIO_CH_FRONT_CENTER       :
      return WASAPI_SPEAKER_FRONT_CENTER;
    case LG_AUDIO_CH_FRONT_LEFT         :
      return WASAPI_SPEAKER_FRONT_LEFT;
    case LG_AUDIO_CH_FRONT_RIGHT        :
      return WASAPI_SPEAKER_FRONT_RIGHT;
    case LG_AUDIO_CH_LFE                :
      return WASAPI_SPEAKER_LOW_FREQUENCY;
    case LG_AUDIO_CH_REAR_LEFT          :
      return WASAPI_SPEAKER_BACK_LEFT;
    case LG_AUDIO_CH_REAR_RIGHT         :
      return WASAPI_SPEAKER_BACK_RIGHT;
    case LG_AUDIO_CH_FRONT_LEFT_CENTER  :
      return WASAPI_SPEAKER_FRONT_LEFT_OF_CENTER;
    case LG_AUDIO_CH_FRONT_RIGHT_CENTER :
      return WASAPI_SPEAKER_FRONT_RIGHT_OF_CENTER;
    case LG_AUDIO_CH_REAR_CENTER        :
      return WASAPI_SPEAKER_BACK_CENTER;
    case LG_AUDIO_CH_SIDE_LEFT          :
      return WASAPI_SPEAKER_SIDE_LEFT;
    case LG_AUDIO_CH_SIDE_RIGHT         :
      return WASAPI_SPEAKER_SIDE_RIGHT;
    case LG_AUDIO_CH_TOP_CENTER         :
      return WASAPI_SPEAKER_TOP_CENTER;
    case LG_AUDIO_CH_TOP_FRONT_LEFT     :
      return WASAPI_SPEAKER_TOP_FRONT_LEFT;
    case LG_AUDIO_CH_TOP_FRONT_CENTER   :
      return WASAPI_SPEAKER_TOP_FRONT_CENTER;
    case LG_AUDIO_CH_TOP_FRONT_RIGHT    :
      return WASAPI_SPEAKER_TOP_FRONT_RIGHT;
    case LG_AUDIO_CH_TOP_REAR_LEFT      :
      return WASAPI_SPEAKER_TOP_BACK_LEFT;
    case LG_AUDIO_CH_TOP_REAR_CENTER    :
      return WASAPI_SPEAKER_TOP_BACK_CENTER;
    case LG_AUDIO_CH_TOP_REAR_RIGHT     :
      return WASAPI_SPEAKER_TOP_BACK_RIGHT;
  }

  return 0;
}

uint32_t wasapi_channelMask(const LG_AudioFormat * format)
{
  if (!format || !format->channelCount ||
      format->channelCount > LG_AUDIO_MAX_CHANNELS)
    return 0;

  if (format->channelCount == 1 &&
      format->channels[0] == LG_AUDIO_CH_MONO)
    return WASAPI_SPEAKER_FRONT_CENTER;

  uint32_t mask    = 0;
  uint32_t lastBit = 0;
  for (uint8_t i = 0; i < format->channelCount; ++i)
  {
    if (format->channels[i] == LG_AUDIO_CH_MONO)
      return 0;

    const uint32_t bit = wasapi_channelBit(format->channels[i]);
    if (!bit || bit <= lastBit)
      return 0;

    mask    |= bit;
    lastBit = bit;
  }

  return mask;
}

bool wasapi_formatInfo(const LG_AudioFormat * format,
    WasapiFormatInfo * info)
{
  if (!format || !info || !format->sampleRate || !format->channelCount ||
      format->channelCount > LG_AUDIO_MAX_CHANNELS)
    return false;

  const int      sampleSize     = wasapi_sampleSize(format->sampleFormat);
  const uint32_t channelMask    = wasapi_channelMask(format);
  const uint32_t blockAlign     = (uint32_t)sampleSize * format->channelCount;
  const uint64_t bytesPerSecond = (uint64_t)blockAlign * format->sampleRate;
  if (!sampleSize || !channelMask || blockAlign > UINT16_MAX ||
      bytesPerSecond > UINT32_MAX)
    return false;

  WasapiSampleType sampleType;
  switch (format->sampleFormat)
  {
    case LG_AUDIO_FMT_U8:
    case LG_AUDIO_FMT_S16_LE:
    case LG_AUDIO_FMT_S24_LE:
    case LG_AUDIO_FMT_S32_LE:
      sampleType = WASAPI_SAMPLE_TYPE_PCM;
      break;

    case LG_AUDIO_FMT_F32_LE:
    case LG_AUDIO_FMT_F64_LE:
    case LG_AUDIO_FMT_F32_NE:
      sampleType = WASAPI_SAMPLE_TYPE_FLOAT;
      break;

    default:
      return false;
  }

  info->blockAlign     = (uint16_t)blockAlign;
  info->bytesPerSecond = (uint32_t)bytesPerSecond;
  info->bitsPerSample  = (uint16_t)(sampleSize * CHAR_BIT);
  info->channelMask    = channelMask;
  info->sampleType     = sampleType;
  return true;
}

uint64_t wasapi_framesToDuration(uint32_t frames, uint32_t sampleRate)
{
  if (!sampleRate)
    return 0;

  return ((uint64_t)frames * WASAPI_TICKS_PER_SECOND + sampleRate - 1) /
    sampleRate;
}

uint32_t wasapi_availableFrames(uint32_t bufferFrames,
    uint32_t paddingFrames)
{
  return paddingFrames < bufferFrames ? bufferFrames - paddingFrames : 0;
}

uint8_t wasapi_silenceByte(LG_AudioSampleFormat format)
{
  return format == LG_AUDIO_FMT_U8 ? UINT8_C(0x80) : UINT8_C(0);
}
