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

#include <inttypes.h>
#include <stdio.h>

#define TEST_ASSERT(expr) \
  do \
  { \
    if (!(expr)) \
    { \
      fprintf(stderr, "%s:%d: assertion failed: %s\n", \
          __FILE__, __LINE__, #expr); \
      return 1; \
    } \
  } \
  while (0)

static int testSampleSizes(void)
{
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_U8    ) == 1);
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_S16_LE) == 2);
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_S24_LE) == 3);
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_S32_LE) == 4);
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_F32_LE) == 4);
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_F32_NE) == 4);
  TEST_ASSERT(wasapi_sampleSize(LG_AUDIO_FMT_F64_LE) == 8);
  return 0;
}

static int testChannelMasks(void)
{
  LG_AudioFormat stereo =
  {
    .channelCount = 2,
    .channels     =
    {
      LG_AUDIO_CH_FRONT_LEFT,
      LG_AUDIO_CH_FRONT_RIGHT
    }
  };
  TEST_ASSERT(wasapi_channelMask(&stereo) == UINT32_C(0x3));

  LG_AudioFormat surround =
  {
    .channelCount = 6,
    .channels     =
    {
      LG_AUDIO_CH_FRONT_LEFT,
      LG_AUDIO_CH_FRONT_RIGHT,
      LG_AUDIO_CH_FRONT_CENTER,
      LG_AUDIO_CH_LFE,
      LG_AUDIO_CH_REAR_LEFT,
      LG_AUDIO_CH_REAR_RIGHT
    }
  };
  TEST_ASSERT(wasapi_channelMask(&surround) == UINT32_C(0x3f));

  LG_AudioFormat mono =
  {
    .channelCount = 1,
    .channels     = { LG_AUDIO_CH_MONO }
  };
  TEST_ASSERT(wasapi_channelMask(&mono) == UINT32_C(0x4));

  stereo.channels[0] = LG_AUDIO_CH_UNKNOWN;
  TEST_ASSERT(wasapi_channelMask(&stereo) == 0);
  stereo.channels[0] = LG_AUDIO_CH_FRONT_RIGHT;
  stereo.channels[1] = LG_AUDIO_CH_FRONT_LEFT;
  TEST_ASSERT(wasapi_channelMask(&stereo) == 0);
  stereo.channels[0] = LG_AUDIO_CH_FRONT_LEFT;
  stereo.channels[1] = LG_AUDIO_CH_FRONT_LEFT;
  TEST_ASSERT(wasapi_channelMask(&stereo) == 0);
  stereo.channels[1] = LG_AUDIO_CH_MONO;
  TEST_ASSERT(wasapi_channelMask(&stereo) == 0);
  return 0;
}

static int testFormatInfo(void)
{
  LG_AudioFormat format =
  {
    .sampleRate   = 48000,
    .sampleFormat = LG_AUDIO_FMT_S16_LE,
    .channelCount = 2,
    .channels     =
    {
      LG_AUDIO_CH_FRONT_LEFT,
      LG_AUDIO_CH_FRONT_RIGHT
    }
  };
  WasapiFormatInfo info;
  TEST_ASSERT(wasapi_formatInfo(&format, &info));
  TEST_ASSERT(info.blockAlign     == 4);
  TEST_ASSERT(info.bytesPerSecond == 192000);
  TEST_ASSERT(info.bitsPerSample  == 16);
  TEST_ASSERT(info.channelMask    == UINT32_C(0x3));
  TEST_ASSERT(info.sampleType     == WASAPI_SAMPLE_TYPE_PCM);

  format.sampleFormat = LG_AUDIO_FMT_F32_LE;
  TEST_ASSERT(wasapi_formatInfo(&format, &info));
  TEST_ASSERT(info.blockAlign     == 8);
  TEST_ASSERT(info.bytesPerSecond == 384000);
  TEST_ASSERT(info.bitsPerSample  == 32);
  TEST_ASSERT(info.sampleType     == WASAPI_SAMPLE_TYPE_FLOAT);

  TEST_ASSERT(!wasapi_formatInfo(NULL, &info));
  TEST_ASSERT(!wasapi_formatInfo(&format, NULL));

  format.sampleRate   = 0;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  format.sampleRate   = 48000;

  format.channelCount = 0;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  format.channelCount = LG_AUDIO_MAX_CHANNELS + 1;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  format.channelCount = 2;

  format.channels[0]  = LG_AUDIO_CH_UNKNOWN;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  format.channels[0]  = LG_AUDIO_CH_FRONT_RIGHT;
  format.channels[1]  = LG_AUDIO_CH_FRONT_LEFT;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  format.channels[0]  = LG_AUDIO_CH_FRONT_LEFT;
  format.channels[1]  = LG_AUDIO_CH_FRONT_RIGHT;

  format.sampleFormat = (LG_AudioSampleFormat)-1;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  format.sampleFormat = LG_AUDIO_FMT_F64_LE;
  format.sampleRate   = UINT32_MAX;
  TEST_ASSERT(!wasapi_formatInfo(&format, &info));
  return 0;
}

static int testControlPending(void)
{
  WasapiControlPending state =
  {
    .volume = true,
    .mute   = true,
  };

  WasapiControlPending pending = wasapi_controlPendingTake(&state);
  TEST_ASSERT(pending.volume);
  TEST_ASSERT(pending.mute);
  TEST_ASSERT(!state.volume);
  TEST_ASSERT(!state.mute);

  pending = wasapi_controlPendingTake(&state);
  TEST_ASSERT(!pending.volume);
  TEST_ASSERT(!pending.mute);

  state.volume = true;
  state.mute   = true;
  wasapi_controlPendingReset(&state);
  TEST_ASSERT(!state.volume);
  TEST_ASSERT(!state.mute);
  return 0;
}

static int testPeriods(void)
{
  TEST_ASSERT(wasapi_framesToDuration(480, 48000) == UINT64_C(100000));
  TEST_ASSERT(wasapi_framesToDuration(1, 48000) == UINT64_C(209));
  TEST_ASSERT(wasapi_framesToDuration(1, 0) == 0);

  return 0;
}

static int testBufferMath(void)
{
  TEST_ASSERT(wasapi_availableFrames(1024, 256) == 768);
  TEST_ASSERT(wasapi_availableFrames(1024, 1024) == 0);
  TEST_ASSERT(wasapi_availableFrames(1024, 2048) == 0);
  TEST_ASSERT(wasapi_silenceByte(LG_AUDIO_FMT_U8) == UINT8_C(0x80));
  TEST_ASSERT(wasapi_silenceByte(LG_AUDIO_FMT_S16_LE) == 0);
  return 0;
}

int main(void)
{
  if (testSampleSizes() || testChannelMasks() || testFormatInfo() ||
      testControlPending() || testPeriods() || testBufferMath())
    return 1;

  return 0;
}
