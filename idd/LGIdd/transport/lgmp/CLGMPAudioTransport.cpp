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

#include <ntstatus.h>

#include "transport/lgmp/CLGMPAudioTransport.h"

#include <wudfwdm.h>

#include "CDebug.h"
#include "WCWrite.h"
#include "transport/lgmp/CLGMPControl.h"
#include "transport/lgmp/CLGMPHost.h"

#include <avrt.h>
#include <initializer_list>
#include <limits>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
  static_assert(AUDIO_MAX_CHANNELS == KVMFR_AUDIO_MAX_CHANNELS,
    "audio channel limits differ");
  static_assert(AUDIO_STREAM_MAX_CHANNELS ==
      KVMFR_AUDIO_STREAM_MAX_CHANNELS,
    "audio stream channel limits differ");
  static_assert(AUDIO_MAX_FRAMES_PER_PACKET ==
      KVMFR_AUDIO_MAX_FRAMES_PER_PACKET,
    "audio packet limits differ");
  static_assert(AUDIO_MAX_PCM_BYTES == KVMFR_AUDIO_PCM_BYTES,
    "audio PCM limits differ");
  static_assert(static_cast<uint32_t>(AUDIO_CAP_PLAYBACK) ==
      static_cast<uint32_t>(KVMFR_AUDIO_CAP_PLAYBACK) &&
      static_cast<uint32_t>(AUDIO_CAP_CAPTURE) ==
        static_cast<uint32_t>(KVMFR_AUDIO_CAP_CAPTURE) &&
      static_cast<uint32_t>(AUDIO_CAP_CLOCK_FEEDBACK) ==
        static_cast<uint32_t>(KVMFR_AUDIO_CAP_CLOCK_FEEDBACK),
    "audio capability bits differ");
  static_assert(sizeof(LGMPStreamDescriptor) ==
      sizeof(KVMFRStreamDescriptor),
    "LGMP and KVMFR stream descriptor sizes differ");

  constexpr uint32_t AUDIO_CAPABILITY_MASK =
    KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE |
    KVMFR_AUDIO_CAP_CLOCK_FEEDBACK;
  constexpr uint32_t AUDIO_DIRECTION_MASK =
    KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE;
  constexpr uint32_t AUDIO_PACKET_FLAG_MASK =
    AUDIO_PACKET_DISCONTINUITY | AUDIO_PACKET_SILENT |
    AUDIO_PACKET_CLOCK_VALID | AUDIO_PACKET_CLOCK_STABLE;
  constexpr uint32_t WIRE_PACKET_FLAG_MASK =
    KVMFR_AUDIO_MESSAGE_DISCONTINUITY |
    KVMFR_AUDIO_MESSAGE_SILENT |
    KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
    KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;
  constexpr size_t AUDIO_SHARED_MEMORY_PREFLIGHT =
    2U * 1024U * 1024U;

  bool ArmAudioPollTimer(HANDLE timer, uint32_t waitUs)
  {
    LARGE_INTEGER due = {};
    due.QuadPart = -static_cast<LONGLONG>(waitUs) * 10;
    return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
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

  uint64_t NextSerial(uint64_t serial)
  {
    ++serial;
    return serial ? serial : 1;
  }

  bool RequestNewer(uint32_t candidate, uint32_t previous)
  {
    return candidate != previous &&
      static_cast<int32_t>(candidate - previous) > 0;
  }
}

CLGMPAudioTransport::~CLGMPAudioTransport()
{
  TransportStop();

  CSRWExclusiveLock lock(m_lock);
  for (ResponseMemory& response : m_responseMemory)
    lgmpHostMemFree(&response.memory);
  FreeStreams();
  m_initialized = false;
}

bool CLGMPAudioTransport::SameFormat(
  const AudioFormat& left, const AudioFormat& right)
{
  return left.sampleRate == right.sampleRate &&
    left.channelMask == right.channelMask &&
    left.channels == right.channels &&
    left.containerBits == right.containerBits &&
    left.validBits == right.validBits &&
    left.blockAlign == right.blockAlign;
}

bool CLGMPAudioTransport::SamePacket(
  const AudioPacket& left, const AudioPacket& right)
{
  return left.endpointGeneration == right.endpointGeneration &&
    left.streamGeneration == right.streamGeneration &&
    left.flags == right.flags && left.frames == right.frames &&
    left.position == right.position && left.timeNs == right.timeNs &&
    left.rateQ32 == right.rateQ32 &&
    SameFormat(left.format, right.format);
}

bool CLGMPAudioTransport::ValidFormat(const AudioFormat& format)
{
  if (!format.sampleRate ||
      format.sampleRate > KVMFR_AUDIO_MAX_SAMPLE_RATE ||
      !format.channels ||
      format.channels > KVMFR_AUDIO_STREAM_MAX_CHANNELS ||
      !format.channelMask ||
      (format.channelMask & ~KVMFR_AUDIO_CHANNEL_POSITION_MASK) ||
      ChannelCount(format.channelMask) != format.channels)
    return false;

  const bool s16 = format.containerBits == 16 && format.validBits == 16;
  const bool s24 = format.containerBits == 24 && format.validBits == 24;
  const uint16_t bytes = static_cast<uint16_t>(
    format.channels * (format.containerBits / 8U));
  return (s16 || s24) && format.blockAlign == bytes;
}

KVMFRAudioFormat CLGMPAudioTransport::WireFormat(
  const AudioFormat& format)
{
  KVMFRAudioFormat result = {};
  result.version       = KVMFR_AUDIO_FORMAT_VERSION;
  result.size          = sizeof(result);
  result.sampleFormat  = format.containerBits == 16 ?
    KVMFR_AUDIO_SAMPLE_S16_LE : KVMFR_AUDIO_SAMPLE_S24_LE;
  result.sampleRate    = format.sampleRate;
  result.channelMask   = format.channelMask;
  result.channelCount  = format.channels;
  result.validBits     = format.validBits;
  result.containerBits = format.containerBits;
  result.blockAlign    = format.blockAlign;
  return result;
}

KVMFRAudioStreamState CLGMPAudioTransport::WireState(
  AudioStreamState state)
{
  switch (state)
  {
    case AudioStreamState::ACQUIRED:
      return KVMFR_AUDIO_STREAM_ACQUIRED;
    case AudioStreamState::PAUSED:
      return KVMFR_AUDIO_STREAM_PAUSED;
    case AudioStreamState::RUNNING:
      return KVMFR_AUDIO_STREAM_RUNNING;
    default:
      return KVMFR_AUDIO_STREAM_STOPPED;
  }
}

KVMFRStreamDescriptor CLGMPAudioTransport::WireDescriptor(
  const LGMPStreamDescriptor& descriptor)
{
  KVMFRStreamDescriptor wire = {};
  wire.magic      = descriptor.magic;
  wire.version    = descriptor.version;
  wire.size       = descriptor.size;
  wire.offset     = descriptor.offset;
  wire.regionSize = descriptor.regionSize;
  wire.direction  = descriptor.direction;
  wire.policy     = descriptor.policy;
  wire.slotCount  = descriptor.slotCount;
  wire.slotSize   = descriptor.slotSize;
  return wire;
}

bool CLGMPAudioTransport::InitializeStreams()
{
  const LGMPStreamConfig hostToClient =
  {
    LGMP_STREAM_HOST_TO_CLIENT,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_AUDIO_STREAM_SLOT_COUNT,
    static_cast<uint32_t>(KVMFR_AUDIO_STREAM_SLOT_BYTES),
  };
  LGMP_STATUS status = m_host.CreateStream(
    hostToClient, &m_hostToClientStream);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to create host-to-client audio stream: %s",
      lgmpStatusString(status));
    return false;
  }

  const LGMPStreamConfig clientToHost =
  {
    LGMP_STREAM_CLIENT_TO_HOST,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_AUDIO_STREAM_SLOT_COUNT,
    static_cast<uint32_t>(KVMFR_AUDIO_STREAM_SLOT_BYTES),
  };
  status = m_host.CreateStream(clientToHost, &m_clientToHostStream);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to create client-to-host audio stream: %s",
      lgmpStatusString(status));
    lgmpHostStreamFree(&m_hostToClientStream);
    return false;
  }

  LGMPStreamDescriptor descriptor = {};
  lgmpHostStreamGetDescriptor(m_hostToClientStream, &descriptor);
  m_hostToClientDescriptor = WireDescriptor(descriptor);
  lgmpHostStreamGetDescriptor(m_clientToHostStream, &descriptor);
  m_clientToHostDescriptor = WireDescriptor(descriptor);
  return true;
}

void CLGMPAudioTransport::FreeStreams()
{
  lgmpHostStreamFree(&m_clientToHostStream);
  lgmpHostStreamFree(&m_hostToClientStream);
  m_clientToHostDescriptor = {};
  m_hostToClientDescriptor = {};
}

DWORD CALLBACK CLGMPAudioTransport::InboundThreadProc(void * context)
{
  static_cast<CLGMPAudioTransport *>(context)->InboundThread();
  return 0;
}

bool CLGMPAudioTransport::StartInboundWorker()
{
  m_inboundStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_inboundWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  m_inboundPollTimer = CreateWaitableTimerExW(nullptr, nullptr,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!m_inboundPollTimer)
    m_inboundPollTimer = CreateWaitableTimerExW(
      nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  if (!m_inboundStopEvent || !m_inboundWakeEvent ||
      !m_inboundPollTimer)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create LGMP audio worker resources");
    if (m_inboundPollTimer)
      CloseHandle(m_inboundPollTimer);
    if (m_inboundWakeEvent)
      CloseHandle(m_inboundWakeEvent);
    if (m_inboundStopEvent)
      CloseHandle(m_inboundStopEvent);
    m_inboundPollTimer = nullptr;
    m_inboundWakeEvent = nullptr;
    m_inboundStopEvent = nullptr;
    return false;
  }

  m_inboundThread = CreateThread(nullptr, 0, InboundThreadProc,
    this, 0, nullptr);
  if (!m_inboundThread)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create LGMP audio worker");
    CloseHandle(m_inboundPollTimer);
    CloseHandle(m_inboundWakeEvent);
    CloseHandle(m_inboundStopEvent);
    m_inboundPollTimer = nullptr;
    m_inboundWakeEvent = nullptr;
    m_inboundStopEvent = nullptr;
    return false;
  }
  return true;
}

void CLGMPAudioTransport::StopInboundWorker()
{
  HANDLE thread = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    m_inboundStopping = true;
    thread = m_inboundThread;
    if (m_inboundStopEvent)
      SetEvent(m_inboundStopEvent);
  }
  if (thread)
    WaitForSingleObject(thread, INFINITE);

  CSRWExclusiveLock lock(m_lock);
  if (m_inboundThread)
    CloseHandle(m_inboundThread);
  if (m_inboundPollTimer)
    CloseHandle(m_inboundPollTimer);
  if (m_inboundWakeEvent)
    CloseHandle(m_inboundWakeEvent);
  if (m_inboundStopEvent)
    CloseHandle(m_inboundStopEvent);
  m_inboundThread    = nullptr;
  m_inboundPollTimer = nullptr;
  m_inboundWakeEvent = nullptr;
  m_inboundStopEvent = nullptr;
}

void CLGMPAudioTransport::InboundThread()
{
  DWORD avTask = 0;
  HANDLE avTaskHandle =
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &avTask);
  if (avTaskHandle &&
      !AvSetMmThreadPriority(avTaskHandle, AVRT_PRIORITY_HIGH))
    DEBUG_WARN("Failed to raise audio ingress MMCSS priority: %lu",
      GetLastError());

  LGMPStreamPollState streamPoll = {};
  LGMPStreamPollConfig pollConfig = {};
  pollConfig.spinCount = 64U;
  pollConfig.minWaitUs = 25U;
  pollConfig.maxWaitUs = 250U;
  const LGMP_STATUS pollStatus = lgmpStreamPollInit(&streamPoll,
    pollConfig);
  if (pollStatus != LGMP_OK)
  {
    DEBUG_ERROR("Failed to initialize LGMP audio polling: %s",
      lgmpStatusString(pollStatus));
    if (avTaskHandle)
      AvRevertMmThreadCharacteristics(avTaskHandle);
    return;
  }

  const HANDLE idleWaits[] =
    { m_inboundStopEvent, m_inboundWakeEvent };
  const HANDLE activeWaits[] =
    { m_inboundStopEvent, m_inboundWakeEvent, m_inboundPollTimer };
  for (;;)
  {
    if (WaitForSingleObject(m_inboundStopEvent, 0) == WAIT_OBJECT_0)
      break;

    uint64_t before = 0;
    uint64_t hostBefore = 0;
    bool active = false;
    bool playbackCreditAcquired = false;
    {
      CSRWSharedLock lock(m_lock);
      before = m_inboundActivitySerial;
      hostBefore = m_hostSequence;
      active = m_ownerState != OwnerState::NONE;
    }

    if (active)
    {
      DrainInbound();
      {
        CSRWExclusiveLock lock(m_lock);
        const bool hadPlaybackCredit = m_playbackCreditValid;
        PumpState();
        CheckPlaybackCredit();
        playbackCreditAcquired = !hadPlaybackCredit &&
          m_playbackCreditValid;
      }
      NotifyPlaybackReady();
    }

    {
      CSRWSharedLock lock(m_lock);
      if (before != m_inboundActivitySerial ||
          hostBefore != m_hostSequence || playbackCreditAcquired)
        lgmpStreamPollActivity(&streamPoll);
      active = m_ownerState != OwnerState::NONE;
    }

    if (!active)
    {
      const DWORD wait = WaitForMultipleObjects(
        _countof(idleWaits), idleWaits, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0)
        break;
      if (wait != WAIT_OBJECT_0 + 1)
      {
        DEBUG_ERROR_HR(GetLastError(),
          "LGMP audio worker wait failed");
        break;
      }
      lgmpStreamPollActivity(&streamPoll);
      continue;
    }

    const uint32_t waitUs = lgmpStreamPollIdle(&streamPoll);
    if (!waitUs)
      continue;
    if (!ArmAudioPollTimer(m_inboundPollTimer, waitUs))
    {
      DEBUG_ERROR_HR(GetLastError(),
        "Failed to arm LGMP audio timer");
      break;
    }

    const DWORD wait = WaitForMultipleObjects(
      _countof(activeWaits), activeWaits, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_OBJECT_0 + 1)
    {
      lgmpStreamPollActivity(&streamPoll);
      continue;
    }
    if (wait != WAIT_OBJECT_0 + 2)
    {
      DEBUG_ERROR_HR(GetLastError(),
        "LGMP audio worker wait failed");
      break;
    }
  }

  if (avTaskHandle)
    AvRevertMmThreadCharacteristics(avTaskHandle);
}

bool CLGMPAudioTransport::Initialize()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  if (m_initialized)
    return true;

  // LGMP shared allocations are monotonic. Reserve conservatively before
  // the first one so ordinary shared-memory exhaustion cannot strand a
  // partial optional layout. A later local handle allocation can still fail
  // with LGMP_ERR_NO_MEM, but that is independent of shared capacity.
  if (m_host.Available() < AUDIO_SHARED_MEMORY_PREFLIGHT)
  {
    DEBUG_WARN("LGMP audio requires at least %zu bytes of available "
      "shared memory", AUDIO_SHARED_MEMORY_PREFLIGHT);
    return false;
  }
  if (!InitializeStreams())
    return false;

  LGMP_STATUS status = LGMP_OK;
  for (ResponseMemory& response : m_responseMemory)
  {
    status = m_host.Allocate(sizeof(KVMFRAudioStreamResponse),
      &response.memory);
    if (status != LGMP_OK)
      break;
    memset(lgmpHostMemPtr(response.memory), 0,
      sizeof(KVMFRAudioStreamResponse));
  }
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to allocate audio response memory: %s",
      lgmpStatusString(status));
    for (ResponseMemory& response : m_responseMemory)
      lgmpHostMemFree(&response.memory);
    FreeStreams();
    return false;
  }

  m_state.generation = 1;
  m_initialized = true;
  m_inboundStopping = false;
  if (!StartInboundWorker())
  {
    m_initialized = false;
    for (ResponseMemory& response : m_responseMemory)
      lgmpHostMemFree(&response.memory);
    FreeStreams();
    return false;
  }
  return true;
}

bool CLGMPAudioTransport::BindStreams(uint32_t clientID)
{
  LGMP_STATUS status = lgmpHostStreamBind(
    m_hostToClientStream, clientID, nullptr);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to bind host-to-client audio stream for client "
      "%u: %s", clientID, lgmpStatusString(status));
    return false;
  }

  status = lgmpHostStreamBind(m_clientToHostStream, clientID, nullptr);
  if (status == LGMP_OK)
    return true;

  DEBUG_WARN("Failed to bind client-to-host audio stream for client "
    "%u: %s", clientID, lgmpStatusString(status));
  const LGMP_STATUS rollback =
    lgmpHostStreamUnbind(m_hostToClientStream);
  if (rollback != LGMP_OK && rollback != LGMP_ERR_STREAM_UNBOUND)
    DEBUG_WARN("Failed to roll back the audio stream binding: %s",
      lgmpStatusString(rollback));
  return false;
}

bool CLGMPAudioTransport::TryUnbindStreams()
{
  bool complete = true;
  for (PLGMPHostStream stream :
      { m_clientToHostStream, m_hostToClientStream })
  {
    if (!stream)
      continue;
    const LGMP_STATUS status = lgmpHostStreamUnbind(stream);
    if (status == LGMP_ERR_STREAM_BUSY)
      complete = false;
    else if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
    {
      Fail("lgmpHostStreamUnbind", status);
      complete = false;
    }
  }
  return complete;
}

void CLGMPAudioTransport::ForceUnbindStreams()
{
  for (PLGMPHostStream stream :
      { m_clientToHostStream, m_hostToClientStream })
  {
    if (!stream)
      continue;
    const LGMP_STATUS status = lgmpHostStreamForceUnbind(stream);
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
      DEBUG_WARN("Failed to force-unbind an audio stream: %s",
        lgmpStatusString(status));
  }
}

bool CLGMPAudioTransport::ValidateClaim(
  const KVMFRAudioClaim& claim) const
{
  if (claim.messageType != KVMFR_MESSAGE_AUDIO_CLAIM ||
      claim.version != KVMFR_AUDIO_CONTROL_VERSION ||
      claim.size != sizeof(claim) || !claim.requestGeneration ||
      claim.flags != KVMFR_AUDIO_CLAIM_BIND || !claim.claimToken ||
      !claim.protocolMin || claim.protocolMin > claim.protocolMax ||
      claim.protocolMin > KVMFR_AUDIO_PROTOCOL_MAX ||
      claim.protocolMax < KVMFR_AUDIO_PROTOCOL_MIN ||
      (claim.clientCapabilities & ~AUDIO_CAPABILITY_MASK) ||
      !(claim.clientCapabilities & AUDIO_DIRECTION_MASK) ||
      claim.reserved0)
    return false;

  for (uint64_t reserved : claim.reserved)
    if (reserved)
      return false;
  return true;
}

bool CLGMPAudioTransport::ResponsePending(uint32_t clientID,
  const KVMFRAudioStreamResponse& candidate) const
{
  for (const PendingResponse& pending : m_pendingResponse)
    if (pending.valid && pending.clientID == clientID &&
        pending.response.requestGeneration ==
          candidate.requestGeneration &&
        pending.response.claimToken == candidate.claimToken &&
        pending.response.result == candidate.result)
      return true;

  for (const ResponseMemory& response : m_responseMemory)
    if (response.valid && response.clientID == clientID &&
        response.requestGeneration == candidate.requestGeneration &&
        response.claimToken == candidate.claimToken &&
        response.result == candidate.result)
      return true;
  return false;
}

bool CLGMPAudioTransport::QueueResponse(uint32_t clientID,
  const KVMFRAudioStreamResponse& response)
{
  if (ResponsePending(clientID, response))
    return true;

  PendingResponse * available = nullptr;
  PendingResponse * sameClient = nullptr;
  for (PendingResponse& pending : m_pendingResponse)
  {
    if (pending.valid && pending.clientID == clientID)
    {
      if (pending.response.result == KVMFR_AUDIO_RESPONSE_BOUND)
        return false;
      if (!sameClient)
        sameClient = &pending;
      continue;
    }
    if (!pending.valid && !available)
      available = &pending;
  }
  if (sameClient)
    available = sameClient;
  if (!available)
    return false;

  available->valid    = true;
  available->clientID = clientID;
  available->response = response;
  return true;
}

void CLGMPAudioTransport::CleanupResponseMemory()
{
  for (ResponseMemory& response : m_responseMemory)
  {
    if (!response.valid ||
        m_control.PayloadPending(response.memory))
      continue;
    response.valid             = false;
    response.clientID          = 0;
    response.requestGeneration = 0;
    response.claimToken        = 0;
    response.result            = 0;
  }
}

void CLGMPAudioTransport::PumpResponses()
{
  CleanupResponseMemory();
  for (PendingResponse& pending : m_pendingResponse)
  {
    if (!pending.valid)
      continue;

    ResponseMemory * available = nullptr;
    for (ResponseMemory& response : m_responseMemory)
      if (response.memory && !response.valid)
      {
        available = &response;
        break;
      }
    if (!available)
      return;

    memcpy(lgmpHostMemPtr(available->memory), &pending.response,
      sizeof(pending.response));
    WCWrite::Flush();
    unsigned recipients = 0;
    const LGMP_STATUS status = m_control.PostForClient(
      KVMFR_AUDIO_CONTROL_UDATA, available->memory,
      pending.clientID, recipients);
    if (status == LGMP_ERR_QUEUE_FULL)
      return;
    if (status != LGMP_OK)
    {
      Fail("lgmpHostQueuePostForClients(audio)", status);
      return;
    }

    const bool bound =
      pending.response.result == KVMFR_AUDIO_RESPONSE_BOUND;
    if (recipients)
    {
      available->valid             = true;
      available->clientID          = pending.clientID;
      available->requestGeneration =
        pending.response.requestGeneration;
      available->claimToken        = pending.response.claimToken;
      available->result            = pending.response.result;
      if (bound && m_ownerState == OwnerState::BOUND &&
          m_ownerClientID == pending.clientID &&
          m_ownerGeneration == pending.response.requestGeneration &&
          m_ownerToken == pending.response.claimToken)
      {
        m_responseAdmitted = true;
        m_statusDirty      = true;
      }
    }
    else if (bound && m_ownerState == OwnerState::BOUND &&
        m_ownerClientID == pending.clientID &&
        m_ownerGeneration == pending.response.requestGeneration &&
        m_ownerToken == pending.response.claimToken)
      BeginRelease("audio claimant disappeared");

    pending = {};
  }
}

bool CLGMPAudioTransport::OwnerSubscribed() const
{
  if (!m_ownerClientID)
    return false;

  uint32_t clients[LGMP_MAX_CLIENTS] = {};
  unsigned count = 0;
  const LGMP_STATUS status = m_control.GetClientIDs(clients, count);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to query audio owner subscription: %s",
      lgmpStatusString(status));
    return true;
  }
  for (unsigned index = 0; index < count; ++index)
    if (clients[index] == m_ownerClientID)
      return true;
  return false;
}

void CLGMPAudioTransport::RenewLease()
{
  m_ownerDeadline = GetTickCount64() + KVMFR_AUDIO_OWNER_LEASE_MS;
}

void CLGMPAudioTransport::ReleasePendingCapture()
{
  if (!m_pendingCapture.valid)
    return;
  const LGMP_STATUS status = lgmpHostStreamReadRelease(
    m_clientToHostStream, &m_pendingCapture.buffer);
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
      status != LGMP_ERR_STREAM_STALE)
    Fail("lgmpHostStreamReadRelease(audio)", status);
  m_pendingCapture = {};
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
}

void CLGMPAudioTransport::CancelPlaybackCredit()
{
  if (!m_playbackCreditValid)
    return;

  const LGMP_STATUS status = lgmpHostStreamWriteCancel(
    m_hostToClientStream, &m_playbackCredit);
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
      status != LGMP_ERR_STREAM_STALE)
  {
    DEBUG_WARN("Failed to cancel an audio playback reservation: %s",
      lgmpStatusString(status));
    m_failed = true;
  }
  m_playbackCredit = {};
  m_playbackCreditValid = false;
}

void CLGMPAudioTransport::ClearBlockedPlayback(bool notify)
{
  const bool blocked = m_playbackBlocked;
  CancelPlaybackCredit();
  m_playbackBlocked = false;
  m_blockedPlayback = {};
  m_blockedPlaybackSize = 0;
  if (notify && blocked)
    m_playbackReadyPending = true;
}

bool CLGMPAudioTransport::RememberBlockedPlayback(
  const AudioPacket& packet, size_t size)
{
  if (m_playbackBlocked)
    return size == m_blockedPlaybackSize &&
      SamePacket(packet, m_blockedPlayback);

  m_playbackBlocked = true;
  m_blockedPlayback = packet;
  m_blockedPlaybackSize = size;
  return true;
}

void CLGMPAudioTransport::BeginRelease(const char * reason)
{
  if (m_ownerState == OwnerState::NONE)
    return;
  if (m_ownerState != OwnerState::RELEASING)
  {
    DEBUG_INFO("Audio owner %u generation %u releasing: %s",
      m_ownerClientID, m_ownerGeneration, reason);
    m_ownerState          = OwnerState::RELEASING;
    m_releaseGeneration   = m_state.generation;
    m_deactivationIssued  = false;
    m_statusDirty         = false;
    m_announcedPlayback   = 0;
    m_announcedCapture    = 0;
    ClearBlockedPlayback(true);
    if (!m_captureCallbackInFlight)
      ReleasePendingCapture();
    if (!m_inboundStopping && m_inboundWakeEvent)
      SetEvent(m_inboundWakeEvent);

    for (PendingResponse& pending : m_pendingResponse)
      if (pending.valid && pending.clientID == m_ownerClientID &&
          pending.response.result == KVMFR_AUDIO_RESPONSE_BOUND)
        pending = {};
  }
}

void CLGMPAudioTransport::ResetOwner()
{
  ClearBlockedPlayback(false);
  for (ClaimRecord& record : m_claimRecord)
    if (record.valid && record.clientID == m_ownerClientID &&
        record.requestGeneration == m_ownerGeneration &&
        record.claimToken == m_ownerToken)
    {
      record.closed = true;
      break;
    }

  m_ownerState              = OwnerState::NONE;
  m_ownerClientID           = 0;
  m_ownerGeneration         = 0;
  m_clientCapabilities      = 0;
  m_ownerToken              = 0;
  m_ownerDeadline           = 0;
  m_releaseGeneration       = 0;
  m_responseAdmitted        = false;
  m_clientReady             = false;
  m_activationIssued        = false;
  m_deactivationIssued      = false;
  m_ownerResponse           = {};
  m_statusDirty             = false;
  m_announcedPlayback       = 0;
  m_announcedCapture        = 0;
  m_hostSequence            = 0;
  m_nextClientSequence      = 0;
  m_clientSequenceKnown     = false;
  m_captureDiscontinuity    = false;
  m_captureCallbackInFlight = false;
  m_pendingCapture          = {};
  m_ownerSerial             = NextSerial(m_ownerSerial);
}

void CLGMPAudioTransport::MaintainOwner(uint64_t now)
{
  if (m_ownerState == OwnerState::NONE)
    return;
  if (m_ownerState != OwnerState::RELEASING)
  {
    if (!OwnerSubscribed())
      BeginRelease("audio pointer subscriber disappeared");
    else if (m_pendingCapture.valid)
      RenewLease();
    else if (now >= m_ownerDeadline)
      BeginRelease("audio owner lease expired");
  }

  if (m_ownerState != OwnerState::RELEASING)
    return;

  const bool streamsUnbound = TryUnbindStreams();

  const bool inactive = !m_started || !m_activationIssued ||
    (m_stateKnown && !m_state.active &&
      m_state.generation != m_releaseGeneration);
  if (streamsUnbound && inactive)
  {
    DEBUG_INFO("Audio owner %u generation %u released",
      m_ownerClientID, m_ownerGeneration);
    ResetOwner();
  }
}

void CLGMPAudioTransport::InitializeOwner(uint32_t clientID,
  const KVMFRAudioClaim& claim)
{
  m_ownerState             = OwnerState::BOUND;
  m_ownerClientID          = clientID;
  m_ownerGeneration        = claim.requestGeneration;
  m_clientCapabilities     = claim.clientCapabilities;
  m_ownerToken             = claim.claimToken;
  m_responseAdmitted       = false;
  m_clientReady            = false;
  m_activationIssued       = false;
  m_deactivationIssued     = false;
  m_statusDirty            = true;
  m_announcedPlayback      = 0;
  m_announcedCapture       = 0;
  m_hostSequence           = 0;
  m_nextClientSequence     = 0;
  m_clientSequenceKnown    = false;
  m_captureDiscontinuity   = false;
  ClearBlockedPlayback(false);
  m_pendingCapture         = {};
  m_ownerSerial            = NextSerial(m_ownerSerial);
  RenewLease();
  if (!m_inboundStopping && m_inboundWakeEvent)
    SetEvent(m_inboundWakeEvent);
}

void CLGMPAudioTransport::HandleClaim(
  uint32_t clientID, const KVMFRAudioClaim& claim)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_initialized)
    return;

  KVMFRAudioStreamResponse response = {};
  response.magic             = KVMFR_AUDIO_RESPONSE_MAGIC;
  response.version           = KVMFR_AUDIO_CONTROL_VERSION;
  response.size              = sizeof(response);
  response.requestGeneration = claim.requestGeneration;
  response.boundClientID     = clientID;
  response.endpointGeneration = m_state.generation;
  response.claimToken        = claim.claimToken;

  ClaimRecord * claimRecord = nullptr;
  ClaimRecord * claimSlot = nullptr;
  for (ClaimRecord& record : m_claimRecord)
  {
    if (record.valid && record.clientID == clientID)
      claimRecord = &record;
    if (!record.valid && !claimSlot)
      claimSlot = &record;
  }
  if (claimRecord)
    claimSlot = claimRecord;

  const bool claimValid = ValidateClaim(claim);
  if (!claimValid)
    response.result = KVMFR_AUDIO_RESPONSE_INCOMPATIBLE;
  else if (m_failed || !m_started || !m_stateKnown)
    response.result = KVMFR_AUDIO_RESPONSE_ERROR;
  else if (m_ownerState != OwnerState::NONE)
  {
    if (m_ownerState != OwnerState::RELEASING &&
        m_ownerClientID == clientID &&
        m_ownerGeneration == claim.requestGeneration &&
        m_ownerToken == claim.claimToken)
    {
      RenewLease();
      response = m_ownerResponse;
    }
    else
      response.result = KVMFR_AUDIO_RESPONSE_BUSY;
  }
  else if (claimRecord &&
      claimRecord->requestGeneration == claim.requestGeneration &&
      claimRecord->claimToken == claim.claimToken)
  {
    if (claimRecord->response.result == KVMFR_AUDIO_RESPONSE_ERROR)
      response = claimRecord->response;
    else
      response.result = KVMFR_AUDIO_RESPONSE_BUSY;
  }
  else if (claimRecord && !RequestNewer(
      claim.requestGeneration, claimRecord->requestGeneration))
    response.result = KVMFR_AUDIO_RESPONSE_BUSY;
  else if (!claimSlot)
  {
    DEBUG_WARN("Audio claim history is full for client %u", clientID);
    response.result = KVMFR_AUDIO_RESPONSE_ERROR;
  }
  else if (BindStreams(clientID))
  {
    InitializeOwner(clientID, claim);
    response.result          = KVMFR_AUDIO_RESPONSE_BOUND;
    response.protocolVersion = KVMFR_AUDIO_PROTOCOL_MAX;
    response.hostToClient    = m_hostToClientDescriptor;
    response.clientToHost    = m_clientToHostDescriptor;
    m_ownerResponse          = response;
    claimSlot->valid             = true;
    claimSlot->closed            = false;
    claimSlot->clientID          = clientID;
    claimSlot->requestGeneration = claim.requestGeneration;
    claimSlot->claimToken        = claim.claimToken;
    claimSlot->response          = response;
    DEBUG_INFO("Audio owner %u generation %u admitted",
      m_ownerClientID, m_ownerGeneration);
  }
  else
    response.result = KVMFR_AUDIO_RESPONSE_ERROR;

  if (claimValid && claimSlot &&
      response.result == KVMFR_AUDIO_RESPONSE_ERROR &&
      (!claimRecord || RequestNewer(claim.requestGeneration,
        claimRecord->requestGeneration)))
  {
    claimSlot->valid             = true;
    claimSlot->closed            = true;
    claimSlot->clientID          = clientID;
    claimSlot->requestGeneration = claim.requestGeneration;
    claimSlot->claimToken        = claim.claimToken;
    claimSlot->response          = response;
  }

  if (!QueueResponse(clientID, response))
  {
    DEBUG_WARN("Audio response queue is full for client %u", clientID);
    if (response.result == KVMFR_AUDIO_RESPONSE_BOUND &&
        m_ownerClientID == clientID)
      BeginRelease("audio response queue exhausted");
  }
  PumpResponses();
}

KVMFRAudioStatus CLGMPAudioTransport::WireStatus() const
{
  KVMFRAudioStatus status = {};
  status.version           = KVMFR_AUDIO_STATUS_VERSION;
  status.size              = sizeof(status);
  status.protocolVersion   = KVMFR_AUDIO_PROTOCOL_MAX;
  status.requestGeneration = m_ownerGeneration;
  status.capabilities      =
    m_state.capabilities & m_clientCapabilities;
  status.ownerClientID     = m_ownerClientID;
  status.ownerGeneration   = m_ownerGeneration;
  status.endpointGeneration = m_state.generation;
  status.playbackGeneration = m_state.playback.generation;
  status.captureGeneration  = m_state.capture.generation;
  status.playbackState      = WireState(m_state.playback.state);
  status.captureState       = WireState(m_state.capture.state);
  if (m_state.playback.formatValid &&
      ValidFormat(m_state.playback.format))
    status.playbackFormat = WireFormat(m_state.playback.format);
  if (m_state.capture.formatValid &&
      ValidFormat(m_state.capture.format))
    status.captureFormat = WireFormat(m_state.capture.format);

  if (m_state.available)
    status.flags |= KVMFR_AUDIO_STATUS_AVAILABLE;
  if (m_ownerState != OwnerState::NONE)
    status.flags |= KVMFR_AUDIO_STATUS_HAS_OWNER;
  if (status.capabilities & KVMFR_AUDIO_CAP_PLAYBACK)
    status.flags |= KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE;
  if (status.capabilities & KVMFR_AUDIO_CAP_CAPTURE)
    status.flags |= KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE;
  if (m_announcedPlayback)
    status.flags |= KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE;
  if (m_announcedCapture)
    status.flags |= KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE;
  return status;
}

bool CLGMPAudioTransport::CommitMessage(LGMPStreamBuffer& buffer,
  KVMFRAudioMessageType type, uint64_t streamGeneration,
  KVMFRAudioMessageFlags flags, uint64_t position, int64_t timeNs,
  uint64_t rateQ32, uint32_t frames, const void * payload,
  uint32_t length)
{
  const uint32_t bytes = static_cast<uint32_t>(
    sizeof(KVMFRAudioMessage) + length);
  if (!buffer.data || buffer.capacity < bytes)
  {
    const LGMP_STATUS cancel = lgmpHostStreamWriteCancel(
      m_hostToClientStream, &buffer);
    if (cancel != LGMP_OK)
      DEBUG_WARN("Failed to cancel an undersized audio stream "
        "reservation: %s", lgmpStatusString(cancel));
    Fail("undersized host-to-client audio stream slot",
      LGMP_ERR_INVALID_SIZE);
    return false;
  }

  KVMFRAudioMessage message = {};
  message.version          = KVMFR_AUDIO_MESSAGE_VERSION;
  message.size             = sizeof(message);
  message.type             = type;
  message.streamGeneration = streamGeneration;
  message.flags            = flags;
  message.sequence         = m_hostSequence;
  message.position         = position;
  message.timeNs           = timeNs;
  message.rateQ32          = rateQ32;
  message.frames           = frames;
  message.length           = length;
  memcpy(buffer.data, &message, sizeof(message));
  if (length)
    WCWrite::Copy(static_cast<uint8_t *>(buffer.data) + sizeof(message),
      payload, length);

  const LGMP_STATUS status = lgmpHostStreamWriteCommit(
    m_hostToClientStream, &buffer, bytes);
  if (status == LGMP_OK)
  {
    ++m_hostSequence;
    return true;
  }
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    BeginRelease("host-to-client audio stream binding lost");
    return false;
  }
  Fail("lgmpHostStreamWriteCommit(audio)", status);
  return false;
}

bool CLGMPAudioTransport::WriteMessage(KVMFRAudioMessageType type,
  uint64_t streamGeneration, KVMFRAudioMessageFlags flags,
  uint64_t position, int64_t timeNs, uint64_t rateQ32,
  uint32_t frames, const void * payload, uint32_t length)
{
  if (m_playbackCreditValid)
    return false;

  LGMPStreamBuffer buffer = {};
  const LGMP_STATUS status = lgmpHostStreamWriteAcquire(
    m_hostToClientStream, &buffer);
  if (status == LGMP_ERR_STREAM_FULL)
    return false;
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    BeginRelease("host-to-client audio stream binding lost");
    return false;
  }
  if (status != LGMP_OK)
  {
    Fail("lgmpHostStreamWriteAcquire(audio)", status);
    return false;
  }

  return CommitMessage(buffer, type, streamGeneration, flags,
    position, timeNs, rateQ32, frames, payload, length);
}

void CLGMPAudioTransport::PumpState()
{
  if (m_ownerState != OwnerState::BOUND || !m_responseAdmitted ||
      !m_clientReady || !m_stateKnown || m_failed ||
      m_playbackCreditValid)
    return;

  if (m_state.playback.state == AudioStreamState::RUNNING)
    m_playbackRunGeneration = m_state.playback.generation;

  const bool playbackPrewarm =
    (m_state.playback.state == AudioStreamState::ACQUIRED ||
      m_state.playback.state == AudioStreamState::PAUSED) &&
    m_playbackRunGeneration != m_state.playback.generation;
  const bool playbackCurrent = m_state.available && m_state.active &&
    (m_clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) &&
    (m_state.capabilities & AUDIO_CAP_PLAYBACK) &&
    m_state.playback.formatValid &&
    ValidFormat(m_state.playback.format) &&
    (m_state.playback.state == AudioStreamState::RUNNING ||
      playbackPrewarm);
  if (m_announcedPlayback &&
      (!playbackCurrent ||
        m_announcedPlayback != m_state.playback.generation))
  {
    if (!WriteMessage(KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP,
        m_announcedPlayback, 0, 0, 0, 0, 0, nullptr, 0))
      return;
    m_announcedPlayback = 0;
  }

  const bool captureCurrent = m_state.available && m_state.active &&
    (m_clientCapabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
    (m_state.capabilities & AUDIO_CAP_CAPTURE) &&
    m_state.capture.formatValid &&
    ValidFormat(m_state.capture.format) &&
    m_state.capture.state == AudioStreamState::RUNNING;
  if (m_announcedCapture &&
      (!captureCurrent ||
        m_announcedCapture != m_state.capture.generation))
  {
    if (!WriteMessage(KVMFR_AUDIO_MESSAGE_CAPTURE_STOP,
        m_announcedCapture, 0, 0, 0, 0, 0, nullptr, 0))
      return;
    m_announcedCapture = 0;
  }

  const bool startPlayback = playbackCurrent &&
    m_announcedPlayback != m_state.playback.generation;
  const bool startCapture = captureCurrent &&
    m_announcedCapture != m_state.capture.generation;

  // Publish capabilities before starts so the client selects the correct
  // rate-control path. A second status below records the announced streams.
  if (m_statusDirty && (startPlayback || startCapture))
  {
    const KVMFRAudioStatus status = WireStatus();
    if (!WriteMessage(KVMFR_AUDIO_MESSAGE_STATUS,
        0, 0, 0, 0, 0, 0, &status, sizeof(status)))
      return;
    m_statusDirty = false;
  }

  if (startPlayback)
  {
    const KVMFRAudioFormat format = WireFormat(m_state.playback.format);
    if (!WriteMessage(KVMFR_AUDIO_MESSAGE_PLAYBACK_START,
        m_state.playback.generation, 0, 0, 0, 0, 0,
        &format, sizeof(format)))
      return;
    m_announcedPlayback = m_state.playback.generation;
    m_statusDirty       = true;
  }

  if (startCapture)
  {
    const KVMFRAudioFormat format = WireFormat(m_state.capture.format);
    if (!WriteMessage(KVMFR_AUDIO_MESSAGE_CAPTURE_START,
        m_state.capture.generation, 0, 0, 0, 0, 0,
        &format, sizeof(format)))
      return;
    m_announcedCapture = m_state.capture.generation;
    m_statusDirty      = true;
  }

  if (!m_statusDirty)
    return;
  const KVMFRAudioStatus status = WireStatus();
  if (WriteMessage(KVMFR_AUDIO_MESSAGE_STATUS,
      0, 0, 0, 0, 0, 0, &status, sizeof(status)))
    m_statusDirty = false;
}

void CLGMPAudioTransport::CheckPlaybackCredit()
{
  if (!m_playbackBlocked || m_playbackCreditValid ||
      m_playbackReadyPending || m_statusDirty ||
      m_ownerState != OwnerState::BOUND || !m_responseAdmitted ||
      !m_clientReady || !m_stateKnown || !m_state.available ||
      !m_state.active ||
      !(m_clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) ||
      !(m_state.capabilities & AUDIO_CAP_PLAYBACK) ||
      m_blockedPlayback.endpointGeneration != m_state.generation ||
      m_blockedPlayback.streamGeneration !=
        m_state.playback.generation ||
      !m_state.playback.formatValid ||
      m_state.playback.state != AudioStreamState::RUNNING ||
      !SameFormat(m_blockedPlayback.format,
        m_state.playback.format) ||
      m_announcedPlayback != m_blockedPlayback.streamGeneration)
    return;

  m_playbackCredit = {};
  const LGMP_STATUS status = lgmpHostStreamWriteAcquire(
    m_hostToClientStream, &m_playbackCredit);
  if (status == LGMP_ERR_STREAM_FULL)
    return;
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    BeginRelease("host-to-client audio credit binding lost");
    return;
  }
  if (status != LGMP_OK)
  {
    Fail("lgmpHostStreamWriteAcquire(audio credit)", status);
    return;
  }

  const size_t required = sizeof(KVMFRAudioMessage) +
    m_blockedPlaybackSize;
  if (!m_playbackCredit.data ||
      m_playbackCredit.capacity < required)
  {
    const LGMP_STATUS cancel = lgmpHostStreamWriteCancel(
      m_hostToClientStream, &m_playbackCredit);
    m_playbackCredit = {};
    if (cancel != LGMP_OK)
      DEBUG_WARN("Failed to cancel an undersized audio credit: %s",
        lgmpStatusString(cancel));
    Fail("undersized host-to-client audio stream slot",
      LGMP_ERR_INVALID_SIZE);
    return;
  }

  m_playbackCreditValid  = true;
  m_playbackReadyPending = true;
}

bool CLGMPAudioTransport::ParseInbound(
  const LGMPStreamBuffer& buffer, KVMFRAudioMessage& message) const
{
  if (!buffer.data || buffer.size < sizeof(message) ||
      buffer.size > buffer.capacity)
    return false;
  memcpy(&message, buffer.data, sizeof(message));
  if (message.version != KVMFR_AUDIO_MESSAGE_VERSION ||
      message.size != sizeof(message) || message.reserved0 ||
      message.length > KVMFR_AUDIO_PCM_BYTES ||
      message.length >
        (std::numeric_limits<uint32_t>::max)() - sizeof(message))
    return false;
  return buffer.size == sizeof(message) + message.length;
}

bool CLGMPAudioTransport::PendingCaptureCurrent() const
{
  if (!m_pendingCapture.valid)
    return false;

  const AudioPacket& packet = m_pendingCapture.packet;
  return m_stateKnown && m_state.available && m_state.active &&
    (m_clientCapabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
    (m_state.capabilities & AUDIO_CAP_CAPTURE) &&
    m_state.capture.formatValid &&
    ValidFormat(m_state.capture.format) &&
    m_state.capture.state == AudioStreamState::RUNNING &&
    packet.endpointGeneration == m_state.generation &&
    packet.streamGeneration == m_state.capture.generation &&
    SameFormat(packet.format, m_state.capture.format);
}

bool CLGMPAudioTransport::PrepareCapture(
  const KVMFRAudioMessage& message, const LGMPStreamBuffer& buffer)
{
  if (!m_stateKnown || !m_state.available || !m_state.active ||
      !(m_state.capabilities & AUDIO_CAP_CAPTURE) ||
      !m_state.capture.formatValid ||
      !ValidFormat(m_state.capture.format) ||
      m_state.capture.state != AudioStreamState::RUNNING ||
      message.streamGeneration != m_state.capture.generation)
    return false;

  const bool silent =
    (message.flags & KVMFR_AUDIO_MESSAGE_SILENT) != 0;
  const uint32_t bytes =
    message.frames * m_state.capture.format.blockAlign;
  if (bytes > KVMFR_AUDIO_PCM_BYTES ||
      (silent ? message.length != 0 : message.length != bytes))
    return false;

  AudioPacket packet;
  packet.endpointGeneration = m_state.generation;
  packet.streamGeneration   = message.streamGeneration;
  if (message.flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY)
    packet.flags |= AUDIO_PACKET_DISCONTINUITY;
  if (message.flags & KVMFR_AUDIO_MESSAGE_SILENT)
    packet.flags |= AUDIO_PACKET_SILENT;
  if (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID)
    packet.flags |= AUDIO_PACKET_CLOCK_VALID;
  if (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE)
    packet.flags |= AUDIO_PACKET_CLOCK_STABLE;
  if (m_captureDiscontinuity ||
      (m_clientSequenceKnown &&
        message.sequence != m_nextClientSequence) ||
      (!m_clientSequenceKnown && message.sequence != 0))
    packet.flags |= AUDIO_PACKET_DISCONTINUITY;
  packet.frames   = message.frames;
  packet.position = message.position;
  packet.timeNs   = message.timeNs;
  packet.rateQ32  = message.rateQ32;
  packet.format   = m_state.capture.format;

  m_pendingCapture.valid       = true;
  m_pendingCapture.buffer      = buffer;
  m_pendingCapture.packet      = packet;
  m_pendingCapture.data = silent ? nullptr :
    static_cast<const uint8_t *>(buffer.data) +
      sizeof(KVMFRAudioMessage);
  m_pendingCapture.size        = message.length;
  m_pendingCapture.readySerial = m_captureReadySerial;
  m_nextClientSequence = message.sequence + 1;
  m_clientSequenceKnown = true;
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
  return true;
}

void CLGMPAudioTransport::DrainInbound()
{
  for (unsigned count = 0; count < INBOUND_DRAIN_MAX; ++count)
  {
    CSRWSharedLock callbackLock(m_callbackLock);
    IAudioTarget * target = nullptr;
    AudioClockFeedback feedback;
    bool callCapture = false;
    bool callClock = false;
    uint64_t ownerSerial = 0;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_ownerState == OwnerState::RELEASING)
      {
        ReleasePendingCapture();
        LGMPStreamBuffer buffer = {};
        const LGMP_STATUS status = lgmpHostStreamReadPeek(
          m_clientToHostStream, &buffer);
        if (status == LGMP_ERR_STREAM_EMPTY ||
            status == LGMP_ERR_STREAM_UNBOUND ||
            status == LGMP_ERR_STREAM_STALE)
          return;
        if (status != LGMP_OK)
        {
          Fail("lgmpHostStreamReadPeek(audio release)", status);
          return;
        }
        const LGMP_STATUS release = lgmpHostStreamReadRelease(
          m_clientToHostStream, &buffer);
        if (release != LGMP_OK)
        {
          Fail("lgmpHostStreamReadRelease(audio release)", release);
          return;
        }
        m_inboundActivitySerial =
          NextSerial(m_inboundActivitySerial);
        continue;
      }
      if (m_ownerState != OwnerState::BOUND ||
          !m_responseAdmitted || m_failed)
        return;

      target = m_started ? m_target : nullptr;
      ownerSerial = m_ownerSerial;
      if (m_pendingCapture.valid)
      {
        if (!PendingCaptureCurrent())
        {
          ReleasePendingCapture();
          m_captureDiscontinuity = true;
          continue;
        }
        if (m_pendingCapture.readySerial == m_captureReadySerial)
          return;
        m_pendingCapture.readySerial = m_captureReadySerial;
        callCapture = true;
      }
      else
      {
        LGMPStreamBuffer buffer = {};
        const LGMP_STATUS status = lgmpHostStreamReadPeek(
          m_clientToHostStream, &buffer);
        if (status == LGMP_ERR_STREAM_EMPTY)
          return;
        if (status == LGMP_ERR_STREAM_UNBOUND ||
            status == LGMP_ERR_STREAM_STALE)
        {
          BeginRelease("client-to-host audio stream binding lost");
          return;
        }
        if (status != LGMP_OK)
        {
          Fail("lgmpHostStreamReadPeek(audio)", status);
          return;
        }

        KVMFRAudioMessage message = {};
        if (!ParseInbound(buffer, message))
        {
          const LGMP_STATUS release = lgmpHostStreamReadRelease(
            m_clientToHostStream, &buffer);
          if (release != LGMP_OK)
            Fail("lgmpHostStreamReadRelease(audio invalid)", release);
          BeginRelease("malformed client-to-host audio record");
          return;
        }

        const bool sequenceGap =
          (m_clientSequenceKnown &&
            message.sequence != m_nextClientSequence) ||
          (!m_clientSequenceKnown && message.sequence != 0);
        const auto consume = [&]() -> bool
        {
          const LGMP_STATUS release = lgmpHostStreamReadRelease(
            m_clientToHostStream, &buffer);
          if (release != LGMP_OK)
          {
            Fail("lgmpHostStreamReadRelease(audio)", release);
            return false;
          }
          m_nextClientSequence = message.sequence + 1;
          m_clientSequenceKnown = true;
          m_inboundActivitySerial =
            NextSerial(m_inboundActivitySerial);
          return true;
        };

        if (!m_clientReady &&
            message.type != KVMFR_AUDIO_MESSAGE_KEEPALIVE &&
            message.type != KVMFR_AUDIO_MESSAGE_RELEASE)
        {
          if (!consume())
            return;
          BeginRelease("audio data received before client readiness");
          return;
        }

        switch (message.type)
        {
          case KVMFR_AUDIO_MESSAGE_CAPTURE_DATA:
          {
            const bool clockStable =
              (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) != 0;
            const bool clockValid =
              (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) != 0;
            if (!(m_clientCapabilities & KVMFR_AUDIO_CAP_CAPTURE) ||
                (message.flags & ~WIRE_PACKET_FLAG_MASK) ||
                (clockStable && !clockValid) ||
                (!clockValid && (message.position || message.timeNs ||
                  message.rateQ32)) || !message.streamGeneration ||
                !message.frames ||
                message.frames > KVMFR_AUDIO_MAX_FRAMES_PER_PACKET)
            {
              consume();
              BeginRelease("invalid capture audio record");
              return;
            }
            RenewLease();

            const bool captureCurrent = m_stateKnown &&
              m_state.available && m_state.active &&
              (m_state.capabilities & AUDIO_CAP_CAPTURE) &&
              m_state.capture.formatValid &&
              ValidFormat(m_state.capture.format) &&
              m_state.capture.state == AudioStreamState::RUNNING &&
              message.streamGeneration ==
                m_state.capture.generation;
            if (captureCurrent)
            {
              const bool silent = (message.flags &
                KVMFR_AUDIO_MESSAGE_SILENT) != 0;
              const uint32_t bytes = message.frames *
                m_state.capture.format.blockAlign;
              if (bytes > KVMFR_AUDIO_PCM_BYTES ||
                  (silent ? message.length != 0 :
                    message.length != bytes))
              {
                consume();
                BeginRelease("invalid capture audio payload");
                return;
              }
            }

            if (!PrepareCapture(message, buffer))
            {
              consume();
              m_captureDiscontinuity = true;
              continue;
            }
            if (sequenceGap)
              m_pendingCapture.packet.flags |=
                AUDIO_PACKET_DISCONTINUITY;
            callCapture = true;
            break;
          }

          case KVMFR_AUDIO_MESSAGE_CLOCK_FEEDBACK:
          {
            const uint32_t validFlags =
              KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
              KVMFR_AUDIO_MESSAGE_CLOCK_STABLE |
              KVMFR_AUDIO_MESSAGE_DISCONTINUITY;
            KVMFRAudioClockFeedback wire = {};
            if (message.length == sizeof(wire))
              memcpy(&wire, static_cast<const uint8_t *>(buffer.data) +
                sizeof(message), sizeof(wire));
            const bool valid =
              (m_clientCapabilities &
                KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
              message.flags && !(message.flags & ~validFlags) &&
              (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) &&
              (!(message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) ||
                (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID)) &&
              message.streamGeneration && !message.frames &&
              message.length == sizeof(wire) && !wire.reserved &&
              wire.targetRateQ32;
            if (!valid)
            {
              consume();
              BeginRelease("invalid audio clock feedback");
              return;
            }

            const bool current = m_stateKnown && m_state.available &&
              m_state.active && (m_state.capabilities &
                AUDIO_CAP_CLOCK_FEEDBACK) &&
              message.streamGeneration ==
                m_state.playback.generation;
            if (!consume())
              return;
            RenewLease();
            if (sequenceGap)
              m_captureDiscontinuity = true;
            if (!current)
              continue;
            feedback.endpointGeneration = m_state.generation;
            feedback.playbackGeneration = message.streamGeneration;
            feedback.position      = message.position;
            feedback.timeNs        = message.timeNs;
            feedback.rateQ32       = message.rateQ32;
            feedback.targetRateQ32 = wire.targetRateQ32;
            feedback.flags         = AUDIO_CLOCK_VALID;
            if (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE)
              feedback.flags |= AUDIO_CLOCK_STABLE;
            if (sequenceGap ||
                (message.flags &
                  KVMFR_AUDIO_MESSAGE_DISCONTINUITY))
              feedback.flags |= AUDIO_CLOCK_DISCONTINUITY;
            callClock = true;
            break;
          }

          case KVMFR_AUDIO_MESSAGE_KEEPALIVE:
          case KVMFR_AUDIO_MESSAGE_RELEASE:
          {
            const bool empty = !message.streamGeneration &&
              !message.flags && !message.position && !message.timeNs &&
              !message.rateQ32 && !message.frames && !message.length;
            if (!consume())
              return;
            if (!empty)
            {
              BeginRelease("invalid audio owner control record");
              return;
            }
            if (message.type == KVMFR_AUDIO_MESSAGE_RELEASE)
            {
              BeginRelease("client audio release");
              return;
            }
            if (!m_clientReady)
            {
              if (sequenceGap)
              {
                BeginRelease("invalid initial audio keepalive sequence");
                return;
              }
              m_clientReady = true;
              DEBUG_INFO("Audio owner %u generation %u is ready",
                m_ownerClientID, m_ownerGeneration);
            }
            if (sequenceGap)
              m_captureDiscontinuity = true;
            RenewLease();
            continue;
          }

          default:
            consume();
            BeginRelease("unsupported client-to-host audio record");
            return;
        }
      }
      m_captureCallbackInFlight = target && callCapture;
    }

    AudioResult result = AudioResult::UNAVAILABLE;
    if (target && callCapture)
      result = target->SendCapture(m_pendingCapture.packet,
        m_pendingCapture.data, m_pendingCapture.size);
    else if (target && callClock)
      result = target->SetClockFeedback(feedback);

    {
      CSRWExclusiveLock lock(m_lock);
      if (callCapture)
        m_captureCallbackInFlight = false;
      if (ownerSerial != m_ownerSerial ||
          m_ownerState != OwnerState::BOUND)
      {
        if (m_pendingCapture.valid)
          ReleasePendingCapture();
        return;
      }
      if (callClock)
      {
        if (result == AudioResult::FAILED ||
            result == AudioResult::REJECTED)
          BeginRelease("audio clock target failed");
        continue;
      }
      if (!callCapture || !m_pendingCapture.valid)
        continue;
      if (!PendingCaptureCurrent())
      {
        ReleasePendingCapture();
        m_captureDiscontinuity = true;
        continue;
      }
      if (result == AudioResult::BUSY)
        return;

      const LGMP_STATUS release = lgmpHostStreamReadRelease(
        m_clientToHostStream, &m_pendingCapture.buffer);
      if (release != LGMP_OK)
      {
        m_pendingCapture = {};
        Fail("lgmpHostStreamReadRelease(audio capture)", release);
        return;
      }
      m_pendingCapture = {};
      m_inboundActivitySerial =
        NextSerial(m_inboundActivitySerial);
      m_captureDiscontinuity = result != AudioResult::ACCEPTED;
      if (result == AudioResult::FAILED ||
          result == AudioResult::REJECTED)
      {
        BeginRelease("audio capture target failed");
        return;
      }
    }
  }
}

void CLGMPAudioTransport::ProcessActivation()
{
  CSRWSharedLock callbackLock(m_callbackLock);
  IAudioTarget * target = nullptr;
  bool activate = false;
  uint64_t ownerSerial = 0;
  {
    CSRWSharedLock lock(m_lock);
    if (!m_started || !m_target || m_failed ||
        m_ownerState == OwnerState::NONE)
      return;
    if (m_ownerState == OwnerState::BOUND &&
        m_responseAdmitted && m_clientReady &&
        !m_activationIssued)
      activate = true;
    else if (m_ownerState != OwnerState::RELEASING ||
        !m_activationIssued || m_deactivationIssued)
      return;
    target      = m_target;
    ownerSerial = m_ownerSerial;
  }

  const AudioResult result = target->SetAudioActive(activate);
  CSRWExclusiveLock lock(m_lock);
  if (ownerSerial != m_ownerSerial ||
      m_ownerState == OwnerState::NONE)
    return;
  if (activate)
  {
    if (m_ownerState != OwnerState::BOUND || m_activationIssued)
      return;
    if (result == AudioResult::ACCEPTED)
      m_activationIssued = true;
    else if (result == AudioResult::FAILED ||
        result == AudioResult::REJECTED)
      BeginRelease("audio endpoint activation failed");
  }
  else if (m_ownerState == OwnerState::RELEASING &&
      (result == AudioResult::ACCEPTED ||
        result == AudioResult::UNAVAILABLE ||
        result == AudioResult::STALE))
    m_deactivationIssued = true;
}

void CLGMPAudioTransport::NotifyPlaybackReady()
{
  CSRWSharedLock callbackLock(m_callbackLock);
  IAudioTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_playbackReadyPending)
      return;
    m_playbackReadyPending = false;
    if (m_started)
      target = m_target;
  }
  if (target)
    target->PlaybackReceiveReady();
}

void CLGMPAudioTransport::NotifyFailure()
{
  CSRWSharedLock callbackLock(m_callbackLock);
  IAudioTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_failed || m_failureNotified)
      return;
    m_failureNotified = true;
    if (m_started)
      target = m_target;
  }
  if (target)
    target->AudioFailed();
}

void CLGMPAudioTransport::Fail(
  const char * operation, LGMP_STATUS status)
{
  if (!m_failed)
    DEBUG_ERROR("%s Failed (Audio): %s",
      operation, lgmpStatusString(status));
  ClearBlockedPlayback(true);
  m_failed = true;
}

void CLGMPAudioTransport::Process()
{
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_initialized)
      return;
    PumpResponses();
  }

  {
    CSRWExclusiveLock lock(m_lock);
    MaintainOwner(GetTickCount64());
    PumpResponses();
    PumpState();
    CheckPlaybackCredit();
  }
  ProcessActivation();
  NotifyPlaybackReady();
  NotifyFailure();
}

bool CLGMPAudioTransport::Start(IAudioTarget& target)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock callbackLock(m_callbackLock);
  CSRWExclusiveLock lock(m_lock);
  if (!m_initialized || m_failed)
    return false;
  if (m_started)
    return m_target == &target;
  m_target = &target;
  m_started = true;
  m_stateKnown = false;
  m_failureNotified = false;
  return true;
}

void CLGMPAudioTransport::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock callbackLock(m_callbackLock);
  CSRWExclusiveLock lock(m_lock);
  if (!m_started && !m_target)
    return;
  if (m_ownerState != OwnerState::NONE)
    BeginRelease("audio source stopped");
  else
    ClearBlockedPlayback(false);
  m_started = false;
  m_target  = nullptr;
  m_playbackReadyPending = false;
}

void CLGMPAudioTransport::TransportStop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  StopInboundWorker();
  CSRWExclusiveLock callbackLock(m_callbackLock);
  CSRWExclusiveLock lock(m_lock);
  m_started = false;
  m_target  = nullptr;
  m_playbackReadyPending = false;
  ClearBlockedPlayback(false);
  ReleasePendingCapture();
  ForceUnbindStreams();
  ResetOwner();
  for (PendingResponse& response : m_pendingResponse)
    response = {};
  for (ClaimRecord& record : m_claimRecord)
    record = {};
}

void CLGMPAudioTransport::AudioState(
  const AudioEndpointStatus& state)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_started)
    return;

  if (state.playback.state == AudioStreamState::RUNNING)
    m_playbackRunGeneration = state.playback.generation;

  if (m_playbackBlocked)
  {
    const bool playbackCurrent = state.available && state.active &&
      (m_clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) &&
      (state.capabilities & AUDIO_CAP_PLAYBACK) &&
      state.playback.formatValid &&
      ValidFormat(state.playback.format) &&
      state.playback.state == AudioStreamState::RUNNING &&
      state.generation == m_blockedPlayback.endpointGeneration &&
      state.playback.generation ==
        m_blockedPlayback.streamGeneration &&
      SameFormat(state.playback.format, m_blockedPlayback.format);
    if (!playbackCurrent)
      ClearBlockedPlayback(true);
  }

  m_state      = state;
  m_stateKnown = true;
  if (m_ownerState == OwnerState::BOUND && m_responseAdmitted)
    m_statusDirty = true;
  if (!m_inboundStopping && m_inboundWakeEvent)
    SetEvent(m_inboundWakeEvent);
}

AudioResult CLGMPAudioTransport::SendPlayback(
  const AudioPacket& packet, const uint8_t * data, size_t size)
{
  if ((packet.flags & ~AUDIO_PACKET_FLAG_MASK) ||
      ((packet.flags & AUDIO_PACKET_CLOCK_STABLE) &&
        !(packet.flags & AUDIO_PACKET_CLOCK_VALID)) ||
      !packet.frames ||
      packet.frames > AUDIO_MAX_FRAMES_PER_PACKET ||
      !ValidFormat(packet.format))
    return AudioResult::REJECTED;

  const size_t pcmBytes = static_cast<size_t>(packet.frames) *
    packet.format.blockAlign;
  const bool silent = (packet.flags & AUDIO_PACKET_SILENT) != 0;
  if (pcmBytes > AUDIO_MAX_PCM_BYTES ||
      (silent ? size != 0 : (size != pcmBytes || !data)))
    return AudioResult::REJECTED;

  CSRWExclusiveLock lock(m_lock);
  if (m_playbackBlocked &&
      (size != m_blockedPlaybackSize ||
        !SamePacket(packet, m_blockedPlayback)))
  {
    DEBUG_WARN("Audio playback retry did not match the retained packet");
    ClearBlockedPlayback(false);
    return AudioResult::REJECTED;
  }
  if (!m_started)
  {
    ClearBlockedPlayback(false);
    return AudioResult::UNAVAILABLE;
  }
  if (m_failed)
  {
    ClearBlockedPlayback(false);
    return AudioResult::FAILED;
  }
  if (m_ownerState != OwnerState::BOUND ||
      !(m_clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK))
  {
    ClearBlockedPlayback(false);
    return AudioResult::ACCEPTED;
  }
  if (!m_responseAdmitted || !m_clientReady || !m_stateKnown)
  {
    if (!RememberBlockedPlayback(packet, size))
      return AudioResult::REJECTED;
    return AudioResult::BUSY;
  }
  if (!m_state.available || !m_state.active)
  {
    ClearBlockedPlayback(false);
    return AudioResult::UNAVAILABLE;
  }
  if (packet.endpointGeneration != m_state.generation ||
      packet.streamGeneration != m_state.playback.generation)
  {
    ClearBlockedPlayback(false);
    return AudioResult::STALE;
  }
  if (!(m_state.capabilities & AUDIO_CAP_PLAYBACK) ||
      !m_state.playback.formatValid ||
      m_state.playback.state != AudioStreamState::RUNNING ||
      !SameFormat(packet.format, m_state.playback.format))
  {
    ClearBlockedPlayback(false);
    return AudioResult::UNAVAILABLE;
  }

  KVMFRAudioMessageFlags flags = 0;
  if (packet.flags & AUDIO_PACKET_DISCONTINUITY)
    flags |= KVMFR_AUDIO_MESSAGE_DISCONTINUITY;
  if (packet.flags & AUDIO_PACKET_SILENT)
    flags |= KVMFR_AUDIO_MESSAGE_SILENT;
  if (packet.flags & AUDIO_PACKET_CLOCK_VALID)
    flags |= KVMFR_AUDIO_MESSAGE_CLOCK_VALID;
  if (packet.flags & AUDIO_PACKET_CLOCK_STABLE)
    flags |= KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;

  if (m_playbackCreditValid)
  {
    LGMPStreamBuffer buffer = m_playbackCredit;
    m_playbackCredit = {};
    m_playbackCreditValid = false;
    const bool committed = CommitMessage(buffer,
      KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA, packet.streamGeneration,
      flags, packet.position, packet.timeNs, packet.rateQ32,
      packet.frames, data, static_cast<uint32_t>(size));
    ClearBlockedPlayback(false);
    if (committed)
      return AudioResult::ACCEPTED;
    if (m_failed)
      return AudioResult::FAILED;
    return m_ownerState == OwnerState::BOUND ?
      AudioResult::BUSY : AudioResult::ACCEPTED;
  }

  PumpState();
  if (m_failed)
    return AudioResult::FAILED;
  if (m_ownerState != OwnerState::BOUND)
    return AudioResult::ACCEPTED;
  if (m_statusDirty ||
      m_announcedPlayback != packet.streamGeneration)
  {
    if (!RememberBlockedPlayback(packet, size))
      return AudioResult::REJECTED;
    return AudioResult::BUSY;
  }

  if (!WriteMessage(KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA,
      packet.streamGeneration, flags, packet.position, packet.timeNs,
      packet.rateQ32, packet.frames, data, static_cast<uint32_t>(size)))
  {
    if (m_failed)
      return AudioResult::FAILED;
    if (m_ownerState != OwnerState::BOUND)
      return AudioResult::ACCEPTED;
    if (!RememberBlockedPlayback(packet, size))
      return AudioResult::REJECTED;
    return AudioResult::BUSY;
  }
  ClearBlockedPlayback(false);
  return AudioResult::ACCEPTED;
}

void CLGMPAudioTransport::CaptureReceiveReady()
{
  CSRWExclusiveLock lock(m_lock);
  if (m_started)
  {
    m_captureReadySerial = NextSerial(m_captureReadySerial);
    if (!m_inboundStopping && m_inboundWakeEvent)
      SetEvent(m_inboundWakeEvent);
  }
}
