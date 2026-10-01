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

class CAudioHub final : private IAudioDeviceEvents
{
private:
  static constexpr unsigned NO_BINDING = TRANSPORT_MAX_INSTANCES;

  enum class PlaybackState : uint8_t
  {
    IDLE,
    READY,
    CALLING,
    BUSY,
    DONE,
  };

  class CTarget final : public IAudioTarget
  {
  private:
    CAudioHub * m_owner   = nullptr;
    unsigned    m_binding = NO_BINDING;

  public:
    void Attach(CAudioHub& owner, unsigned binding);

    AudioResult SetAudioActive(bool active) override;
    AudioResult SendCapture(const AudioPacket& packet,
      const uint8_t * data, size_t size) override;
    AudioResult SetClockFeedback(
      const AudioClockFeedback& feedback) override;
    void PlaybackReceiveReady() override;
    void AudioFailed() override;
  };

  struct Binding
  {
    CTarget        target;
    IAudioSource * source                = nullptr;
    BackendId      backend               = 0;
    uint32_t       epoch                 = 0;
    PlaybackState  playback              = PlaybackState::IDLE;
    bool           occupied              = false;
    bool           reserved              = false;
    bool           running               = false;
    bool           failed                = false;
    bool           failurePending        = false;
    bool           activeRequested       = false;
    bool           playbackReadyEarly    = false;
    bool           captureReceivePending = false;

    void Reset()
    {
      source                = nullptr;
      backend               = 0;
      epoch                 = 0;
      playback              = PlaybackState::IDLE;
      occupied              = false;
      reserved              = false;
      running               = false;
      failed                = false;
      failurePending        = false;
      activeRequested       = false;
      playbackReadyEarly    = false;
      captureReceivePending = false;
    }
  };

  CSRWLock                         m_lifecycleLock;
  CSRWLock                         m_callbackLock;
  CSRWLock                         m_operationLock;
  CSRWLock                         m_lock;
  std::unique_ptr<ILGAudioDevice> m_device;
  Binding                          m_bindings[TRANSPORT_MAX_INSTANCES];
  AudioEndpointStatus              m_state;
  unsigned                         m_captureAuthority          = NO_BINDING;
  unsigned                         m_clockAuthority            = NO_BINDING;
  uint64_t                         m_captureEndpointGeneration = 0;
  uint64_t                         m_captureStreamGeneration   = 0;
  uint64_t                         m_clockEndpointGeneration   = 0;
  uint64_t                         m_clockStreamGeneration     = 0;
  bool                             m_deviceStarted             = false;
  bool                             m_stateKnown                = false;
  bool                             m_playbackActive            = false;
  bool                             m_playbackAccepted          = false;
  bool                             m_playbackFailed            = false;
  bool                             m_playbackWakeDispatching   = false;
  bool                             m_playbackWakePending       = false;
  bool                             m_stopped                   = false;

  bool StartDevice();
  bool BindingLiveNL(unsigned binding) const;
  bool AnyActiveNL() const;
  void ClearAuthoritiesNL(unsigned binding);
  void RetirePlaybackNL(unsigned binding);
  void DeactivateBindingNL(unsigned binding);
  void RequestPlaybackWakeNL();
  void FlushPlaybackWake();
  bool MarkFailedNL(unsigned binding, bool& deactivate);
  bool MarkFailed(unsigned binding);
  void ResetPlaybackNL();

  AudioResult TargetSetAudioActive(unsigned binding, bool active);
  AudioResult TargetSendCapture(unsigned binding,
    const AudioPacket& packet, const uint8_t * data, size_t size);
  AudioResult TargetSetClockFeedback(unsigned binding,
    const AudioClockFeedback& feedback);
  void TargetPlaybackReceiveReady(unsigned binding);
  void TargetAudioFailed(unsigned binding);

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
};
