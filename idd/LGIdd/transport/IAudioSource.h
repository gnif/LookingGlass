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

#include <stddef.h>
#include <stdint.h>

static constexpr uint32_t AUDIO_MAX_CHANNELS = 18;
static constexpr uint32_t AUDIO_STREAM_MAX_CHANNELS = 8;
static constexpr uint32_t AUDIO_MAX_FRAMES_PER_PACKET = 2048;
static constexpr uint32_t AUDIO_MAX_BYTES_PER_SAMPLE = 3;
static constexpr uint32_t AUDIO_MAX_PCM_BYTES =
  AUDIO_MAX_FRAMES_PER_PACKET * AUDIO_STREAM_MAX_CHANNELS *
  AUDIO_MAX_BYTES_PER_SAMPLE;
static constexpr uint32_t AUDIO_CHANNEL_POSITION_MASK = 0x0003ffffU;

enum AudioCapability : uint32_t
{
  AUDIO_CAP_PLAYBACK       = 1U << 0,
  AUDIO_CAP_CAPTURE        = 1U << 1,
  AUDIO_CAP_CLOCK_FEEDBACK = 1U << 2,
};

enum AudioPacketFlag : uint32_t
{
  AUDIO_PACKET_DISCONTINUITY = 1U << 0,
  AUDIO_PACKET_SILENT        = 1U << 1,
  AUDIO_PACKET_CLOCK_VALID   = 1U << 2,
  AUDIO_PACKET_CLOCK_STABLE  = 1U << 3,
};

enum AudioClockFlag : uint32_t
{
  AUDIO_CLOCK_VALID  = 1U << 0,
  AUDIO_CLOCK_STABLE = 1U << 1,
};

enum class AudioStreamState : uint32_t
{
  UNAVAILABLE,
  STOPPED,
  ACQUIRED,
  PAUSED,
  RUNNING,
};

enum class AudioResult
{
  ACCEPTED,
  BUSY,
  UNAVAILABLE,
  STALE,
  REJECTED,
  FAILED,
};

struct AudioFormat
{
  uint32_t sampleRate    = 0;
  uint32_t channelMask   = 0;
  uint16_t channels      = 0;
  uint16_t containerBits = 0;
  uint16_t validBits     = 0;
  uint16_t blockAlign    = 0;
};

struct AudioStreamStatus
{
  uint64_t         generation  = 0;
  AudioStreamState state       = AudioStreamState::UNAVAILABLE;
  bool             formatValid = false;
  AudioFormat      format;
};

struct AudioEndpointStatus
{
  uint64_t          generation   = 1;
  uint32_t          capabilities = 0;
  bool              available    = false;
  bool              active       = false;
  AudioStreamStatus playback;
  AudioStreamStatus capture;
};

struct AudioPacket
{
  uint64_t    endpointGeneration = 0;
  uint64_t    streamGeneration   = 0;
  uint32_t    flags              = 0;
  uint32_t    frames             = 0;
  uint64_t    position           = 0;
  int64_t     timeNs             = 0;
  uint64_t    rateQ32            = 0;
  AudioFormat format;
};

struct AudioClockFeedback
{
  uint64_t endpointGeneration = 0;
  uint64_t playbackGeneration = 0;
  uint64_t position           = 0;
  int64_t  timeNs             = 0;
  uint64_t targetRateQ32      = 0;
  uint32_t flags              = 0;
};

class IAudioTarget
{
public:
  virtual ~IAudioTarget() = default;

  // Methods are thread-safe and nonblocking. Packet generations and format
  // must exactly match the latest AudioState. A packet contains 1..2048
  // frames and only the declared AudioPacketFlag bits. Non-silent payload
  // size is frames * blockAlign; a silent packet has size zero and is
  // expanded to zero PCM by the target. CLOCK_STABLE requires CLOCK_VALID;
  // position, timeNs, and rateQ32 are meaningful only when CLOCK_VALID.

  // Activation is asynchronous. AudioState reports when the local endpoint
  // is actually bound. Deactivation restores the driver's local loopback.
  // A new activation returns BUSY until a preceding deactivation has reached
  // an inactive AudioState, preventing PCM from crossing owner handoffs.
  virtual AudioResult SetAudioActive(bool active) = 0;

  // ACCEPTED consumes a copied packet. BUSY consumes nothing, and the source
  // retries the exact packet after CaptureReceiveReady.
  virtual AudioResult SendCapture(const AudioPacket& packet,
    const uint8_t * data, size_t size) = 0;
  virtual AudioResult SetClockFeedback(
    const AudioClockFeedback& feedback) = 0;

  // The source calls this after SendPlayback returned BUSY and it can accept
  // an exact retry of the retained playback packet.
  virtual void PlaybackReceiveReady() = 0;
  virtual void AudioFailed() = 0;
};

class IAudioSource
{
public:
  virtual ~IAudioSource() = default;

  // Methods are thread-safe and nonblocking except Stop. Start attaches the
  // target. The source must wait for its initial AudioState callback before
  // making target calls. Stop is a callback-quiescence barrier and leaves no
  // work which can access target.
  virtual bool Start(IAudioTarget& target) = 0;
  virtual void Stop() = 0;

  // All callbacks into a source are serialized. BUSY consumes neither the
  // packet nor its borrowed payload; the hub retains and retries both after
  // PlaybackReceiveReady.
  virtual void AudioState(const AudioEndpointStatus& state) = 0;
  virtual AudioResult SendPlayback(const AudioPacket& packet,
    const uint8_t * data, size_t size) = 0;
  virtual void CaptureReceiveReady() = 0;
};
