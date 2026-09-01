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
#include "common/KVMFRAudio.h"
#include "transport/IAudioSource.h"

#include <LGProtocol/LGMPConfig.h>

#include <Windows.h>

#include <stddef.h>
#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
  #include "lgmp/stream.h"
}

class CLGMPControl;
class CLGMPHost;
class CLGMPTransport;

class CLGMPAudioTransport final : public IAudioSource
{
private:
  static constexpr unsigned RESPONSE_COUNT = LGMP_Q_POINTER_LEN;
  static constexpr unsigned CLAIM_HISTORY_COUNT = 1024;
  static constexpr unsigned INBOUND_DRAIN_MAX =
    KVMFR_AUDIO_STREAM_SLOT_COUNT * 2U;

  enum class OwnerState
  {
    NONE,
    BOUND,
    RELEASING,
  };

  struct PendingResponse
  {
    bool valid = false;
    uint32_t clientID = 0;
    KVMFRAudioStreamResponse response = {};
  };

  struct ResponseMemory
  {
    PLGMPMemory memory = nullptr;
    bool valid = false;
    uint32_t clientID = 0;
    uint32_t requestGeneration = 0;
    uint64_t claimToken = 0;
    KVMFRAudioResponseResult result = 0;
  };

  struct ClaimRecord
  {
    bool valid = false;
    bool closed = false;
    uint32_t clientID = 0;
    uint32_t requestGeneration = 0;
    uint64_t claimToken = 0;
    KVMFRAudioStreamResponse response = {};
  };

  struct PendingCapture
  {
    bool valid = false;
    LGMPStreamBuffer buffer = {};
    AudioPacket packet = {};
    const uint8_t * data = nullptr;
    size_t size = 0;
    uint64_t readySerial = 0;
  };

  CLGMPHost&    m_host;
  CLGMPControl& m_control;

  PLGMPHostStream m_hostToClientStream = nullptr;
  PLGMPHostStream m_clientToHostStream = nullptr;
  KVMFRStreamDescriptor m_hostToClientDescriptor = {};
  KVMFRStreamDescriptor m_clientToHostDescriptor = {};
  ResponseMemory m_responseMemory[RESPONSE_COUNT];
  PendingResponse m_pendingResponse[LGMP_MAX_CLIENTS];
  ClaimRecord m_claimRecord[CLAIM_HISTORY_COUNT];

  CSRWLock m_lifecycleLock;
  CSRWLock m_callbackLock;
  CSRWLock m_lock;
  IAudioTarget * m_target = nullptr;
  bool m_initialized = false;
  bool m_started = false;
  bool m_failed = false;
  bool m_failureNotified = false;

  OwnerState m_ownerState = OwnerState::NONE;
  uint32_t m_ownerClientID = 0;
  uint32_t m_ownerGeneration = 0;
  uint32_t m_clientCapabilities = 0;
  uint64_t m_ownerToken = 0;
  uint64_t m_ownerSerial = 0;
  uint64_t m_ownerDeadline = 0;
  uint64_t m_releaseGeneration = 0;
  bool m_responseAdmitted = false;
  bool m_clientReady = false;
  bool m_activationIssued = false;
  bool m_deactivationIssued = false;
  KVMFRAudioStreamResponse m_ownerResponse = {};

  AudioEndpointStatus m_state;
  bool m_stateKnown = false;
  bool m_statusDirty = false;
  uint64_t m_announcedPlayback = 0;
  uint64_t m_announcedCapture = 0;
  uint64_t m_hostSequence = 0;
  uint64_t m_nextClientSequence = 0;
  bool m_clientSequenceKnown = false;
  bool m_captureDiscontinuity = false;
  bool m_playbackBlocked = false;
  bool m_playbackReadyPending = false;
  bool m_playbackCreditValid = false;
  LGMPStreamBuffer m_playbackCredit = {};
  AudioPacket m_blockedPlayback = {};
  size_t m_blockedPlaybackSize = 0;
  uint64_t m_captureReadySerial = 0;
  PendingCapture m_pendingCapture;

  bool InitializeStreams();
  void FreeStreams();
  bool BindStreams(uint32_t clientID);
  bool TryUnbindStreams();
  void ForceUnbindStreams();

  static bool SameFormat(const AudioFormat& left,
    const AudioFormat& right);
  static bool SamePacket(const AudioPacket& left,
    const AudioPacket& right);
  static bool ValidFormat(const AudioFormat& format);
  static KVMFRAudioFormat WireFormat(const AudioFormat& format);
  static KVMFRAudioStreamState WireState(AudioStreamState state);
  static KVMFRStreamDescriptor WireDescriptor(
    const LGMPStreamDescriptor& descriptor);

  bool ValidateClaim(const KVMFRAudioClaim& claim) const;
  bool ResponsePending(uint32_t clientID,
    const KVMFRAudioStreamResponse& response) const;
  bool QueueResponse(uint32_t clientID,
    const KVMFRAudioStreamResponse& response);
  void CleanupResponseMemory();
  void PumpResponses();

  bool OwnerSubscribed() const;
  void RenewLease();
  void BeginRelease(const char * reason);
  void ReleasePendingCapture();
  void CancelPlaybackCredit();
  void ClearBlockedPlayback(bool notify);
  bool RememberBlockedPlayback(const AudioPacket& packet, size_t size);
  void MaintainOwner(uint64_t now);
  void ResetOwner();

  KVMFRAudioStatus WireStatus() const;
  bool CommitMessage(LGMPStreamBuffer& buffer,
    KVMFRAudioMessageType type, uint64_t streamGeneration,
    KVMFRAudioMessageFlags flags, uint64_t position, int64_t timeNs,
    uint64_t rateQ32, uint32_t frames, const void * payload,
    uint32_t length);
  bool WriteMessage(KVMFRAudioMessageType type,
    uint64_t streamGeneration, KVMFRAudioMessageFlags flags,
    uint64_t position, int64_t timeNs, uint64_t rateQ32,
    uint32_t frames, const void * payload, uint32_t length);
  void PumpState();
  void CheckPlaybackCredit();

  bool ParseInbound(const LGMPStreamBuffer& buffer,
    KVMFRAudioMessage& message) const;
  bool PendingCaptureCurrent() const;
  bool PrepareCapture(const KVMFRAudioMessage& message,
    const LGMPStreamBuffer& buffer);
  void DrainInbound();
  void ProcessActivation();
  void NotifyPlaybackReady();
  void NotifyFailure();
  void Fail(const char * operation, LGMP_STATUS status);

  void InitializeOwner(uint32_t clientID,
    const KVMFRAudioClaim& claim);
  void HandleClaim(uint32_t clientID,
    const KVMFRAudioClaim& claim);
  bool Initialize();
  void Process();
  void TransportStop();

  friend class CLGMPTransport;

public:
  CLGMPAudioTransport(CLGMPHost& host, CLGMPControl& control) :
    m_host(host), m_control(control) {}
  ~CLGMPAudioTransport() override;

  CLGMPAudioTransport(const CLGMPAudioTransport&) = delete;
  CLGMPAudioTransport& operator=(
    const CLGMPAudioTransport&) = delete;

  bool Start(IAudioTarget& target) override;
  void Stop() override;
  void AudioState(const AudioEndpointStatus& state) override;
  AudioResult SendPlayback(const AudioPacket& packet,
    const uint8_t * data, size_t size) override;
  void CaptureReceiveReady() override;
};
