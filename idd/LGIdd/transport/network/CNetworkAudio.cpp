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

#include "transport/network/CNetworkAudio.h"

#include "CDebug.h"
#include "CSRWLock.h"

#include <LGProtocol/NetworkServices.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <new>
#include <string.h>
#include <utility>
#include <vector>

namespace
{
  constexpr uint32_t PLAYBACK_STREAM_ID = 1;
  constexpr uint32_t CAPTURE_STREAM_ID  = 2;
  constexpr uint64_t AUDIO_LEASE_MS     = 2000;
  constexpr uint64_t AUTHORITY_STALL_MS = 20;
  constexpr size_t   CLOCK_CACHE_SIZE   = 8;
  constexpr size_t   RECENT_PACKET_SIZE = 16;

  constexpr LGNetAudioRoles DIRECTION_ROLES =
    LG_NET_AUDIO_ROLE_PLAYBACK | LG_NET_AUDIO_ROLE_CAPTURE;
  constexpr LGNetAudioRoles DELIVERY_ROLES =
    LG_NET_AUDIO_ROLE_DATAGRAM_PCM | LG_NET_AUDIO_ROLE_RELIABLE_PCM;

  bool SamePeer(const NetworkAudioPeer& left,
    const NetworkAudioPeer& right)
  {
    return left.client == right.client &&
      left.generation == right.generation;
  }

  bool SameSubscription(const LGNetAudioSubscribe& left,
    const LGNetAudioSubscribe& right)
  {
    return left.subscriberID == right.subscriberID &&
      left.directions == right.directions &&
      left.targetLatencyUs == right.targetLatencyUs &&
      left.maxPacketFrames == right.maxPacketFrames &&
      left.flags == right.flags;
  }

  uint64_t NextSerial(uint64_t serial)
  {
    ++serial;
    return serial ? serial : 1;
  }

  uint64_t MonotonicNs()
  {
    const uint64_t result = GetTickCount64() * UINT64_C(1000000);
    return result ? result : 1;
  }

  uint32_t ChannelCount(uint32_t mask)
  {
    uint32_t count = 0;
    while (mask)
    {
      count += mask & 1U;
      mask >>= 1;
    }
    return count;
  }

  size_t SampleBytes(AudioSampleFormat format)
  {
    switch (format)
    {
      case AudioSampleFormat::S16_LE:
        return 2;

      case AudioSampleFormat::S24_LE:
        return 3;

      case AudioSampleFormat::S32_LE:
      case AudioSampleFormat::F32_LE:
        return 4;

      default:
        return 0;
    }
  }

  bool ValidFormat(const AudioFormat& format)
  {
    const size_t bytes = SampleBytes(format.sampleFormat);
    return bytes && format.sampleRate && format.sampleRate <= 384000U &&
      format.channels && format.channels <= AUDIO_STREAM_MAX_CHANNELS &&
      format.channelMask &&
      !(format.channelMask & ~AUDIO_CHANNEL_POSITION_MASK) &&
      ChannelCount(format.channelMask) == format.channels &&
      format.containerBits == bytes * 8U &&
      format.validBits == format.containerBits &&
      format.blockAlign == bytes * format.channels;
  }

  bool SameFormat(const AudioFormat& left, const AudioFormat& right)
  {
    return left.sampleFormat == right.sampleFormat &&
      left.sampleRate == right.sampleRate &&
      left.channelMask == right.channelMask &&
      left.channels == right.channels &&
      left.containerBits == right.containerBits &&
      left.validBits == right.validBits &&
      left.blockAlign == right.blockAlign;
  }

  bool SamePacket(const AudioPacket& left, const AudioPacket& right)
  {
    return left.endpointGeneration == right.endpointGeneration &&
      left.streamGeneration == right.streamGeneration &&
      left.flags == right.flags && left.frames == right.frames &&
      left.position == right.position && left.timeNs == right.timeNs &&
      left.rateQ32 == right.rateQ32 && SameFormat(left.format, right.format);
  }

  LGNetAudioSampleFormat WireSampleFormat(AudioSampleFormat format)
  {
    switch (format)
    {
      case AudioSampleFormat::S16_LE:
        return LG_NET_AUDIO_SAMPLE_S16_LE;

      case AudioSampleFormat::S24_LE:
        return LG_NET_AUDIO_SAMPLE_S24_LE;

      case AudioSampleFormat::S32_LE:
        return LG_NET_AUDIO_SAMPLE_S32_LE;

      case AudioSampleFormat::F32_LE:
        return LG_NET_AUDIO_SAMPLE_F32_LE;

      default:
        return 0;
    }
  }

  LGNetAudioStateCode WireState(AudioStreamState state)
  {
    switch (state)
    {
      case AudioStreamState::RUNNING:
        return LG_NET_AUDIO_STATE_RUNNING;

      case AudioStreamState::ACQUIRED:
      case AudioStreamState::PAUSED:
        return LG_NET_AUDIO_STATE_PAUSED;

      case AudioStreamState::UNAVAILABLE:
      case AudioStreamState::STOPPED:
      default:
        return LG_NET_AUDIO_STATE_STOPPED;
    }
  }

  bool DeadlineReached(uint64_t now, uint64_t deadline)
  {
    return static_cast<int64_t>(now - deadline) >= 0;
  }

  bool AudioVersionSupported(uint16_t version)
  {
    return version >=
        LG_NET_AUDIO_OWNERSHIP_INTRODUCED_SERVICE_VERSION &&
      version <= LG_NET_AUDIO_VERSION_CURRENT;
  }

  bool ReliableFlags(const LGNetEnvelope& envelope,
    LGNetEnvelopeFlags expected)
  {
    constexpr LGNetEnvelopeFlags MASK =
      LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL |
      LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
      LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_ERROR |
      LG_NET_ENVELOPE_EARLY_DATA;
    return (envelope.flags & MASK) ==
      (expected | LG_NET_ENVELOPE_RELIABLE);
  }

  bool DeliveryFlags(const LGNetEnvelope& envelope, bool datagram,
    LGNetEnvelopeFlags expected)
  {
    constexpr LGNetEnvelopeFlags MASK =
      LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL |
      LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
      LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_ERROR |
      LG_NET_ENVELOPE_EARLY_DATA;
    const LGNetEnvelopeFlags delivery = datagram ?
      LG_NET_ENVELOPE_DATAGRAM : LG_NET_ENVELOPE_RELIABLE;
    return (envelope.flags & MASK) == (expected | delivery);
  }
}

struct CNetworkAudio::Impl
{
  enum class PumpStage
  {
    GRANT,
    RENEWAL_GRANT,
    PLAYBACK_STOP,
    CAPTURE_STOP,
    PLAYBACK_START,
    PLAYBACK_VOLUME,
    PLAYBACK_MUTE,
    CAPTURE_START,
    CAPTURE_VOLUME,
    CAPTURE_MUTE,
    PLAYBACK_STATUS,
    CAPTURE_STATUS,
    PLAYBACK_BARRIER,
    CAPTURE_BARRIER,
    DONE,
  };

  enum class PumpEffect
  {
    NONE,
    RENEWAL,
    EXPIRY,
    PLAYBACK_STOPPED,
    CAPTURE_STOPPED,
    PLAYBACK_STARTED,
    CAPTURE_STARTED,
    STATE_SEQUENCE,
    BARRIER,
  };

  enum class BlockDisposition
  {
    STALE,
    RETRY,
    WAIT,
  };

  struct ClockRecord
  {
    bool                 valid = false;
    LGNetAudioClockState state = {};
  };

  struct PacketRecord
  {
    bool     valid       = false;
    uint64_t formatEpoch = 0;
    uint64_t packetID    = 0;
  };

  struct PeerState
  {
    NetworkAudioPeer      peer;
    NetworkAudioPeerInfo  info;
    uint64_t              serial             = 0;
    bool                  hasRequest         = false;
    bool                  subscribed         = false;
    bool                  expiring           = false;
    bool                  feedbackAuthority  = false;
    bool                  feedbackRevoked    = false;
    bool                  captureOwner       = false;
    bool                  captureRevoked     = false;
    bool                  ready              = false;
    bool                  awaitingAck        = false;
    bool                  playbackGap        = true;
    bool                  captureGap         = true;
    bool                  resetPlayback      = false;
    bool                  resetCapture       = false;
    bool                  capturePacketKnown = false;
    uint64_t              deadline           = 0;
    uint64_t              requestID          = 0;
    uint64_t              subscriptionEpoch  = 0;
    uint64_t              sendReadySerial    = 0;
    LGNetAudioSubscribe   request            = {};
    LGNetAudioRoles       requestedRoles     = 0;
    LGNetAudioRoles       grantedRoles       = 0;
    LGNetAudioGrantStatus grantStatus        = LG_NET_AUDIO_GRANT_BUSY;

    uint64_t  playbackSequence  = 0;
    uint64_t  captureSequence   = 0;
    uint64_t  playbackPacketID  = 0;
    uint64_t  capturePacketID   = 0;
    uint64_t  barrierID         = 0;
    uint64_t  playbackBarrierID = 0;
    uint64_t  captureBarrierID  = 0;
    uint8_t   pendingBarriers   = 0;
    uint64_t  announcedPlayback = 0;
    uint64_t  announcedCapture  = 0;
    uint64_t  pumpToken         = 0;
    PumpStage pumpStage         = PumpStage::DONE;
    bool      pumpGrant         = false;
    bool      pumpInFlight      = false;

    std::array<ClockRecord, CLOCK_CACHE_SIZE>    captureClock       = {};
    size_t                                       captureClockNext   = 0;
    std::array<PacketRecord, RECENT_PACKET_SIZE> playbackPacket     = {};
    size_t                                       playbackPacketNext = 0;
  };

  struct PumpAction
  {
    NetworkAudioPeer     peer;
    uint64_t             peerSerial     = 0;
    uint64_t             pumpToken      = 0;
    uint64_t             componentEpoch = 0;
    uint64_t             requestID      = 0;
    uint64_t             value          = 0;
    bool                 playback       = false;
    uint16_t             messageType    = 0;
    LGNetEnvelopeFlags   flags          = LG_NET_ENVELOPE_FINAL;
    PumpEffect           effect         = PumpEffect::NONE;
    std::vector<uint8_t> payload;
  };

  struct PlaybackTarget
  {
    NetworkAudioPeer     peer;
    NetworkAudioPeerInfo info;
    uint64_t             peerSerial        = 0;
    uint64_t             subscriptionEpoch = 0;
    uint64_t             formatEpoch       = 0;
    uint32_t             maxPacketFrames   = 0;
    LGNetAudioRoles      roles             = 0;
    bool                 authority         = false;
    bool                 discontinuity     = false;
  };

  struct PendingPlayback
  {
    bool                        valid        = false;
    AudioPacket                 packet;
    size_t                      size         = 0;
    std::vector<PlaybackTarget> targets;
    size_t                      target       = 0;
    uint32_t                    frameOffset  = 0;
    uint32_t                    chunkFrames  = 0;
    uint64_t                    packetID     = 0;
    bool                        clockSent    = false;
    uint64_t                    blockedSince = 0;
  };

  struct PendingCapture
  {
    bool                                     valid        = false;
    bool                                     callback     = false;
    NetworkAudioPeer                         peer;
    uint64_t                                 peerSerial   = 0;
    AudioPacket                              packet;
    size_t                                   size         = 0;
    std::array<uint8_t, AUDIO_MAX_PCM_BYTES> data         = {};
  };

  CSRWLock m_lifecycleLock;
  CSRWLock m_processLock;
  CSRWLock m_senderCallbackLock;
  CSRWLock m_targetCallbackLock;
  CSRWLock m_lock;

  INetworkAudioSender  * m_sender               = nullptr;
  IAudioTarget         * m_target               = nullptr;
  bool                   m_started              = false;
  bool                   m_stateKnown           = false;
  bool                   m_activationCommanded  = false;
  bool                   m_failed               = false;
  bool                   m_failureNotified      = false;
  bool                   m_playbackReadyPending = false;
  bool                   m_playbackWaiting      = false;
  NetworkAudioPeer       m_playbackWaitingPeer;
  AudioEndpointStatus    m_state;
  std::vector<PeerState> m_peers;
  uint64_t               m_nextPeerSerial       = 0;
  uint64_t               m_nextEpoch            = 0;
  PendingPlayback        m_playback;
  PendingCapture         m_capture;

  PeerState * FindPeerNL(const NetworkAudioPeer& peer)
  {
    for (PeerState& state : m_peers)
      if (SamePeer(state.peer, peer))
        return &state;
    return nullptr;
  }

  const PeerState * FindPeerNL(const NetworkAudioPeer& peer) const
  {
    for (const PeerState& state : m_peers)
      if (SamePeer(state.peer, peer))
        return &state;
    return nullptr;
  }

  bool PlaybackCapableNL() const
  {
    return !m_stateKnown ||
      (m_state.capabilities & AUDIO_CAP_PLAYBACK) != 0;
  }

  bool CaptureCapableNL() const
  {
    return !m_stateKnown ||
      (m_state.capabilities & AUDIO_CAP_CAPTURE) != 0;
  }

  bool FeedbackCapableNL() const
  {
    return !m_stateKnown ||
      (m_state.capabilities & AUDIO_CAP_CLOCK_FEEDBACK) != 0;
  }

  static bool PeerCanDeliver(const PeerState& peer)
  {
    const bool reliable =
      (peer.requestedRoles & LG_NET_AUDIO_ROLE_RELIABLE_PCM) &&
      (peer.info.capabilityFlags & LG_NET_CAPABILITY_RELIABLE) &&
      peer.info.maxReliablePayload >= LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE;
    const bool datagram =
      (peer.requestedRoles & LG_NET_AUDIO_ROLE_DATAGRAM_PCM) &&
      (peer.info.capabilityFlags & LG_NET_CAPABILITY_DATAGRAM) &&
      peer.info.maxDatagramPayload >= LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE;
    return reliable || datagram;
  }

  bool CaptureCandidateNL(const PeerState& peer) const
  {
    return peer.hasRequest && !peer.expiring && !peer.captureRevoked &&
      CaptureCapableNL() && PeerCanDeliver(peer) &&
      (peer.requestedRoles & LG_NET_AUDIO_ROLE_CAPTURE) &&
      (peer.requestedRoles & LG_NET_AUDIO_ROLE_EXCLUSIVE_CAPTURE);
  }

  bool FeedbackCandidateNL(const PeerState& peer) const
  {
    return peer.hasRequest && !peer.expiring && PlaybackCapableNL() &&
      FeedbackCapableNL() && PeerCanDeliver(peer) &&
      !peer.feedbackRevoked &&
      (peer.requestedRoles & LG_NET_AUDIO_ROLE_PLAYBACK) &&
      (peer.requestedRoles & LG_NET_AUDIO_ROLE_CLOCK_FEEDBACK);
  }

  LGNetAudioRoles DeliveryRolesNL(const PeerState& peer) const
  {
    LGNetAudioRoles result = 0;
    if ((peer.requestedRoles & LG_NET_AUDIO_ROLE_RELIABLE_PCM) &&
        (peer.info.capabilityFlags & LG_NET_CAPABILITY_RELIABLE) &&
        peer.info.maxReliablePayload >= LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE)
      result |= LG_NET_AUDIO_ROLE_RELIABLE_PCM;
    if ((peer.requestedRoles & LG_NET_AUDIO_ROLE_DATAGRAM_PCM) &&
        (peer.info.capabilityFlags & LG_NET_CAPABILITY_DATAGRAM) &&
        peer.info.maxDatagramPayload >= LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE)
      result |= LG_NET_AUDIO_ROLE_DATAGRAM_PCM;
    return result;
  }

  LGNetAudioRoles DesiredRolesNL(const PeerState& peer) const
  {
    if (peer.expiring)
      return 0;

    LGNetAudioRoles result = DeliveryRolesNL(peer);
    if ((peer.requestedRoles & LG_NET_AUDIO_ROLE_PLAYBACK) &&
        PlaybackCapableNL())
      result |= LG_NET_AUDIO_ROLE_PLAYBACK;
    if (peer.captureOwner)
      result |= LG_NET_AUDIO_ROLE_CAPTURE |
        LG_NET_AUDIO_ROLE_EXCLUSIVE_CAPTURE;
    if (peer.feedbackAuthority)
      result |= LG_NET_AUDIO_ROLE_CLOCK_FEEDBACK;
    if (!(result & DIRECTION_ROLES) || !(result & DELIVERY_ROLES))
      return 0;
    return result;
  }

  void SchedulePumpNL(PeerState& peer, bool grant)
  {
    peer.ready               = false;
    peer.awaitingAck         = false;
    peer.pendingBarriers     = 0;
    peer.playbackBarrierID   = 0;
    peer.captureBarrierID    = 0;
    peer.pumpGrant           = peer.pumpGrant || grant;
    peer.pumpStage           = peer.pumpGrant ?
      PumpStage::GRANT : PumpStage::PLAYBACK_STOP;
    peer.pumpToken           = NextSerial(peer.pumpToken);
  }

  void ClearSubscriptionNL(PeerState& peer)
  {
    if (m_capture.valid && SamePeer(m_capture.peer, peer.peer))
      m_capture.valid = false;
    if (m_playbackWaiting && SamePeer(m_playbackWaitingPeer, peer.peer))
    {
      m_playbackWaiting      = false;
      m_playbackReadyPending = true;
    }

    peer.hasRequest         = false;
    peer.subscribed         = false;
    peer.expiring           = false;
    peer.feedbackAuthority  = false;
    peer.feedbackRevoked    = false;
    peer.captureOwner       = false;
    peer.captureRevoked     = false;
    peer.ready              = false;
    peer.awaitingAck        = false;
    peer.pendingBarriers    = 0;
    peer.deadline           = 0;
    peer.requestID          = 0;
    peer.request            = {};
    peer.requestedRoles     = 0;
    peer.grantedRoles       = 0;
    peer.pumpGrant          = false;
    peer.pumpStage          = PumpStage::DONE;
    peer.pumpToken          = NextSerial(peer.pumpToken);
    peer.playbackGap        = true;
    peer.captureGap         = true;
    peer.resetPlayback      = false;
    peer.resetCapture       = false;
    peer.capturePacketKnown = false;
    peer.capturePacketID    = 0;
    peer.captureClock       = {};
    peer.captureClockNext   = 0;
    peer.playbackPacket     = {};
    peer.playbackPacketNext = 0;
  }

  void RebalanceRolesNL(uint64_t forcedSerial = 0);
  bool BuildPumpActionNL(PeerState& peer, PumpAction& action);
  void PumpPeer(const NetworkAudioPeer& peer);
  void PumpAll();
  void PublishBarrierNL(PeerState& peer, const PumpAction& action);
  void RollbackBarrierNL(PeerState& peer, const PumpAction& action);
  void ApplyPumpResultNL(PeerState& peer, const PumpAction& action);

  bool PlaybackCurrentNL(const PeerState& peer) const;
  bool CaptureCurrentNL(const PeerState& peer) const;
  bool WireFormatNL(const PeerState& peer, bool playback,
    LGNetAudioFormat& format) const;

  NetworkAudioSendResult Send(const NetworkAudioPeer& peer,
    bool datagram, uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength);
  NetworkAudioSendResult SendPCM(const PlaybackTarget& target,
    uint16_t messageType, const void * payload, uint32_t payloadLength);

  NetworkAudioPacketResult HandleSubscribe(PeerState& peer,
    const LGNetPacketView& packet, uint64_t now);
  NetworkAudioPacketResult HandleCurrentPacket(const NetworkAudioPeer& peer,
    const LGNetPacketView& packet, bool datagram, uint64_t now);
  NetworkAudioPacketResult HandleClock(const NetworkAudioPeer& peer,
    const LGNetPacketView& packet);
  NetworkAudioPacketResult HandleCapture(const NetworkAudioPeer& peer,
    const LGNetPacketView& packet);

  void ProcessActivation();
  void NotifyPlaybackReady();
  void NotifyFailure();
  void MarkFailed(const char * reason);
  void RetryCapture();
  void RevokeStalledAuthority(uint64_t now);
  uint64_t PlaybackReadySerial(const PlaybackTarget& target);
  BlockDisposition HandlePlaybackBlock(
    const PlaybackTarget& target, uint64_t readySerial);

  AudioResult BeginPlayback(const AudioPacket& packet, size_t size);
  AudioResult ContinuePlayback(const uint8_t * data);
};

void CNetworkAudio::Impl::RebalanceRolesNL(uint64_t forcedSerial)
{
  PeerState * capture  = nullptr;
  PeerState * feedback = nullptr;

  for (PeerState& peer : m_peers)
  {
    if (peer.captureOwner && CaptureCandidateNL(peer))
      capture = &peer;
    if (peer.feedbackAuthority && FeedbackCandidateNL(peer))
      feedback = &peer;
  }

  if (!capture)
    for (PeerState& peer : m_peers)
      if (CaptureCandidateNL(peer))
      {
        capture = &peer;
        break;
      }
  if (!feedback)
    for (PeerState& peer : m_peers)
      if (FeedbackCandidateNL(peer))
      {
        feedback = &peer;
        break;
      }

  for (PeerState& peer : m_peers)
  {
    peer.captureOwner      = &peer == capture;
    peer.feedbackAuthority = &peer == feedback;
  }

  for (PeerState& peer : m_peers)
  {
    if (!peer.hasRequest)
      continue;

    const LGNetAudioRoles  desired = DesiredRolesNL(peer);
    LGNetAudioGrantStatus  status;
    if (peer.expiring)
      status = LG_NET_AUDIO_GRANT_DENIED;
    else if (!desired)
    {
      const bool captureBusy =
        (peer.requestedRoles & LG_NET_AUDIO_ROLE_CAPTURE) &&
        (peer.requestedRoles & LG_NET_AUDIO_ROLE_EXCLUSIVE_CAPTURE) &&
        capture && capture != &peer;
      status = captureBusy ?
        LG_NET_AUDIO_GRANT_BUSY : LG_NET_AUDIO_GRANT_UNSUPPORTED;
    }
    else
      status = desired == peer.requestedRoles ?
        LG_NET_AUDIO_GRANT_GRANTED : LG_NET_AUDIO_GRANT_PARTIAL;

    const bool changed = peer.serial == forcedSerial ||
      desired != peer.grantedRoles || status != peer.grantStatus;
    peer.subscribed = status == LG_NET_AUDIO_GRANT_GRANTED ||
      status == LG_NET_AUDIO_GRANT_PARTIAL;
    if (!changed)
      continue;

    if (m_capture.valid && m_capture.peerSerial == peer.serial &&
        SamePeer(m_capture.peer, peer.peer))
      m_capture.valid = false;
    peer.subscriptionEpoch  = NextSerial(m_nextEpoch);
    m_nextEpoch             = peer.subscriptionEpoch;
    peer.grantedRoles       = desired;
    peer.grantStatus        = status;
    peer.playbackGap        = true;
    peer.captureGap         = true;
    peer.capturePacketKnown = false;
    peer.capturePacketID    = 0;
    peer.captureClock       = {};
    peer.captureClockNext   = 0;
    peer.playbackPacket     = {};
    peer.playbackPacketNext = 0;
    SchedulePumpNL(peer, true);
  }
}

bool CNetworkAudio::Impl::WireFormatNL(const PeerState& peer,
  bool playback, LGNetAudioFormat& format) const
{
  const AudioStreamStatus& stream = playback ?
    m_state.playback : m_state.capture;
  if (!stream.generation || !stream.formatValid ||
      !ValidFormat(stream.format))
    return false;

  const uint32_t maxFrames = (std::min)(
    peer.request.maxPacketFrames, AUDIO_MAX_FRAMES_PER_PACKET);
  if (!maxFrames)
    return false;

  uint64_t preferred =
    static_cast<uint64_t>(stream.format.sampleRate) *
      peer.request.targetLatencyUs;
  preferred = (preferred + UINT64_C(999999)) / UINT64_C(1000000);
  preferred = (std::max)(preferred, UINT64_C(1));
  preferred = (std::min)(preferred, static_cast<uint64_t>(maxFrames));

  format.streamID        = playback ?
    PLAYBACK_STREAM_ID : CAPTURE_STREAM_ID;
  format.direction       = playback ?
    LG_NET_AUDIO_DIRECTION_PLAYBACK : LG_NET_AUDIO_DIRECTION_CAPTURE;
  format.sampleFormat    = WireSampleFormat(stream.format.sampleFormat);
  format.formatEpoch     = stream.generation;
  format.sampleRate      = stream.format.sampleRate;
  format.channels        = stream.format.channels;
  format.framesPerPacket = static_cast<uint16_t>(preferred);
  format.channelMask     = stream.format.channelMask;
  format.flags           = LG_NET_AUDIO_FORMAT_INTERLEAVED;
  format.maxPacketFrames = maxFrames;
  return lgNetAudioFormatValid(&format);
}

bool CNetworkAudio::Impl::PlaybackCurrentNL(const PeerState& peer) const
{
  LGNetAudioFormat format = {};
  return m_stateKnown && m_state.available && m_state.active &&
    peer.subscribed &&
    (peer.grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK) &&
    m_state.playback.state != AudioStreamState::UNAVAILABLE &&
    m_state.playback.state != AudioStreamState::STOPPED &&
    WireFormatNL(peer, true, format);
}

bool CNetworkAudio::Impl::CaptureCurrentNL(const PeerState& peer) const
{
  LGNetAudioFormat format = {};
  return m_stateKnown && m_state.available && m_state.active &&
    peer.subscribed && peer.captureOwner &&
    (peer.grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE) &&
    m_state.capture.state != AudioStreamState::UNAVAILABLE &&
    m_state.capture.state != AudioStreamState::STOPPED &&
    WireFormatNL(peer, false, format);
}

bool CNetworkAudio::Impl::BuildPumpActionNL(
  PeerState& peer, PumpAction& action)
{
  action.peer           = peer.peer;
  action.peerSerial     = peer.serial;
  action.pumpToken      = peer.pumpToken;
  action.componentEpoch = peer.subscriptionEpoch;

  for (;;)
  {
    switch (peer.pumpStage)
    {
      case PumpStage::GRANT:
      case PumpStage::RENEWAL_GRANT:
      {
        const bool                   renewal =
          peer.pumpStage == PumpStage::RENEWAL_GRANT;
        LGNetAudioSubscriptionGrant grant   = {};
        grant.subscriberID      = peer.request.subscriberID;
        grant.subscriptionEpoch = peer.subscriptionEpoch;
        grant.requestedRoles    = peer.requestedRoles;
        grant.grantedRoles      = peer.grantedRoles;
        grant.status            = peer.grantStatus;
        if (peer.subscribed)
        {
          grant.targetLatencyUs = peer.request.targetLatencyUs;
          grant.maxPacketFrames = (std::min)(
            peer.request.maxPacketFrames, AUDIO_MAX_FRAMES_PER_PACKET);
        }

        action.payload.resize(
          LG_NET_AUDIO_SUBSCRIPTION_GRANT_WIRE_SIZE);
        if (!lgNetAudioSubscriptionGrantEncode(action.payload.data(),
              action.payload.size(), &grant))
          return false;
        action.messageType = LG_NET_AUDIO_MESSAGE_SUBSCRIPTION_GRANT;
        action.flags       = LG_NET_ENVELOPE_RESPONSE |
          LG_NET_ENVELOPE_FINAL;
        action.effect      = peer.expiring ? PumpEffect::EXPIRY :
          (renewal ? PumpEffect::RENEWAL : PumpEffect::NONE);
        action.requestID   = peer.requestID;
        return true;
      }

      case PumpStage::PLAYBACK_STOP:
        if (peer.announcedPlayback &&
            (peer.resetPlayback || !PlaybackCurrentNL(peer) ||
              peer.announcedPlayback != m_state.playback.generation))
        {
          LGNetAudioControl control = {};
          control.streamID    = PLAYBACK_STREAM_ID;
          control.direction   = LG_NET_AUDIO_DIRECTION_PLAYBACK;
          control.flags       = LG_NET_AUDIO_CONTROL_FLUSH;
          control.sequence    = NextSerial(peer.playbackSequence);
          control.formatEpoch = peer.announcedPlayback;
          action.payload.resize(LG_NET_AUDIO_CONTROL_WIRE_SIZE);
          if (!lgNetAudioControlEncode(action.payload.data(),
                action.payload.size(), &control))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_PLAYBACK_STOP;
          action.effect      = PumpEffect::PLAYBACK_STOPPED;
          action.value       = control.sequence;
          return true;
        }
        peer.resetPlayback = false;
        peer.pumpStage     = PumpStage::CAPTURE_STOP;
        break;

      case PumpStage::CAPTURE_STOP:
        if (peer.announcedCapture &&
            (peer.resetCapture || !CaptureCurrentNL(peer) ||
              peer.announcedCapture != m_state.capture.generation))
        {
          LGNetAudioControl control = {};
          control.streamID    = CAPTURE_STREAM_ID;
          control.direction   = LG_NET_AUDIO_DIRECTION_CAPTURE;
          control.flags       = LG_NET_AUDIO_CONTROL_FLUSH;
          control.sequence    = NextSerial(peer.captureSequence);
          control.formatEpoch = peer.announcedCapture;
          action.payload.resize(LG_NET_AUDIO_CONTROL_WIRE_SIZE);
          if (!lgNetAudioControlEncode(action.payload.data(),
                action.payload.size(), &control))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_CAPTURE_STOP;
          action.effect      = PumpEffect::CAPTURE_STOPPED;
          action.value       = control.sequence;
          return true;
        }
        peer.resetCapture = false;
        peer.pumpStage    = PumpStage::PLAYBACK_START;
        break;

      case PumpStage::PLAYBACK_START:
        if (PlaybackCurrentNL(peer) &&
            peer.announcedPlayback != m_state.playback.generation)
        {
          LGNetAudioFormat format = {};
          if (!WireFormatNL(peer, true, format))
            return false;
          action.payload.resize(LG_NET_AUDIO_FORMAT_WIRE_SIZE);
          if (!lgNetAudioFormatEncode(action.payload.data(),
                action.payload.size(), &format))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_PLAYBACK_START;
          action.effect      = PumpEffect::PLAYBACK_STARTED;
          action.value       = format.formatEpoch;
          return true;
        }
        peer.pumpStage = PumpStage::CAPTURE_START;
        break;

      case PumpStage::PLAYBACK_VOLUME:
      {
        LGNetAudioFormat format = {};
        if (!WireFormatNL(peer, true, format))
          return false;
        LGNetAudioVolume volume = {};
        volume.streamID     = PLAYBACK_STREAM_ID;
        volume.direction    = LG_NET_AUDIO_DIRECTION_PLAYBACK;
        volume.channelCount = format.channels;
        volume.sequence     = NextSerial(peer.playbackSequence);
        volume.channelMask  = format.channelMask;
        const size_t wireSize = lgNetAudioVolumeSize(&volume);
        action.payload.resize(wireSize);
        if (!wireSize || !lgNetAudioVolumeEncode(action.payload.data(),
              action.payload.size(), &volume))
          return false;
        action.messageType = LG_NET_AUDIO_MESSAGE_PLAYBACK_VOLUME;
        action.effect      = PumpEffect::STATE_SEQUENCE;
        action.value       = volume.sequence;
        action.playback    = true;
        return true;
      }

      case PumpStage::PLAYBACK_MUTE:
      {
        LGNetAudioMute mute = {};
        mute.streamID    = PLAYBACK_STREAM_ID;
        mute.direction   = LG_NET_AUDIO_DIRECTION_PLAYBACK;
        mute.sequence    = NextSerial(peer.playbackSequence);
        mute.channelMask = m_state.playback.format.channelMask;
        action.payload.resize(LG_NET_AUDIO_MUTE_WIRE_SIZE);
        if (!lgNetAudioMuteEncode(action.payload.data(),
              action.payload.size(), &mute))
          return false;
        action.messageType = LG_NET_AUDIO_MESSAGE_PLAYBACK_MUTE;
        action.effect      = PumpEffect::STATE_SEQUENCE;
        action.value       = mute.sequence;
        action.playback    = true;
        return true;
      }

      case PumpStage::CAPTURE_START:
        if (CaptureCurrentNL(peer) &&
            peer.announcedCapture != m_state.capture.generation)
        {
          LGNetAudioFormat format = {};
          if (!WireFormatNL(peer, false, format))
            return false;
          action.payload.resize(LG_NET_AUDIO_FORMAT_WIRE_SIZE);
          if (!lgNetAudioFormatEncode(action.payload.data(),
                action.payload.size(), &format))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_CAPTURE_START;
          action.effect      = PumpEffect::CAPTURE_STARTED;
          action.value       = format.formatEpoch;
          return true;
        }
        peer.pumpStage = PumpStage::PLAYBACK_STATUS;
        break;

      case PumpStage::CAPTURE_VOLUME:
      {
        LGNetAudioFormat format = {};
        if (!WireFormatNL(peer, false, format))
          return false;
        LGNetAudioVolume volume = {};
        volume.streamID     = CAPTURE_STREAM_ID;
        volume.direction    = LG_NET_AUDIO_DIRECTION_CAPTURE;
        volume.channelCount = format.channels;
        volume.sequence     = NextSerial(peer.captureSequence);
        volume.channelMask  = format.channelMask;
        const size_t wireSize = lgNetAudioVolumeSize(&volume);
        action.payload.resize(wireSize);
        if (!wireSize || !lgNetAudioVolumeEncode(action.payload.data(),
              action.payload.size(), &volume))
          return false;
        action.messageType = LG_NET_AUDIO_MESSAGE_CAPTURE_VOLUME;
        action.effect      = PumpEffect::STATE_SEQUENCE;
        action.value       = volume.sequence;
        return true;
      }

      case PumpStage::CAPTURE_MUTE:
      {
        LGNetAudioMute mute = {};
        mute.streamID    = CAPTURE_STREAM_ID;
        mute.direction   = LG_NET_AUDIO_DIRECTION_CAPTURE;
        mute.sequence    = NextSerial(peer.captureSequence);
        mute.channelMask = m_state.capture.format.channelMask;
        action.payload.resize(LG_NET_AUDIO_MUTE_WIRE_SIZE);
        if (!lgNetAudioMuteEncode(action.payload.data(),
              action.payload.size(), &mute))
          return false;
        action.messageType = LG_NET_AUDIO_MESSAGE_CAPTURE_MUTE;
        action.effect      = PumpEffect::STATE_SEQUENCE;
        action.value       = mute.sequence;
        return true;
      }

      case PumpStage::PLAYBACK_STATUS:
        if (peer.subscribed &&
            (peer.grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK))
        {
          const bool current = PlaybackCurrentNL(peer);
          LGNetAudioState state = {};
          state.streamID         = PLAYBACK_STREAM_ID;
          state.direction        = LG_NET_AUDIO_DIRECTION_PLAYBACK;
          state.state            = current ?
            WireState(m_state.playback.state) :
            static_cast<LGNetAudioStateCode>(
              LG_NET_AUDIO_STATE_STOPPED);
          state.stateSequence    = NextSerial(peer.playbackSequence);
          state.formatEpoch      = current ? m_state.playback.generation : 0;
          state.volumeMillibels  = 0;
          state.flags            = current ?
            LG_NET_AUDIO_STATE_FORMAT_ACTIVE : 0;
          action.payload.resize(LG_NET_AUDIO_STATE_WIRE_SIZE);
          if (!lgNetAudioStateEncode(action.payload.data(),
                action.payload.size(), &state))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_STATUS;
          action.effect      = PumpEffect::STATE_SEQUENCE;
          action.value       = state.stateSequence;
          action.playback    = true;
          return true;
        }
        peer.pumpStage = PumpStage::CAPTURE_STATUS;
        break;

      case PumpStage::CAPTURE_STATUS:
        if (peer.subscribed &&
            (peer.grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE))
        {
          const bool current = CaptureCurrentNL(peer);
          LGNetAudioState state = {};
          state.streamID         = CAPTURE_STREAM_ID;
          state.direction        = LG_NET_AUDIO_DIRECTION_CAPTURE;
          state.state            = current ?
            WireState(m_state.capture.state) :
            static_cast<LGNetAudioStateCode>(
              LG_NET_AUDIO_STATE_STOPPED);
          state.stateSequence    = NextSerial(peer.captureSequence);
          state.formatEpoch      = current ? m_state.capture.generation : 0;
          state.volumeMillibels  = 0;
          state.flags            = current ?
            LG_NET_AUDIO_STATE_FORMAT_ACTIVE : 0;
          action.payload.resize(LG_NET_AUDIO_STATE_WIRE_SIZE);
          if (!lgNetAudioStateEncode(action.payload.data(),
                action.payload.size(), &state))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_STATUS;
          action.effect      = PumpEffect::STATE_SEQUENCE;
          action.value       = state.stateSequence;
          return true;
        }
        peer.pumpStage = PumpStage::PLAYBACK_BARRIER;
        break;

      case PumpStage::PLAYBACK_BARRIER:
        if (peer.subscribed &&
            (peer.grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK))
        {
          LGNetAudioBarrier barrier = {};
          barrier.streamID      = PLAYBACK_STREAM_ID;
          barrier.direction     = LG_NET_AUDIO_DIRECTION_PLAYBACK;
          barrier.barrierID     = NextSerial(peer.barrierID);
          barrier.stateSequence = peer.playbackSequence;
          action.requestID      = barrier.barrierID;
          action.payload.resize(LG_NET_AUDIO_BARRIER_WIRE_SIZE);
          if (!lgNetAudioBarrierEncode(action.payload.data(),
                action.payload.size(), &barrier))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_STATE_BARRIER;
          action.flags       = LG_NET_ENVELOPE_ACK_REQUIRED |
            LG_NET_ENVELOPE_FINAL;
          action.effect      = PumpEffect::BARRIER;
          action.value       = barrier.barrierID;
          action.playback    = true;
          return true;
        }
        peer.pumpStage = PumpStage::CAPTURE_BARRIER;
        break;

      case PumpStage::CAPTURE_BARRIER:
        if (peer.subscribed &&
            (peer.grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE))
        {
          LGNetAudioBarrier barrier = {};
          barrier.streamID      = CAPTURE_STREAM_ID;
          barrier.direction     = LG_NET_AUDIO_DIRECTION_CAPTURE;
          barrier.barrierID     = NextSerial(peer.barrierID);
          barrier.stateSequence = peer.captureSequence;
          action.requestID      = barrier.barrierID;
          action.payload.resize(LG_NET_AUDIO_BARRIER_WIRE_SIZE);
          if (!lgNetAudioBarrierEncode(action.payload.data(),
                action.payload.size(), &barrier))
            return false;
          action.messageType = LG_NET_AUDIO_MESSAGE_STATE_BARRIER;
          action.flags       = LG_NET_ENVELOPE_ACK_REQUIRED |
            LG_NET_ENVELOPE_FINAL;
          action.effect      = PumpEffect::BARRIER;
          action.value       = barrier.barrierID;
          action.playback    = false;
          return true;
        }
        peer.pumpStage = PumpStage::DONE;
        break;

      case PumpStage::DONE:
        return false;
    }
  }
}

NetworkAudioSendResult CNetworkAudio::Impl::Send(
  const NetworkAudioPeer& peer, bool datagram, uint16_t messageType,
  LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
  const void * payload, uint32_t payloadLength)
{
  CSRWSharedLock callbackLock(m_senderCallbackLock);
  INetworkAudioSender * sender;
  {
    CSRWSharedLock lock(m_lock);
    sender = m_sender;
  }
  if (!sender)
    return NetworkAudioSendResult::FAILED;

  return sender->SendAudio(peer, datagram, LG_NET_SERVICE_AUDIO,
    messageType, flags, componentEpoch, requestID, payload, payloadLength);
}

void CNetworkAudio::Impl::ApplyPumpResultNL(
  PeerState& peer, const PumpAction& action)
{
  switch (action.effect)
  {
    case PumpEffect::NONE:
      peer.pumpGrant     = false;
      peer.resetPlayback = peer.announcedPlayback != 0;
      peer.resetCapture  = peer.announcedCapture != 0;
      peer.pumpStage     = PumpStage::PLAYBACK_STOP;
      break;

    case PumpEffect::RENEWAL:
      peer.pumpStage = PumpStage::DONE;
      peer.ready     = peer.subscribed && !peer.pendingBarriers;
      break;

    case PumpEffect::EXPIRY:
      ClearSubscriptionNL(peer);
      break;

    case PumpEffect::PLAYBACK_STOPPED:
      peer.playbackSequence  = action.value;
      peer.announcedPlayback = 0;
      peer.resetPlayback     = false;
      peer.pumpStage         = PumpStage::CAPTURE_STOP;
      break;

    case PumpEffect::CAPTURE_STOPPED:
      peer.captureSequence  = action.value;
      peer.announcedCapture = 0;
      peer.resetCapture     = false;
      peer.pumpStage        = PumpStage::PLAYBACK_START;
      break;

    case PumpEffect::PLAYBACK_STARTED:
      peer.announcedPlayback = action.value;
      peer.pumpStage         = PumpStage::PLAYBACK_VOLUME;
      break;

    case PumpEffect::CAPTURE_STARTED:
      peer.announcedCapture = action.value;
      peer.pumpStage        = PumpStage::CAPTURE_VOLUME;
      break;

    case PumpEffect::STATE_SEQUENCE:
      if (action.playback)
        peer.playbackSequence = action.value;
      else
        peer.captureSequence = action.value;
      switch (peer.pumpStage)
      {
        case PumpStage::PLAYBACK_VOLUME:
          peer.pumpStage = PumpStage::PLAYBACK_MUTE;
          break;

        case PumpStage::PLAYBACK_MUTE:
          peer.pumpStage = PumpStage::CAPTURE_START;
          break;

        case PumpStage::CAPTURE_VOLUME:
          peer.pumpStage = PumpStage::CAPTURE_MUTE;
          break;

        case PumpStage::CAPTURE_MUTE:
          peer.pumpStage = PumpStage::PLAYBACK_STATUS;
          break;

        case PumpStage::PLAYBACK_STATUS:
          peer.pumpStage = PumpStage::CAPTURE_STATUS;
          break;

        case PumpStage::CAPTURE_STATUS:
          peer.pumpStage = PumpStage::PLAYBACK_BARRIER;
          break;

        default:
          peer.pumpStage = PumpStage::DONE;
          break;
      }
      break;

    case PumpEffect::BARRIER:
      peer.barrierID = action.value;
      if (action.playback)
        peer.pumpStage = PumpStage::CAPTURE_BARRIER;
      else
        peer.pumpStage = PumpStage::DONE;
      peer.awaitingAck = peer.pendingBarriers != 0;
      peer.ready       = !peer.awaitingAck &&
        peer.pumpStage == PumpStage::DONE;
      break;
  }
}

void CNetworkAudio::Impl::PublishBarrierNL(
  PeerState& peer, const PumpAction& action)
{
  const uint8_t barrierBit = action.playback ? 1U : 2U;
  uint64_t&     barrierID  = action.playback ?
    peer.playbackBarrierID : peer.captureBarrierID;

  barrierID            = action.value;
  peer.pendingBarriers |= barrierBit;
  peer.awaitingAck      = true;
  peer.ready            = false;
}

void CNetworkAudio::Impl::RollbackBarrierNL(
  PeerState& peer, const PumpAction& action)
{
  const uint8_t barrierBit = action.playback ? 1U : 2U;
  uint64_t&     barrierID  = action.playback ?
    peer.playbackBarrierID : peer.captureBarrierID;
  if (barrierID != action.value)
    return;

  barrierID             = 0;
  peer.pendingBarriers &= static_cast<uint8_t>(~barrierBit);
  peer.awaitingAck       = peer.pendingBarriers != 0;
  peer.ready             = false;
}

void CNetworkAudio::Impl::PumpPeer(const NetworkAudioPeer& key)
{
  for (unsigned iteration = 0; iteration < 16U; ++iteration)
  {
    PumpAction action;
    bool       built = false;
    try
    {
      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(key);
      if (!peer || peer->pumpInFlight)
        return;
      if (peer->pumpStage == PumpStage::DONE)
      {
        if (peer->subscribed && !peer->pendingBarriers)
          peer->ready = true;
        if (m_playbackWaiting &&
            SamePeer(m_playbackWaitingPeer, peer->peer) &&
            (peer->ready || !peer->subscribed))
        {
          m_playbackWaiting      = false;
          m_playbackReadyPending = true;
        }
        return;
      }
      peer->pumpInFlight = true;
      built              = BuildPumpActionNL(*peer, action);
      if (!built)
      {
        peer->pumpInFlight = false;
        if (peer->pumpStage != PumpStage::DONE)
        {
          DEBUG_ERROR("Failed to encode network audio state");
          peer->pumpStage = PumpStage::DONE;
          MarkFailed("network audio state encoding");
        }
        else
        {
          if (peer->subscribed && !peer->pendingBarriers)
            peer->ready = true;
          if (m_playbackWaiting &&
              SamePeer(m_playbackWaitingPeer, peer->peer) &&
              (peer->ready || !peer->subscribed))
          {
            m_playbackWaiting      = false;
            m_playbackReadyPending = true;
          }
        }
        return;
      }
      if (action.effect == PumpEffect::BARRIER)
        PublishBarrierNL(*peer, action);
    }
    catch (...)
    {
      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(key);
      if (peer)
      {
        peer->pumpInFlight = false;
        peer->pumpStage    = PumpStage::DONE;
      }
      MarkFailed("network audio state allocation");
      return;
    }

    const NetworkAudioSendResult result = Send(action.peer, false,
      action.messageType, action.flags, action.componentEpoch,
      action.requestID, action.payload.data(),
      static_cast<uint32_t>(action.payload.size()));

    CSRWExclusiveLock lock(m_lock);
    PeerState * peer = FindPeerNL(action.peer);
    if (!peer || peer->serial != action.peerSerial)
      return;
    peer->pumpInFlight = false;
    if (result == NetworkAudioSendResult::FAILED)
    {
      ClearSubscriptionNL(*peer);
      RebalanceRolesNL();
      return;
    }
    if (result == NetworkAudioSendResult::BLOCKED)
    {
      if (peer->pumpToken == action.pumpToken &&
          peer->subscriptionEpoch == action.componentEpoch &&
          action.effect == PumpEffect::BARRIER)
        RollbackBarrierNL(*peer, action);
      return;
    }
    if (peer->pumpToken != action.pumpToken ||
        peer->subscriptionEpoch != action.componentEpoch)
    {
      if (peer->subscriptionEpoch == action.componentEpoch)
      {
        switch (action.effect)
        {
          case PumpEffect::PLAYBACK_STOPPED:
            peer->playbackSequence  = action.value;
            peer->announcedPlayback = 0;
            break;

          case PumpEffect::CAPTURE_STOPPED:
            peer->captureSequence  = action.value;
            peer->announcedCapture = 0;
            break;

          case PumpEffect::PLAYBACK_STARTED:
            peer->announcedPlayback = action.value;
            break;

          case PumpEffect::CAPTURE_STARTED:
            peer->announcedCapture = action.value;
            break;

          case PumpEffect::STATE_SEQUENCE:
            if (action.playback)
              peer->playbackSequence = action.value;
            else
              peer->captureSequence = action.value;
            break;

          case PumpEffect::NONE:
          case PumpEffect::RENEWAL:
          case PumpEffect::EXPIRY:
          case PumpEffect::BARRIER:
            break;
        }
      }
      continue;
    }
    ApplyPumpResultNL(*peer, action);
  }
}

void CNetworkAudio::Impl::PumpAll()
{
  std::vector<NetworkAudioPeer> peers;
  try
  {
    CSRWSharedLock lock(m_lock);
    peers.reserve(m_peers.size());
    for (const PeerState& peer : m_peers)
      if (peer.pumpStage != PumpStage::DONE)
        peers.push_back(peer.peer);
  }
  catch (...)
  {
    CSRWExclusiveLock lock(m_lock);
    MarkFailed("network audio pump allocation");
    return;
  }

  for (const NetworkAudioPeer& peer : peers)
    PumpPeer(peer);
}

NetworkAudioPacketResult CNetworkAudio::Impl::HandleSubscribe(
  PeerState& peer, const LGNetPacketView& packet, uint64_t now)
{
  LGNetAudioSubscribe subscribe = {};
  if (lgNetAudioSubscribeDecodeForVersion(&subscribe,
        packet.envelope.serviceVersion, packet.payload,
        packet.payloadLength) != LG_NET_PARSE_OK)
    return NetworkAudioPacketResult::INVALID;

  const LGNetAudioRoles requestedRoles =
    lgNetAudioSubscribeRoles(&subscribe);
  if (peer.hasRequest && peer.subscribed && !peer.expiring &&
      peer.requestID == packet.envelope.requestID &&
      peer.requestedRoles == requestedRoles &&
      SameSubscription(peer.request, subscribe))
  {
    peer.deadline = now + AUDIO_LEASE_MS;
    if (!peer.pumpInFlight && peer.pumpStage == PumpStage::DONE)
    {
      peer.pumpStage = PumpStage::RENEWAL_GRANT;
      peer.pumpToken = NextSerial(peer.pumpToken);
    }
    return NetworkAudioPacketResult::HANDLED;
  }

  peer.hasRequest      = true;
  peer.expiring        = false;
  peer.feedbackRevoked = false;
  peer.captureRevoked  = false;
  peer.request         = subscribe;
  peer.requestID       = packet.envelope.requestID;
  peer.requestedRoles  = requestedRoles;
  peer.deadline        = now + AUDIO_LEASE_MS;
  peer.captureClock    = {};
  peer.playbackPacket  = {};
  RebalanceRolesNL(peer.serial);
  return NetworkAudioPacketResult::HANDLED;
}

NetworkAudioPacketResult CNetworkAudio::Impl::HandleClock(
  const NetworkAudioPeer& key, const LGNetPacketView& packet)
{
  LGNetAudioClockState clock = {};
  if (lgNetAudioClockStateDecode(&clock, packet.payload,
        packet.payloadLength) != LG_NET_PARSE_OK)
    return NetworkAudioPacketResult::INVALID;

  if (clock.direction == LG_NET_AUDIO_DIRECTION_CAPTURE)
  {
    CSRWExclusiveLock lock(m_lock);
    PeerState * peer = FindPeerNL(key);
    if (!peer || !peer->hasRequest ||
        peer->subscriptionEpoch != packet.envelope.componentEpoch)
      return NetworkAudioPacketResult::IGNORED;
    if (clock.subscriptionEpoch != peer->subscriptionEpoch ||
        clock.streamID != CAPTURE_STREAM_ID)
      return NetworkAudioPacketResult::INVALID;

    peer->deadline = GetTickCount64() + AUDIO_LEASE_MS;
    LGNetAudioFormat format = {};
    if (!peer->captureOwner ||
        !peer->ready || !CaptureCurrentNL(*peer) ||
        !WireFormatNL(*peer, false, format) ||
        clock.formatEpoch != format.formatEpoch)
      return NetworkAudioPacketResult::IGNORED;
    if (peer->capturePacketKnown &&
        clock.packetID <= peer->capturePacketID)
      return NetworkAudioPacketResult::IGNORED;

    ClockRecord * record = nullptr;
    for (ClockRecord& candidate : peer->captureClock)
      if (candidate.valid && candidate.state.packetID == clock.packetID &&
          candidate.state.formatEpoch == clock.formatEpoch)
      {
        record = &candidate;
        break;
      }
    if (!record)
      record = &peer->captureClock[
        peer->captureClockNext++ % CLOCK_CACHE_SIZE];
    record->valid = true;
    record->state = clock;
    return NetworkAudioPacketResult::HANDLED;
  }

  if (clock.direction != LG_NET_AUDIO_DIRECTION_PLAYBACK ||
      clock.streamID != PLAYBACK_STREAM_ID)
    return NetworkAudioPacketResult::INVALID;

  CSRWSharedLock callbackLock(m_targetCallbackLock);
  IAudioTarget       * target            = nullptr;
  AudioClockFeedback   feedback          = {};
  uint64_t             peerSerial        = 0;
  uint64_t             subscriptionEpoch = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    PeerState * peer = FindPeerNL(key);
    if (!peer || !peer->hasRequest ||
        peer->subscriptionEpoch != packet.envelope.componentEpoch)
      return NetworkAudioPacketResult::IGNORED;
    if (clock.subscriptionEpoch != peer->subscriptionEpoch)
      return NetworkAudioPacketResult::INVALID;
    peer->deadline = GetTickCount64() + AUDIO_LEASE_MS;

    if (!peer->feedbackAuthority || !peer->ready ||
        !PlaybackCurrentNL(*peer) ||
        clock.formatEpoch != m_state.playback.generation)
      return NetworkAudioPacketResult::IGNORED;

    bool knownPacket = false;
    for (const PacketRecord& record : peer->playbackPacket)
      if (record.valid && record.packetID == clock.packetID &&
          record.formatEpoch == clock.formatEpoch)
      {
        knownPacket = true;
        break;
      }
    if (!knownPacket)
      return NetworkAudioPacketResult::IGNORED;
    if (!(clock.flags & LG_NET_AUDIO_CLOCK_VALID) ||
        !clock.targetRateQ32)
      return NetworkAudioPacketResult::HANDLED;

    feedback.endpointGeneration = m_state.generation;
    feedback.playbackGeneration = m_state.playback.generation;
    feedback.position           = clock.framePosition;
    feedback.timeNs             = clock.timeNs;
    feedback.rateQ32            = clock.rateQ32;
    feedback.targetRateQ32      = clock.targetRateQ32;
    if (clock.flags & LG_NET_AUDIO_CLOCK_VALID)
      feedback.flags |= AUDIO_CLOCK_VALID;
    if (clock.flags & LG_NET_AUDIO_CLOCK_STABLE)
      feedback.flags |= AUDIO_CLOCK_STABLE;
    target            = m_started ? m_target : nullptr;
    peerSerial        = peer->serial;
    subscriptionEpoch = peer->subscriptionEpoch;
  }

  if (!target)
    return NetworkAudioPacketResult::IGNORED;
  const AudioResult result = target->SetClockFeedback(feedback);
  if (result == AudioResult::FAILED || result == AudioResult::REJECTED)
  {
    CSRWExclusiveLock lock(m_lock);
    PeerState * peer = FindPeerNL(key);
    if (peer && peer->serial == peerSerial &&
        peer->subscriptionEpoch == subscriptionEpoch &&
        peer->feedbackAuthority &&
        m_state.playback.generation == feedback.playbackGeneration)
    {
      peer->feedbackAuthority = false;
      peer->feedbackRevoked   = true;
      RebalanceRolesNL();
    }
  }
  return NetworkAudioPacketResult::HANDLED;
}

NetworkAudioPacketResult CNetworkAudio::Impl::HandleCapture(
  const NetworkAudioPeer& key, const LGNetPacketView& packet)
{
  LGNetAudioData audio = {};
  if (lgNetAudioDataDecode(&audio, packet.payload,
        packet.payloadLength) != LG_NET_PARSE_OK)
    return NetworkAudioPacketResult::INVALID;
  if (audio.direction != LG_NET_AUDIO_DIRECTION_CAPTURE ||
      audio.streamID != CAPTURE_STREAM_ID ||
      audio.frameCount > AUDIO_MAX_FRAMES_PER_PACKET ||
      audio.dataLength > AUDIO_MAX_PCM_BYTES)
    return NetworkAudioPacketResult::INVALID;

  CSRWSharedLock callbackLock(m_targetCallbackLock);
  IAudioTarget * target     = nullptr;
  AudioPacket    capture    = {};
  uint64_t       peerSerial = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    PeerState * peer = FindPeerNL(key);
    if (!peer || !peer->hasRequest ||
        peer->subscriptionEpoch != packet.envelope.componentEpoch)
      return NetworkAudioPacketResult::IGNORED;

    LGNetAudioFormat format = {};
    if (!peer->captureOwner || !peer->ready || !CaptureCurrentNL(*peer) ||
        !WireFormatNL(*peer, false, format))
    {
      peer->captureGap = true;
      return NetworkAudioPacketResult::IGNORED;
    }
    if (!lgNetAudioDataMatchesFormat(&audio, &format))
      return NetworkAudioPacketResult::INVALID;
    peer->deadline = GetTickCount64() + AUDIO_LEASE_MS;

    if (peer->capturePacketKnown &&
        audio.packetID <= peer->capturePacketID)
      return NetworkAudioPacketResult::IGNORED;
    const bool sequenceGap = peer->capturePacketKnown &&
      audio.packetID != peer->capturePacketID + 1U;
    peer->capturePacketKnown = true;
    peer->capturePacketID    = audio.packetID;
    if (m_capture.valid || m_capture.callback)
    {
      peer->captureGap = true;
      return NetworkAudioPacketResult::IGNORED;
    }

    capture.endpointGeneration = m_state.generation;
    capture.streamGeneration   = m_state.capture.generation;
    capture.frames             = audio.frameCount;
    capture.format             = m_state.capture.format;
    if (audio.flags & LG_NET_AUDIO_DATA_SILENT)
      capture.flags |= AUDIO_PACKET_SILENT;
    if ((audio.flags & (LG_NET_AUDIO_DATA_DISCONTINUITY |
          LG_NET_AUDIO_DATA_END_OF_STREAM)) || peer->captureGap ||
        sequenceGap)
      capture.flags |= AUDIO_PACKET_DISCONTINUITY;

    bool clockFound = false;
    for (ClockRecord& record : peer->captureClock)
      if (record.valid && record.state.packetID == audio.packetID &&
          record.state.formatEpoch == audio.formatEpoch)
      {
        const LGNetAudioClockState& clock = record.state;
        if (clock.flags & LG_NET_AUDIO_CLOCK_VALID)
        {
          capture.position = clock.framePosition;
          capture.timeNs   = clock.timeNs;
          capture.rateQ32  = clock.rateQ32;
          capture.flags |= AUDIO_PACKET_CLOCK_VALID;
        }
        if (clock.flags & LG_NET_AUDIO_CLOCK_STABLE)
          capture.flags |= AUDIO_PACKET_CLOCK_STABLE;
        record.valid = false;
        clockFound   = true;
        break;
      }
    if (!clockFound)
      capture.flags |= AUDIO_PACKET_DISCONTINUITY;

    target     = m_started ? m_target : nullptr;
    peerSerial = peer->serial;
    if (!target)
    {
      peer->captureGap = true;
      return NetworkAudioPacketResult::IGNORED;
    }
    peer->captureGap   = false;
    m_capture.callback = true;
  }

  const AudioResult result = target->SendCapture(
    capture, audio.data, audio.dataLength);

  CSRWExclusiveLock lock(m_lock);
  m_capture.callback = false;
  PeerState * peer = FindPeerNL(key);
  if (!peer || peer->serial != peerSerial || !peer->captureOwner ||
      peer->subscriptionEpoch != packet.envelope.componentEpoch)
    return NetworkAudioPacketResult::IGNORED;
  if (result == AudioResult::BUSY)
  {
    m_capture.valid      = true;
    m_capture.peer       = key;
    m_capture.peerSerial = peerSerial;
    m_capture.packet     = capture;
    m_capture.size       = audio.dataLength;
    if (audio.dataLength)
      memcpy(m_capture.data.data(), audio.data, audio.dataLength);
    return NetworkAudioPacketResult::HANDLED;
  }

  if (result == AudioResult::FAILED || result == AudioResult::REJECTED)
  {
    peer->captureRevoked = true;
    peer->captureGap     = true;
    RebalanceRolesNL();
  }
  else if (result != AudioResult::ACCEPTED)
    peer->captureGap = true;
  return NetworkAudioPacketResult::HANDLED;
}

NetworkAudioPacketResult CNetworkAudio::Impl::HandleCurrentPacket(
  const NetworkAudioPeer& key, const LGNetPacketView& packet,
  bool datagram, uint64_t now)
{
  switch (packet.envelope.messageType)
  {
    case LG_NET_AUDIO_MESSAGE_CLOCK_STATE:
      if (packet.envelope.requestID ||
          !DeliveryFlags(packet.envelope, datagram,
            LG_NET_ENVELOPE_FINAL))
        return NetworkAudioPacketResult::INVALID;
      return HandleClock(key, packet);

    case LG_NET_AUDIO_MESSAGE_CAPTURE_DATA:
      if (packet.envelope.requestID ||
          !DeliveryFlags(packet.envelope, datagram,
            LG_NET_ENVELOPE_FINAL))
        return NetworkAudioPacketResult::INVALID;
      return HandleCapture(key, packet);

    case LG_NET_AUDIO_MESSAGE_STATE_ACK:
    {
      if (datagram || !packet.envelope.requestID ||
          !ReliableFlags(packet.envelope,
            LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL))
        return NetworkAudioPacketResult::INVALID;
      LGNetAudioBarrier barrier = {};
      if (lgNetAudioBarrierDecode(&barrier, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK)
        return NetworkAudioPacketResult::INVALID;

      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(key);
      if (!peer || !peer->hasRequest ||
          peer->subscriptionEpoch != packet.envelope.componentEpoch)
        return NetworkAudioPacketResult::IGNORED;
      peer->deadline = now + AUDIO_LEASE_MS;
      uint8_t  barrierBit;
      uint64_t barrierID;
      uint64_t stateSequence;
      if (barrier.streamID == PLAYBACK_STREAM_ID &&
          barrier.direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
          (peer->grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK))
      {
        barrierBit    = 1U;
        barrierID     = peer->playbackBarrierID;
        stateSequence = peer->playbackSequence;
      }
      else if (barrier.streamID == CAPTURE_STREAM_ID &&
          barrier.direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
          (peer->grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE))
      {
        barrierBit    = 2U;
        barrierID     = peer->captureBarrierID;
        stateSequence = peer->captureSequence;
      }
      else
        return NetworkAudioPacketResult::INVALID;
      if (!peer->awaitingAck || !(peer->pendingBarriers & barrierBit) ||
          packet.envelope.requestID != barrier.barrierID ||
          barrier.barrierID != barrierID ||
          barrier.stateSequence != stateSequence)
        return NetworkAudioPacketResult::IGNORED;
      peer->pendingBarriers &= static_cast<uint8_t>(~barrierBit);
      peer->awaitingAck = peer->pendingBarriers != 0;
      peer->ready       = !peer->awaitingAck &&
        peer->pumpStage == PumpStage::DONE;
      if (m_playbackWaiting && SamePeer(m_playbackWaitingPeer, key))
      {
        m_playbackWaiting      = false;
        m_playbackReadyPending = true;
      }
      return NetworkAudioPacketResult::HANDLED;
    }

    case LG_NET_AUDIO_MESSAGE_KEEPALIVE:
    case LG_NET_AUDIO_MESSAGE_RELEASE:
    {
      if ((packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_RELEASE &&
            datagram) ||
          packet.envelope.requestID ||
          !DeliveryFlags(packet.envelope, datagram,
            LG_NET_ENVELOPE_FINAL))
        return NetworkAudioPacketResult::INVALID;
      LGNetAudioControl control = {};
      if (lgNetAudioControlDecode(&control, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK)
        return NetworkAudioPacketResult::INVALID;

      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(key);
      if (!peer || !peer->hasRequest ||
          peer->subscriptionEpoch != packet.envelope.componentEpoch)
        return NetworkAudioPacketResult::IGNORED;
      const bool playback = control.streamID == PLAYBACK_STREAM_ID &&
        control.direction == LG_NET_AUDIO_DIRECTION_PLAYBACK;
      const bool capture  = control.streamID == CAPTURE_STREAM_ID &&
        control.direction == LG_NET_AUDIO_DIRECTION_CAPTURE;
      if ((!playback && !capture) || control.detail ||
          (playback &&
            !(peer->grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK)) ||
          (capture &&
            !(peer->grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE)) ||
          (packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_KEEPALIVE &&
            control.flags))
        return NetworkAudioPacketResult::INVALID;
      peer->deadline = now + AUDIO_LEASE_MS;
      if (packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_RELEASE)
      {
        if (m_capture.valid && SamePeer(m_capture.peer, key))
          m_capture.valid = false;
        ClearSubscriptionNL(*peer);
        RebalanceRolesNL();
      }
      return NetworkAudioPacketResult::HANDLED;
    }

    default:
      return NetworkAudioPacketResult::INVALID;
  }
}

NetworkAudioSendResult CNetworkAudio::Impl::SendPCM(
  const PlaybackTarget& target, uint16_t messageType,
  const void * payload, uint32_t payloadLength)
{
  /* CLOCK_STATE and PCM are an ordered pair. Keep both on the same reliable
   * stream until a future protocol version provides atomic datagram framing. */
  if (!(target.roles & LG_NET_AUDIO_ROLE_RELIABLE_PCM) ||
      payloadLength > target.info.maxReliablePayload)
    return NetworkAudioSendResult::FAILED;

  return Send(target.peer, false, messageType, LG_NET_ENVELOPE_FINAL,
    target.subscriptionEpoch, 0, payload, payloadLength);
}

AudioResult CNetworkAudio::Impl::BeginPlayback(
  const AudioPacket& packet, size_t size)
{
  if (!m_started)
    return AudioResult::UNAVAILABLE;
  if (m_failed)
    return AudioResult::FAILED;
  if (!m_stateKnown || !m_state.available || !m_state.active ||
      !(m_state.capabilities & AUDIO_CAP_PLAYBACK) ||
      !m_state.playback.formatValid ||
      m_state.playback.state != AudioStreamState::RUNNING)
    return AudioResult::UNAVAILABLE;
  if (packet.endpointGeneration != m_state.generation ||
      packet.streamGeneration != m_state.playback.generation)
    return AudioResult::STALE;
  if (!SameFormat(packet.format, m_state.playback.format))
    return AudioResult::UNAVAILABLE;

  PendingPlayback pending;
  pending.valid  = true;
  pending.packet = packet;
  pending.size   = size;
  try
  {
    pending.targets.reserve(m_peers.size());
    for (const PeerState& peer : m_peers)
    {
      if (!peer.subscribed ||
          !(peer.grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK) ||
          !PlaybackCurrentNL(peer))
        continue;

      LGNetAudioFormat format = {};
      if (!WireFormatNL(peer, true, format))
        continue;
      PlaybackTarget target;
      target.peer              = peer.peer;
      target.info              = peer.info;
      target.peerSerial        = peer.serial;
      target.subscriptionEpoch = peer.subscriptionEpoch;
      target.formatEpoch       = format.formatEpoch;
      target.maxPacketFrames   = format.maxPacketFrames;
      target.roles             = peer.grantedRoles;
      target.authority         = peer.feedbackAuthority;
      target.discontinuity     = peer.playbackGap;
      pending.targets.push_back(target);
    }
    std::stable_sort(pending.targets.begin(), pending.targets.end(),
      [](const PlaybackTarget& left, const PlaybackTarget& right)
      {
        return left.authority && !right.authority;
      });
  }
  catch (...)
  {
    return AudioResult::FAILED;
  }

  if (pending.targets.empty())
    return AudioResult::ACCEPTED;
  m_playback = std::move(pending);
  return AudioResult::BUSY;
}

uint64_t CNetworkAudio::Impl::PlaybackReadySerial(
  const PlaybackTarget& target)
{
  CSRWSharedLock lock(m_lock);
  const PeerState * peer = FindPeerNL(target.peer);
  if (!peer || peer->serial != target.peerSerial ||
      peer->subscriptionEpoch != target.subscriptionEpoch)
    return 0;
  return peer->sendReadySerial;
}

CNetworkAudio::Impl::BlockDisposition
CNetworkAudio::Impl::HandlePlaybackBlock(
  const PlaybackTarget& target, uint64_t readySerial)
{
  CSRWExclusiveLock lock(m_lock);
  PeerState * peer = FindPeerNL(target.peer);
  if (!peer || peer->serial != target.peerSerial ||
      peer->subscriptionEpoch != target.subscriptionEpoch ||
      !peer->feedbackAuthority)
    return BlockDisposition::STALE;
  if (peer->sendReadySerial != readySerial)
    return BlockDisposition::RETRY;

  m_playbackWaiting     = true;
  m_playbackWaitingPeer = target.peer;
  return BlockDisposition::WAIT;
}

AudioResult CNetworkAudio::Impl::ContinuePlayback(const uint8_t * data)
{
  while (m_playback.valid &&
      m_playback.target < m_playback.targets.size())
  {
    PlaybackTarget& target    =
      m_playback.targets[m_playback.target];
    bool            ready     = false;
    bool            authority = false;
    bool            gap       = target.discontinuity;
    {
      CSRWSharedLock lock(m_lock);
      const PeerState * peer = FindPeerNL(target.peer);
      if (!peer || peer->serial != target.peerSerial ||
          peer->subscriptionEpoch != target.subscriptionEpoch ||
          !peer->subscribed ||
          !(peer->grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK) ||
          !PlaybackCurrentNL(*peer))
      {
        ++m_playback.target;
        m_playback.frameOffset = 0;
        m_playback.chunkFrames = 0;
        m_playback.packetID    = 0;
        m_playback.clockSent   = false;
        continue;
      }
      ready     = peer->ready && peer->pumpStage == PumpStage::DONE;
      authority = target.authority && peer->feedbackAuthority;
      gap       = gap || peer->playbackGap;
    }

    if (!ready)
    {
      if (authority)
      {
        CSRWExclusiveLock lock(m_lock);
        m_playbackWaiting     = true;
        m_playbackWaitingPeer = target.peer;
        return AudioResult::BUSY;
      }
      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(target.peer);
      if (peer && peer->serial == target.peerSerial)
        peer->playbackGap = true;
      ++m_playback.target;
      m_playback.frameOffset = 0;
      m_playback.chunkFrames = 0;
      m_playback.packetID    = 0;
      m_playback.clockSent   = false;
      continue;
    }

    const uint32_t remaining =
      m_playback.packet.frames - m_playback.frameOffset;
    if (!m_playback.chunkFrames)
    {
      const uint32_t blockAlign = m_playback.packet.format.blockAlign;
      uint32_t       capacity   = 0;
      if (target.roles & LG_NET_AUDIO_ROLE_DATAGRAM_PCM)
      {
        const uint32_t bytes = target.info.maxDatagramPayload >
            LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE ?
          target.info.maxDatagramPayload -
            LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE : 0;
        capacity = (m_playback.packet.flags & AUDIO_PACKET_SILENT) ?
          target.maxPacketFrames : bytes / blockAlign;
      }
      if (!capacity &&
          (target.roles & LG_NET_AUDIO_ROLE_RELIABLE_PCM))
      {
        const uint32_t bytes = target.info.maxReliablePayload >
            LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE ?
          target.info.maxReliablePayload -
            LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE : 0;
        capacity = (m_playback.packet.flags & AUDIO_PACKET_SILENT) ?
          target.maxPacketFrames : bytes / blockAlign;
      }
      capacity = (std::min)(capacity, target.maxPacketFrames);
      capacity = (std::min)(capacity, AUDIO_MAX_FRAMES_PER_PACKET);
      if (!capacity)
      {
        CSRWExclusiveLock lock(m_lock);
        PeerState * peer = FindPeerNL(target.peer);
        if (peer && peer->serial == target.peerSerial)
          peer->playbackGap = true;
        ++m_playback.target;
        continue;
      }
      m_playback.chunkFrames = (std::min)(remaining, capacity);
      {
        CSRWExclusiveLock lock(m_lock);
        PeerState * peer = FindPeerNL(target.peer);
        if (!peer || peer->serial != target.peerSerial ||
            peer->subscriptionEpoch != target.subscriptionEpoch)
        {
          ++m_playback.target;
          m_playback.frameOffset = 0;
          m_playback.chunkFrames = 0;
          continue;
        }
        peer->playbackPacketID = NextSerial(peer->playbackPacketID);
        m_playback.packetID    = peer->playbackPacketID;
      }
    }

    const uint32_t frameOffset = m_playback.frameOffset;
    const uint32_t frames      = m_playback.chunkFrames;
    const uint64_t timeAdvance =
      static_cast<uint64_t>(frameOffset) * UINT64_C(1000000000) /
        m_playback.packet.format.sampleRate;
    const bool     clockValid  =
      (m_playback.packet.flags & AUDIO_PACKET_CLOCK_VALID) != 0;
    const bool     clockStable =
      (m_playback.packet.flags & AUDIO_PACKET_CLOCK_STABLE) != 0;

    LGNetAudioClockState clock = {};
    clock.streamID          = PLAYBACK_STREAM_ID;
    clock.direction         = LG_NET_AUDIO_DIRECTION_PLAYBACK;
    clock.flags             = clockValid ? LG_NET_AUDIO_CLOCK_VALID : 0;
    if (clockStable)
      clock.flags |= LG_NET_AUDIO_CLOCK_STABLE;
    clock.formatEpoch       = target.formatEpoch;
    clock.subscriptionEpoch = target.subscriptionEpoch;
    clock.packetID          = m_playback.packetID;
    clock.framePosition     = m_playback.packet.position + frameOffset;
    clock.timeNs            = clockValid ?
      m_playback.packet.timeNs + static_cast<int64_t>(timeAdvance) : 0;
    clock.rateQ32           = clockValid ? m_playback.packet.rateQ32 : 0;
    clock.targetRateQ32     = 0;

    std::array<uint8_t, LG_NET_AUDIO_CLOCK_STATE_WIRE_SIZE> clockPayload;
    if (!lgNetAudioClockStateEncode(clockPayload.data(),
          clockPayload.size(), &clock))
      return AudioResult::FAILED;
    if (!m_playback.clockSent)
    {
      const uint64_t readySerial = PlaybackReadySerial(target);
      const NetworkAudioSendResult result = SendPCM(target,
        LG_NET_AUDIO_MESSAGE_CLOCK_STATE, clockPayload.data(),
        static_cast<uint32_t>(clockPayload.size()));
      if (result != NetworkAudioSendResult::ACCEPTED)
      {
        if (authority && result == NetworkAudioSendResult::BLOCKED)
        {
          const BlockDisposition disposition =
            HandlePlaybackBlock(target, readySerial);
          if (disposition == BlockDisposition::RETRY)
            continue;
          if (disposition == BlockDisposition::WAIT)
          {
            if (!m_playback.blockedSince)
              m_playback.blockedSince = GetTickCount64();
            return AudioResult::BUSY;
          }
        }

        CSRWExclusiveLock lock(m_lock);
        PeerState * peer = FindPeerNL(target.peer);
        if (peer && peer->serial == target.peerSerial &&
            peer->subscriptionEpoch == target.subscriptionEpoch)
        {
          peer->playbackGap = true;
          if (authority && peer->feedbackAuthority)
          {
            peer->feedbackAuthority = false;
            peer->feedbackRevoked   = true;
            RebalanceRolesNL();
          }
        }
        ++m_playback.target;
        m_playback.frameOffset = 0;
        m_playback.chunkFrames = 0;
        m_playback.packetID    = 0;
        m_playback.clockSent   = false;
        continue;
      }
      m_playback.clockSent = true;
    }

    const int64_t packetTime = m_playback.packet.timeNs +
      static_cast<int64_t>(timeAdvance);
    LGNetAudioData audio = {};
    audio.streamID       = PLAYBACK_STREAM_ID;
    audio.direction      = LG_NET_AUDIO_DIRECTION_PLAYBACK;
    audio.formatEpoch    = target.formatEpoch;
    audio.packetID       = m_playback.packetID;
    audio.timestampNs    = packetTime > 0 ?
      static_cast<uint64_t>(packetTime) : MonotonicNs();
    audio.startFrame     = m_playback.packet.position + frameOffset;
    audio.frameCount     = frames;
    if (!frameOffset &&
        ((m_playback.packet.flags & AUDIO_PACKET_DISCONTINUITY) || gap))
      audio.flags |= LG_NET_AUDIO_DATA_DISCONTINUITY;
    if (m_playback.packet.flags & AUDIO_PACKET_SILENT)
      audio.flags |= LG_NET_AUDIO_DATA_SILENT;
    else
    {
      audio.dataLength = frames * m_playback.packet.format.blockAlign;
      audio.data       = data +
        static_cast<size_t>(frameOffset) *
          m_playback.packet.format.blockAlign;
    }

    const size_t         wireSize = lgNetAudioDataSize(&audio);
    std::vector<uint8_t> payload;
    try
    {
      payload.resize(wireSize);
    }
    catch (...)
    {
      return AudioResult::FAILED;
    }
    if (!wireSize || !lgNetAudioDataEncode(
          payload.data(), payload.size(), &audio))
      return AudioResult::FAILED;

    const uint64_t readySerial = PlaybackReadySerial(target);
    const NetworkAudioSendResult result = SendPCM(target,
      LG_NET_AUDIO_MESSAGE_PLAYBACK_DATA, payload.data(),
      static_cast<uint32_t>(payload.size()));
    if (result != NetworkAudioSendResult::ACCEPTED)
    {
      if (authority && result == NetworkAudioSendResult::BLOCKED)
      {
        m_playback.clockSent = false;
        const BlockDisposition disposition =
          HandlePlaybackBlock(target, readySerial);
        if (disposition == BlockDisposition::RETRY)
          continue;
        if (disposition == BlockDisposition::WAIT)
        {
          if (!m_playback.blockedSince)
            m_playback.blockedSince = GetTickCount64();
          return AudioResult::BUSY;
        }
      }

      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(target.peer);
      if (peer && peer->serial == target.peerSerial &&
          peer->subscriptionEpoch == target.subscriptionEpoch)
      {
        peer->playbackGap = true;
        if (authority && peer->feedbackAuthority)
        {
          peer->feedbackAuthority = false;
          peer->feedbackRevoked   = true;
          RebalanceRolesNL();
        }
      }
      ++m_playback.target;
      m_playback.frameOffset = 0;
      m_playback.chunkFrames = 0;
      m_playback.packetID    = 0;
      m_playback.clockSent   = false;
      continue;
    }

    {
      CSRWExclusiveLock lock(m_lock);
      PeerState * peer = FindPeerNL(target.peer);
      if (peer && peer->serial == target.peerSerial &&
          peer->subscriptionEpoch == target.subscriptionEpoch)
      {
        PacketRecord& record = peer->playbackPacket[
          peer->playbackPacketNext++ % RECENT_PACKET_SIZE];
        record.valid       = true;
        record.formatEpoch = target.formatEpoch;
        record.packetID    = m_playback.packetID;
        peer->playbackGap  = false;
      }
    }

    m_playback.blockedSince = 0;
    m_playback.frameOffset += frames;
    m_playback.chunkFrames  = 0;
    m_playback.packetID     = 0;
    m_playback.clockSent    = false;
    if (m_playback.frameOffset == m_playback.packet.frames)
    {
      ++m_playback.target;
      m_playback.frameOffset = 0;
    }
  }

  m_playback = {};
  {
    CSRWExclusiveLock lock(m_lock);
    m_playbackWaiting = false;
  }
  return AudioResult::ACCEPTED;
}

void CNetworkAudio::Impl::MarkFailed(const char * reason)
{
  if (!m_failed)
    DEBUG_ERROR("%s failed", reason);
  m_failed = true;
  if (m_playback.valid)
    m_playbackReadyPending = true;
}

void CNetworkAudio::Impl::NotifyFailure()
{
  CSRWSharedLock callbackLock(m_targetCallbackLock);
  IAudioTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_failed || m_failureNotified)
      return;
    m_failureNotified = true;
    target            = m_started ? m_target : nullptr;
  }
  if (target)
    target->AudioFailed();
}

void CNetworkAudio::Impl::NotifyPlaybackReady()
{
  CSRWSharedLock callbackLock(m_targetCallbackLock);
  IAudioTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_playbackReadyPending)
      return;
    m_playbackReadyPending = false;
    target                 = m_started ? m_target : nullptr;
  }
  if (target)
    target->PlaybackReceiveReady();
}

void CNetworkAudio::Impl::ProcessActivation()
{
  CSRWSharedLock callbackLock(m_targetCallbackLock);
  IAudioTarget * target = nullptr;
  bool           active = false;
  {
    CSRWSharedLock lock(m_lock);
    if (!m_started || !m_target || !m_stateKnown || m_failed)
      return;
    for (const PeerState& peer : m_peers)
      if (peer.subscribed)
      {
        active = true;
        break;
      }
    if (active == m_activationCommanded)
      return;
    target = m_target;
  }

  const AudioResult result = target->SetAudioActive(active);
  CSRWExclusiveLock lock(m_lock);
  if (!m_started || m_target != target)
    return;
  if (result == AudioResult::ACCEPTED ||
      (!active && (result == AudioResult::UNAVAILABLE ||
        result == AudioResult::STALE)))
    m_activationCommanded = active;
  else if (result == AudioResult::FAILED ||
      result == AudioResult::REJECTED)
    MarkFailed("network audio endpoint activation");
}

void CNetworkAudio::Impl::RetryCapture()
{
  CSRWSharedLock callbackLock(m_targetCallbackLock);
  IAudioTarget    * target     = nullptr;
  AudioPacket       packet     = {};
  const uint8_t   * data       = nullptr;
  size_t            size       = 0;
  NetworkAudioPeer  peerKey;
  uint64_t          peerSerial = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_capture.valid || m_capture.callback ||
        !m_started || !m_target)
      return;
    PeerState * peer = FindPeerNL(m_capture.peer);
    if (!peer || peer->serial != m_capture.peerSerial ||
        !peer->captureOwner || !peer->ready ||
        !CaptureCurrentNL(*peer) ||
        m_capture.packet.endpointGeneration != m_state.generation ||
        m_capture.packet.streamGeneration != m_state.capture.generation ||
        !SameFormat(m_capture.packet.format, m_state.capture.format))
    {
      if (peer)
        peer->captureGap = true;
      m_capture.valid = false;
      return;
    }

    m_capture.callback = true;
    target             = m_target;
    packet             = m_capture.packet;
    data               = m_capture.size ? m_capture.data.data() : nullptr;
    size               = m_capture.size;
    peerKey            = m_capture.peer;
    peerSerial         = m_capture.peerSerial;
  }

  const AudioResult result = target->SendCapture(packet, data, size);
  CSRWExclusiveLock lock(m_lock);
  m_capture.callback = false;
  PeerState * peer = FindPeerNL(peerKey);
  if (!peer || peer->serial != peerSerial ||
      !m_capture.valid || m_capture.peerSerial != peerSerial)
  {
    m_capture.valid = false;
    return;
  }
  if (result == AudioResult::BUSY)
    return;
  m_capture.valid = false;
  if (result == AudioResult::FAILED || result == AudioResult::REJECTED)
  {
    peer->captureRevoked = true;
    peer->captureGap     = true;
    RebalanceRolesNL();
  }
  else if (result != AudioResult::ACCEPTED)
    peer->captureGap = true;
}

void CNetworkAudio::Impl::RevokeStalledAuthority(uint64_t now)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_playback.valid || !m_playback.blockedSince ||
      !DeadlineReached(now,
        m_playback.blockedSince + AUTHORITY_STALL_MS) ||
      m_playback.target >= m_playback.targets.size())
    return;

  const PlaybackTarget& target =
    m_playback.targets[m_playback.target];
  PeerState * peer = FindPeerNL(target.peer);
  if (peer && peer->serial == target.peerSerial &&
      peer->feedbackAuthority)
  {
    DEBUG_WARN("Network audio feedback authority %u:%u stalled",
      peer->peer.client, peer->peer.generation);
    peer->feedbackAuthority = false;
    peer->feedbackRevoked   = true;
    peer->playbackGap       = true;
    RebalanceRolesNL();
  }
  m_playback.blockedSince   = 0;
  m_playbackWaiting         = false;
  m_playbackReadyPending    = true;
}

CNetworkAudio::CNetworkAudio() :
  m_impl(new (std::nothrow) Impl())
{
}

CNetworkAudio::~CNetworkAudio()
{
  if (!m_impl)
    return;
  Stop();
  DetachSender();
}

bool CNetworkAudio::AttachSender(INetworkAudioSender& sender)
{
  if (!m_impl)
    return false;
  CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_sender)
    return m_impl->m_sender == &sender;
  m_impl->m_sender = &sender;
  return true;
}

void CNetworkAudio::DetachSender()
{
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  m_impl->m_sender = nullptr;
  m_impl->m_peers.clear();
  m_impl->m_playback        = {};
  m_impl->m_capture         = {};
  m_impl->m_playbackWaiting = false;
}

bool CNetworkAudio::PeerConnected(const NetworkAudioPeer& key)
{
  if (!m_impl || !key.client || !key.generation)
    return false;

  NetworkAudioPeerInfo info;
  {
    CSRWSharedLock callbackLock(m_impl->m_senderCallbackLock);
    INetworkAudioSender * sender;
    {
      CSRWSharedLock lock(m_impl->m_lock);
      sender = m_impl->m_sender;
    }
    if (!sender || !sender->AudioPeerInfo(key, info))
      return false;
  }

  if (!AudioVersionSupported(info.serviceVersion) ||
      !(info.capabilityFlags & LG_NET_CAPABILITY_RELIABLE) ||
      info.maxReliablePayload <
        LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE + AUDIO_MAX_PCM_BYTES)
    return false;

  CSRWExclusiveLock lock(m_impl->m_lock);
  Impl::PeerState * existing = m_impl->FindPeerNL(key);
  if (existing)
  {
    existing->info = info;
    return true;
  }

  try
  {
    Impl::PeerState peer;
    peer.peer   = key;
    peer.info   = info;
    peer.serial = NextSerial(m_impl->m_nextPeerSerial);

    m_impl->m_nextPeerSerial = peer.serial;
    m_impl->m_peers.push_back(peer);
  }
  catch (...)
  {
    return false;
  }
  return true;
}

void CNetworkAudio::PeerDisconnected(const NetworkAudioPeer& key)
{
  if (!m_impl)
    return;
  CSRWExclusiveLock lock(m_impl->m_lock);
  const auto found = std::find_if(m_impl->m_peers.begin(),
    m_impl->m_peers.end(), [&key](const Impl::PeerState& peer)
    {
      return SamePeer(peer.peer, key);
    });
  if (found == m_impl->m_peers.end())
    return;
  if (m_impl->m_capture.valid && SamePeer(m_impl->m_capture.peer, key))
    m_impl->m_capture.valid = false;
  if (m_impl->m_playbackWaiting &&
      SamePeer(m_impl->m_playbackWaitingPeer, key))
  {
    m_impl->m_playbackWaiting      = false;
    m_impl->m_playbackReadyPending = true;
  }
  m_impl->m_peers.erase(found);
  m_impl->RebalanceRolesNL();
}

NetworkAudioPacketResult CNetworkAudio::HandlePacket(
  const NetworkAudioPeer& key, const LGNetPacketView& packet,
  bool datagram)
{
  if (!m_impl || packet.envelope.service != LG_NET_SERVICE_AUDIO ||
      !AudioVersionSupported(packet.envelope.serviceVersion) ||
      packet.payloadLength != packet.envelope.payloadLength)
    return NetworkAudioPacketResult::INVALID;

  const uint64_t now = GetTickCount64();
  if (packet.envelope.messageType == LG_NET_AUDIO_MESSAGE_SUBSCRIBE)
  {
    if (datagram || packet.envelope.componentEpoch ||
        !packet.envelope.requestID ||
        !ReliableFlags(packet.envelope,
          LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL))
      return NetworkAudioPacketResult::INVALID;
    CSRWExclusiveLock lock(m_impl->m_lock);
    Impl::PeerState * peer = m_impl->FindPeerNL(key);
    if (!peer || peer->info.serviceVersion !=
          packet.envelope.serviceVersion)
      return NetworkAudioPacketResult::IGNORED;
    return m_impl->HandleSubscribe(*peer, packet, now);
  }

  {
    CSRWSharedLock lock(m_impl->m_lock);
    const Impl::PeerState * peer = m_impl->FindPeerNL(key);
    if (!peer || !peer->hasRequest ||
        !lgNetAudioEnvelopeMatchesSubscription(
          &packet.envelope, peer->subscriptionEpoch))
      return NetworkAudioPacketResult::IGNORED;
  }

  const NetworkAudioPacketResult result =
    m_impl->HandleCurrentPacket(key, packet, datagram, now);
  CSRWExclusiveLock processLock =
    CSRWExclusiveLock::Try(m_impl->m_processLock);
  if (processLock)
    m_impl->NotifyPlaybackReady();
  return result;
}

void CNetworkAudio::Process(uint64_t nowMs)
{
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    bool rebalance = false;
    for (Impl::PeerState& peer : m_impl->m_peers)
      if (peer.hasRequest && !peer.expiring &&
          DeadlineReached(nowMs, peer.deadline))
      {
        DEBUG_WARN("Network audio subscriber %u:%u expired",
          peer.peer.client, peer.peer.generation);
        if (m_impl->m_capture.valid &&
            SamePeer(m_impl->m_capture.peer, peer.peer))
          m_impl->m_capture.valid = false;
        peer.expiring = true;
        peer.deadline = 0;
        rebalance     = true;
      }
    if (rebalance)
      m_impl->RebalanceRolesNL();
  }

  m_impl->RevokeStalledAuthority(nowMs);
  m_impl->ProcessActivation();
  m_impl->PumpAll();
  m_impl->NotifyPlaybackReady();
  m_impl->NotifyFailure();
}

void CNetworkAudio::SendReady(const NetworkAudioPeer& key,
  LGNetService service, bool datagram)
{
  if (!m_impl || service != LG_NET_SERVICE_AUDIO)
    return;
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    Impl::PeerState * peer = m_impl->FindPeerNL(key);
    if (peer)
      peer->sendReadySerial = NextSerial(peer->sendReadySerial);
    if (m_impl->m_playbackWaiting &&
        SamePeer(m_impl->m_playbackWaitingPeer, key))
    {
      m_impl->m_playbackWaiting      = false;
      m_impl->m_playbackReadyPending = true;
    }
  }
  CSRWExclusiveLock processLock =
    CSRWExclusiveLock::Try(m_impl->m_processLock);
  if (processLock)
  {
    if (!datagram)
      m_impl->PumpPeer(key);
    m_impl->NotifyPlaybackReady();
  }
}

bool CNetworkAudio::Start(IAudioTarget& target)
{
  if (!m_impl)
    return false;
  CSRWExclusiveLock lifecycleLock(m_impl->m_lifecycleLock);
  CSRWExclusiveLock callbackLock(m_impl->m_targetCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_started)
    return m_impl->m_target == &target;
  m_impl->m_target              = &target;
  m_impl->m_started             = true;
  m_impl->m_stateKnown          = false;
  m_impl->m_failed              = false;
  m_impl->m_failureNotified     = false;
  m_impl->m_activationCommanded = false;
  return true;
}

void CNetworkAudio::Stop()
{
  if (!m_impl)
    return;
  CSRWExclusiveLock lifecycleLock(m_impl->m_lifecycleLock);
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock callbackLock(m_impl->m_targetCallbackLock);

  IAudioTarget * target;
  bool           deactivate;
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (!m_impl->m_started && !m_impl->m_target)
      return;
    target     = m_impl->m_target;
    deactivate = m_impl->m_activationCommanded;
    for (Impl::PeerState& peer : m_impl->m_peers)
      m_impl->ClearSubscriptionNL(peer);
    m_impl->m_playback             = {};
    m_impl->m_capture              = {};
    m_impl->m_started              = false;
    m_impl->m_target               = nullptr;
    m_impl->m_stateKnown           = false;
    m_impl->m_activationCommanded  = false;
    m_impl->m_playbackReadyPending = false;
    m_impl->m_playbackWaiting      = false;
  }
  callbackLock.Unlock();
  processLock.Unlock();
  if (deactivate && target)
    (void)target->SetAudioActive(false);
}

void CNetworkAudio::AudioState(const AudioEndpointStatus& state)
{
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (!m_impl->m_started)
    return;

  const uint64_t previousEndpoint = m_impl->m_state.generation;
  const uint64_t previousPlayback = m_impl->m_state.playback.generation;
  const uint64_t previousCapture  = m_impl->m_state.capture.generation;
  m_impl->m_state                 = state;
  m_impl->m_stateKnown            = true;
  if (state.generation != previousEndpoint ||
      state.playback.generation != previousPlayback)
    for (Impl::PeerState& peer : m_impl->m_peers)
      peer.feedbackRevoked = false;
  if (state.generation != previousEndpoint ||
      state.capture.generation != previousCapture)
    for (Impl::PeerState& peer : m_impl->m_peers)
      peer.captureRevoked = false;

  if (m_impl->m_playback.valid &&
      (m_impl->m_playback.packet.endpointGeneration != state.generation ||
        m_impl->m_playback.packet.streamGeneration !=
          state.playback.generation ||
        !state.playback.formatValid ||
        !SameFormat(m_impl->m_playback.packet.format,
          state.playback.format)))
  {
    m_impl->m_playback             = {};
    m_impl->m_playbackReadyPending = true;
    m_impl->m_playbackWaiting      = false;
  }
  if (m_impl->m_capture.valid &&
      (m_impl->m_capture.packet.endpointGeneration != state.generation ||
        m_impl->m_capture.packet.streamGeneration !=
          state.capture.generation ||
        !state.capture.formatValid ||
        !SameFormat(m_impl->m_capture.packet.format,
          state.capture.format)))
    m_impl->m_capture.valid = false;

  m_impl->RebalanceRolesNL();
  for (Impl::PeerState& peer : m_impl->m_peers)
    if (peer.hasRequest)
      m_impl->SchedulePumpNL(peer, false);
}

AudioResult CNetworkAudio::SendPlayback(
  const AudioPacket& packet, const uint8_t * data, size_t size)
{
  constexpr uint32_t PACKET_FLAGS =
    AUDIO_PACKET_DISCONTINUITY | AUDIO_PACKET_SILENT |
    AUDIO_PACKET_CLOCK_VALID | AUDIO_PACKET_CLOCK_STABLE;
  if (!m_impl || (packet.flags & ~PACKET_FLAGS) ||
      ((packet.flags & AUDIO_PACKET_CLOCK_STABLE) &&
        !(packet.flags & AUDIO_PACKET_CLOCK_VALID)) ||
      (!(packet.flags & AUDIO_PACKET_CLOCK_VALID) &&
        (packet.position || packet.timeNs || packet.rateQ32)) ||
      packet.rateQ32 > LG_NET_AUDIO_MAX_RATE_Q32 ||
      !packet.frames || packet.frames > AUDIO_MAX_FRAMES_PER_PACKET ||
      !ValidFormat(packet.format))
    return AudioResult::REJECTED;

  const uint64_t lastFrameOffset = packet.frames - 1U;
  const uint64_t maxTimeAdvance  =
    lastFrameOffset * UINT64_C(1000000000) / packet.format.sampleRate;
  if (packet.position > UINT64_MAX - lastFrameOffset ||
      ((packet.flags & AUDIO_PACKET_CLOCK_VALID) &&
        packet.timeNs > INT64_MAX - static_cast<int64_t>(maxTimeAdvance)))
    return AudioResult::REJECTED;

  const size_t pcmBytes = static_cast<size_t>(packet.frames) *
    packet.format.blockAlign;
  const bool silent = (packet.flags & AUDIO_PACKET_SILENT) != 0;
  if (pcmBytes > AUDIO_MAX_PCM_BYTES ||
      (silent ? size != 0 : (size != pcmBytes || !data)))
    return AudioResult::REJECTED;

  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (m_impl->m_playback.valid)
    {
      if (m_impl->m_playback.size != size ||
          !SamePacket(m_impl->m_playback.packet, packet))
      {
        DEBUG_WARN("Network audio playback retry did not match");
        m_impl->m_playback = {};
        return AudioResult::REJECTED;
      }
    }
    else
    {
      const AudioResult begin = m_impl->BeginPlayback(packet, size);
      if (begin != AudioResult::BUSY)
        return begin;
    }
    m_impl->m_playbackWaiting = false;
  }

  const AudioResult result = m_impl->ContinuePlayback(data);
  if (result == AudioResult::FAILED || result == AudioResult::REJECTED)
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    m_impl->m_playback = {};
    if (result == AudioResult::FAILED)
      m_impl->MarkFailed("network audio playback");
  }
  return result;
}

void CNetworkAudio::CaptureReceiveReady()
{
  if (m_impl)
    m_impl->RetryCapture();
}
