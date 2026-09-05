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
#include "transport/IAudioSource.h"

#include <LGProtocol/KVMFRAudio.h>
#include <LGProtocol/LGMPConfig.h>

#include <Windows.h>

#include <stddef.h>
#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
  #include "lgmp/spmc.h"
  #include "lgmp/stream.h"
}

class CLGMPControl;
class CLGMPHost;
class CLGMPTransport;

class CLGMPAudio final : public IAudioSource
{
private:
  static constexpr unsigned RESPONSE_COUNT = LGMP_Q_POINTER_LEN;
  static constexpr unsigned CLAIM_HISTORY_COUNT = 1024;
  static constexpr uint32_t INVALID_SLOT = UINT32_MAX;
  static constexpr unsigned INBOUND_DRAIN_MAX =
    KVMFR_AUDIO_STREAM_SLOT_COUNT * 2U;
  static constexpr uint64_t AUTHORITY_STALL_MS = 20U;
  static constexpr uint64_t CAPTURE_REVOKE_ACK_MS =
    KVMFR_AUDIO_OWNER_LEASE_MS;

  enum class SubscriberState
  {
    NONE,
    BOUND,
    RELEASING,
  };

  enum class PumpStage
  {
    IDLE,
    STATUS,
    PLAYBACK_STOP,
    CAPTURE_STOP,
    PLAYBACK_START,
    PLAYBACK_VOLUME,
    PLAYBACK_MUTE,
    CAPTURE_START,
    CAPTURE_VOLUME,
    CAPTURE_MUTE,
    BARRIER,
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
    uint32_t subscriber = INVALID_SLOT;
    uint64_t subscriberSerial = 0;
    LGMPStreamBuffer buffer = {};
    AudioPacket packet = {};
    const uint8_t * data = nullptr;
    size_t size = 0;
    uint64_t readySerial = 0;
  };

  struct Subscriber
  {
    PLGMPHostStream hostControl = nullptr;
    PLGMPHostStream clientControl = nullptr;
    KVMFRStreamDescriptor hostControlDescriptor = {};
    KVMFRStreamDescriptor clientControlDescriptor = {};

    SubscriberState state = SubscriberState::NONE;
    uint32_t clientID = 0;
    uint32_t requestGeneration = 0;
    uint32_t clientCapabilities = 0;
    KVMFRAudioSubscriptionFlags subscriptions = 0;
    KVMFRAudioCapabilities grantedCapabilities = 0;
    uint32_t bindingEpoch = 0;
    uint32_t roleEpoch = 0;
    uint32_t playbackReaderID = 0;
    uint32_t playbackReaderEpoch = 0;
    uint32_t hostControlEpoch = 0;
    uint32_t clientControlEpoch = 0;
    uint32_t captureEpoch = 0;
    uint64_t claimToken = 0;
    uint64_t serial = 0;
    uint64_t deadline = 0;
    bool responseAdmitted = false;
    bool subscriptionReceived = false;
    bool ready = false;
    bool awaitingAck = false;
    bool forceUnbind = false;
    bool feedbackRevoked = false;
    bool captureRevoking = false;
    uint64_t captureRevokeRevision = 0;
    uint64_t captureRevokeDeadline = 0;

    uint64_t hostSequence = 0;
    uint64_t nextControlSequence = 0;
    bool controlSequenceKnown = false;
    uint64_t nextCaptureSequence = 0;
    bool captureSequenceKnown = false;
    bool captureDiscontinuity = false;

    uint64_t announcedPlayback = 0;
    uint64_t announcedCapture = 0;
    uint64_t stateRevision = 0;
    uint64_t pumpRevision = 0;
    PumpStage pumpStage = PumpStage::IDLE;
    KVMFRAudioStatus pendingStatus = {};
    KVMFRAudioStateBarrier barrier = {};
    uint64_t barrierGeneration = 0;
    KVMFRAudioStreamResponse response = {};
  };

  CLGMPHost&    m_host;
  CLGMPControl& m_control;

  PLGMPHostSPMC m_playbackStream = nullptr;
  KVMFRSPMCDescriptor m_playbackDescriptor = {};
  PLGMPHostStream m_captureStream = nullptr;
  KVMFRStreamDescriptor m_captureDescriptor = {};
  Subscriber m_subscribers[LGMP_MAX_CLIENTS];
  ResponseMemory m_responseMemory[RESPONSE_COUNT];
  PendingResponse m_pendingResponse[LGMP_MAX_CLIENTS];
  ClaimRecord m_claimRecord[CLAIM_HISTORY_COUNT];

  CSRWLock m_lifecycleLock;
  CSRWLock m_callbackLock;
  CSRWLock m_lock;
  HANDLE m_inboundStopEvent = nullptr;
  HANDLE m_inboundWakeEvent = nullptr;
  HANDLE m_inboundPollTimer = nullptr;
  HANDLE m_inboundThread = nullptr;
  bool m_inboundStopping = false;
  IAudioTarget * m_target = nullptr;
  bool m_initialized = false;
  bool m_claimResponderReady = false;
  bool m_started = false;
  bool m_failed = false;
  bool m_failureNotified = false;

  uint32_t m_feedbackAuthority = INVALID_SLOT;
  uint32_t m_captureOwner = INVALID_SLOT;
  uint32_t m_captureBinding = INVALID_SLOT;
  uint32_t m_nextBindingEpoch = 0;
  uint32_t m_nextRoleEpoch = 0;
  uint64_t m_nextSubscriberSerial = 0;
  bool m_activationCommanded = false;

  AudioEndpointStatus m_state;
  bool                m_stateKnown              = false;
  uint64_t            m_playbackRunGeneration   = 0;
  uint64_t            m_previousPlaybackGeneration = 0;
  bool                m_captureCallbackInFlight = false;
  bool                m_playbackBlocked         = false;
  bool                m_playbackReadyPending    = false;
  AudioPacket         m_blockedPlayback         = {};
  size_t              m_blockedPlaybackSize     = 0;
  uint64_t            m_authorityBlockedSince   = 0;
  uint64_t            m_captureReadySerial      = 0;
  uint64_t            m_inboundActivitySerial   = 0;
  PendingCapture      m_pendingCapture;

  bool InitializeStreams();
  void FreeStreams();
  bool StartInboundWorker();
  void StopInboundWorker();
  void InboundThread();
  static DWORD CALLBACK InboundThreadProc(void * context);
  bool BindSubscriber(uint32_t index, uint32_t clientID,
    const KVMFRAudioClaim& claim);
  bool TryUnbindSubscriber(uint32_t index);
  void ForceUnbindSubscriber(uint32_t index);
  void ResetSubscriber(uint32_t index);

  static bool SameFormat(const AudioFormat& left,
    const AudioFormat& right);
  static bool SamePacket(const AudioPacket& left,
    const AudioPacket& right);
  static bool ValidFormat(const AudioFormat& format);
  static KVMFRAudioFormat WireFormat(const AudioFormat& format);
  static KVMFRAudioStreamState WireState(AudioStreamState state);
  static KVMFRStreamDescriptor WireDescriptor(
    const LGMPStreamDescriptor& descriptor);
  static KVMFRSPMCDescriptor WireDescriptor(
    const LGMPSPMCDescriptor& descriptor);

  bool ValidateClaim(const KVMFRAudioClaim& claim) const;
  bool ResponsePending(uint32_t clientID,
    const KVMFRAudioStreamResponse& response) const;
  bool QueueResponse(uint32_t clientID,
    const KVMFRAudioStreamResponse& response);
  void CleanupResponseMemory();
  void PumpResponses();

  uint32_t FindSubscriber(uint32_t clientID) const;
  uint32_t FindFreeSubscriber() const;
  bool SubscriberPresent(uint32_t clientID,
    const uint32_t clients[LGMP_MAX_CLIENTS], unsigned count) const;
  void RenewLease(Subscriber& subscriber);
  void BeginRelease(uint32_t index, const char * reason,
    bool force = false);
  void MaintainSubscribers(uint64_t now);
  bool HasBoundSubscribers() const;
  bool HasReadyPlayback() const;
  bool PlaybackEligible(const Subscriber& subscriber) const;
  bool FeedbackEligible(const Subscriber& subscriber) const;
  bool CaptureEligible(const Subscriber& subscriber) const;
  void RefreshGranted(uint32_t index, bool bumpRole);
  void AssignFeedbackAuthority(uint32_t index);
  void RevokeFeedbackAuthority(const char * reason);
  void PromoteFeedbackAuthority(uint64_t playbackGeneration);
  void AssignCaptureOwner();
  void MaintainCaptureBinding();
  void MarkStateDirty(uint32_t index);
  void MarkAllStateDirty();
  void ReleasePendingCapture();
  void ClearBlockedPlayback(bool notify);
  bool RememberBlockedPlayback(const AudioPacket& packet, size_t size);

  KVMFRAudioStatus WireStatus(uint32_t index) const;
  bool WriteControlMessage(uint32_t index,
    KVMFRAudioMessageType type, uint64_t streamGeneration,
    KVMFRAudioMessageFlags flags, uint64_t position, int64_t timeNs,
    uint64_t rateQ32, uint32_t frames, const void * payload,
    uint32_t length);
  bool PlaybackCurrent(const KVMFRAudioStatus& status) const;
  bool CaptureCurrent(const KVMFRAudioStatus& status) const;
  void BeginStatePump(uint32_t index);
  void PumpSubscriberState(uint32_t index);
  void PumpState();
  void CheckPlaybackBackpressure(uint64_t now);

  bool ParseInbound(const LGMPStreamBuffer& buffer,
    KVMFRAudioMessage& message) const;
  bool PendingCaptureCurrent(uint32_t index) const;
  bool PrepareCapture(const KVMFRAudioMessage& message,
    const LGMPStreamBuffer& buffer, uint32_t index);
  void DrainControl(uint32_t index);
  void DrainCapture();
  void DrainInbound();
  void ProcessActivation();
  void NotifyPlaybackReady();
  void NotifyFailure();
  void Fail(const char * operation, LGMP_STATUS status);

  void HandleClaim(uint32_t clientID,
    const KVMFRAudioClaim& claim);
  bool Initialize();
  bool CanRespondToClaims();
  void Process();
  void TransportStop();

  friend class CLGMPTransport;

public:
  CLGMPAudio(CLGMPHost& host, CLGMPControl& control) :
    m_host(host), m_control(control) {}
  ~CLGMPAudio() override;

  CLGMPAudio(const CLGMPAudio&) = delete;
  CLGMPAudio& operator=(
    const CLGMPAudio&) = delete;

  bool Start(IAudioTarget& target) override;
  void Stop() override;
  void AudioState(const AudioEndpointStatus& state) override;
  AudioResult SendPlayback(const AudioPacket& packet,
    const uint8_t * data, size_t size) override;
  void CaptureReceiveReady() override;
};
