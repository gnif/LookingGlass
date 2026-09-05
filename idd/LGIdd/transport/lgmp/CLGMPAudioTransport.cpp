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

#include <LGProtocol/KVMFR.h>

#include <avrt.h>
#include <initializer_list>
#include <limits>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
  static_assert(sizeof(KVMFRCursor) == KVMFR_AUDIO_CURSOR_COMPAT_BYTES,
    "KVMFR cursor compatibility layout changed");
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
  static_assert(
      static_cast<uint32_t>(AudioSampleFormat::S16_LE) ==
        KVMFR_AUDIO_SAMPLE_S16_LE &&
      static_cast<uint32_t>(AudioSampleFormat::S24_LE) ==
        KVMFR_AUDIO_SAMPLE_S24_LE &&
      static_cast<uint32_t>(AudioSampleFormat::S32_LE) ==
        KVMFR_AUDIO_SAMPLE_S32_LE &&
      static_cast<uint32_t>(AudioSampleFormat::F32_LE) ==
        KVMFR_AUDIO_SAMPLE_F32_LE,
    "audio sample formats differ");
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
  static_assert(sizeof(LGMPSPMCDescriptor) ==
      sizeof(KVMFRSPMCDescriptor),
    "LGMP and KVMFR SPMC descriptor sizes differ");
  static_assert(LGMP_MAX_CLIENTS ==
      KVMFR_AUDIO_MAX_PLAYBACK_READERS,
    "audio reader and LGMP client limits differ");

  constexpr uint32_t AUDIO_CAPABILITY_MASK =
    KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE |
    KVMFR_AUDIO_CAP_CLOCK_FEEDBACK;
  constexpr uint32_t AUDIO_SUBSCRIPTION_MASK =
    KVMFR_AUDIO_SUBSCRIBE_PLAYBACK | KVMFR_AUDIO_SUBSCRIBE_CAPTURE;
  constexpr uint32_t AUDIO_PACKET_FLAG_MASK =
    AUDIO_PACKET_DISCONTINUITY | AUDIO_PACKET_SILENT |
    AUDIO_PACKET_CLOCK_VALID | AUDIO_PACKET_CLOCK_STABLE;
  constexpr uint32_t WIRE_PACKET_FLAG_MASK =
    KVMFR_AUDIO_MESSAGE_DISCONTINUITY |
    KVMFR_AUDIO_MESSAGE_SILENT |
    KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
    KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;
  constexpr size_t AUDIO_SHARED_MEMORY_PREFLIGHT =
    3U * 1024U * 1024U;

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

  uint32_t NextEpoch(uint32_t epoch)
  {
    ++epoch;
    return epoch ? epoch : 1;
  }

  bool RequestNewer(uint32_t candidate, uint32_t previous)
  {
    return candidate != previous &&
      static_cast<int32_t>(candidate - previous) > 0;
  }

  bool EmptyControl(const KVMFRAudioMessage& message)
  {
    return !message.streamGeneration && !message.flags &&
      !message.position && !message.timeNs && !message.rateQ32 &&
      !message.frames && !message.length;
  }
}

CLGMPAudio::~CLGMPAudio()
{
  TransportStop();
  CSRWExclusiveLock lock(m_lock);
  for (ResponseMemory& response : m_responseMemory)
    lgmpHostMemFree(&response.memory);
  FreeStreams();
  m_initialized = false;
}

bool CLGMPAudio::SameFormat(
  const AudioFormat& left, const AudioFormat& right)
{
  return left.sampleFormat == right.sampleFormat &&
    left.sampleRate == right.sampleRate &&
    left.channelMask == right.channelMask &&
    left.channels == right.channels &&
    left.containerBits == right.containerBits &&
    left.validBits == right.validBits &&
    left.blockAlign == right.blockAlign;
}

bool CLGMPAudio::SamePacket(
  const AudioPacket& left, const AudioPacket& right)
{
  return left.endpointGeneration == right.endpointGeneration &&
    left.streamGeneration == right.streamGeneration &&
    left.flags == right.flags && left.frames == right.frames &&
    left.position == right.position && left.timeNs == right.timeNs &&
    left.rateQ32 == right.rateQ32 && SameFormat(left.format, right.format);
}

bool CLGMPAudio::ValidFormat(const AudioFormat& format)
{
  if (!format.sampleRate ||
      format.sampleRate > KVMFR_AUDIO_MAX_SAMPLE_RATE ||
      !format.channels ||
      format.channels > KVMFR_AUDIO_STREAM_MAX_CHANNELS ||
      !format.channelMask ||
      (format.channelMask & ~KVMFR_AUDIO_CHANNEL_POSITION_MASK) ||
      ChannelCount(format.channelMask) != format.channels)
    return false;

  bool validSampleFormat = false;
  switch (format.sampleFormat)
  {
    case AudioSampleFormat::S16_LE:
      validSampleFormat =
        format.containerBits == 16 && format.validBits == 16;
      break;
    case AudioSampleFormat::S24_LE:
      validSampleFormat =
        format.containerBits == 24 && format.validBits == 24;
      break;
    case AudioSampleFormat::S32_LE:
    case AudioSampleFormat::F32_LE:
      validSampleFormat =
        format.containerBits == 32 && format.validBits == 32;
      break;
    default:
      return false;
  }

  const uint16_t bytes = static_cast<uint16_t>(
    format.channels * (format.containerBits / 8U));
  return validSampleFormat && format.blockAlign == bytes;
}

KVMFRAudioFormat CLGMPAudio::WireFormat(
  const AudioFormat& format)
{
  KVMFRAudioFormat result = {};
  result.version       = KVMFR_AUDIO_FORMAT_VERSION;
  result.size          = sizeof(result);
  result.sampleFormat  = static_cast<uint32_t>(format.sampleFormat);
  result.sampleRate    = format.sampleRate;
  result.channelMask   = format.channelMask;
  result.channelCount  = format.channels;
  result.validBits     = format.validBits;
  result.containerBits = format.containerBits;
  result.blockAlign    = format.blockAlign;
  return result;
}

KVMFRAudioStreamState CLGMPAudio::WireState(
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

KVMFRStreamDescriptor CLGMPAudio::WireDescriptor(
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

KVMFRSPMCDescriptor CLGMPAudio::WireDescriptor(
  const LGMPSPMCDescriptor& descriptor)
{
  KVMFRSPMCDescriptor wire = {};
  wire.magic      = descriptor.magic;
  wire.version    = descriptor.version;
  wire.size       = descriptor.size;
  wire.offset     = descriptor.offset;
  wire.regionSize = descriptor.regionSize;
  wire.slotCount  = descriptor.slotCount;
  wire.slotSize   = descriptor.slotSize;
  wire.maxReaders = descriptor.maxReaders;
  wire.reserved   = descriptor.reserved;
  return wire;
}

bool CLGMPAudio::InitializeStreams()
{
  const LGMPSPMCConfig playback =
  {
    KVMFR_AUDIO_STREAM_SLOT_COUNT,
    static_cast<uint32_t>(KVMFR_AUDIO_PLAYBACK_SLOT_BYTES),
    KVMFR_AUDIO_MAX_PLAYBACK_READERS,
  };
  LGMP_STATUS status = m_host.CreateSPMC(playback, &m_playbackStream);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to create playback audio stream: %s",
      lgmpStatusString(status));
    return false;
  }

  const LGMPStreamConfig capture =
  {
    LGMP_STREAM_CLIENT_TO_HOST,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_AUDIO_STREAM_SLOT_COUNT,
    static_cast<uint32_t>(KVMFR_AUDIO_CAPTURE_SLOT_BYTES),
  };
  status = m_host.CreateStream(capture, &m_captureStream);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to create capture audio stream: %s",
      lgmpStatusString(status));
    FreeStreams();
    return false;
  }

  const LGMPStreamConfig hostControl =
  {
    LGMP_STREAM_HOST_TO_CLIENT,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_AUDIO_STREAM_SLOT_COUNT,
    static_cast<uint32_t>(KVMFR_AUDIO_HOST_CONTROL_SLOT_BYTES),
  };
  const LGMPStreamConfig clientControl =
  {
    LGMP_STREAM_CLIENT_TO_HOST,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_AUDIO_STREAM_SLOT_COUNT,
    static_cast<uint32_t>(KVMFR_AUDIO_CLIENT_CONTROL_SLOT_BYTES),
  };
  for (Subscriber& subscriber : m_subscribers)
  {
    status = m_host.CreateStream(hostControl, &subscriber.hostControl);
    if (status == LGMP_OK)
      status = m_host.CreateStream(
        clientControl, &subscriber.clientControl);
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("Failed to create audio control stream: %s",
        lgmpStatusString(status));
      FreeStreams();
      return false;
    }

    LGMPStreamDescriptor descriptor = {};
    lgmpHostStreamGetDescriptor(subscriber.hostControl, &descriptor);
    subscriber.hostControlDescriptor = WireDescriptor(descriptor);
    lgmpHostStreamGetDescriptor(subscriber.clientControl, &descriptor);
    subscriber.clientControlDescriptor = WireDescriptor(descriptor);
  }

  LGMPSPMCDescriptor playbackDescriptor = {};
  lgmpHostSPMCGetDescriptor(m_playbackStream, &playbackDescriptor);
  m_playbackDescriptor = WireDescriptor(playbackDescriptor);
  LGMPStreamDescriptor captureDescriptor = {};
  lgmpHostStreamGetDescriptor(m_captureStream, &captureDescriptor);
  m_captureDescriptor = WireDescriptor(captureDescriptor);
  return true;
}

void CLGMPAudio::FreeStreams()
{
  for (Subscriber& subscriber : m_subscribers)
  {
    lgmpHostStreamFree(&subscriber.clientControl);
    lgmpHostStreamFree(&subscriber.hostControl);
    subscriber.clientControlDescriptor = {};
    subscriber.hostControlDescriptor = {};
  }
  lgmpHostStreamFree(&m_captureStream);
  lgmpHostSPMCFree(&m_playbackStream);
  m_captureDescriptor = {};
  m_playbackDescriptor = {};
}

DWORD CALLBACK CLGMPAudio::InboundThreadProc(void * context)
{
  static_cast<CLGMPAudio *>(context)->InboundThread();
  return 0;
}

bool CLGMPAudio::StartInboundWorker()
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

void CLGMPAudio::StopInboundWorker()
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

void CLGMPAudio::InboundThread()
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
    {
      CSRWExclusiveLock lock(m_lock);
      if (!m_inboundStopping)
        Fail("lgmpStreamPollInit(audio)", pollStatus);
    }
    if (avTaskHandle)
      AvRevertMmThreadCharacteristics(avTaskHandle);
    return;
  }

  const HANDLE idleWaits[] =
    { m_inboundStopEvent, m_inboundWakeEvent };
  const HANDLE activeWaits[] =
    { m_inboundStopEvent, m_inboundWakeEvent, m_inboundPollTimer };
  bool workerFailed = false;
  for (;;)
  {
    const DWORD stopped = WaitForSingleObject(m_inboundStopEvent, 0);
    if (stopped == WAIT_OBJECT_0)
      break;
    if (stopped == WAIT_FAILED)
    {
      DEBUG_ERROR_HR(GetLastError(),
        "LGMP audio stop-event poll failed");
      workerFailed = true;
      break;
    }

    uint64_t before = 0;
    bool active = false;
    {
      CSRWSharedLock lock(m_lock);
      before = m_inboundActivitySerial;
      for (const Subscriber& subscriber : m_subscribers)
        if (subscriber.state != SubscriberState::NONE)
        {
          active = true;
          break;
        }
    }

    if (active)
    {
      DrainInbound();
      {
        CSRWExclusiveLock lock(m_lock);
        PumpState();
        CheckPlaybackBackpressure(GetTickCount64());
      }
      NotifyPlaybackReady();
    }

    {
      CSRWSharedLock lock(m_lock);
      if (before != m_inboundActivitySerial)
        lgmpStreamPollActivity(&streamPoll);
      active = false;
      for (const Subscriber& subscriber : m_subscribers)
        if (subscriber.state != SubscriberState::NONE)
        {
          active = true;
          break;
        }
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
        workerFailed = true;
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
      workerFailed = true;
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
      workerFailed = true;
      break;
    }
  }

  if (workerFailed)
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_inboundStopping)
      Fail("audio ingress worker", LGMP_ERR_CLOCK_FAILURE);
  }
  if (avTaskHandle)
    AvRevertMmThreadCharacteristics(avTaskHandle);
}

bool CLGMPAudio::Initialize()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  if (m_initialized)
    return true;

  if (!m_claimResponderReady)
  {
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
      return false;
    }
    m_claimResponderReady = true;
  }

  // LGMP shared allocations are monotonic. Reserve conservatively before
  // the first data-plane allocation; the small response pool above remains
  // available when the optional layout cannot be created.
  if (m_host.Available() < AUDIO_SHARED_MEMORY_PREFLIGHT)
  {
    DEBUG_WARN("LGMP audio requires at least %zu bytes of available "
      "shared memory", AUDIO_SHARED_MEMORY_PREFLIGHT);
    return false;
  }
  if (!InitializeStreams())
    return false;

  m_state.generation = 1;
  m_initialized = true;
  m_inboundStopping = false;
  if (!StartInboundWorker())
  {
    m_initialized = false;
    FreeStreams();
    return false;
  }
  return true;
}

bool CLGMPAudio::BindSubscriber(uint32_t index,
  uint32_t clientID, const KVMFRAudioClaim& claim)
{
  if (index >= LGMP_MAX_CLIENTS ||
      m_subscribers[index].state != SubscriberState::NONE)
    return false;

  Subscriber& subscriber = m_subscribers[index];
  LGMP_STATUS status = lgmpHostStreamBind(
    subscriber.hostControl, clientID, &subscriber.hostControlEpoch);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to bind host audio control for client %u: %s",
      clientID, lgmpStatusString(status));
    return false;
  }

  status = lgmpHostStreamBind(
    subscriber.clientControl, clientID, &subscriber.clientControlEpoch);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to bind client audio control for client %u: %s",
      clientID, lgmpStatusString(status));
    (void)lgmpHostStreamUnbind(subscriber.hostControl);
    return false;
  }

  status = lgmpHostSPMCReaderBind(m_playbackStream, clientID,
    &subscriber.playbackReaderID, &subscriber.playbackReaderEpoch);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to bind playback reader for client %u: %s",
      clientID, lgmpStatusString(status));
    (void)lgmpHostStreamUnbind(subscriber.clientControl);
    (void)lgmpHostStreamUnbind(subscriber.hostControl);
    return false;
  }

  subscriber.state = SubscriberState::BOUND;
  subscriber.clientID = clientID;
  subscriber.requestGeneration = claim.requestGeneration;
  subscriber.clientCapabilities = claim.clientCapabilities;
  subscriber.subscriptions = claim.subscriptions;
  subscriber.bindingEpoch =
    m_nextBindingEpoch = NextEpoch(m_nextBindingEpoch);
  subscriber.roleEpoch = m_nextRoleEpoch = NextEpoch(m_nextRoleEpoch);
  subscriber.claimToken = claim.claimToken;
  subscriber.serial =
    m_nextSubscriberSerial = NextSerial(m_nextSubscriberSerial);
  subscriber.stateRevision = 1;
  RenewLease(subscriber);
  RefreshGranted(index, false);
  return true;
}

bool CLGMPAudio::TryUnbindSubscriber(uint32_t index)
{
  if (index >= LGMP_MAX_CLIENTS)
    return true;
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state == SubscriberState::NONE)
    return true;
  if (m_captureCallbackInFlight && m_captureBinding == index)
    return false;

  if (m_pendingCapture.valid &&
      m_pendingCapture.subscriber == index)
    ReleasePendingCapture();

  bool complete = true;
  if (m_captureBinding == index)
  {
    const LGMP_STATUS status = lgmpHostStreamUnbind(m_captureStream);
    if (status == LGMP_ERR_STREAM_BUSY)
      complete = false;
    else if (status == LGMP_OK || status == LGMP_ERR_STREAM_UNBOUND)
    {
      m_captureBinding = INVALID_SLOT;
      if (m_captureOwner == index)
        m_captureOwner = INVALID_SLOT;
      subscriber.captureEpoch = 0;
      subscriber.captureRevoking = false;
      subscriber.captureRevokeDeadline = 0;
      AssignCaptureOwner();
    }
    else
    {
      Fail("lgmpHostStreamUnbind(audio capture)", status);
      complete = false;
    }
  }

  for (PLGMPHostStream stream :
      { subscriber.clientControl, subscriber.hostControl })
  {
    const LGMP_STATUS status = lgmpHostStreamUnbind(stream);
    if (status == LGMP_ERR_STREAM_BUSY)
      complete = false;
    else if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
    {
      Fail("lgmpHostStreamUnbind(audio control)", status);
      complete = false;
    }
  }

  const LGMP_STATUS readerStatus = lgmpHostSPMCReaderUnbind(
    m_playbackStream, subscriber.playbackReaderID);
  if (readerStatus == LGMP_ERR_STREAM_BUSY)
    complete = false;
  else if (readerStatus != LGMP_OK &&
      readerStatus != LGMP_ERR_STREAM_UNBOUND)
  {
    Fail("lgmpHostSPMCReaderUnbind(audio)", readerStatus);
    complete = false;
  }
  return complete;
}

void CLGMPAudio::ForceUnbindSubscriber(uint32_t index)
{
  if (index >= LGMP_MAX_CLIENTS)
    return;
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state == SubscriberState::NONE)
    return;

  if (m_pendingCapture.valid &&
      m_pendingCapture.subscriber == index)
    ReleasePendingCapture();
  if (m_captureBinding == index)
  {
    const LGMP_STATUS status =
      lgmpHostStreamForceUnbind(m_captureStream);
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
      DEBUG_WARN("Failed to force-unbind audio capture: %s",
        lgmpStatusString(status));
  }

  for (PLGMPHostStream stream :
      { subscriber.clientControl, subscriber.hostControl })
  {
    const LGMP_STATUS status = lgmpHostStreamForceUnbind(stream);
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
      DEBUG_WARN("Failed to force-unbind audio control: %s",
        lgmpStatusString(status));
  }

  const LGMP_STATUS status = lgmpHostSPMCReaderForceUnbind(
    m_playbackStream, subscriber.playbackReaderID);
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
    DEBUG_WARN("Failed to force-unbind audio reader: %s",
      lgmpStatusString(status));
}

void CLGMPAudio::ResetSubscriber(uint32_t index)
{
  if (index >= LGMP_MAX_CLIENTS)
    return;
  Subscriber& subscriber = m_subscribers[index];
  const uint32_t clientID = subscriber.clientID;
  const uint32_t requestGeneration = subscriber.requestGeneration;
  const uint64_t claimToken = subscriber.claimToken;
  PLGMPHostStream hostControl = subscriber.hostControl;
  PLGMPHostStream clientControl = subscriber.clientControl;
  const KVMFRStreamDescriptor hostDescriptor =
    subscriber.hostControlDescriptor;
  const KVMFRStreamDescriptor clientDescriptor =
    subscriber.clientControlDescriptor;

  for (ClaimRecord& record : m_claimRecord)
    if (record.valid && record.clientID == clientID &&
        record.requestGeneration == requestGeneration &&
        record.claimToken == claimToken)
    {
      record.closed = true;
      break;
    }

  if (m_feedbackAuthority == index)
    m_feedbackAuthority = INVALID_SLOT;
  if (m_captureOwner == index)
    m_captureOwner = INVALID_SLOT;
  if (m_captureBinding == index)
    m_captureBinding = INVALID_SLOT;

  subscriber = {};
  subscriber.hostControl = hostControl;
  subscriber.clientControl = clientControl;
  subscriber.hostControlDescriptor = hostDescriptor;
  subscriber.clientControlDescriptor = clientDescriptor;
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
}

bool CLGMPAudio::ValidateClaim(
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
      !(claim.clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) ||
      ((claim.clientCapabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
        !(claim.clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK)) ||
      (claim.subscriptions & ~AUDIO_SUBSCRIPTION_MASK) ||
      ((claim.subscriptions & KVMFR_AUDIO_SUBSCRIBE_CAPTURE) &&
        !(claim.clientCapabilities & KVMFR_AUDIO_CAP_CAPTURE)))
    return false;

  for (uint64_t reserved : claim.reserved)
    if (reserved)
      return false;
  return true;
}

bool CLGMPAudio::ResponsePending(uint32_t clientID,
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

bool CLGMPAudio::QueueResponse(uint32_t clientID,
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

  available->valid = true;
  available->clientID = clientID;
  available->response = response;
  return true;
}

void CLGMPAudio::CleanupResponseMemory()
{
  for (ResponseMemory& response : m_responseMemory)
  {
    if (!response.valid || m_control.PayloadPending(response.memory))
      continue;
    response.valid = false;
    response.clientID = 0;
    response.requestGeneration = 0;
    response.claimToken = 0;
    response.result = 0;
  }
}

void CLGMPAudio::PumpResponses()
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
    const uint32_t index = bound ?
      FindSubscriber(pending.clientID) : INVALID_SLOT;
    if (recipients)
    {
      available->valid = true;
      available->clientID = pending.clientID;
      available->requestGeneration =
        pending.response.requestGeneration;
      available->claimToken = pending.response.claimToken;
      available->result = pending.response.result;
      if (index != INVALID_SLOT)
      {
        Subscriber& subscriber = m_subscribers[index];
        if (subscriber.requestGeneration ==
              pending.response.requestGeneration &&
            subscriber.claimToken == pending.response.claimToken)
        {
          subscriber.responseAdmitted = true;
          RenewLease(subscriber);
          MarkStateDirty(index);
        }
      }
    }
    else if (index != INVALID_SLOT)
      BeginRelease(index, "audio claimant disappeared");

    pending = {};
  }
}

uint32_t CLGMPAudio::FindSubscriber(uint32_t clientID) const
{
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    if (m_subscribers[index].state != SubscriberState::NONE &&
        m_subscribers[index].clientID == clientID)
      return index;
  return INVALID_SLOT;
}

uint32_t CLGMPAudio::FindFreeSubscriber() const
{
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    if (m_subscribers[index].state == SubscriberState::NONE)
      return index;
  return INVALID_SLOT;
}

bool CLGMPAudio::SubscriberPresent(uint32_t clientID,
  const uint32_t clients[LGMP_MAX_CLIENTS], unsigned count) const
{
  for (unsigned index = 0; index < count; ++index)
    if (clients[index] == clientID)
      return true;
  return false;
}

void CLGMPAudio::RenewLease(Subscriber& subscriber)
{
  subscriber.deadline =
    GetTickCount64() + KVMFR_AUDIO_OWNER_LEASE_MS;
}

void CLGMPAudio::ReleasePendingCapture()
{
  if (!m_pendingCapture.valid)
    return;
  const LGMP_STATUS status = lgmpHostStreamReadRelease(
    m_captureStream, &m_pendingCapture.buffer);
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
      status != LGMP_ERR_STREAM_STALE)
    Fail("lgmpHostStreamReadRelease(audio capture)", status);
  m_pendingCapture = {};
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
}

void CLGMPAudio::ClearBlockedPlayback(bool notify)
{
  const bool blocked = m_playbackBlocked;
  m_playbackBlocked = false;
  m_blockedPlayback = {};
  m_blockedPlaybackSize = 0;
  m_authorityBlockedSince = 0;
  if (notify && blocked)
    m_playbackReadyPending = true;
}

bool CLGMPAudio::RememberBlockedPlayback(
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

bool CLGMPAudio::HasBoundSubscribers() const
{
  for (const Subscriber& subscriber : m_subscribers)
    if (subscriber.state == SubscriberState::BOUND &&
        subscriber.responseAdmitted &&
        subscriber.subscriptionReceived &&
        (((subscriber.subscriptions &
            KVMFR_AUDIO_SUBSCRIBE_PLAYBACK) &&
          (subscriber.clientCapabilities &
            KVMFR_AUDIO_CAP_PLAYBACK)) ||
         ((subscriber.subscriptions &
            KVMFR_AUDIO_SUBSCRIBE_CAPTURE) &&
          (subscriber.clientCapabilities &
            KVMFR_AUDIO_CAP_CAPTURE))))
      return true;
  return false;
}

bool CLGMPAudio::PlaybackEligible(
  const Subscriber& subscriber) const
{
  return subscriber.state == SubscriberState::BOUND &&
    (subscriber.clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) &&
    (subscriber.subscriptions & KVMFR_AUDIO_SUBSCRIBE_PLAYBACK);
}

bool CLGMPAudio::FeedbackEligible(
  const Subscriber& subscriber) const
{
  return PlaybackEligible(subscriber) &&
    (subscriber.clientCapabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK);
}

bool CLGMPAudio::CaptureEligible(
  const Subscriber& subscriber) const
{
  return subscriber.state == SubscriberState::BOUND &&
    (subscriber.clientCapabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
    (subscriber.subscriptions & KVMFR_AUDIO_SUBSCRIBE_CAPTURE);
}

bool CLGMPAudio::HasReadyPlayback() const
{
  for (const Subscriber& subscriber : m_subscribers)
    if (PlaybackEligible(subscriber) && subscriber.ready &&
        subscriber.announcedPlayback ==
          m_state.playback.generation)
      return true;
  return false;
}

void CLGMPAudio::MarkStateDirty(uint32_t index)
{
  if (index >= LGMP_MAX_CLIENTS ||
      m_subscribers[index].state != SubscriberState::BOUND)
    return;
  Subscriber& subscriber = m_subscribers[index];
  subscriber.stateRevision = NextSerial(subscriber.stateRevision);
  subscriber.pumpStage = PumpStage::IDLE;
  subscriber.ready = false;
  subscriber.awaitingAck = false;
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
  if (!m_inboundStopping && m_inboundWakeEvent)
    SetEvent(m_inboundWakeEvent);
}

void CLGMPAudio::MarkAllStateDirty()
{
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    MarkStateDirty(index);
}

void CLGMPAudio::RefreshGranted(uint32_t index, bool bumpRole)
{
  if (index >= LGMP_MAX_CLIENTS)
    return;
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state != SubscriberState::BOUND)
    return;

  KVMFRAudioCapabilities granted = 0;
  if (m_stateKnown &&
      (m_state.capabilities & KVMFR_AUDIO_CAP_PLAYBACK) &&
      (subscriber.clientCapabilities & KVMFR_AUDIO_CAP_PLAYBACK))
    granted |= KVMFR_AUDIO_CAP_PLAYBACK;
  if (m_captureOwner == index && CaptureEligible(subscriber) &&
      (m_state.capabilities & KVMFR_AUDIO_CAP_CAPTURE))
    granted |= KVMFR_AUDIO_CAP_CAPTURE;
  if (m_feedbackAuthority == index && FeedbackEligible(subscriber) &&
      (m_state.capabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK))
    granted |= KVMFR_AUDIO_CAP_CLOCK_FEEDBACK;
  if (granted == subscriber.grantedCapabilities)
    return;

  subscriber.grantedCapabilities = granted;
  if (bumpRole && subscriber.subscriptionReceived)
    subscriber.roleEpoch =
      m_nextRoleEpoch = NextEpoch(m_nextRoleEpoch);
  subscriber.response.grantedCapabilities =
    granted | KVMFR_AUDIO_CAP_PLAYBACK;
  subscriber.response.roleEpoch = subscriber.roleEpoch;
  MarkStateDirty(index);
}

void CLGMPAudio::AssignFeedbackAuthority(uint32_t index)
{
  if (m_feedbackAuthority != INVALID_SLOT ||
      index >= LGMP_MAX_CLIENTS ||
      !FeedbackEligible(m_subscribers[index]))
    return;
  m_feedbackAuthority = index;
  RefreshGranted(index, true);
  DEBUG_INFO("Audio feedback authority assigned to client %u",
    m_subscribers[index].clientID);
}

void CLGMPAudio::RevokeFeedbackAuthority(const char * reason)
{
  if (m_feedbackAuthority == INVALID_SLOT)
    return;
  const uint32_t index = m_feedbackAuthority;
  m_feedbackAuthority = INVALID_SLOT;
  m_subscribers[index].feedbackRevoked = true;
  DEBUG_INFO("Audio feedback authority client %u revoked: %s",
    m_subscribers[index].clientID, reason);
  RefreshGranted(index, true);
  m_authorityBlockedSince = 0;
  if (m_playbackBlocked)
    ClearBlockedPlayback(true);
}

void CLGMPAudio::PromoteFeedbackAuthority(
  uint64_t playbackGeneration)
{
  if (!playbackGeneration || m_feedbackAuthority != INVALID_SLOT)
    return;
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    if (FeedbackEligible(m_subscribers[index]) &&
        !m_subscribers[index].feedbackRevoked)
    {
      AssignFeedbackAuthority(index);
      return;
    }
}

void CLGMPAudio::AssignCaptureOwner()
{
  if (m_captureOwner != INVALID_SLOT ||
      m_captureBinding != INVALID_SLOT)
    return;
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
  {
    Subscriber& subscriber = m_subscribers[index];
    if (!CaptureEligible(subscriber))
      continue;
    uint32_t epoch = 0;
    const LGMP_STATUS status = lgmpHostStreamBind(
      m_captureStream, subscriber.clientID, &epoch);
    if (status == LGMP_ERR_STREAM_BUSY)
      return;
    if (status != LGMP_OK)
    {
      Fail("lgmpHostStreamBind(audio capture)", status);
      return;
    }
    m_captureOwner = index;
    m_captureBinding = index;
    subscriber.captureEpoch = epoch;
    subscriber.nextCaptureSequence = 0;
    subscriber.captureSequenceKnown = false;
    subscriber.captureDiscontinuity = true;
    subscriber.captureRevokeDeadline = 0;
    RefreshGranted(index, true);
    DEBUG_INFO("Audio capture assigned to client %u",
      subscriber.clientID);
    return;
  }
}

void CLGMPAudio::MaintainCaptureBinding()
{
  if (m_captureBinding == INVALID_SLOT)
  {
    AssignCaptureOwner();
    return;
  }

  const uint32_t index = m_captureBinding;
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state != SubscriberState::BOUND ||
      !subscriber.captureRevoking || m_captureCallbackInFlight)
    return;

  const bool acknowledged =
    subscriber.pumpRevision >= subscriber.captureRevokeRevision &&
    !subscriber.awaitingAck && subscriber.ready;
  const uint64_t now = GetTickCount64();
  const bool timedOut = subscriber.captureRevokeDeadline &&
    now >= subscriber.captureRevokeDeadline;
  if (!acknowledged && !timedOut)
    return;
  if (!acknowledged && subscriber.captureRevokeDeadline != 1)
  {
    DEBUG_WARN("Audio capture revoke acknowledgement timed out for "
      "client %u", subscriber.clientID);
    subscriber.captureRevokeDeadline = 1;
  }

  if (m_pendingCapture.valid &&
      m_pendingCapture.subscriber == index)
    ReleasePendingCapture();
  const LGMP_STATUS status = lgmpHostStreamUnbind(m_captureStream);
  if (status == LGMP_ERR_STREAM_BUSY)
    return;
  if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
  {
    Fail("lgmpHostStreamUnbind(audio capture role)", status);
    return;
  }

  m_captureBinding = INVALID_SLOT;
  subscriber.captureEpoch = 0;
  subscriber.captureRevoking = false;
  subscriber.captureRevokeDeadline = 0;
  AssignCaptureOwner();
}

void CLGMPAudio::BeginRelease(uint32_t index,
  const char * reason, bool force)
{
  if (index >= LGMP_MAX_CLIENTS)
    return;
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state == SubscriberState::NONE)
    return;
  subscriber.forceUnbind = subscriber.forceUnbind || force;
  if (subscriber.state == SubscriberState::RELEASING)
    return;

  DEBUG_INFO("Audio client %u generation %u releasing: %s",
    subscriber.clientID, subscriber.requestGeneration, reason);
  subscriber.state = SubscriberState::RELEASING;
  subscriber.ready = false;
  subscriber.awaitingAck = false;
  subscriber.pumpStage = PumpStage::IDLE;
  if (m_feedbackAuthority == index)
    RevokeFeedbackAuthority(reason);
  if (m_captureOwner == index)
    m_captureOwner = INVALID_SLOT;
  if (m_pendingCapture.valid &&
      m_pendingCapture.subscriber == index &&
      !m_captureCallbackInFlight)
    ReleasePendingCapture();

  for (PendingResponse& pending : m_pendingResponse)
    if (pending.valid && pending.clientID == subscriber.clientID &&
        pending.response.result == KVMFR_AUDIO_RESPONSE_BOUND)
      pending = {};
  if (!HasReadyPlayback())
    ClearBlockedPlayback(true);
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
  if (!m_inboundStopping && m_inboundWakeEvent)
    SetEvent(m_inboundWakeEvent);
}

void CLGMPAudio::MaintainSubscribers(uint64_t now)
{
  MaintainCaptureBinding();
  uint32_t clients[LGMP_MAX_CLIENTS] = {};
  unsigned count = 0;
  const LGMP_STATUS query = m_control.GetClientIDs(clients, count);
  const bool clientsKnown = query == LGMP_OK;
  if (!clientsKnown)
    DEBUG_WARN("Failed to query audio pointer subscribers: %s",
      lgmpStatusString(query));

  bool assignCapture = false;
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
  {
    Subscriber& subscriber = m_subscribers[index];
    if (subscriber.state == SubscriberState::NONE)
      continue;
    if (clientsKnown && !SubscriberPresent(
        subscriber.clientID, clients, count))
      subscriber.forceUnbind = true;
    if (subscriber.state == SubscriberState::BOUND)
    {
      if (subscriber.forceUnbind)
        BeginRelease(index,
          "audio pointer subscriber disappeared", true);
      else if (m_pendingCapture.valid &&
          m_pendingCapture.subscriber == index)
        RenewLease(subscriber);
      else if (now >= subscriber.deadline)
        BeginRelease(index, "audio client lease expired");
    }

    if (subscriber.state != SubscriberState::RELEASING)
      continue;
    if (m_captureCallbackInFlight && m_captureBinding == index)
      continue;
    if (subscriber.forceUnbind)
    {
      ForceUnbindSubscriber(index);
      const bool wasCapture = m_captureOwner == index;
      ResetSubscriber(index);
      assignCapture = assignCapture || wasCapture;
      continue;
    }
    if (TryUnbindSubscriber(index))
    {
      DEBUG_INFO("Audio client %u generation %u released",
        subscriber.clientID, subscriber.requestGeneration);
      const bool wasCapture = m_captureOwner == index;
      ResetSubscriber(index);
      assignCapture = assignCapture || wasCapture;
    }
  }
  if (assignCapture || (m_captureOwner == INVALID_SLOT &&
      m_captureBinding == INVALID_SLOT))
    AssignCaptureOwner();
}

void CLGMPAudio::HandleClaim(uint32_t clientID,
  const KVMFRAudioClaim& claim)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_claimResponderReady)
    return;

  KVMFRAudioStreamResponse response = {};
  response.magic = KVMFR_AUDIO_RESPONSE_MAGIC;
  response.version = KVMFR_AUDIO_CONTROL_VERSION;
  response.size = sizeof(response);
  response.requestGeneration = claim.requestGeneration;
  response.boundClientID = clientID;
  response.endpointGeneration = m_state.generation;
  response.claimToken = claim.claimToken;

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
  const uint32_t existing = FindSubscriber(clientID);
  if (!claimValid)
    response.result = KVMFR_AUDIO_RESPONSE_INCOMPATIBLE;
  else if (!m_initialized || m_failed)
    response.result = KVMFR_AUDIO_RESPONSE_ERROR;
  else if (existing != INVALID_SLOT)
  {
    Subscriber& subscriber = m_subscribers[existing];
    if (subscriber.state == SubscriberState::BOUND &&
        subscriber.requestGeneration == claim.requestGeneration &&
        subscriber.claimToken == claim.claimToken)
    {
      RenewLease(subscriber);
      response = subscriber.response;
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
  else
  {
    const uint32_t index = FindFreeSubscriber();
    if (index == INVALID_SLOT)
      response.result = KVMFR_AUDIO_RESPONSE_BUSY;
    else if (!BindSubscriber(index, clientID, claim))
      response.result = KVMFR_AUDIO_RESPONSE_ERROR;
    else
    {
      Subscriber& subscriber = m_subscribers[index];
      // Admission grants playback independently from the live subscription.
      // The subscription controls whether this reader consumes PCM.
      if (m_feedbackAuthority == INVALID_SLOT &&
          (claim.subscriptions & KVMFR_AUDIO_SUBSCRIBE_PLAYBACK) &&
          (claim.clientCapabilities &
            KVMFR_AUDIO_CAP_CLOCK_FEEDBACK))
        AssignFeedbackAuthority(index);
      AssignCaptureOwner();
      RefreshGranted(index, false);

      response.result = KVMFR_AUDIO_RESPONSE_BOUND;
      response.protocolVersion = KVMFR_AUDIO_PROTOCOL_MAX;
      response.grantedCapabilities =
        subscriber.grantedCapabilities | KVMFR_AUDIO_CAP_PLAYBACK;
      response.bindingEpoch = subscriber.bindingEpoch;
      response.roleEpoch = subscriber.roleEpoch;
      response.playbackReaderID = subscriber.playbackReaderID;
      response.playback = m_playbackDescriptor;
      response.hostControl = subscriber.hostControlDescriptor;
      response.clientControl = subscriber.clientControlDescriptor;
      response.capture = m_captureDescriptor;
      subscriber.response = response;

      claimSlot->valid = true;
      claimSlot->closed = false;
      claimSlot->clientID = clientID;
      claimSlot->requestGeneration = claim.requestGeneration;
      claimSlot->claimToken = claim.claimToken;
      claimSlot->response = response;
      DEBUG_INFO("Audio client %u generation %u admitted in slot %u",
        clientID, claim.requestGeneration, index);
    }
  }

  if (claimValid && claimSlot &&
      response.result == KVMFR_AUDIO_RESPONSE_ERROR &&
      (!claimRecord || RequestNewer(claim.requestGeneration,
        claimRecord->requestGeneration)))
  {
    claimSlot->valid = true;
    claimSlot->closed = true;
    claimSlot->clientID = clientID;
    claimSlot->requestGeneration = claim.requestGeneration;
    claimSlot->claimToken = claim.claimToken;
    claimSlot->response = response;
  }

  if (!QueueResponse(clientID, response))
  {
    DEBUG_WARN("Audio response queue is full for client %u", clientID);
    if (response.result == KVMFR_AUDIO_RESPONSE_BOUND)
    {
      const uint32_t index = FindSubscriber(clientID);
      if (index != INVALID_SLOT)
        BeginRelease(index, "audio response queue exhausted");
    }
  }
  PumpResponses();
}

KVMFRAudioStatus CLGMPAudio::WireStatus(uint32_t index) const
{
  KVMFRAudioStatus status = {};
  if (index >= LGMP_MAX_CLIENTS)
    return status;
  const Subscriber& subscriber = m_subscribers[index];

  status.version = KVMFR_AUDIO_STATUS_VERSION;
  status.size = sizeof(status);
  status.protocolVersion = KVMFR_AUDIO_PROTOCOL_MAX;
  status.requestGeneration = subscriber.requestGeneration;
  status.capabilities =
    m_state.capabilities & subscriber.clientCapabilities;
  status.grantedCapabilities =
    subscriber.grantedCapabilities & status.capabilities;
  status.bindingEpoch = subscriber.bindingEpoch;
  status.roleEpoch = subscriber.roleEpoch;
  status.boundClientID = subscriber.clientID;
  status.endpointGeneration = m_state.generation;
  status.playbackGeneration = m_state.playback.generation;
  status.captureGeneration = m_state.capture.generation;
  status.playbackState = WireState(m_state.playback.state);
  status.captureState = WireState(m_state.capture.state);

  if ((status.capabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
      m_feedbackAuthority != INVALID_SLOT)
  {
    const Subscriber& authority =
      m_subscribers[m_feedbackAuthority];
    if (authority.state == SubscriberState::BOUND)
    {
      status.feedbackAuthorityClientID = authority.clientID;
      status.feedbackAuthorityEpoch = authority.roleEpoch;
    }
  }
  if ((status.capabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
      m_captureOwner != INVALID_SLOT &&
      m_subscribers[m_captureOwner].state == SubscriberState::BOUND)
    status.captureOwnerClientID =
      m_subscribers[m_captureOwner].clientID;

  if (m_state.playback.formatValid &&
      ValidFormat(m_state.playback.format))
    status.playbackFormat = WireFormat(m_state.playback.format);
  if (m_state.capture.formatValid &&
      ValidFormat(m_state.capture.format))
    status.captureFormat = WireFormat(m_state.capture.format);

  if (m_state.available)
    status.flags |= KVMFR_AUDIO_STATUS_AVAILABLE;
  if (subscriber.state == SubscriberState::BOUND)
    status.flags |= KVMFR_AUDIO_STATUS_BOUND;
  if (status.capabilities & KVMFR_AUDIO_CAP_PLAYBACK)
    status.flags |= KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE;
  if (status.capabilities & KVMFR_AUDIO_CAP_CAPTURE)
    status.flags |= KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE;

  const bool playbackPrewarm =
    (m_state.playback.state == AudioStreamState::ACQUIRED ||
      m_state.playback.state == AudioStreamState::PAUSED) &&
    m_playbackRunGeneration != m_state.playback.generation;
  if (m_state.available && m_state.active &&
      PlaybackEligible(subscriber) &&
      (status.grantedCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) &&
      m_state.playback.generation &&
      m_state.playback.formatValid &&
      ValidFormat(m_state.playback.format) &&
      (m_state.playback.state == AudioStreamState::RUNNING ||
        playbackPrewarm))
    status.flags |= KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE;

  if (m_state.available && m_state.active &&
      CaptureEligible(subscriber) &&
      (status.grantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
      m_state.capture.generation &&
      m_state.capture.formatValid &&
      ValidFormat(m_state.capture.format) &&
      m_state.capture.state == AudioStreamState::RUNNING)
    status.flags |= KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE;
  return status;
}

bool CLGMPAudio::WriteControlMessage(uint32_t index,
  KVMFRAudioMessageType type, uint64_t streamGeneration,
  KVMFRAudioMessageFlags flags, uint64_t position, int64_t timeNs,
  uint64_t rateQ32, uint32_t frames, const void * payload,
  uint32_t length)
{
  if (index >= LGMP_MAX_CLIENTS)
    return false;
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state != SubscriberState::BOUND)
    return false;

  LGMPStreamBuffer buffer = {};
  LGMP_STATUS status = lgmpHostStreamWriteAcquire(
    subscriber.hostControl, &buffer);
  if (status == LGMP_ERR_STREAM_FULL)
    return false;
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    BeginRelease(index, "host audio control binding lost");
    return false;
  }
  if (status != LGMP_OK)
  {
    Fail("lgmpHostStreamWriteAcquire(audio control)", status);
    return false;
  }

  const uint32_t bytes =
    static_cast<uint32_t>(sizeof(KVMFRAudioMessage) + length);
  if (!buffer.data || buffer.capacity < bytes || (length && !payload))
  {
    const LGMP_STATUS cancel = lgmpHostStreamWriteCancel(
      subscriber.hostControl, &buffer);
    if (cancel != LGMP_OK)
      DEBUG_WARN("Failed to cancel audio control reservation: %s",
        lgmpStatusString(cancel));
    Fail("undersized host audio control slot", LGMP_ERR_INVALID_SIZE);
    return false;
  }

  KVMFRAudioMessage message = {};
  message.version = KVMFR_AUDIO_MESSAGE_VERSION;
  message.size = sizeof(message);
  message.type = type;
  message.streamGeneration = streamGeneration;
  message.flags = flags;
  message.sequence = subscriber.hostSequence;
  message.position = position;
  message.timeNs = timeNs;
  message.rateQ32 = rateQ32;
  message.frames = frames;
  message.length = length;
  WCWrite::Copy(buffer.data, &message, sizeof(message));
  if (length)
    WCWrite::Copy(static_cast<uint8_t *>(buffer.data) +
      sizeof(message), payload, length);
  WCWrite::Flush();

  status = lgmpHostStreamWriteCommit(
    subscriber.hostControl, &buffer, bytes);
  if (status == LGMP_OK)
  {
    ++subscriber.hostSequence;
    m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
    return true;
  }
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
    BeginRelease(index, "host audio control binding lost");
  else
    Fail("lgmpHostStreamWriteCommit(audio control)", status);
  return false;
}

bool CLGMPAudio::PlaybackCurrent(
  const KVMFRAudioStatus& status) const
{
  return (status.flags & KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE) != 0;
}

bool CLGMPAudio::CaptureCurrent(
  const KVMFRAudioStatus& status) const
{
  return (status.flags & KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE) != 0;
}

void CLGMPAudio::BeginStatePump(uint32_t index)
{
  Subscriber& subscriber = m_subscribers[index];
  subscriber.pendingStatus = WireStatus(index);
  subscriber.pumpRevision = subscriber.stateRevision;
  subscriber.pumpStage = PumpStage::STATUS;
  subscriber.ready = false;
  subscriber.awaitingAck = false;
}

void CLGMPAudio::PumpSubscriberState(uint32_t index)
{
  Subscriber& subscriber = m_subscribers[index];
  if (subscriber.state != SubscriberState::BOUND ||
      !subscriber.responseAdmitted ||
      !subscriber.subscriptionReceived || !m_stateKnown || m_failed)
    return;
  if (subscriber.pumpRevision != subscriber.stateRevision)
    BeginStatePump(index);
  else if (subscriber.pumpStage == PumpStage::IDLE)
    return;

  for (unsigned step = 0; step < 12U; ++step)
  {
    if (subscriber.state != SubscriberState::BOUND ||
        subscriber.pumpRevision != subscriber.stateRevision)
      return;
    const KVMFRAudioStatus& status = subscriber.pendingStatus;
    switch (subscriber.pumpStage)
    {
      case PumpStage::STATUS:
        if (!WriteControlMessage(index, KVMFR_AUDIO_MESSAGE_STATUS,
            0, 0, 0, 0, 0, 0, &status, sizeof(status)))
          return;
        subscriber.pumpStage = PumpStage::PLAYBACK_STOP;
        break;

      case PumpStage::PLAYBACK_STOP:
        if (subscriber.announcedPlayback &&
            (!PlaybackCurrent(status) ||
              subscriber.announcedPlayback !=
                status.playbackGeneration))
        {
          if (!WriteControlMessage(index,
              KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP,
              subscriber.announcedPlayback, 0, 0, 0, 0, 0,
              nullptr, 0))
            return;
          subscriber.announcedPlayback = 0;
        }
        subscriber.pumpStage = PumpStage::CAPTURE_STOP;
        break;

      case PumpStage::CAPTURE_STOP:
        if (subscriber.announcedCapture &&
            (!CaptureCurrent(status) ||
              subscriber.announcedCapture != status.captureGeneration))
        {
          if (!WriteControlMessage(index,
              KVMFR_AUDIO_MESSAGE_CAPTURE_STOP,
              subscriber.announcedCapture, 0, 0, 0, 0, 0,
              nullptr, 0))
            return;
          subscriber.announcedCapture = 0;
        }
        subscriber.pumpStage = PumpStage::PLAYBACK_START;
        break;

      case PumpStage::PLAYBACK_START:
        if (PlaybackCurrent(status) &&
            subscriber.announcedPlayback != status.playbackGeneration)
        {
          if (!WriteControlMessage(index,
              KVMFR_AUDIO_MESSAGE_PLAYBACK_START,
              status.playbackGeneration, 0, 0, 0, 0, 0,
              &status.playbackFormat, sizeof(status.playbackFormat)))
            return;
          subscriber.announcedPlayback = status.playbackGeneration;
          subscriber.pumpStage = PumpStage::PLAYBACK_VOLUME;
        }
        else
          subscriber.pumpStage = PumpStage::CAPTURE_START;
        break;

      case PumpStage::PLAYBACK_VOLUME:
      {
        KVMFRAudioVolume volume = {};
        volume.version = KVMFR_AUDIO_VOLUME_VERSION;
        volume.size = sizeof(volume);
        volume.channelCount = status.playbackFormat.channelCount;
        for (unsigned channel = 0;
            channel < volume.channelCount; ++channel)
          volume.values[channel] = UINT16_MAX;
        if (!WriteControlMessage(index,
            KVMFR_AUDIO_MESSAGE_PLAYBACK_VOLUME,
            status.playbackGeneration, 0, 0, 0, 0, 0,
            &volume, sizeof(volume)))
          return;
        subscriber.pumpStage = PumpStage::PLAYBACK_MUTE;
        break;
      }

      case PumpStage::PLAYBACK_MUTE:
        if (!WriteControlMessage(index,
            KVMFR_AUDIO_MESSAGE_PLAYBACK_MUTE,
            status.playbackGeneration, 0, 0, 0, 0, 0,
            nullptr, 0))
          return;
        subscriber.pumpStage = PumpStage::CAPTURE_START;
        break;

      case PumpStage::CAPTURE_START:
        if (CaptureCurrent(status) &&
            subscriber.announcedCapture != status.captureGeneration)
        {
          if (!WriteControlMessage(index,
              KVMFR_AUDIO_MESSAGE_CAPTURE_START,
              status.captureGeneration, 0, 0, 0, 0, 0,
              &status.captureFormat, sizeof(status.captureFormat)))
            return;
          subscriber.announcedCapture = status.captureGeneration;
          subscriber.pumpStage = PumpStage::CAPTURE_VOLUME;
        }
        else
          subscriber.pumpStage = PumpStage::BARRIER;
        break;

      case PumpStage::CAPTURE_VOLUME:
      {
        KVMFRAudioVolume volume = {};
        volume.version = KVMFR_AUDIO_VOLUME_VERSION;
        volume.size = sizeof(volume);
        volume.channelCount = status.captureFormat.channelCount;
        for (unsigned channel = 0;
            channel < volume.channelCount; ++channel)
          volume.values[channel] = UINT16_MAX;
        if (!WriteControlMessage(index,
            KVMFR_AUDIO_MESSAGE_CAPTURE_VOLUME,
            status.captureGeneration, 0, 0, 0, 0, 0,
            &volume, sizeof(volume)))
          return;
        subscriber.pumpStage = PumpStage::CAPTURE_MUTE;
        break;
      }

      case PumpStage::CAPTURE_MUTE:
        if (!WriteControlMessage(index,
            KVMFR_AUDIO_MESSAGE_CAPTURE_MUTE,
            status.captureGeneration, 0, 0, 0, 0, 0,
            nullptr, 0))
          return;
        subscriber.pumpStage = PumpStage::BARRIER;
        break;

      case PumpStage::BARRIER:
      {
        KVMFRAudioStateBarrier barrier = {};
        barrier.bindingEpoch = subscriber.bindingEpoch;
        barrier.roleEpoch = subscriber.roleEpoch;
        barrier.stateSerial = NextSerial(subscriber.barrier.stateSerial);
        const uint64_t generation = PlaybackCurrent(status) ?
          status.playbackGeneration : 0;
        if (!WriteControlMessage(index,
            KVMFR_AUDIO_MESSAGE_STATE_BARRIER, generation,
            0, 0, 0, 0, 0, &barrier, sizeof(barrier)))
          return;
        subscriber.barrier = barrier;
        subscriber.barrierGeneration = generation;
        subscriber.awaitingAck = true;
        subscriber.pumpStage = PumpStage::IDLE;
        break;
      }

      case PumpStage::IDLE:
        return;
    }
  }
}

void CLGMPAudio::PumpState()
{
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    PumpSubscriberState(index);
}

void CLGMPAudio::CheckPlaybackBackpressure(uint64_t now)
{
  if (!m_playbackBlocked)
    return;
  if (!m_started || m_failed || !m_stateKnown ||
      !m_state.available || !m_state.active ||
      m_blockedPlayback.endpointGeneration != m_state.generation ||
      m_blockedPlayback.streamGeneration !=
        m_state.playback.generation ||
      !m_state.playback.formatValid ||
      m_state.playback.state != AudioStreamState::RUNNING ||
      !SameFormat(m_blockedPlayback.format,
        m_state.playback.format))
  {
    ClearBlockedPlayback(true);
    return;
  }

  bool eligible = false;
  for (const Subscriber& subscriber : m_subscribers)
    if (subscriber.responseAdmitted &&
        subscriber.subscriptionReceived &&
        PlaybackEligible(subscriber))
    {
      eligible = true;
      break;
    }
  if (!eligible)
  {
    ClearBlockedPlayback(true);
    return;
  }
  if (!HasReadyPlayback())
    return;

  if (m_feedbackAuthority == INVALID_SLOT)
  {
    ClearBlockedPlayback(true);
    return;
  }
  Subscriber& authority = m_subscribers[m_feedbackAuthority];
  if (!authority.ready || !PlaybackEligible(authority) ||
      !(authority.grantedCapabilities &
        KVMFR_AUDIO_CAP_CLOCK_FEEDBACK))
  {
    ClearBlockedPlayback(true);
    return;
  }

  LGMPSPMCReaderState reader = {};
  const LGMP_STATUS status = lgmpHostSPMCReaderGetState(
    m_playbackStream, authority.playbackReaderID, &reader);
  if (status == LGMP_ERR_STREAM_BUSY)
    return;
  bool stalled = false;
  if (status == LGMP_OK)
  {
    if (reader.state != LGMP_SPMC_READER_READY ||
        reader.clientID != authority.clientID ||
        reader.epoch != authority.playbackReaderEpoch)
    {
      RevokeFeedbackAuthority("playback reader binding lost");
      return;
    }
    stalled = reader.producerSequence - reader.consumerSequence >=
      KVMFR_AUDIO_STREAM_SLOT_COUNT;
  }
  else
  {
    Fail("lgmpHostSPMCReaderGetState(audio)", status);
    return;
  }

  if (!stalled)
  {
    ClearBlockedPlayback(true);
    return;
  }
  if (!m_authorityBlockedSince)
    m_authorityBlockedSince = now;
  else if (now - m_authorityBlockedSince >= AUTHORITY_STALL_MS)
    RevokeFeedbackAuthority("playback reader stalled");
}

bool CLGMPAudio::ParseInbound(
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

bool CLGMPAudio::PendingCaptureCurrent(uint32_t index) const
{
  if (!m_pendingCapture.valid || index >= LGMP_MAX_CLIENTS ||
      m_pendingCapture.subscriber != index)
    return false;
  const Subscriber& subscriber = m_subscribers[index];
  const AudioPacket& packet = m_pendingCapture.packet;
  return subscriber.state == SubscriberState::BOUND &&
    subscriber.serial == m_pendingCapture.subscriberSerial &&
    m_captureOwner == index && m_captureBinding == index &&
    CaptureEligible(subscriber) &&
    (subscriber.grantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
    subscriber.announcedCapture == packet.streamGeneration &&
    m_stateKnown && m_state.available && m_state.active &&
    (m_state.capabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
    m_state.capture.formatValid &&
    ValidFormat(m_state.capture.format) &&
    m_state.capture.state == AudioStreamState::RUNNING &&
    packet.endpointGeneration == m_state.generation &&
    packet.streamGeneration == m_state.capture.generation &&
    SameFormat(packet.format, m_state.capture.format);
}

bool CLGMPAudio::PrepareCapture(
  const KVMFRAudioMessage& message, const LGMPStreamBuffer& buffer,
  uint32_t index)
{
  if (index >= LGMP_MAX_CLIENTS ||
      m_subscribers[index].state != SubscriberState::BOUND ||
      m_captureOwner != index || m_captureBinding != index ||
      !m_stateKnown ||
      !m_state.available || !m_state.active ||
      !(m_state.capabilities & KVMFR_AUDIO_CAP_CAPTURE) ||
      !m_state.capture.formatValid ||
      !ValidFormat(m_state.capture.format) ||
      m_state.capture.state != AudioStreamState::RUNNING ||
      message.streamGeneration != m_state.capture.generation)
    return false;

  Subscriber& subscriber = m_subscribers[index];
  const bool silent =
    (message.flags & KVMFR_AUDIO_MESSAGE_SILENT) != 0;
  const uint32_t bytes =
    message.frames * m_state.capture.format.blockAlign;
  if (bytes > KVMFR_AUDIO_PCM_BYTES ||
      (silent ? message.length != 0 : message.length != bytes))
    return false;

  AudioPacket packet = {};
  packet.endpointGeneration = m_state.generation;
  packet.streamGeneration = message.streamGeneration;
  if (message.flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY)
    packet.flags |= AUDIO_PACKET_DISCONTINUITY;
  if (message.flags & KVMFR_AUDIO_MESSAGE_SILENT)
    packet.flags |= AUDIO_PACKET_SILENT;
  if (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID)
    packet.flags |= AUDIO_PACKET_CLOCK_VALID;
  if (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE)
    packet.flags |= AUDIO_PACKET_CLOCK_STABLE;
  if (subscriber.captureDiscontinuity)
    packet.flags |= AUDIO_PACKET_DISCONTINUITY;
  packet.frames = message.frames;
  packet.position = message.position;
  packet.timeNs = message.timeNs;
  packet.rateQ32 = message.rateQ32;
  packet.format = m_state.capture.format;

  m_pendingCapture.valid = true;
  m_pendingCapture.subscriber = index;
  m_pendingCapture.subscriberSerial = subscriber.serial;
  m_pendingCapture.buffer = buffer;
  m_pendingCapture.packet = packet;
  m_pendingCapture.data = silent ? nullptr :
    static_cast<const uint8_t *>(buffer.data) +
      sizeof(KVMFRAudioMessage);
  m_pendingCapture.size = message.length;
  m_pendingCapture.readySerial = m_captureReadySerial;
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
  return true;
}

void CLGMPAudio::DrainControl(uint32_t index)
{
  for (unsigned count = 0; count < INBOUND_DRAIN_MAX; ++count)
  {
    CSRWSharedLock callbackLock(m_callbackLock);
    IAudioTarget * target = nullptr;
    AudioClockFeedback feedback = {};
    bool callClock = false;
    uint64_t subscriberSerial = 0;
    uint32_t feedbackRoleEpoch = 0;
    {
      CSRWExclusiveLock lock(m_lock);
      if (index >= LGMP_MAX_CLIENTS)
        return;
      Subscriber& subscriber = m_subscribers[index];
      if (subscriber.state == SubscriberState::NONE)
        return;

      LGMPStreamBuffer buffer = {};
      const LGMP_STATUS peek = lgmpHostStreamReadPeek(
        subscriber.clientControl, &buffer);
      if (peek == LGMP_ERR_STREAM_EMPTY)
        return;
      if (peek == LGMP_ERR_STREAM_UNBOUND ||
          peek == LGMP_ERR_STREAM_STALE)
      {
        if (subscriber.state == SubscriberState::BOUND)
          BeginRelease(index, "client audio control binding lost");
        return;
      }
      if (peek != LGMP_OK)
      {
        Fail("lgmpHostStreamReadPeek(audio control)", peek);
        return;
      }

      const auto release = [&]() -> bool
      {
        const LGMP_STATUS status = lgmpHostStreamReadRelease(
          subscriber.clientControl, &buffer);
        if (status != LGMP_OK)
        {
          Fail("lgmpHostStreamReadRelease(audio control)", status);
          return false;
        }
        m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
        return true;
      };

      if (subscriber.state == SubscriberState::RELEASING)
      {
        if (!release())
          return;
        continue;
      }

      KVMFRAudioMessage message = {};
      if (!ParseInbound(buffer, message))
      {
        (void)release();
        BeginRelease(index, "malformed client audio control record");
        return;
      }

      const bool sequenceValid = subscriber.controlSequenceKnown ?
        message.sequence == subscriber.nextControlSequence :
        message.sequence == 0;
      if (!sequenceValid)
      {
        (void)release();
        BeginRelease(index, "client audio control sequence gap");
        return;
      }
      subscriber.nextControlSequence = message.sequence + 1;
      subscriber.controlSequenceKnown = true;

      const uint8_t * payload =
        static_cast<const uint8_t *>(buffer.data) + sizeof(message);
      switch (message.type)
      {
        case KVMFR_AUDIO_MESSAGE_PLAYBACK_SUBSCRIBE:
        {
          KVMFRAudioSubscription subscription = {};
          if (message.length == sizeof(subscription))
            memcpy(&subscription, payload, sizeof(subscription));
          const bool valid = !message.streamGeneration &&
            !message.flags && !message.position && !message.timeNs &&
            !message.rateQ32 && !message.frames &&
            message.length == sizeof(subscription) &&
            subscription.bindingEpoch == subscriber.bindingEpoch &&
            subscription.roleEpoch &&
            !(subscription.subscriptions &
              ~AUDIO_SUBSCRIPTION_MASK) && !subscription.reserved &&
            (!(subscription.subscriptions &
                KVMFR_AUDIO_SUBSCRIBE_CAPTURE) ||
              (subscriber.clientCapabilities &
                KVMFR_AUDIO_CAP_CAPTURE));
          const bool initial = !subscriber.subscriptionReceived;
          if (!valid || (initial &&
              subscription.subscriptions !=
                subscriber.subscriptions))
          {
            (void)release();
            BeginRelease(index, "invalid audio subscription");
            return;
          }

          if (!release())
            return;
          RenewLease(subscriber);
          if (subscription.roleEpoch != subscriber.roleEpoch)
          {
            if (static_cast<int32_t>(subscription.roleEpoch -
                subscriber.roleEpoch) > 0)
            {
              BeginRelease(index,
                "invalid future audio subscription");
              return;
            }
            if (initial)
            {
              subscriber.subscriptionReceived = true;
              MarkStateDirty(index);
              if (m_captureOwner == INVALID_SLOT)
                AssignCaptureOwner();
            }
            continue;
          }

          const KVMFRAudioSubscriptionFlags previous =
            subscriber.subscriptions;
          subscriber.subscriptions = subscription.subscriptions;
          subscriber.subscriptionReceived = true;

          if (m_feedbackAuthority == index &&
              !FeedbackEligible(subscriber))
            RevokeFeedbackAuthority("playback subscription removed");
          if (m_captureOwner == index &&
              !CaptureEligible(subscriber))
          {
            m_captureOwner = INVALID_SLOT;
            subscriber.captureRevoking = true;
            subscriber.captureRevokeDeadline =
              GetTickCount64() + CAPTURE_REVOKE_ACK_MS;
            RefreshGranted(index, true);
          }
          if (previous != subscriber.subscriptions ||
              !subscriber.pumpRevision)
            MarkStateDirty(index);
          if (subscriber.captureRevoking)
            subscriber.captureRevokeRevision =
              subscriber.stateRevision;
          if (m_captureOwner == INVALID_SLOT &&
              m_captureBinding == INVALID_SLOT)
            AssignCaptureOwner();
          continue;
        }

        case KVMFR_AUDIO_MESSAGE_STATE_ACK:
        {
          KVMFRAudioStateBarrier barrier = {};
          if (message.length == sizeof(barrier))
            memcpy(&barrier, payload, sizeof(barrier));
          const bool valid = !message.flags && !message.position &&
            !message.timeNs && !message.rateQ32 && !message.frames &&
            message.length == sizeof(barrier) &&
            barrier.bindingEpoch == subscriber.bindingEpoch &&
            barrier.stateSerial;
          if (!valid)
          {
            (void)release();
            BeginRelease(index, "invalid audio state acknowledgement");
            return;
          }
          if (!release())
            return;
          RenewLease(subscriber);
          if (barrier.roleEpoch != subscriber.roleEpoch)
          {
            if (static_cast<int32_t>(barrier.roleEpoch -
                subscriber.roleEpoch) < 0)
              continue;
            BeginRelease(index,
              "invalid future audio state acknowledgement");
            return;
          }
          if (subscriber.awaitingAck &&
              message.streamGeneration ==
                subscriber.barrierGeneration &&
              !memcmp(&barrier, &subscriber.barrier,
                sizeof(barrier)))
          {
            subscriber.awaitingAck = false;
            subscriber.ready = true;
            DEBUG_INFO("Audio client %u generation %u is ready",
              subscriber.clientID,
              subscriber.requestGeneration);
          }
          continue;
        }

        case KVMFR_AUDIO_MESSAGE_CLOCK_FEEDBACK:
        {
          KVMFRAudioClockFeedback wire = {};
          if (message.length == sizeof(wire))
            memcpy(&wire, payload, sizeof(wire));
          const uint32_t validFlags =
            KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
            KVMFR_AUDIO_MESSAGE_CLOCK_STABLE |
            KVMFR_AUDIO_MESSAGE_DISCONTINUITY;
          const bool valid = !(message.flags & ~validFlags) &&
            (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) &&
            (!(message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) ||
              (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID)) &&
            message.streamGeneration && !message.frames &&
            message.length == sizeof(wire) && wire.targetRateQ32 &&
            wire.bindingEpoch == subscriber.bindingEpoch &&
            wire.roleEpoch;
          if (!valid)
          {
            (void)release();
            BeginRelease(index, "invalid audio clock feedback");
            return;
          }

          if (!release())
            return;
          RenewLease(subscriber);
          if (wire.roleEpoch != subscriber.roleEpoch)
          {
            if (static_cast<int32_t>(wire.roleEpoch -
                subscriber.roleEpoch) < 0)
              continue;
            BeginRelease(index,
              "invalid future audio clock feedback");
            return;
          }
          const bool current = m_feedbackAuthority == index &&
            subscriber.ready &&
            (subscriber.grantedCapabilities &
              KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
            m_stateKnown && m_state.available && m_state.active &&
            (m_state.capabilities &
              KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
            message.streamGeneration ==
              m_state.playback.generation;
          if (!current)
            continue;

          feedback.endpointGeneration = m_state.generation;
          feedback.playbackGeneration = message.streamGeneration;
          feedback.position = message.position;
          feedback.timeNs = message.timeNs;
          feedback.rateQ32 = message.rateQ32;
          feedback.targetRateQ32 = wire.targetRateQ32;
          feedback.flags = AUDIO_CLOCK_VALID;
          if (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE)
            feedback.flags |= AUDIO_CLOCK_STABLE;
          if (message.flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY)
            feedback.flags |= AUDIO_CLOCK_DISCONTINUITY;
          target = m_started ? m_target : nullptr;
          subscriberSerial = subscriber.serial;
          feedbackRoleEpoch = subscriber.roleEpoch;
          callClock = target != nullptr;
          break;
        }

        case KVMFR_AUDIO_MESSAGE_KEEPALIVE:
        case KVMFR_AUDIO_MESSAGE_RELEASE:
          if (!EmptyControl(message))
          {
            (void)release();
            BeginRelease(index, "invalid audio client control");
            return;
          }
          if (!release())
            return;
          if (message.type == KVMFR_AUDIO_MESSAGE_RELEASE)
          {
            BeginRelease(index, "client audio release");
            return;
          }
          RenewLease(subscriber);
          continue;

        default:
          (void)release();
          BeginRelease(index, "unsupported client audio control");
          return;
      }
    }

    if (!callClock)
      continue;
    const AudioResult result = target->SetClockFeedback(feedback);
    {
      CSRWExclusiveLock lock(m_lock);
      if (index >= LGMP_MAX_CLIENTS ||
          m_subscribers[index].state != SubscriberState::BOUND ||
          m_subscribers[index].serial != subscriberSerial)
        return;
      if (result == AudioResult::FAILED ||
          result == AudioResult::REJECTED)
      {
        Subscriber& subscriber = m_subscribers[index];
        if (m_feedbackAuthority != index || !subscriber.ready ||
            subscriber.roleEpoch != feedbackRoleEpoch ||
            !(subscriber.grantedCapabilities &
              KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) ||
            !m_stateKnown || m_state.playback.generation !=
              feedback.playbackGeneration)
          continue;
        RevokeFeedbackAuthority("audio clock target failed");
      }
    }
  }
}

void CLGMPAudio::DrainCapture()
{
  for (unsigned count = 0; count < INBOUND_DRAIN_MAX; ++count)
  {
    CSRWSharedLock callbackLock(m_callbackLock);
    IAudioTarget * target = nullptr;
    bool callCapture = false;
    uint32_t index = INVALID_SLOT;
    uint64_t subscriberSerial = 0;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_captureBinding == INVALID_SLOT)
        return;
      index = m_captureBinding;
      Subscriber& subscriber = m_subscribers[index];

      if (m_pendingCapture.valid)
      {
        if (!PendingCaptureCurrent(index))
        {
          ReleasePendingCapture();
          subscriber.captureDiscontinuity = true;
          continue;
        }
        if (m_pendingCapture.readySerial == m_captureReadySerial)
          return;
        m_pendingCapture.readySerial = m_captureReadySerial;
        target = m_started ? m_target : nullptr;
        subscriberSerial = subscriber.serial;
        callCapture = target != nullptr;
      }
      else
      {
        LGMPStreamBuffer buffer = {};
        const LGMP_STATUS peek = lgmpHostStreamReadPeek(
          m_captureStream, &buffer);
        if (peek == LGMP_ERR_STREAM_EMPTY)
          return;
        if (peek == LGMP_ERR_STREAM_UNBOUND ||
            peek == LGMP_ERR_STREAM_STALE)
        {
          if (subscriber.state == SubscriberState::BOUND)
            BeginRelease(index, "capture audio binding lost");
          return;
        }
        if (peek != LGMP_OK)
        {
          Fail("lgmpHostStreamReadPeek(audio capture)", peek);
          return;
        }

        const auto release = [&]() -> bool
        {
          const LGMP_STATUS status = lgmpHostStreamReadRelease(
            m_captureStream, &buffer);
          if (status != LGMP_OK)
          {
            Fail("lgmpHostStreamReadRelease(audio capture)", status);
            return false;
          }
          m_inboundActivitySerial =
            NextSerial(m_inboundActivitySerial);
          return true;
        };

        if (subscriber.state == SubscriberState::RELEASING ||
            subscriber.captureRevoking)
        {
          if (!release())
            return;
          continue;
        }

        KVMFRAudioMessage message = {};
        if (!ParseInbound(buffer, message))
        {
          (void)release();
          BeginRelease(index, "malformed capture audio record");
          return;
        }
        const bool sequenceGap =
          (subscriber.captureSequenceKnown &&
            message.sequence != subscriber.nextCaptureSequence) ||
          (!subscriber.captureSequenceKnown && message.sequence != 0);
        subscriber.nextCaptureSequence = message.sequence + 1;
        subscriber.captureSequenceKnown = true;
        if (sequenceGap)
          subscriber.captureDiscontinuity = true;

        const bool clockStable =
          (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) != 0;
        const bool clockValid =
          (message.flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) != 0;
        const bool structurallyValid =
          message.type == KVMFR_AUDIO_MESSAGE_CAPTURE_DATA &&
          !(message.flags & ~WIRE_PACKET_FLAG_MASK) &&
          (!clockStable || clockValid) &&
          (clockValid || (!message.position && !message.timeNs &&
            !message.rateQ32)) && message.streamGeneration &&
          message.frames &&
          message.frames <= KVMFR_AUDIO_MAX_FRAMES_PER_PACKET;
        if (!structurallyValid)
        {
          (void)release();
          BeginRelease(index, "invalid capture audio record");
          return;
        }
        RenewLease(subscriber);
        const bool current = m_captureOwner == index &&
          (subscriber.grantedCapabilities &
            KVMFR_AUDIO_CAP_CAPTURE) &&
          subscriber.announcedCapture == message.streamGeneration &&
          m_stateKnown && m_state.available && m_state.active &&
          (m_state.capabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
          m_state.capture.formatValid &&
          ValidFormat(m_state.capture.format) &&
          m_state.capture.state == AudioStreamState::RUNNING &&
          message.streamGeneration == m_state.capture.generation;
        if (!current)
        {
          if (!release())
            return;
          subscriber.captureDiscontinuity = true;
          continue;
        }

        const bool silent = (message.flags &
          KVMFR_AUDIO_MESSAGE_SILENT) != 0;
        const uint32_t bytes = message.frames *
          m_state.capture.format.blockAlign;
        if (bytes > KVMFR_AUDIO_PCM_BYTES ||
            (silent ? message.length != 0 :
              message.length != bytes))
        {
          (void)release();
          BeginRelease(index, "invalid capture audio payload");
          return;
        }
        if (!PrepareCapture(message, buffer, index))
        {
          if (!release())
            return;
          subscriber.captureDiscontinuity = true;
          continue;
        }
        target = m_started ? m_target : nullptr;
        subscriberSerial = subscriber.serial;
        callCapture = target != nullptr;
      }
      m_captureCallbackInFlight = callCapture;
    }

    AudioResult result = AudioResult::UNAVAILABLE;
    if (callCapture)
      result = target->SendCapture(m_pendingCapture.packet,
        m_pendingCapture.data, m_pendingCapture.size);

    {
      CSRWExclusiveLock lock(m_lock);
      m_captureCallbackInFlight = false;
      if (index >= LGMP_MAX_CLIENTS ||
          m_subscribers[index].serial != subscriberSerial ||
          !m_pendingCapture.valid)
      {
        if (m_pendingCapture.valid)
          ReleasePendingCapture();
        return;
      }
      Subscriber& subscriber = m_subscribers[index];
      if (!PendingCaptureCurrent(index))
      {
        ReleasePendingCapture();
        subscriber.captureDiscontinuity = true;
        continue;
      }
      if (result == AudioResult::BUSY)
        return;

      const LGMP_STATUS release = lgmpHostStreamReadRelease(
        m_captureStream, &m_pendingCapture.buffer);
      if (release != LGMP_OK)
      {
        m_pendingCapture = {};
        Fail("lgmpHostStreamReadRelease(audio capture)", release);
        return;
      }
      m_pendingCapture = {};
      m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
      subscriber.captureDiscontinuity =
        result != AudioResult::ACCEPTED;
      if (result == AudioResult::FAILED ||
          result == AudioResult::REJECTED)
      {
        BeginRelease(index, "audio capture target failed");
        return;
      }
    }
  }
}

void CLGMPAudio::DrainInbound()
{
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    DrainControl(index);
  DrainCapture();
}

void CLGMPAudio::ProcessActivation()
{
  CSRWSharedLock callbackLock(m_callbackLock);
  IAudioTarget * target = nullptr;
  bool activate = false;
  {
    CSRWSharedLock lock(m_lock);
    if (!m_started || !m_target || m_failed || !m_stateKnown)
      return;
    activate = HasBoundSubscribers();
    if (activate == m_activationCommanded)
      return;
    target = m_target;
  }

  const AudioResult result = target->SetAudioActive(activate);
  CSRWExclusiveLock lock(m_lock);
  if (!m_started || m_target != target)
    return;
  if (result == AudioResult::ACCEPTED ||
      (!activate && (result == AudioResult::UNAVAILABLE ||
        result == AudioResult::STALE)))
    m_activationCommanded = activate;
  else if (result == AudioResult::FAILED ||
      result == AudioResult::REJECTED)
    Fail("audio endpoint activation", LGMP_ERR_CORRUPTED);
}

void CLGMPAudio::NotifyPlaybackReady()
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

void CLGMPAudio::NotifyFailure()
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

void CLGMPAudio::Fail(
  const char * operation, LGMP_STATUS status)
{
  if (!m_failed)
    DEBUG_ERROR("%s Failed (Audio): %s",
      operation, lgmpStatusString(status));
  ClearBlockedPlayback(true);
  m_failed = true;
}

void CLGMPAudio::Process()
{
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_claimResponderReady)
      return;
    PumpResponses();
    if (!m_initialized)
      return;
    MaintainSubscribers(GetTickCount64());
    PumpResponses();
    PumpState();
    CheckPlaybackBackpressure(GetTickCount64());
  }
  ProcessActivation();
  NotifyPlaybackReady();
  NotifyFailure();
}

bool CLGMPAudio::CanRespondToClaims()
{
  CSRWSharedLock lock(m_lock);
  return m_claimResponderReady;
}

bool CLGMPAudio::Start(IAudioTarget& target)
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
  m_activationCommanded = false;
  return true;
}

void CLGMPAudio::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock callbackLock(m_callbackLock);
  CSRWExclusiveLock lock(m_lock);
  if (!m_started && !m_target)
    return;
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    BeginRelease(index, "audio source stopped");
  ClearBlockedPlayback(false);
  m_started = false;
  m_target = nullptr;
  m_playbackReadyPending = false;
  m_activationCommanded = false;
}

void CLGMPAudio::TransportStop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  StopInboundWorker();
  CSRWExclusiveLock callbackLock(m_callbackLock);
  CSRWExclusiveLock lock(m_lock);
  m_started = false;
  m_target = nullptr;
  m_playbackReadyPending = false;
  ClearBlockedPlayback(false);
  ReleasePendingCapture();
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
  {
    if (m_subscribers[index].state == SubscriberState::NONE)
      continue;
    ForceUnbindSubscriber(index);
    ResetSubscriber(index);
  }
  m_feedbackAuthority = INVALID_SLOT;
  m_captureOwner = INVALID_SLOT;
  m_captureBinding = INVALID_SLOT;
  m_activationCommanded = false;
  for (PendingResponse& response : m_pendingResponse)
    response = {};
  for (ClaimRecord& record : m_claimRecord)
    record = {};
}

void CLGMPAudio::AudioState(
  const AudioEndpointStatus& state)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_started)
    return;

  const uint64_t previousPlayback = m_previousPlaybackGeneration;
  if (state.playback.state == AudioStreamState::RUNNING)
    m_playbackRunGeneration = state.playback.generation;

  if (m_playbackBlocked)
  {
    const bool playbackCurrent = state.available && state.active &&
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

  m_state = state;
  m_stateKnown = true;
  m_previousPlaybackGeneration = state.playback.generation;
  for (uint32_t index = 0; index < LGMP_MAX_CLIENTS; ++index)
    RefreshGranted(index, true);
  MarkAllStateDirty();
  if (state.playback.generation &&
      state.playback.generation != previousPlayback)
  {
    for (Subscriber& subscriber : m_subscribers)
      if (subscriber.state == SubscriberState::BOUND)
        subscriber.feedbackRevoked = false;
    PromoteFeedbackAuthority(state.playback.generation);
  }
  if (m_captureOwner == INVALID_SLOT)
    AssignCaptureOwner();
  if (!m_inboundStopping && m_inboundWakeEvent)
    SetEvent(m_inboundWakeEvent);
}

AudioResult CLGMPAudio::SendPlayback(
  const AudioPacket& packet, const uint8_t * data, size_t size)
{
  if ((packet.flags & ~AUDIO_PACKET_FLAG_MASK) ||
      ((packet.flags & AUDIO_PACKET_CLOCK_STABLE) &&
        !(packet.flags & AUDIO_PACKET_CLOCK_VALID)) ||
      (!(packet.flags & AUDIO_PACKET_CLOCK_VALID) &&
        (packet.position || packet.timeNs || packet.rateQ32)) ||
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
    DEBUG_WARN("Audio playback retry did not match retained packet");
    ClearBlockedPlayback(false);
    return AudioResult::REJECTED;
  }
  const bool retainedRetry = m_playbackBlocked;
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

  bool hasPlaybackSubscriber = false;
  for (const Subscriber& subscriber : m_subscribers)
    if (subscriber.responseAdmitted &&
        subscriber.subscriptionReceived &&
        PlaybackEligible(subscriber))
    {
      hasPlaybackSubscriber = true;
      break;
    }
  if (!hasPlaybackSubscriber)
  {
    ClearBlockedPlayback(false);
    return AudioResult::ACCEPTED;
  }

  if (!m_stateKnown)
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

  PumpState();
  if (m_failed)
    return AudioResult::FAILED;
  if (!HasReadyPlayback())
  {
    if (!RememberBlockedPlayback(packet, size))
      return AudioResult::REJECTED;
    if (!m_inboundStopping && m_inboundWakeEvent)
      SetEvent(m_inboundWakeEvent);
    return AudioResult::BUSY;
  }

  if (m_feedbackAuthority != INVALID_SLOT)
  {
    Subscriber& authority = m_subscribers[m_feedbackAuthority];
    if (authority.ready && PlaybackEligible(authority) &&
        (authority.grantedCapabilities &
          KVMFR_AUDIO_CAP_CLOCK_FEEDBACK))
    {
      LGMPSPMCReaderState reader = {};
      const LGMP_STATUS query = lgmpHostSPMCReaderGetState(
        m_playbackStream, authority.playbackReaderID, &reader);
      if (query == LGMP_ERR_STREAM_BUSY)
      {
        if (!RememberBlockedPlayback(packet, size))
          return AudioResult::REJECTED;
        if (!m_inboundStopping && m_inboundWakeEvent)
          SetEvent(m_inboundWakeEvent);
        return AudioResult::BUSY;
      }
      bool stalled = false;
      if (query == LGMP_OK)
      {
        if (reader.state != LGMP_SPMC_READER_READY ||
            reader.clientID != authority.clientID ||
            reader.epoch != authority.playbackReaderEpoch)
          RevokeFeedbackAuthority("playback reader binding lost");
        else
          stalled = reader.producerSequence -
            reader.consumerSequence >= KVMFR_AUDIO_STREAM_SLOT_COUNT;
      }
      else
      {
        Fail("lgmpHostSPMCReaderGetState(audio)", query);
        return AudioResult::FAILED;
      }

      if (stalled && m_feedbackAuthority != INVALID_SLOT)
      {
        const uint64_t now = GetTickCount64();
        if (!m_authorityBlockedSince)
          m_authorityBlockedSince = now;
        if (now - m_authorityBlockedSince >= AUTHORITY_STALL_MS)
          RevokeFeedbackAuthority("playback reader stalled");
        else
        {
          if (!RememberBlockedPlayback(packet, size))
            return AudioResult::REJECTED;
          if (!m_inboundStopping && m_inboundWakeEvent)
            SetEvent(m_inboundWakeEvent);
          return AudioResult::BUSY;
        }
      }
      else
        m_authorityBlockedSince = 0;
    }
  }

  LGMPSPMCBuffer buffer = {};
  LGMP_STATUS status = lgmpHostSPMCWriteAcquire(
    m_playbackStream, &buffer);
  if (status == LGMP_ERR_STREAM_BUSY)
  {
    if (!RememberBlockedPlayback(packet, size))
      return AudioResult::REJECTED;
    return AudioResult::BUSY;
  }
  if (status != LGMP_OK)
  {
    Fail("lgmpHostSPMCWriteAcquire(audio)", status);
    return AudioResult::FAILED;
  }

  const size_t required = sizeof(KVMFRAudioMessage) + size;
  if (!buffer.data || buffer.capacity < required)
  {
    const LGMP_STATUS cancel =
      lgmpHostSPMCWriteCancel(m_playbackStream, &buffer);
    if (cancel != LGMP_OK)
      DEBUG_WARN("Failed to cancel audio playback reservation: %s",
        lgmpStatusString(cancel));
    Fail("undersized playback audio slot", LGMP_ERR_INVALID_SIZE);
    return AudioResult::FAILED;
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

  KVMFRAudioMessage message = {};
  message.version = KVMFR_AUDIO_MESSAGE_VERSION;
  message.size = sizeof(message);
  message.type = KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA;
  message.streamGeneration = packet.streamGeneration;
  message.flags = flags;
  message.sequence = buffer.sequence;
  message.position = packet.position;
  message.timeNs = packet.timeNs;
  message.rateQ32 = packet.rateQ32;
  message.frames = packet.frames;
  message.length = static_cast<uint32_t>(size);
  WCWrite::Copy(buffer.data, &message, sizeof(message));
  if (size)
    WCWrite::Copy(static_cast<uint8_t *>(buffer.data) +
      sizeof(message), data, size);
  WCWrite::Flush();

  status = lgmpHostSPMCWriteCommit(m_playbackStream, &buffer,
    static_cast<uint32_t>(required));
  if (status != LGMP_OK)
  {
    Fail("lgmpHostSPMCWriteCommit(audio)", status);
    return AudioResult::FAILED;
  }

  ClearBlockedPlayback(false);
  if (retainedRetry)
    m_playbackReadyPending = false;
  m_inboundActivitySerial = NextSerial(m_inboundActivitySerial);
  return AudioResult::ACCEPTED;
}

void CLGMPAudio::CaptureReceiveReady()
{
  CSRWExclusiveLock lock(m_lock);
  if (m_started)
  {
    m_captureReadySerial = NextSerial(m_captureReadySerial);
    if (!m_inboundStopping && m_inboundWakeEvent)
      SetEvent(m_inboundWakeEvent);
  }
}
