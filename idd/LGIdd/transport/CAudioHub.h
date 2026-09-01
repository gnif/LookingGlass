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

#include "CSRWLock.h"
#include "audio/CLGAudioDevice.h"
#include "transport/IAudioSource.h"
#include "transport/ITransport.h"

#include <memory>

class CAudioHub final : public IAudioTarget,
  private ILGAudioDeviceEvents
{
private:
  CSRWLock m_lifecycleLock;
  CSRWLock m_callbackLock;
  CSRWLock m_lock;
  std::unique_ptr<ILGAudioDevice> m_device;
  IAudioSource * m_source = nullptr;
  BackendId m_backend = 0;
  uint32_t  m_epoch   = 0;
  bool m_deviceStarted = false;
  bool m_active         = false;
  bool m_running        = false;
  bool m_reserved       = false;
  bool m_failed         = false;
  bool m_failurePending = false;
  bool m_deactivating   = false;
  uint64_t m_deactivationGeneration = 0;
  bool m_stopped        = false;

  bool StartDevice();
  bool MarkFailed(IAudioSource * source);

  void LGAudioState(const AudioEndpointStatus& state) override;
  AudioResult LGAudioPlayback(const AudioPacket& packet,
    const uint8_t * data, size_t size) override;
  void LGAudioCaptureReady() override;

public:
  CAudioHub();
  explicit CAudioHub(std::unique_ptr<ILGAudioDevice> device);
  ~CAudioHub() override;

  CAudioHub(const CAudioHub&) = delete;
  CAudioHub& operator=(const CAudioHub&) = delete;

  // Start only creates the passive connector worker. Broker absence or an
  // incompatible/busy broker is reported through state and is not failure.
  bool Start();
  bool Bind(BackendId backend, uint32_t epoch, IAudioSource& source);
  void Unbind(BackendId backend, uint32_t epoch);
  bool TakeFailure(SourceKey& source);
  LGAudioDeviceSnapshot Snapshot() const;
  void Stop();

  AudioResult SetAudioActive(bool active) override;
  AudioResult SendCapture(const AudioPacket& packet,
    const uint8_t * data, size_t size) override;
  AudioResult SetClockFeedback(
    const AudioClockFeedback& feedback) override;
  void PlaybackReceiveReady() override;
  void AudioFailed() override;
};
