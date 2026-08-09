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

#ifndef _H_LG_CLIENT_AUDIO_INTERFACE_
#define _H_LG_CLIENT_AUDIO_INTERFACE_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LG_AUDIO_MAX_CHANNELS 32

typedef enum LG_AudioSampleFormat
{
  LG_AUDIO_FMT_U8,
  LG_AUDIO_FMT_S16_LE,
  /* Signed 24-bit little-endian samples packed into three bytes. */
  LG_AUDIO_FMT_S24_LE,
  LG_AUDIO_FMT_S32_LE,
  /* IEEE 754 little-endian floating-point samples. */
  LG_AUDIO_FMT_F32_LE,
  LG_AUDIO_FMT_F64_LE,
}
LG_AudioSampleFormat;

typedef enum LG_AudioChannel
{
  LG_AUDIO_CH_UNKNOWN,
  LG_AUDIO_CH_MONO,
  LG_AUDIO_CH_FRONT_LEFT,
  LG_AUDIO_CH_FRONT_RIGHT,
  LG_AUDIO_CH_FRONT_CENTER,
  LG_AUDIO_CH_LFE,
  LG_AUDIO_CH_REAR_LEFT,
  LG_AUDIO_CH_REAR_RIGHT,
  LG_AUDIO_CH_FRONT_LEFT_CENTER,
  LG_AUDIO_CH_FRONT_RIGHT_CENTER,
  LG_AUDIO_CH_REAR_CENTER,
  LG_AUDIO_CH_SIDE_LEFT,
  LG_AUDIO_CH_SIDE_RIGHT,
  LG_AUDIO_CH_TOP_CENTER,
  LG_AUDIO_CH_TOP_FRONT_LEFT,
  LG_AUDIO_CH_TOP_FRONT_CENTER,
  LG_AUDIO_CH_TOP_FRONT_RIGHT,
  LG_AUDIO_CH_TOP_REAR_LEFT,
  LG_AUDIO_CH_TOP_REAR_CENTER,
  LG_AUDIO_CH_TOP_REAR_RIGHT,
}
LG_AudioChannel;

typedef struct LG_AudioFormat
{
  LG_AudioSampleFormat sampleFormat;
  uint32_t              sampleRate;
  uint8_t               channelCount;

  /* Samples are interleaved in this order. Unknown positions are explicit. */
  LG_AudioChannel channels[LG_AUDIO_MAX_CHANNELS];
}
LG_AudioFormat;

typedef struct LG_AudioClock
{
  /* Frame position and time describe the same point on the stream timeline.
   * Time is monotonic nanoseconds in the publisher's clock domain; consumers
   * must use differences unless they know that they share that domain. */
  uint64_t position;
  int64_t  time;
  double   rate; /* measured frames per second, or zero when unavailable */
  bool     stable;
  bool     discontinuity;
}
LG_AudioClock;

typedef struct LG_AudioStatus
{
  bool     available;
  /* Changes whenever the provider endpoint is replaced or restarted. */
  uint32_t generation;
}
LG_AudioStatus;

typedef void (*LG_AudioStatusFn)(void * opaque,
    const LG_AudioStatus * status);

typedef struct LG_AudioEventOps
{
  /* Format and clock pointers are borrowed for the duration of each call.
   * A NULL source clock indicates that the provider has no usable clock.
   * Providers must serialize event delivery for an attachment. Stream
   * generations are nonzero and uniquely identify each stream instance. */
  void (*playbackStart)(void * opaque, uint32_t generation,
      const LG_AudioFormat * format, const LG_AudioClock * sourceClock);
  void (*playbackStop)(void * opaque, uint32_t generation);
  void (*playbackVolume)(void * opaque, uint32_t generation,
      uint8_t channels, const uint16_t volume[]);
  void (*playbackMute)(void * opaque, uint32_t generation, bool mute);
  /* Sample data is borrowed for the duration of the call. When supplied, the
   * source clock position and time identify the first frame in this packet. */
  void (*playbackData)(void * opaque, uint32_t generation,
      const void * data, size_t frames,
      const LG_AudioClock * sourceClock);

  void (*recordStart)(void * opaque, uint32_t generation,
      const LG_AudioFormat * format);
  void (*recordStop)(void * opaque, uint32_t generation);
  void (*recordVolume)(void * opaque, uint32_t generation,
      uint8_t channels, const uint16_t volume[]);
  void (*recordMute)(void * opaque, uint32_t generation, bool mute);
}
LG_AudioEventOps;

typedef struct LG_AudioOps
{
  const char * name;

  /* Registration must synchronously report the current status after releasing
   * any backend locks. Passing NULL unregisters the listener. Providers with
   * no status listener are assumed to remain available while their transport
   * session is connected. Status callbacks must not be invoked synchronously
   * from event delivery or from another operation in this interface. Passing
   * NULL must synchronously quiesce any in-flight status callback. */
  void (*setStatusListener)(void * opaque, LG_AudioStatusFn callback,
      void * callbackOpaque);

  /* Attach begins event delivery and replays any active streams. Detach must
   * synchronously quiesce event callbacks before returning. The owner must
   * stop event delivery before dropping an endpoint that can no longer be
   * detached. */
  bool (*attach)(void * opaque, const LG_AudioEventOps * events,
      void * eventOpaque);
  void (*detach)(void * opaque);

  /* Send microphone frames to the guest. Data is borrowed for the duration of
   * the call and uses the format supplied with the matching recordStart. The
   * source clock, when present, identifies the first frame in the buffer. */
  bool (*recordData)(void * opaque, uint32_t generation,
      const void * data, size_t frames, const LG_AudioClock * sourceClock);

  /* Optional active synchronization feedback. This is called outside the
   * realtime audio callback with the measured playback device clock. Its
   * position uses the device's independent output-frame timeline and its time
   * includes the backend's estimated presentation latency. */
  bool (*clockFeedback)(void * opaque, uint32_t generation,
      const LG_AudioClock * playbackClock);
}
LG_AudioOps;

#endif
