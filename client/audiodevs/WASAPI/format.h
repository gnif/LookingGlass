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

#ifndef _H_LG_WASAPI_FORMAT_
#define _H_LG_WASAPI_FORMAT_

#include "interface/audio.h"

#include <stdint.h>

typedef enum WasapiSampleType
{
  WASAPI_SAMPLE_TYPE_PCM,
  WASAPI_SAMPLE_TYPE_FLOAT
}
WasapiSampleType;

typedef struct WasapiFormatInfo
{
  uint16_t         blockAlign;
  uint32_t         bytesPerSecond;
  uint16_t         bitsPerSample;
  uint32_t         channelMask;
  WasapiSampleType sampleType;
}
WasapiFormatInfo;

typedef struct WasapiControlPending
{
  bool volume;
  bool mute;
}
WasapiControlPending;

void                 wasapi_controlPendingReset (WasapiControlPending * state);
WasapiControlPending wasapi_controlPendingTake  (WasapiControlPending * state);

int      wasapi_sampleSize                 (LG_AudioSampleFormat format);
uint32_t wasapi_channelMask                (const LG_AudioFormat * format);
bool     wasapi_formatInfo                 (const LG_AudioFormat * format,
    WasapiFormatInfo * info);
uint64_t wasapi_framesToDuration           (uint32_t frames,
    uint32_t sampleRate);
uint32_t wasapi_availableFrames            (uint32_t bufferFrames,
    uint32_t paddingFrames);
uint8_t  wasapi_silenceByte                (LG_AudioSampleFormat format);

#endif
