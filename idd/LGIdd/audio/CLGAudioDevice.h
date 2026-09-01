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

#include "transport/IAudioSource.h"

#include <memory>

struct LGAudioDeviceSnapshot
{
  AudioEndpointStatus status;
  bool     open            = false;
  bool     bound           = false;
  bool     activeRequested = false;
  bool     deactivating    = false;
  uint64_t renderPackets   = 0;
  uint64_t renderBytes     = 0;
  uint64_t capturePackets  = 0;
  uint64_t captureBytes    = 0;
  uint64_t reconnects      = 0;
  uint32_t lastError       = 0;
};

class ILGAudioDeviceEvents
{
public:
  virtual ~ILGAudioDeviceEvents() = default;

  virtual void LGAudioState(const AudioEndpointStatus& state) = 0;
  virtual AudioResult LGAudioPlayback(const AudioPacket& packet,
    const uint8_t * data, size_t size) = 0;
  virtual void LGAudioCaptureReady() = 0;
};

class ILGAudioDevice
{
public:
  virtual ~ILGAudioDevice() = default;

  // Methods are thread-safe and nonblocking except Stop. Stop is a callback-
  // quiescence barrier; after it returns no event callback can still run.
  virtual bool Start(ILGAudioDeviceEvents& events) = 0;
  virtual void Stop() = 0;
  virtual LGAudioDeviceSnapshot Snapshot() const = 0;
  virtual AudioResult SetActive(bool active) = 0;
  virtual AudioResult SendCapture(const AudioPacket& packet,
    const uint8_t * data, size_t size) = 0;
  virtual AudioResult SetClockFeedback(
    const AudioClockFeedback& feedback) = 0;
  virtual void PlaybackReceiveReady() = 0;
};

class CLGAudioDevice final : public ILGAudioDevice
{
private:
  class CState;
  std::unique_ptr<CState> m_state;

public:
  CLGAudioDevice();
  ~CLGAudioDevice();

  CLGAudioDevice(const CLGAudioDevice&) = delete;
  CLGAudioDevice& operator=(const CLGAudioDevice&) = delete;

  bool Start(ILGAudioDeviceEvents& events) override;
  void Stop() override;

  LGAudioDeviceSnapshot Snapshot() const override;
  AudioResult SetActive(bool active) override;
  AudioResult SendCapture(const AudioPacket& packet,
    const uint8_t * data, size_t size) override;
  AudioResult SetClockFeedback(
    const AudioClockFeedback& feedback) override;
  void PlaybackReceiveReady() override;
};
