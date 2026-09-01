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

#include "audio/CLGAudioDevice.h"

#include "CDebug.h"
#include "CSRWLock.h"
#include "LGAudioBroker.h"

#include <Windows.h>

#include <algorithm>
#include <memory>
#include <new>
#include <string.h>

namespace
{
  static_assert(AUDIO_CHANNEL_POSITION_MASK ==
      (1U << AUDIO_MAX_CHANNELS) - 1,
    "Audio channel-position geometry changed");
  static_assert(LGAUDIO_BROKER_MAX_PAYLOAD_BYTES >= AUDIO_MAX_PCM_BYTES,
    "LGAudio broker cannot carry the largest transport audio packet");

  static constexpr unsigned RENDER_SLOT_COUNT = 4;
  static constexpr unsigned CAPTURE_SLOT_COUNT = 4;
  static constexpr DWORD OPEN_RETRY_MS = 500;
  static constexpr DWORD ACTIVE_POLL_MS = 10;
  static constexpr DWORD IDLE_POLL_MS = 100;
  static constexpr size_t BROKER_PACKET_BYTES =
    sizeof(LGAudioBrokerPacketHeader) + AUDIO_MAX_PCM_BYTES;

  uint64_t NextGeneration(uint64_t generation)
  {
    ++generation;
    return generation ? generation : 1;
  }

  unsigned PopCount(uint32_t value)
  {
    unsigned count = 0;
    while (value)
    {
      value &= value - 1;
      ++count;
    }
    return count;
  }

  bool SameFormat(const AudioFormat& left, const AudioFormat& right)
  {
    return left.sampleRate == right.sampleRate &&
      left.channelMask == right.channelMask &&
      left.channels == right.channels &&
      left.containerBits == right.containerBits &&
      left.validBits == right.validBits &&
      left.blockAlign == right.blockAlign;
  }

  bool ValidFormat(const AudioFormat& format)
  {
    return format.sampleRate && format.sampleRate <= 192000 &&
      format.channels && format.channels <= AUDIO_STREAM_MAX_CHANNELS &&
      format.channelMask &&
      !(format.channelMask & ~AUDIO_CHANNEL_POSITION_MASK) &&
      PopCount(format.channelMask) == format.channels &&
      (format.containerBits == 16 || format.containerBits == 24) &&
      format.validBits == format.containerBits &&
      format.blockAlign ==
        format.channels * (format.containerBits / 8);
  }

  AudioFormat BrokerFormat(const LGAudioBrokerFormat& source)
  {
    AudioFormat result;
    result.sampleRate    = source.sampleRate;
    result.channelMask   = source.channelMask;
    result.channels      = source.channels;
    result.containerBits = source.containerBits;
    result.validBits     = source.validBits;
    result.blockAlign    = source.blockAlign;
    return result;
  }

  LGAudioBrokerFormat DeviceFormat(const AudioFormat& source)
  {
    LGAudioBrokerFormat result = {};
    result.sampleRate    = source.sampleRate;
    result.channelMask   = source.channelMask;
    result.channels      = source.channels;
    result.containerBits = source.containerBits;
    result.validBits     = source.validBits;
    result.blockAlign    = source.blockAlign;
    return result;
  }

  AudioStreamState BrokerStreamState(uint32_t state)
  {
    switch (state)
    {
      case LGAUDIO_BROKER_STREAM_STOPPED:
        return AudioStreamState::STOPPED;
      case LGAUDIO_BROKER_STREAM_ACQUIRED:
        return AudioStreamState::ACQUIRED;
      case LGAUDIO_BROKER_STREAM_PAUSED:
        return AudioStreamState::PAUSED;
      case LGAUDIO_BROKER_STREAM_RUNNING:
        return AudioStreamState::RUNNING;
      default:
        return AudioStreamState::UNAVAILABLE;
    }
  }

  AudioStreamStatus BrokerStream(
    const LGAudioBrokerStreamState& source)
  {
    AudioStreamStatus result;
    result.generation  = source.generation;
    result.state       = BrokerStreamState(source.state);
    result.formatValid =
      (source.flags & LGAUDIO_BROKER_STREAM_FORMAT_VALID) != 0;
    if (result.formatValid)
    {
      result.format = BrokerFormat(source.format);
      if (!ValidFormat(result.format))
      {
        result.format      = {};
        result.formatValid = false;
      }
    }
    return result;
  }

  bool SameStream(const AudioStreamStatus& left,
    const AudioStreamStatus& right)
  {
    return left.generation == right.generation &&
      left.state == right.state &&
      left.formatValid == right.formatValid &&
      (!left.formatValid || SameFormat(left.format, right.format));
  }

  bool SameStatus(const AudioEndpointStatus& left,
    const AudioEndpointStatus& right)
  {
    return left.generation == right.generation &&
      left.capabilities == right.capabilities &&
      left.available == right.available &&
      left.active == right.active &&
      SameStream(left.playback, right.playback) &&
      SameStream(left.capture, right.capture);
  }

  bool RecoverableIoError(DWORD error)
  {
    return error == ERROR_OPERATION_ABORTED ||
      error == ERROR_NOT_READY ||
      error == ERROR_RETRY ||
      error == ERROR_DEVICE_NOT_CONNECTED;
  }
}

class CLGAudioDevice::CState
{
private:
  enum class ActivationState
  {
    IDLE,
    ACTIVATING,
    ACTIVE,
    DEACTIVATING,
  };

  enum class RenderState
  {
    IDLE,
    PENDING,
    READY,
  };

  struct RenderSlot
  {
    OVERLAPPED overlapped = {};
    RenderState state     = RenderState::IDLE;
    DWORD transferred     = 0;
    uint8_t data[BROKER_PACKET_BYTES];
  };

  struct CaptureSlot
  {
    AudioPacket packet;
    size_t      size = 0;
    uint8_t     data[AUDIO_MAX_PCM_BYTES];
  };

  mutable CSRWLock m_lock;
  ILGAudioDeviceEvents * m_events = nullptr;
  HANDLE m_stopEvent    = nullptr;
  HANDLE m_wakeEvent    = nullptr;
  HANDLE m_controlEvent = nullptr;
  HANDLE m_writeEvent   = nullptr;
  HANDLE m_thread       = nullptr;
  HANDLE m_handle       = INVALID_HANDLE_VALUE;
  bool   m_started      = false;
  bool   m_stopping     = false;
  bool   m_bound        = false;
  ActivationState m_activation = ActivationState::IDLE;
  bool   m_controlDirty  = true;

  AudioEndpointStatus m_status;
  uint64_t m_endpointGeneration = 1;
  CaptureSlot m_capture[CAPTURE_SLOT_COUNT];
  unsigned m_captureHead  = 0;
  unsigned m_captureCount = 0;

  LGAudioBrokerCaps  m_caps        = {};
  LGAudioBrokerState m_brokerState = {};
  uint64_t m_lastAdapterGeneration = 0;
  uint64_t m_lastBindingGeneration = 0;
  uint64_t m_captureStreamGeneration = 0;
  uint64_t m_nextCaptureSequence     = 0;
  uint64_t m_nextRenderSequence      = 0;
  bool     m_captureDiscontinuity    = false;
  bool     m_playbackBlocked         = false;
  uint64_t m_playbackReadySerial     = 0;
  bool     m_renderSequenceValid     = false;
  bool     m_renderDiscontinuity     = false;
  DWORD    m_lastOpenError           = ERROR_SUCCESS;
  DWORD    m_lastError               = ERROR_SUCCESS;
  uint64_t m_renderPackets           = 0;
  uint64_t m_renderBytes             = 0;
  uint64_t m_capturePackets          = 0;
  uint64_t m_captureBytes            = 0;
  uint64_t m_reconnects              = 0;
  bool     m_everOpened              = false;
  uint64_t m_nextOpenAt              = 0;
  uint64_t m_nextPollAt              = 0;

  RenderSlot m_render[RENDER_SLOT_COUNT];
  OVERLAPPED m_writeOverlapped = {};
  bool       m_writePending    = false;
  DWORD      m_writeBytes      = 0;
  uint8_t    m_writeData[BROKER_PACKET_BYTES];

  bool Stopping() const
  {
    return WaitForSingleObject(m_stopEvent, 0) == 0;
  }

  void ClearPlaybackBlock()
  {
    CSRWExclusiveLock lock(m_lock);
    m_playbackBlocked = false;
  }

  bool PlaybackBlocked() const
  {
    CSRWSharedLock lock(m_lock);
    return m_playbackBlocked;
  }

  static DWORD CALLBACK ThreadProc(void * opaque)
  {
    static_cast<CState *>(opaque)->Thread();
    return 0;
  }

  bool Ioctl(DWORD code, const void * input, DWORD inputSize,
    void * output, DWORD outputSize, DWORD& returned, DWORD& error)
  {
    returned = 0;
    error    = ERROR_SUCCESS;
    OVERLAPPED overlapped = {};
    overlapped.hEvent = m_controlEvent;
    ResetEvent(m_controlEvent);

    const BOOL immediate = DeviceIoControl(m_handle, code,
      const_cast<void *>(input), inputSize, output, outputSize,
      &returned, &overlapped);
    if (immediate)
      return true;

    error = GetLastError();
    if (error != ERROR_IO_PENDING)
      return false;

    const HANDLE waits[] = { m_stopEvent, m_controlEvent };
    const DWORD wait = WaitForMultipleObjects(
      ARRAYSIZE(waits), waits, FALSE, INFINITE);
    if (wait == 0)
    {
      CancelIoEx(m_handle, &overlapped);
      GetOverlappedResult(m_handle, &overlapped, &returned, TRUE);
      error = ERROR_OPERATION_ABORTED;
      return false;
    }
    if (wait != 1)
    {
      CancelIoEx(m_handle, &overlapped);
      GetOverlappedResult(m_handle, &overlapped, &returned, TRUE);
      error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
      return false;
    }

    if (!GetOverlappedResult(m_handle, &overlapped, &returned, FALSE))
    {
      error = GetLastError();
      return false;
    }
    return true;
  }

  void SetHandle(HANDLE handle)
  {
    CSRWExclusiveLock lock(m_lock);
    m_handle = handle;
  }

  void Publish(AudioEndpointStatus next, bool advance)
  {
    ILGAudioDeviceEvents * events = nullptr;
    {
      CSRWExclusiveLock lock(m_lock);
      if (advance)
        m_endpointGeneration = NextGeneration(m_endpointGeneration);
      next.generation = m_endpointGeneration;
      if (SameStatus(next, m_status))
        return;
      m_status = next;
      events   = m_events;
    }
    if (events)
      events->LGAudioState(next);
  }

  void PublishUnavailable(bool advance)
  {
    AudioEndpointStatus next;
    next.capabilities = 0;
    Publish(next, advance);
  }

  uint32_t Capabilities() const
  {
    uint32_t result = 0;
    if (m_caps.capabilities & LGAUDIO_BROKER_CAP_RENDER_READ)
      result |= AUDIO_CAP_PLAYBACK;
    if (m_caps.capabilities & LGAUDIO_BROKER_CAP_CAPTURE_WRITE)
      result |= AUDIO_CAP_CAPTURE;
    if (m_caps.capabilities & LGAUDIO_BROKER_CAP_CLOCK_CONTROL)
      result |= AUDIO_CAP_CLOCK_FEEDBACK;
    return result;
  }

  void PublishBrokerState(bool forceAdvance = false)
  {
    AudioEndpointStatus previous = Status();
    AudioEndpointStatus next;
    next.capabilities = Capabilities();
    next.available =
      (m_brokerState.flags & LGAUDIO_BROKER_STATE_ADAPTER_PRESENT) != 0;
    next.active =
      (m_brokerState.flags & LGAUDIO_BROKER_STATE_BOUND) != 0 &&
      (m_brokerState.flags &
        LGAUDIO_BROKER_STATE_DATA_PLANE_ACTIVE) != 0;
    {
      CSRWExclusiveLock lock(m_lock);
      m_bound =
        (m_brokerState.flags & LGAUDIO_BROKER_STATE_BOUND) != 0;
    }
    if (next.available)
    {
      next.playback = BrokerStream(m_brokerState.render);
      next.capture  = BrokerStream(m_brokerState.capture);
    }

    const bool bindingChanged =
      m_lastBindingGeneration != m_brokerState.bindingGeneration;
    const bool advance = forceAdvance ||
      previous.available != next.available ||
      previous.active != next.active ||
      m_lastAdapterGeneration != m_brokerState.adapterGeneration ||
      bindingChanged;
    m_lastAdapterGeneration = m_brokerState.adapterGeneration;
    m_lastBindingGeneration = m_brokerState.bindingGeneration;

    if (m_captureStreamGeneration != next.capture.generation ||
        previous.active != next.active || bindingChanged)
    {
      m_captureStreamGeneration = next.capture.generation;
      m_nextCaptureSequence = 0;
      m_captureDiscontinuity = false;
    }
    if (previous.playback.generation != next.playback.generation ||
        previous.active != next.active || bindingChanged)
    {
      ClearPlaybackBlock();
      m_renderSequenceValid = false;
      m_renderDiscontinuity = true;
    }
    Publish(next, advance);
  }

  bool QueryCaps(DWORD& error)
  {
    LGAudioBrokerCaps caps = {};
    DWORD returned = 0;
    if (!Ioctl(IOCTL_LGAUDIO_BROKER_QUERY_CAPS,
        nullptr, 0, &caps, sizeof(caps), returned, error))
      return false;
    if (returned != sizeof(caps) || caps.size != sizeof(caps) ||
        caps.version != LGAUDIO_BROKER_ABI_VERSION ||
        caps.minimumVersion > LGAUDIO_BROKER_ABI_VERSION ||
        caps.maximumVersion < LGAUDIO_BROKER_ABI_VERSION ||
        !(caps.capabilities & LGAUDIO_BROKER_CAP_CONTROL) ||
        !(caps.capabilities & LGAUDIO_BROKER_CAP_RENDER_READ) ||
        !(caps.capabilities & LGAUDIO_BROKER_CAP_CAPTURE_WRITE) ||
        !caps.maximumRenderTransfer ||
        !caps.maximumCaptureTransfer)
    {
      error = ERROR_REVISION_MISMATCH;
      return false;
    }
    m_caps = caps;
    return true;
  }

  bool QueryState(DWORD& error)
  {
    LGAudioBrokerState state = {};
    DWORD returned = 0;
    if (!Ioctl(IOCTL_LGAUDIO_BROKER_QUERY_STATE,
        nullptr, 0, &state, sizeof(state), returned, error))
      return false;
    if (returned != sizeof(state) || state.size != sizeof(state) ||
        state.version != LGAUDIO_BROKER_ABI_VERSION)
    {
      error = ERROR_INVALID_DATA;
      return false;
    }
    m_brokerState = state;
    return true;
  }

  bool Bind(DWORD& error)
  {
    LGAudioBrokerBind bind = {};
    bind.size              = sizeof(bind);
    bind.version           = LGAUDIO_BROKER_ABI_VERSION;
    bind.adapterGeneration = m_brokerState.adapterGeneration;
    DWORD returned = 0;
    if (!Ioctl(IOCTL_LGAUDIO_BROKER_BIND,
        &bind, sizeof(bind), &bind, sizeof(bind), returned, error))
      return false;
    if (returned != sizeof(bind) || !bind.bindingGeneration)
    {
      error = ERROR_INVALID_DATA;
      return false;
    }
    return true;
  }

  bool Unbind(DWORD& error)
  {
    LGAudioBrokerHeader header = {};
    header.size    = sizeof(header);
    header.version = LGAUDIO_BROKER_ABI_VERSION;
    DWORD returned = 0;
    return Ioctl(IOCTL_LGAUDIO_BROKER_UNBIND,
      &header, sizeof(header), nullptr, 0, returned, error);
  }

  void CancelDataIo()
  {
    if (m_handle == INVALID_HANDLE_VALUE)
      return;

    for (RenderSlot& slot : m_render)
      if (slot.state == RenderState::PENDING)
        CancelIoEx(m_handle, &slot.overlapped);
    if (m_writePending)
      CancelIoEx(m_handle, &m_writeOverlapped);

    for (RenderSlot& slot : m_render)
    {
      if (slot.state == RenderState::PENDING)
      {
        DWORD transferred = 0;
        GetOverlappedResult(
          m_handle, &slot.overlapped, &transferred, TRUE);
      }
      slot.state       = RenderState::IDLE;
      slot.transferred = 0;
      ResetEvent(slot.overlapped.hEvent);
    }
    if (m_writePending)
    {
      DWORD transferred = 0;
      GetOverlappedResult(
        m_handle, &m_writeOverlapped, &transferred, TRUE);
    }
    m_writePending = false;
    m_writeBytes   = 0;
    ResetEvent(m_writeOverlapped.hEvent);
    m_captureDiscontinuity = false;
    ClearPlaybackBlock();
    m_renderSequenceValid = false;
    m_renderDiscontinuity = true;
  }

  void ClearCaptureQueue()
  {
    CSRWExclusiveLock lock(m_lock);
    m_captureHead  = 0;
    m_captureCount = 0;
  }

  void CloseBroker(bool publish = true)
  {
    if (m_handle == INVALID_HANDLE_VALUE)
      return;

    CancelIoEx(m_handle, nullptr);
    CancelDataIo();
    const HANDLE handle = m_handle;
    SetHandle(INVALID_HANDLE_VALUE);
    CloseHandle(handle);
    bool inactiveBarrier = false;
    {
      CSRWExclusiveLock lock(m_lock);
      m_bound = false;
      if (m_activation == ActivationState::DEACTIVATING)
      {
        m_activation = ActivationState::IDLE;
        inactiveBarrier = true;
      }
    }
    m_caps = {};
    m_brokerState = {};
    m_lastAdapterGeneration = 0;
    m_lastBindingGeneration = 0;
    m_captureStreamGeneration = 0;
    m_nextCaptureSequence = 0;
    m_captureDiscontinuity = false;
    ClearCaptureQueue();
    if (!Stopping())
      m_nextOpenAt = GetTickCount64() + OPEN_RETRY_MS;
    if (publish || inactiveBarrier)
      PublishUnavailable(true);
  }

  bool FinishAbsentDeactivation()
  {
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_handle != INVALID_HANDLE_VALUE ||
          m_activation != ActivationState::DEACTIVATING)
        return false;
      m_activation = ActivationState::IDLE;
      m_captureHead  = 0;
      m_captureCount = 0;
    }
    PublishUnavailable(true);
    return true;
  }

  bool OpenBroker()
  {
    const uint64_t now = GetTickCount64();
    if (now < m_nextOpenAt)
      return false;

    HANDLE handle = CreateFileW(LGAUDIO_BROKER_USER_DEVICE_PATH,
      GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
      const DWORD error = GetLastError();
      {
        CSRWExclusiveLock lock(m_lock);
        m_lastError = error;
      }
      if (error != m_lastOpenError)
      {
        DEBUG_INFO("Looking Glass Audio broker unavailable: %lu", error);
        m_lastOpenError = error;
      }
      m_nextOpenAt = now + OPEN_RETRY_MS;
      return false;
    }

    SetHandle(handle);
    DWORD error = ERROR_SUCCESS;
    if (!QueryCaps(error) || !QueryState(error))
    {
      if (!Stopping())
        DEBUG_WARN("Looking Glass Audio broker query failed: %lu", error);
      CloseBroker(false);
      m_nextOpenAt = now + OPEN_RETRY_MS;
      PublishUnavailable(false);
      return false;
    }

    m_lastOpenError = ERROR_SUCCESS;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_everOpened)
        ++m_reconnects;
      m_everOpened = true;
      m_lastError  = ERROR_SUCCESS;
    }
    m_nextPollAt    = 0;
    DEBUG_INFO("Looking Glass Audio broker connected");
    PublishBrokerState();
    return true;
  }

  bool DesiredActive(bool& dirty)
  {
    CSRWExclusiveLock lock(m_lock);
    dirty = m_controlDirty;
    m_controlDirty = false;
    return m_activation == ActivationState::ACTIVATING ||
      m_activation == ActivationState::ACTIVE;
  }

  bool FinishActivation(bool bound, bool dataPlaneActive)
  {
    CSRWExclusiveLock lock(m_lock);
    if (bound && dataPlaneActive &&
        m_activation == ActivationState::ACTIVATING)
      m_activation = ActivationState::ACTIVE;
    else if (!bound && m_activation == ActivationState::DEACTIVATING)
    {
      m_activation = ActivationState::IDLE;
      return true;
    }
    return false;
  }

  void DropQueuedCapture()
  {
    bool ready = false;
    ILGAudioDeviceEvents * events = nullptr;
    {
      CSRWExclusiveLock lock(m_lock);
      ready = m_captureCount == CAPTURE_SLOT_COUNT;
      m_captureHead  = 0;
      m_captureCount = 0;
      events = m_events;
    }
    if (ready && events)
      events->LGAudioCaptureReady();
  }

  bool Reconcile(bool forced)
  {
    bool dirty = false;
    const bool desired = DesiredActive(dirty);
    const uint64_t now = GetTickCount64();
    if (!forced && !dirty && now < m_nextPollAt)
      return true;

    DWORD error = ERROR_SUCCESS;
    if (!QueryState(error))
    {
      if (Stopping())
        return false;
      DEBUG_WARN("Looking Glass Audio state query failed: %lu", error);
      CloseBroker();
      return false;
    }

    const bool present =
      (m_brokerState.flags & LGAUDIO_BROKER_STATE_ADAPTER_PRESENT) != 0;
    const bool initiallyBound =
      (m_brokerState.flags & LGAUDIO_BROKER_STATE_BOUND) != 0;
    if (desired && present && !initiallyBound)
    {
      if (!Bind(error))
      {
        if (error != ERROR_RETRY && error != ERROR_NOT_READY &&
            error != ERROR_DEVICE_NOT_CONNECTED)
          DEBUG_WARN("Looking Glass Audio broker bind failed: %lu", error);
      }
      if (!QueryState(error))
      {
        CloseBroker();
        return false;
      }
    }
    else if (!desired && initiallyBound)
    {
      CancelDataIo();
      DropQueuedCapture();
      if (!Unbind(error) && !Stopping())
        DEBUG_WARN("Looking Glass Audio broker unbind failed: %lu", error);
      if (!QueryState(error))
      {
        CloseBroker();
        return false;
      }
    }

    const bool bound =
      (m_brokerState.flags & LGAUDIO_BROKER_STATE_BOUND) != 0;
    const bool dataPlaneActive =
      (m_brokerState.flags &
        LGAUDIO_BROKER_STATE_DATA_PLANE_ACTIVE) != 0;
    const bool inactiveBarrier =
      FinishActivation(bound, dataPlaneActive);
    PublishBrokerState(inactiveBarrier);
    m_nextPollAt = now +
      (desired ? ACTIVE_POLL_MS : IDLE_POLL_MS);
    return true;
  }

  bool ValidateRender(const LGAudioBrokerPacketHeader& header,
    DWORD transferred, AudioPacket& packet) const
  {
    const AudioEndpointStatus state = Status();
    const AudioFormat format = BrokerFormat(header.format);
    const uint32_t allowedFlags = LGAUDIO_BROKER_PACKET_DISCONTINUITY;
    if (!state.available || !state.active ||
        state.playback.state != AudioStreamState::RUNNING ||
        !state.playback.formatValid ||
        header.size != sizeof(header) ||
        header.version != LGAUDIO_BROKER_ABI_VERSION ||
        (header.flags & ~allowedFlags) ||
        header.direction != LGAUDIO_BROKER_PACKET_RENDER ||
        header.adapterGeneration != m_brokerState.adapterGeneration ||
        header.bindingGeneration != m_brokerState.bindingGeneration ||
        header.streamGeneration != state.playback.generation ||
        header.reserved[0] || header.reserved[1] ||
        !ValidFormat(format) ||
        !SameFormat(format, state.playback.format) ||
        !header.frameCount ||
        header.frameCount > AUDIO_MAX_FRAMES_PER_PACKET ||
        !header.payloadBytes ||
        header.payloadBytes > AUDIO_MAX_PCM_BYTES ||
        header.payloadBytes != header.frameCount * format.blockAlign ||
        transferred != sizeof(header) + header.payloadBytes)
      return false;

    packet.endpointGeneration = state.generation;
    packet.streamGeneration   = header.streamGeneration;
    packet.flags = header.flags & LGAUDIO_BROKER_PACKET_DISCONTINUITY ?
      AUDIO_PACKET_DISCONTINUITY : 0;
    packet.frames = header.frameCount;
    packet.format = format;
    return true;
  }

  int NextReadyRender() const
  {
    int selected = -1;
    uint64_t sequence = 0;
    for (unsigned index = 0; index < RENDER_SLOT_COUNT; ++index)
    {
      const RenderSlot& slot = m_render[index];
      if (slot.state != RenderState::READY ||
          slot.transferred < sizeof(LGAudioBrokerPacketHeader))
        continue;
      LGAudioBrokerPacketHeader header = {};
      memcpy(&header, slot.data, sizeof(header));
      if (selected < 0 || header.sequence < sequence)
      {
        selected = static_cast<int>(index);
        sequence = header.sequence;
      }
    }
    return selected;
  }

  void DeliverRender()
  {
    for (RenderSlot& slot : m_render)
    {
      if (slot.state != RenderState::READY ||
          slot.transferred >= sizeof(LGAudioBrokerPacketHeader))
        continue;
      DEBUG_WARN("Rejected truncated Looking Glass Audio render packet");
      slot.state       = RenderState::IDLE;
      slot.transferred = 0;
      m_renderDiscontinuity = true;
    }

    if (PlaybackBlocked())
      return;

    for (;;)
    {
      const int index = NextReadyRender();
      if (index < 0)
        return;
      RenderSlot& slot = m_render[index];
      LGAudioBrokerPacketHeader header = {};
      memcpy(&header, slot.data, sizeof(header));
      AudioPacket packet;
      if (!ValidateRender(header, slot.transferred, packet))
      {
        DEBUG_WARN("Rejected malformed Looking Glass Audio render packet");
        slot.state = RenderState::IDLE;
        m_renderDiscontinuity = true;
        continue;
      }

      if (m_renderSequenceValid && header.sequence != m_nextRenderSequence)
        packet.flags |= AUDIO_PACKET_DISCONTINUITY;
      if (m_renderDiscontinuity)
        packet.flags |= AUDIO_PACKET_DISCONTINUITY;

      ILGAudioDeviceEvents * events = nullptr;
      uint64_t readySerial = 0;
      {
        CSRWSharedLock lock(m_lock);
        events      = m_events;
        readySerial = m_playbackReadySerial;
      }
      const uint8_t * payload = slot.data + sizeof(header);
      const AudioResult result = events ?
        events->LGAudioPlayback(packet, payload, header.payloadBytes) :
        AudioResult::UNAVAILABLE;
      if (result == AudioResult::BUSY)
      {
        CSRWExclusiveLock lock(m_lock);
        m_playbackBlocked = m_playbackReadySerial == readySerial;
        return;
      }

      slot.state = RenderState::IDLE;
      m_nextRenderSequence  = header.sequence + 1;
      m_renderSequenceValid = true;
      m_renderDiscontinuity = result != AudioResult::ACCEPTED;
      if (result == AudioResult::ACCEPTED)
      {
        CSRWExclusiveLock lock(m_lock);
        ++m_renderPackets;
        m_renderBytes += header.payloadBytes;
      }
    }
  }

  bool CompleteRenderIo(bool& disconnected)
  {
    bool changed = false;
    for (RenderSlot& slot : m_render)
    {
      if (slot.state != RenderState::PENDING)
        continue;
      DWORD transferred = 0;
      if (GetOverlappedResult(
          m_handle, &slot.overlapped, &transferred, FALSE))
      {
        ResetEvent(slot.overlapped.hEvent);
        slot.transferred = transferred;
        slot.state       = RenderState::READY;
        changed          = true;
        continue;
      }
      const DWORD error = GetLastError();
      if (error == ERROR_IO_INCOMPLETE)
        continue;
      slot.state       = RenderState::IDLE;
      slot.transferred = 0;
      ResetEvent(slot.overlapped.hEvent);
      changed = true;
      if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE)
        disconnected = true;
      else if (!RecoverableIoError(error))
        DEBUG_WARN("Looking Glass Audio render read failed: %lu", error);
    }
    return changed;
  }

  void StartRenderIo()
  {
    const AudioEndpointStatus state = Status();
    if (!state.active || !state.playback.formatValid ||
        state.playback.state != AudioStreamState::RUNNING)
      return;

    const DWORD payload = static_cast<DWORD>(std::min<size_t>(
      AUDIO_MAX_FRAMES_PER_PACKET * state.playback.format.blockAlign,
      m_caps.maximumRenderTransfer));
    const DWORD aligned = payload -
      payload % state.playback.format.blockAlign;
    if (!aligned)
      return;

    for (RenderSlot& slot : m_render)
    {
      if (slot.state != RenderState::IDLE)
        continue;
      ResetEvent(slot.overlapped.hEvent);
      DWORD transferred = 0;
      const BOOL immediate = ReadFile(m_handle, slot.data,
        sizeof(LGAudioBrokerPacketHeader) + aligned,
        nullptr, &slot.overlapped);
      if (!immediate)
      {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
          slot.state = RenderState::PENDING;
          continue;
        }
        if (!RecoverableIoError(error))
          DEBUG_WARN("Looking Glass Audio render read failed: %lu", error);
        ResetEvent(slot.overlapped.hEvent);
        continue;
      }
      if (!GetOverlappedResult(
          m_handle, &slot.overlapped, &transferred, FALSE))
      {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_INCOMPLETE && !RecoverableIoError(error))
          DEBUG_WARN("Looking Glass Audio render completion failed: %lu",
            error);
        if (error == ERROR_IO_INCOMPLETE)
          slot.state = RenderState::PENDING;
        else
          ResetEvent(slot.overlapped.hEvent);
        continue;
      }
      slot.transferred = transferred;
      slot.state       = RenderState::READY;
      ResetEvent(slot.overlapped.hEvent);
    }
  }

  bool PopCapture(CaptureSlot& result, bool& becameReady)
  {
    ILGAudioDeviceEvents * events = nullptr;
    {
      CSRWExclusiveLock lock(m_lock);
      if (!m_captureCount)
        return false;
      becameReady = m_captureCount == CAPTURE_SLOT_COUNT;
      CaptureSlot& source = m_capture[m_captureHead];
      result.packet = source.packet;
      result.size   = source.size;
      memcpy(result.data, source.data, source.size);
      m_captureHead = (m_captureHead + 1) % CAPTURE_SLOT_COUNT;
      --m_captureCount;
      events = m_events;
    }
    if (becameReady && events)
      events->LGAudioCaptureReady();
    return true;
  }

  bool CompleteWriteIo(bool& disconnected)
  {
    if (!m_writePending)
      return false;

    DWORD transferred = 0;
    if (!GetOverlappedResult(
        m_handle, &m_writeOverlapped, &transferred, FALSE))
    {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_INCOMPLETE)
        return false;
      m_writePending = false;
      m_writeBytes   = 0;
      m_captureDiscontinuity = true;
      ResetEvent(m_writeOverlapped.hEvent);
      if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE)
        disconnected = true;
      else if (!RecoverableIoError(error))
        DEBUG_WARN("Looking Glass Audio capture write failed: %lu", error);
      return true;
    }

    m_writePending = false;
    ResetEvent(m_writeOverlapped.hEvent);
    if (transferred == m_writeBytes)
    {
      ++m_nextCaptureSequence;
      m_captureDiscontinuity = false;
      CSRWExclusiveLock lock(m_lock);
      ++m_capturePackets;
      m_captureBytes += m_writeBytes - sizeof(LGAudioBrokerPacketHeader);
    }
    else
    {
      m_captureDiscontinuity = true;
      DEBUG_WARN("Looking Glass Audio capture write was truncated");
    }
    m_writeBytes = 0;
    return true;
  }

  void StartWriteIo()
  {
    if (m_writePending)
      return;

    const AudioEndpointStatus state = Status();
    if (!state.active || !state.capture.formatValid ||
        state.capture.state != AudioStreamState::RUNNING)
      return;

    CaptureSlot capture;
    bool becameReady = false;
    while (PopCapture(capture, becameReady))
    {
      if (capture.packet.endpointGeneration != state.generation ||
          capture.packet.streamGeneration != state.capture.generation ||
          !SameFormat(capture.packet.format, state.capture.format))
        continue;

      LGAudioBrokerPacketHeader header = {};
      header.size              = sizeof(header);
      header.version           = LGAUDIO_BROKER_ABI_VERSION;
      header.flags = capture.packet.flags & AUDIO_PACKET_DISCONTINUITY ?
        LGAUDIO_BROKER_PACKET_DISCONTINUITY : 0;
      if (m_captureDiscontinuity)
        header.flags |= LGAUDIO_BROKER_PACKET_DISCONTINUITY;
      header.direction         = LGAUDIO_BROKER_PACKET_CAPTURE;
      header.adapterGeneration = m_brokerState.adapterGeneration;
      header.bindingGeneration = m_brokerState.bindingGeneration;
      header.streamGeneration  = state.capture.generation;
      header.sequence          = m_nextCaptureSequence;
      header.format            = DeviceFormat(state.capture.format);
      header.frameCount        = capture.packet.frames;
      header.payloadBytes      = static_cast<uint32_t>(capture.size);
      memcpy(m_writeData, &header, sizeof(header));
      memcpy(m_writeData + sizeof(header), capture.data, capture.size);
      m_writeBytes = static_cast<DWORD>(sizeof(header) + capture.size);

      ResetEvent(m_writeOverlapped.hEvent);
      const BOOL immediate = WriteFile(m_handle, m_writeData,
        m_writeBytes, nullptr, &m_writeOverlapped);
      m_writePending = true;
      if (!immediate && GetLastError() != ERROR_IO_PENDING)
      {
        const DWORD error = GetLastError();
        m_writePending = false;
        m_writeBytes   = 0;
        m_captureDiscontinuity = true;
        ResetEvent(m_writeOverlapped.hEvent);
        if (!RecoverableIoError(error))
          DEBUG_WARN("Looking Glass Audio capture write failed: %lu", error);
      }
      return;
    }
  }

  DWORD WaitTimeout() const
  {
    const uint64_t now = GetTickCount64();
    const uint64_t deadline = m_handle == INVALID_HANDLE_VALUE ?
      m_nextOpenAt : m_nextPollAt;
    if (deadline <= now)
      return 0;
    return static_cast<DWORD>(std::min<uint64_t>(
      deadline - now, OPEN_RETRY_MS));
  }

  void Thread()
  {
    while (!Stopping())
    {
      if (m_handle == INVALID_HANDLE_VALUE)
        FinishAbsentDeactivation();
      if (m_handle == INVALID_HANDLE_VALUE && !OpenBroker())
      {
        const HANDLE waits[] = { m_stopEvent, m_wakeEvent };
        WaitForMultipleObjects(
          ARRAYSIZE(waits), waits, FALSE, WaitTimeout());
        continue;
      }

      bool disconnected = false;
      CompleteWriteIo(disconnected);
      CompleteRenderIo(disconnected);
      if (disconnected)
      {
        CloseBroker();
        m_nextOpenAt = GetTickCount64() + OPEN_RETRY_MS;
        continue;
      }

      if (!Reconcile(false))
        continue;
      DeliverRender();
      StartWriteIo();
      StartRenderIo();

      HANDLE waits[2 + RENDER_SLOT_COUNT + 1] = {};
      unsigned count = 0;
      waits[count++] = m_stopEvent;
      waits[count++] = m_wakeEvent;
      for (RenderSlot& slot : m_render)
        waits[count++] = slot.overlapped.hEvent;
      waits[count++] = m_writeOverlapped.hEvent;
      WaitForMultipleObjects(count, waits, FALSE, WaitTimeout());
    }

    {
      CSRWExclusiveLock lock(m_lock);
      m_activation = ActivationState::DEACTIVATING;
    }
    if (m_handle != INVALID_HANDLE_VALUE)
    {
      CancelDataIo();
      if (m_brokerState.flags & LGAUDIO_BROKER_STATE_BOUND)
      {
        DWORD error = ERROR_SUCCESS;
        Unbind(error);
      }
      CloseBroker(false);
    }
    FinishAbsentDeactivation();
  }

  void CloseEvents()
  {
    for (RenderSlot& slot : m_render)
    {
      if (slot.overlapped.hEvent)
        CloseHandle(slot.overlapped.hEvent);
      slot.overlapped = {};
      slot.state      = RenderState::IDLE;
    }
    if (m_writeEvent)
      CloseHandle(m_writeEvent);
    if (m_controlEvent)
      CloseHandle(m_controlEvent);
    if (m_wakeEvent)
      CloseHandle(m_wakeEvent);
    if (m_stopEvent)
      CloseHandle(m_stopEvent);
    m_writeEvent   = nullptr;
    m_controlEvent = nullptr;
    m_wakeEvent    = nullptr;
    m_stopEvent    = nullptr;
    m_writeOverlapped = {};
  }

public:
  CState()
  {
    m_status.generation = m_endpointGeneration;
  }

  ~CState()
  {
    Stop();
  }

  bool Start(ILGAudioDeviceEvents& events)
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_started)
      return !m_stopping;

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_controlEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent || !m_wakeEvent || !m_controlEvent || !m_writeEvent)
    {
      CloseEvents();
      return false;
    }
    m_writeOverlapped.hEvent = m_writeEvent;
    for (RenderSlot& slot : m_render)
    {
      slot.overlapped.hEvent =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!slot.overlapped.hEvent)
      {
        CloseEvents();
        return false;
      }
    }

    m_events   = &events;
    m_started  = true;
    m_stopping = false;
    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!m_thread)
    {
      m_started = false;
      m_events  = nullptr;
      CloseEvents();
      return false;
    }
    return true;
  }

  void Stop()
  {
    HANDLE thread = nullptr;
    {
      CSRWExclusiveLock lock(m_lock);
      if (!m_started)
        return;
      m_stopping = true;
      thread = m_thread;
      SetEvent(m_stopEvent);
      SetEvent(m_wakeEvent);
    }
    if (thread)
      WaitForSingleObject(thread, INFINITE);

    CSRWExclusiveLock lock(m_lock);
    if (m_thread)
      CloseHandle(m_thread);
    m_thread    = nullptr;
    m_events    = nullptr;
    m_started   = false;
    m_stopping  = false;
    m_handle    = INVALID_HANDLE_VALUE;
    m_bound     = false;
    m_captureHead  = 0;
    m_captureCount = 0;
    m_activation   = ActivationState::IDLE;
    m_controlDirty = true;
    m_status = {};
    m_status.generation = m_endpointGeneration;
    CloseEvents();
  }

  AudioEndpointStatus Status() const
  {
    CSRWSharedLock lock(m_lock);
    return m_status;
  }

  LGAudioDeviceSnapshot Snapshot() const
  {
    CSRWSharedLock lock(m_lock);
    LGAudioDeviceSnapshot result;
    result.status          = m_status;
    result.open            = m_handle != INVALID_HANDLE_VALUE;
    result.bound           = m_bound;
    result.activeRequested =
      m_activation == ActivationState::ACTIVATING ||
      m_activation == ActivationState::ACTIVE;
    result.deactivating =
      m_activation == ActivationState::DEACTIVATING;
    result.renderPackets  = m_renderPackets;
    result.renderBytes    = m_renderBytes;
    result.capturePackets = m_capturePackets;
    result.captureBytes   = m_captureBytes;
    result.reconnects     = m_reconnects;
    result.lastError      = m_lastError;
    return result;
  }

  AudioResult SetActive(bool active)
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_started || m_stopping)
      return AudioResult::UNAVAILABLE;
    if (active)
    {
      if (m_activation == ActivationState::DEACTIVATING)
        return AudioResult::BUSY;
      if (m_activation == ActivationState::IDLE)
        m_activation = ActivationState::ACTIVATING;
    }
    else if (m_activation == ActivationState::ACTIVATING ||
             m_activation == ActivationState::ACTIVE)
    {
      m_activation = ActivationState::DEACTIVATING;
      m_controlDirty  = true;
      SetEvent(m_wakeEvent);
    }
    if (active)
    {
      m_controlDirty = true;
      SetEvent(m_wakeEvent);
    }
    return AudioResult::ACCEPTED;
  }

  AudioResult SendCapture(const AudioPacket& packet,
    const uint8_t * data, size_t size)
  {
    const uint32_t allowedFlags = AUDIO_PACKET_DISCONTINUITY |
      AUDIO_PACKET_SILENT | AUDIO_PACKET_CLOCK_VALID |
      AUDIO_PACKET_CLOCK_STABLE;
    if ((packet.flags & ~allowedFlags) ||
        ((packet.flags & AUDIO_PACKET_CLOCK_STABLE) &&
          !(packet.flags & AUDIO_PACKET_CLOCK_VALID)) ||
        !packet.frames ||
        packet.frames > AUDIO_MAX_FRAMES_PER_PACKET ||
        !ValidFormat(packet.format))
      return AudioResult::REJECTED;

    const size_t pcmBytes =
      static_cast<size_t>(packet.frames) * packet.format.blockAlign;
    const bool silent = (packet.flags & AUDIO_PACKET_SILENT) != 0;
    if (pcmBytes > AUDIO_MAX_PCM_BYTES ||
        (silent ? size != 0 : (size != pcmBytes || !data)))
      return AudioResult::REJECTED;

    CSRWExclusiveLock lock(m_lock);
    if (!m_started || m_stopping ||
        m_activation != ActivationState::ACTIVE ||
        !m_status.available || !m_status.active)
      return AudioResult::UNAVAILABLE;
    if (packet.endpointGeneration != m_status.generation ||
        packet.streamGeneration != m_status.capture.generation)
      return AudioResult::STALE;
    if (!m_status.capture.formatValid ||
        m_status.capture.state != AudioStreamState::RUNNING ||
        !SameFormat(packet.format, m_status.capture.format))
      return AudioResult::UNAVAILABLE;
    if (m_captureCount == CAPTURE_SLOT_COUNT)
      return AudioResult::BUSY;

    CaptureSlot& slot =
      m_capture[(m_captureHead + m_captureCount) % CAPTURE_SLOT_COUNT];
    slot.packet = packet;
    slot.size   = pcmBytes;
    if (silent)
      memset(slot.data, 0, pcmBytes);
    else
      memcpy(slot.data, data, pcmBytes);
    ++m_captureCount;
    SetEvent(m_wakeEvent);
    return AudioResult::ACCEPTED;
  }

  AudioResult SetClockFeedback(const AudioClockFeedback& feedback)
  {
    const uint32_t allowedFlags =
      AUDIO_CLOCK_VALID | AUDIO_CLOCK_STABLE |
      AUDIO_CLOCK_DISCONTINUITY;
    if ((feedback.flags & ~allowedFlags) ||
        !(feedback.flags & AUDIO_CLOCK_VALID))
      return AudioResult::REJECTED;
    CSRWSharedLock lock(m_lock);
    if (!m_started || m_stopping || !m_status.active)
      return AudioResult::UNAVAILABLE;
    if (feedback.endpointGeneration != m_status.generation ||
        feedback.playbackGeneration != m_status.playback.generation)
      return AudioResult::STALE;
    // The v1 broker advertises no clock-control capability or IOCTL yet.
    return AudioResult::UNAVAILABLE;
  }

  void PlaybackReceiveReady()
  {
    bool wake = false;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_started && !m_stopping)
      {
        m_playbackReadySerial = NextGeneration(m_playbackReadySerial);
        m_playbackBlocked = false;
        wake = true;
      }
    }
    if (wake)
      SetEvent(m_wakeEvent);
  }
};

CLGAudioDevice::CLGAudioDevice() :
  m_state(new (std::nothrow) CState())
{
}

CLGAudioDevice::~CLGAudioDevice()
{
  Stop();
}

bool CLGAudioDevice::Start(ILGAudioDeviceEvents& events)
{
  return m_state && m_state->Start(events);
}

void CLGAudioDevice::Stop()
{
  if (m_state)
    m_state->Stop();
}

LGAudioDeviceSnapshot CLGAudioDevice::Snapshot() const
{
  return m_state ? m_state->Snapshot() : LGAudioDeviceSnapshot {};
}

AudioResult CLGAudioDevice::SetActive(bool active)
{
  return m_state ?
    m_state->SetActive(active) : AudioResult::UNAVAILABLE;
}

AudioResult CLGAudioDevice::SendCapture(const AudioPacket& packet,
  const uint8_t * data, size_t size)
{
  return m_state ? m_state->SendCapture(packet, data, size) :
    AudioResult::UNAVAILABLE;
}

AudioResult CLGAudioDevice::SetClockFeedback(
  const AudioClockFeedback& feedback)
{
  return m_state ? m_state->SetClockFeedback(feedback) :
    AudioResult::UNAVAILABLE;
}

void CLGAudioDevice::PlaybackReceiveReady()
{
  if (m_state)
    m_state->PlaybackReceiveReady();
}
