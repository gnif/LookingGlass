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

#include "transport/CAudioHub.h"

#include <new>

namespace
{
  bool SameFormat(const AudioFormat& left, const AudioFormat& right)
  {
    return
      left.sampleFormat  == right.sampleFormat  &&
      left.sampleRate    == right.sampleRate    &&
      left.channelMask   == right.channelMask   &&
      left.channels      == right.channels      &&
      left.containerBits == right.containerBits &&
      left.validBits     == right.validBits     &&
      left.blockAlign    == right.blockAlign;
  }

  bool SameStream(const AudioStreamStatus& left,
    const AudioStreamStatus& right)
  {
    return
      left.generation  == right.generation  &&
      left.state       == right.state       &&
      left.formatValid == right.formatValid &&
      (!left.formatValid || SameFormat(left.format, right.format));
  }
}

void CAudioHub::CTarget::Attach(CAudioHub& owner, unsigned binding)
{
  m_owner   = &owner;
  m_binding = binding;
}

AudioResult CAudioHub::CTarget::SetAudioActive(bool active)
{
  return m_owner ? m_owner->TargetSetAudioActive(m_binding, active) :
    AudioResult::UNAVAILABLE;
}

AudioResult CAudioHub::CTarget::SendCapture(const AudioPacket& packet,
  const uint8_t * data, size_t size)
{
  return m_owner ?
    m_owner->TargetSendCapture(m_binding, packet, data, size) :
    AudioResult::UNAVAILABLE;
}

AudioResult CAudioHub::CTarget::SetClockFeedback(
  const AudioClockFeedback& feedback)
{
  return m_owner ?
    m_owner->TargetSetClockFeedback(m_binding, feedback) :
    AudioResult::UNAVAILABLE;
}

void CAudioHub::CTarget::PlaybackReceiveReady()
{
  if (m_owner)
    m_owner->TargetPlaybackReceiveReady(m_binding);
}

void CAudioHub::CTarget::AudioFailed()
{
  if (m_owner)
    m_owner->TargetAudioFailed(m_binding);
}

CAudioHub::CAudioHub() :
  m_device(new (std::nothrow) CLGAudioDevice())
{
  for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
    m_bindings[i].target.Attach(*this, i);
}

CAudioHub::CAudioHub(std::unique_ptr<ILGAudioDevice> device) :
  m_device(std::move(device))
{
  for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
    m_bindings[i].target.Attach(*this, i);
}

CAudioHub::~CAudioHub()
{
  Stop();
}

bool CAudioHub::BindingLiveNL(unsigned binding) const
{
  if (binding >= TRANSPORT_MAX_INSTANCES)
    return false;
  const Binding& target = m_bindings[binding];
  return target.occupied && target.running && !target.failed &&
    target.source != nullptr;
}

bool CAudioHub::AnyActiveNL() const
{
  for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
    if (BindingLiveNL(i) && m_bindings[i].activeRequested)
      return true;
  return false;
}

void CAudioHub::ClearAuthoritiesNL(unsigned binding)
{
  if (m_captureAuthority == binding)
  {
    m_captureAuthority          = NO_BINDING;
    m_captureEndpointGeneration = 0;
    m_captureStreamGeneration   = 0;
  }
  if (m_clockAuthority == binding)
  {
    m_clockAuthority          = NO_BINDING;
    m_clockEndpointGeneration = 0;
    m_clockStreamGeneration   = 0;
  }
}

void CAudioHub::RequestPlaybackWakeNL()
{
  if (!m_stopped && m_deviceStarted)
    m_playbackWakePending = true;
}

void CAudioHub::RetirePlaybackNL(unsigned binding)
{
  if (binding >= TRANSPORT_MAX_INSTANCES)
    return;

  Binding& target = m_bindings[binding];
  if (m_playbackActive &&
      (target.playback == PlaybackState::READY ||
       target.playback == PlaybackState::BUSY))
    RequestPlaybackWakeNL();
  if (target.playback != PlaybackState::IDLE)
    target.playback = PlaybackState::DONE;
  target.playbackReadyEarly = false;
}

void CAudioHub::DeactivateBindingNL(unsigned binding)
{
  if (binding >= TRANSPORT_MAX_INSTANCES)
    return;

  ClearAuthoritiesNL(binding);
  m_bindings[binding].captureReceivePending = false;
  RetirePlaybackNL(binding);
}

void CAudioHub::FlushPlaybackWake()
{
  for (;;)
  {
    // PlaybackReceiveReady may synchronously enter LGAudioPlayback. Defer it
    // until neither source callbacks nor target operations are serialized.
    CSRWExclusiveLock operationLock =
      CSRWExclusiveLock::Try(m_operationLock);
    if (!operationLock)
      return;
    CSRWExclusiveLock callbackLock =
      CSRWExclusiveLock::Try(m_callbackLock);
    if (!callbackLock)
      return;

    bool wake = false;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_playbackWakeDispatching)
        return;
      if (m_playbackWakePending && !m_stopped && m_deviceStarted)
      {
        m_playbackWakeDispatching = true;
        m_playbackWakePending     = false;
        wake                      = true;
      }
      else if (m_stopped || !m_deviceStarted)
        m_playbackWakePending = false;
    }
    callbackLock.Unlock();
    operationLock.Unlock();
    if (!wake)
      return;

    m_device->PlaybackReceiveReady();
    {
      CSRWExclusiveLock lock(m_lock);
      m_playbackWakeDispatching = false;
    }
  }
}

void CAudioHub::ResetPlaybackNL()
{
  m_playbackActive      = false;
  m_playbackAccepted    = false;
  m_playbackFailed      = false;
  m_playbackWakePending = false;
  for (Binding& binding : m_bindings)
  {
    binding.playback           = PlaybackState::IDLE;
    binding.playbackReadyEarly = false;
  }
}

bool CAudioHub::MarkFailedNL(unsigned binding, bool& deactivate)
{
  deactivate = false;
  if (binding >= TRANSPORT_MAX_INSTANCES)
    return false;

  Binding& target = m_bindings[binding];
  if (!target.occupied || !target.source ||
      (!target.running && !target.reserved))
    return false;

  const bool wasActive = AnyActiveNL();
  const bool changed   = !target.failed;
  target.failed                = true;
  target.failurePending        = true;
  target.activeRequested       = false;
  DeactivateBindingNL(binding);
  deactivate = wasActive && !AnyActiveNL();
  return changed;
}

bool CAudioHub::MarkFailed(unsigned binding)
{
  bool changed       = false;
  bool deactivate    = false;
  bool deviceStarted = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      changed       = MarkFailedNL(binding, deactivate);
      deviceStarted = m_deviceStarted;
    }
    if (deactivate && deviceStarted)
      (void)m_device->SetActive(false);
  }
  FlushPlaybackWake();
  return changed;
}

bool CAudioHub::StartDevice()
{
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_device)
      return false;
    if (m_deviceStarted)
      return true;
  }

  if (!m_device->Start(*this))
    return false;

  CSRWExclusiveLock callbackLock(m_callbackLock);
  const LGAudioDeviceSnapshot snapshot = m_device->Snapshot();
  CSRWExclusiveLock lock(m_lock);
  if (m_stopped)
  {
    lock.Unlock();
    callbackLock.Unlock();
    m_device->Stop();
    return false;
  }
  m_deviceStarted = true;
  m_state         = snapshot.status;
  m_stateKnown    = true;
  lock.Unlock();
  callbackLock.Unlock();
  FlushPlaybackWake();
  return true;
}

bool CAudioHub::Start()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  return StartDevice();
}

bool CAudioHub::Bind(
  BackendId backend, uint32_t epoch, IAudioSource& source)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (!backend || !epoch || !StartDevice())
    return false;

  unsigned index         = NO_BINDING;
  bool     bindComplete  = false;
  bool     bindResult    = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      bindComplete = true;
    else
    {
      for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
      {
        const Binding& binding = m_bindings[i];
        if (binding.occupied && binding.backend == backend &&
            binding.epoch == epoch)
        {
          bindComplete = true;
          bindResult   = binding.source == &source && binding.running &&
            !binding.failed;
          break;
        }
        if (index == NO_BINDING && !binding.occupied)
          index = i;
      }
      if (!bindComplete && index == NO_BINDING)
        bindComplete = true;
    }

    if (!bindComplete)
    {
      Binding& binding = m_bindings[index];
      binding.source         = &source;
      binding.backend        = backend;
      binding.epoch          = epoch;
      binding.occupied       = true;
      binding.reserved       = true;
      binding.running        = false;
      binding.failed         = false;
      binding.failurePending = false;
    }
  }
  FlushPlaybackWake();
  if (bindComplete)
    return bindResult;

  CSRWExclusiveLock callbackLock(m_callbackLock);
  const bool          started  = source.Start(m_bindings[index].target);
  bool                attached = false;
  AudioEndpointStatus state    = {};
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    CSRWExclusiveLock lock(m_lock);
    Binding& binding = m_bindings[index];
    if (started && !m_stopped && binding.occupied &&
        binding.source == &source && binding.backend == backend &&
        binding.epoch == epoch && binding.reserved && !binding.failed)
    {
      binding.running  = true;
      binding.reserved = false;
      state            = m_state;
      attached         = true;
    }
  }

  if (!attached)
  {
    source.Stop();
    {
      CSRWExclusiveLock operationLock(m_operationLock);
      CSRWExclusiveLock lock(m_lock);
      Binding& binding = m_bindings[index];
      if (binding.occupied && binding.source == &source &&
          binding.backend == backend && binding.epoch == epoch)
        binding.Reset();
    }
    callbackLock.Unlock();
    FlushPlaybackWake();
    return false;
  }

  source.AudioState(state);
  callbackLock.Unlock();
  FlushPlaybackWake();
  return true;
}

void CAudioHub::Unbind(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  IAudioSource * source        = nullptr;
  unsigned       index         = NO_BINDING;
  bool           stop          = false;
  bool           deactivate    = false;
  bool           deviceStarted = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
      {
        Binding& binding = m_bindings[i];
        if (!binding.occupied || binding.backend != backend ||
            binding.epoch != epoch || !binding.source)
          continue;
        const bool wasActive = AnyActiveNL();
        index  = i;
        source = binding.source;
        stop   = binding.running || binding.reserved;

        binding.running               = false;
        binding.reserved              = true;
        binding.activeRequested       = false;
        binding.failurePending        = false;
        DeactivateBindingNL(i);

        deactivate    = wasActive && !AnyActiveNL();
        deviceStarted = m_deviceStarted;
        break;
      }
    }
    if (deactivate && deviceStarted)
      (void)m_device->SetActive(false);
  }
  FlushPlaybackWake();
  if (!source || index >= TRANSPORT_MAX_INSTANCES)
    return;

  {
    CSRWExclusiveLock callbackLock(m_callbackLock);
    if (stop)
      source->Stop();
  }

  {
    CSRWExclusiveLock operationLock(m_operationLock);
    CSRWExclusiveLock lock(m_lock);
    Binding& binding = m_bindings[index];
    if (binding.occupied && binding.source == source &&
        binding.backend == backend && binding.epoch == epoch)
      binding.Reset();
  }
  FlushPlaybackWake();
}

bool CAudioHub::TakeFailure(SourceKey& source)
{
  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  for (Binding& binding : m_bindings)
  {
    if (!binding.occupied || !binding.failurePending)
      continue;
    source         = {};
    source.backend = binding.backend;
    source.epoch   = binding.epoch;

    binding.failurePending = false;
    return true;
  }
  return false;
}

LGAudioDeviceSnapshot CAudioHub::Snapshot() const
{
  CSRWSharedLock lifecycleLock(const_cast<CSRWLock&>(m_lifecycleLock));
  return m_device ? m_device->Snapshot() : LGAudioDeviceSnapshot {};
}

void CAudioHub::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  IAudioSource * sources[TRANSPORT_MAX_INSTANCES] = {};
  unsigned       sourceCount                      = 0;
  bool           stopDevice                       = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_stopped)
        return;
      m_stopped  = true;
      stopDevice = m_deviceStarted;
      for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
      {
        Binding& binding = m_bindings[i];
        if (!binding.occupied || !binding.source)
          continue;
        if (binding.running || binding.reserved)
          sources[sourceCount++] = binding.source;
        binding.running               = false;
        binding.reserved              = true;
        binding.activeRequested       = false;
        binding.failurePending        = false;
        binding.captureReceivePending = false;
        ClearAuthoritiesNL(i);
      }
      ResetPlaybackNL();
    }
    if (stopDevice)
      (void)m_device->SetActive(false);
  }

  {
    CSRWExclusiveLock callbackLock(m_callbackLock);
    for (unsigned i = 0; i < sourceCount; ++i)
      sources[i]->Stop();
  }
  if (stopDevice)
    m_device->Stop();

  CSRWExclusiveLock operationLock(m_operationLock);
  CSRWExclusiveLock lock(m_lock);
  for (Binding& binding : m_bindings)
    binding.Reset();
  m_deviceStarted = false;
  m_state         = {};
  m_stateKnown    = false;
}

AudioResult CAudioHub::TargetSetAudioActive(
  unsigned binding, bool active)
{
  AudioResult result        = AudioResult::ACCEPTED;
  bool        deactivate    = false;
  bool        deviceChanged = false;
  bool        nowActive     = false;
  bool        requestDone   = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_stopped || !BindingLiveNL(binding) || !m_deviceStarted)
      {
        result      = AudioResult::UNAVAILABLE;
        requestDone = true;
      }
      else
      {
        Binding& target = m_bindings[binding];
        if (target.activeRequested == active)
          requestDone = true;
        else
        {
          const bool wasActive   = AnyActiveNL();
          target.activeRequested = active;
          nowActive              = AnyActiveNL();
          deviceChanged          = wasActive != nowActive;
          if (!deviceChanged && !active)
            DeactivateBindingNL(binding);
        }
      }
    }

    if (!requestDone && deviceChanged)
    {
      result = m_device->SetActive(nowActive);
      CSRWExclusiveLock lock(m_lock);
      if (m_stopped || !BindingLiveNL(binding) ||
          m_bindings[binding].activeRequested != active)
        result = AudioResult::UNAVAILABLE;
      else if (result == AudioResult::ACCEPTED ||
          (!active && (result == AudioResult::UNAVAILABLE ||
            result == AudioResult::STALE)))
      {
        if (!active)
          DeactivateBindingNL(binding);
      }
      else if (result == AudioResult::FAILED)
      {
        if (!active)
          m_bindings[binding].activeRequested = true;
        (void)MarkFailedNL(binding, deactivate);
      }
      else
      {
        m_bindings[binding].activeRequested = !active;
        if (active)
          DeactivateBindingNL(binding);
      }
    }

    if (deactivate)
      (void)m_device->SetActive(false);
  }
  FlushPlaybackWake();
  return result;
}

AudioResult CAudioHub::TargetSendCapture(unsigned binding,
  const AudioPacket& packet, const uint8_t * data, size_t size)
{
  AudioResult result      = AudioResult::UNAVAILABLE;
  bool        deactivate  = false;
  bool        requestDone = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_stopped || !BindingLiveNL(binding) ||
          !m_bindings[binding].activeRequested || !m_deviceStarted)
        requestDone = true;
      else
      {
        if (m_captureAuthority != NO_BINDING &&
            (m_captureAuthority >= TRANSPORT_MAX_INSTANCES ||
             !BindingLiveNL(m_captureAuthority) ||
             !m_bindings[m_captureAuthority].activeRequested ||
             !m_stateKnown || !m_state.available || !m_state.active ||
             m_state.capture.state != AudioStreamState::RUNNING ||
             m_state.generation != m_captureEndpointGeneration ||
             m_state.capture.generation != m_captureStreamGeneration))
        {
          m_captureAuthority          = NO_BINDING;
          m_captureEndpointGeneration = 0;
          m_captureStreamGeneration   = 0;
        }
        if (m_captureAuthority != NO_BINDING &&
            m_captureAuthority != binding)
          requestDone = true;
      }
    }

    if (!requestDone)
    {
      result = m_device->SendCapture(packet, data, size);
      {
        CSRWExclusiveLock lock(m_lock);
        if (m_stopped || !BindingLiveNL(binding) ||
            !m_bindings[binding].activeRequested || !m_deviceStarted)
          result = AudioResult::UNAVAILABLE;
        else
        {
          Binding& target = m_bindings[binding];
          target.captureReceivePending = result == AudioResult::BUSY;
          if (result == AudioResult::ACCEPTED &&
              m_captureAuthority == NO_BINDING)
          {
            m_captureAuthority          = binding;
            m_captureEndpointGeneration = packet.endpointGeneration;
            m_captureStreamGeneration   = packet.streamGeneration;
          }
          if (result == AudioResult::FAILED)
            (void)MarkFailedNL(binding, deactivate);
        }
      }
    }

    if (deactivate)
      (void)m_device->SetActive(false);
  }
  FlushPlaybackWake();
  return result;
}

AudioResult CAudioHub::TargetSetClockFeedback(unsigned binding,
  const AudioClockFeedback& feedback)
{
  AudioResult result      = AudioResult::UNAVAILABLE;
  bool        deactivate  = false;
  bool        requestDone = false;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_stopped || !BindingLiveNL(binding) ||
          !m_bindings[binding].activeRequested || !m_deviceStarted)
        requestDone = true;
      else
      {
        if (m_clockAuthority != NO_BINDING &&
            (m_clockAuthority >= TRANSPORT_MAX_INSTANCES ||
             !BindingLiveNL(m_clockAuthority) ||
             !m_bindings[m_clockAuthority].activeRequested ||
             !m_stateKnown || !m_state.available || !m_state.active ||
             m_state.playback.state != AudioStreamState::RUNNING ||
             m_state.generation != m_clockEndpointGeneration ||
             m_state.playback.generation != m_clockStreamGeneration))
        {
          m_clockAuthority          = NO_BINDING;
          m_clockEndpointGeneration = 0;
          m_clockStreamGeneration   = 0;
        }
        if (m_clockAuthority != NO_BINDING &&
            m_clockAuthority != binding)
          requestDone = true;
      }
    }

    if (!requestDone)
    {
      result = m_device->SetClockFeedback(feedback);
      {
        CSRWExclusiveLock lock(m_lock);
        if (m_stopped || !BindingLiveNL(binding) ||
            !m_bindings[binding].activeRequested || !m_deviceStarted)
          result = AudioResult::UNAVAILABLE;
        else
        {
          if (result == AudioResult::ACCEPTED &&
              m_clockAuthority == NO_BINDING)
          {
            m_clockAuthority          = binding;
            m_clockEndpointGeneration = feedback.endpointGeneration;
            m_clockStreamGeneration   = feedback.playbackGeneration;
          }
          if (result == AudioResult::FAILED)
            (void)MarkFailedNL(binding, deactivate);
        }
      }
    }

    if (deactivate)
      (void)m_device->SetActive(false);
  }
  FlushPlaybackWake();
  return result;
}

void CAudioHub::TargetPlaybackReceiveReady(unsigned binding)
{
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    {
      CSRWExclusiveLock lock(m_lock);
      if (!m_stopped && BindingLiveNL(binding) &&
          m_bindings[binding].activeRequested && m_playbackActive)
      {
        Binding& target = m_bindings[binding];
        if (target.playback == PlaybackState::BUSY)
        {
          target.playback = PlaybackState::READY;
          RequestPlaybackWakeNL();
        }
        else if (target.playback == PlaybackState::CALLING)
          target.playbackReadyEarly = true;
      }
    }
  }
  FlushPlaybackWake();
}

void CAudioHub::TargetAudioFailed(unsigned binding)
{
  (void)MarkFailed(binding);
}

void CAudioHub::LGAudioState(const AudioEndpointStatus& state)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IAudioSource * sources[TRANSPORT_MAX_INSTANCES] = {};
  unsigned       sourceCount                      = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;

    const bool endpointChanged = !m_stateKnown ||
      m_state.capabilities != state.capabilities ||
      m_state.generation != state.generation ||
      m_state.available  != state.available  ||
      m_state.active     != state.active;
    const bool playbackChanged = endpointChanged ||
      !SameStream(m_state.playback, state.playback);
    const bool captureChanged  = endpointChanged ||
      !SameStream(m_state.capture, state.capture);
    if (captureChanged)
    {
      m_captureAuthority          = NO_BINDING;
      m_captureEndpointGeneration = 0;
      m_captureStreamGeneration   = 0;
      for (Binding& binding : m_bindings)
        binding.captureReceivePending = false;
    }
    if (playbackChanged)
    {
      m_clockAuthority          = NO_BINDING;
      m_clockEndpointGeneration = 0;
      m_clockStreamGeneration   = 0;
      if (m_playbackActive)
      {
        for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
          RetirePlaybackNL(i);
      }
      else
        ResetPlaybackNL();
    }
    m_state      = state;
    m_stateKnown = true;
    for (Binding& binding : m_bindings)
      if (binding.occupied && binding.running && !binding.failed &&
          binding.source)
        sources[sourceCount++] = binding.source;
  }
  for (unsigned i = 0; i < sourceCount; ++i)
    sources[i]->AudioState(state);
  callbackLock.Unlock();
  FlushPlaybackWake();
}

AudioResult CAudioHub::LGAudioPlayback(const AudioPacket& packet,
  const uint8_t * data, size_t size)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  unsigned calls   [TRANSPORT_MAX_INSTANCES]  = {};
  unsigned failures[TRANSPORT_MAX_INSTANCES]  = {};
  unsigned callCount                          = 0;
  unsigned failureCount                       = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return AudioResult::UNAVAILABLE;
    if (!m_playbackActive)
    {
      m_playbackActive   = true;
      m_playbackAccepted = false;
      m_playbackFailed   = false;
      for (Binding& binding : m_bindings)
      {
        binding.playback           = binding.occupied && binding.running &&
            !binding.failed && binding.activeRequested && binding.source ?
          PlaybackState::READY : PlaybackState::IDLE;
        binding.playbackReadyEarly = false;
      }
    }
    for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
    {
      Binding& binding = m_bindings[i];
      if (!BindingLiveNL(i) || !binding.activeRequested ||
          binding.playback != PlaybackState::READY)
        continue;
      binding.playback           = PlaybackState::CALLING;
      binding.playbackReadyEarly = false;
      calls[callCount++]         = i;
    }
  }

  for (unsigned i = 0; i < callCount; ++i)
  {
    const unsigned index  = calls[i];
    IAudioSource * source = nullptr;
    {
      CSRWSharedLock lock(m_lock);
      if (BindingLiveNL(index) && m_bindings[index].activeRequested &&
          m_bindings[index].playback == PlaybackState::CALLING)
        source = m_bindings[index].source;
    }
    const AudioResult result = source ?
      source->SendPlayback(packet, data, size) : AudioResult::UNAVAILABLE;
    {
      CSRWExclusiveLock lock(m_lock);
      Binding& binding = m_bindings[index];
      if (!BindingLiveNL(index) || !binding.activeRequested ||
          binding.playback != PlaybackState::CALLING)
        continue;
      switch (result)
      {
        case AudioResult::ACCEPTED:
          binding.playback   = PlaybackState::DONE;
          m_playbackAccepted = true;
          break;
        case AudioResult::BUSY:
          if (binding.playbackReadyEarly)
          {
            binding.playback = PlaybackState::READY;
            RequestPlaybackWakeNL();
          }
          else
            binding.playback = PlaybackState::BUSY;
          break;
        case AudioResult::FAILED:
          binding.playback         = PlaybackState::DONE;
          m_playbackFailed         = true;
          failures[failureCount++] = index;
          break;
        default:
          binding.playback = PlaybackState::DONE;
          break;
      }
      binding.playbackReadyEarly = false;
    }
  }

  AudioResult result = AudioResult::UNAVAILABLE;
  {
    CSRWExclusiveLock lock(m_lock);
    bool pending = false;
    for (const Binding& binding : m_bindings)
      if (binding.playback == PlaybackState::READY ||
          binding.playback == PlaybackState::CALLING ||
          binding.playback == PlaybackState::BUSY)
      {
        pending = true;
        break;
      }
    if (pending)
      result = AudioResult::BUSY;
    else
    {
      result = m_playbackAccepted ? AudioResult::ACCEPTED :
        (m_playbackFailed ? AudioResult::FAILED :
          AudioResult::UNAVAILABLE);
      ResetPlaybackNL();
    }
  }
  callbackLock.Unlock();
  for (unsigned i = 0; i < failureCount; ++i)
    (void)MarkFailed(failures[i]);
  FlushPlaybackWake();
  return result;
}

void CAudioHub::LGAudioCaptureReady()
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IAudioSource * sources[TRANSPORT_MAX_INSTANCES] = {};
  unsigned       sourceCount                      = 0;
  {
    CSRWExclusiveLock operationLock(m_operationLock);
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;
    for (unsigned i = 0; i < TRANSPORT_MAX_INSTANCES; ++i)
    {
      Binding& binding = m_bindings[i];
      if (!BindingLiveNL(i) || !binding.activeRequested ||
          !binding.captureReceivePending)
        continue;
      binding.captureReceivePending = false;
      sources[sourceCount++]        = binding.source;
    }
  }
  for (unsigned i = 0; i < sourceCount; ++i)
    sources[i]->CaptureReceiveReady();
  callbackLock.Unlock();
  FlushPlaybackWake();
}
