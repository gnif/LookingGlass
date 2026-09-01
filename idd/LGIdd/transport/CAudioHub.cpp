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

CAudioHub::CAudioHub() :
  m_device(new (std::nothrow) CLGAudioDevice())
{
}

CAudioHub::CAudioHub(std::unique_ptr<ILGAudioDevice> device) :
  m_device(std::move(device))
{
}

CAudioHub::~CAudioHub()
{
  Stop();
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

  CSRWExclusiveLock lock(m_lock);
  if (m_stopped)
  {
    lock.Unlock();
    m_device->Stop();
    return false;
  }
  m_deviceStarted = true;
  return true;
}

bool CAudioHub::Start()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  return StartDevice();
}

bool CAudioHub::MarkFailed(IAudioSource * source)
{
  CSRWExclusiveLock lock(m_lock);
  if (!source || m_source != source || (!m_active && !m_reserved))
    return false;
  const bool changed = !m_failed;
  m_failed         = true;
  m_failurePending = true;
  return changed;
}

bool CAudioHub::Bind(
  BackendId backend, uint32_t epoch, IAudioSource& source)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (!backend || !epoch || !StartDevice())
    return false;

  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped || m_source || m_active || m_reserved ||
        m_deactivating)
      return false;
    m_source         = &source;
    m_backend        = backend;
    m_epoch          = epoch;
    m_active         = true;
    m_reserved       = true;
    m_failed         = false;
    m_failurePending = false;
  }

  CSRWExclusiveLock callbackLock(m_callbackLock);
  const bool started = source.Start(*this);
  bool attached = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (started && m_source == &source && m_backend == backend &&
        m_epoch == epoch && m_active && m_reserved && !m_failed &&
        !m_stopped)
    {
      m_running  = true;
      m_reserved = false;
      attached   = true;
    }
    else
    {
      m_active  = false;
      m_running = false;
    }
  }

  if (!attached)
  {
    source.Stop();
    CSRWExclusiveLock lock(m_lock);
    if (m_source == &source && m_backend == backend && m_epoch == epoch)
    {
      m_source         = nullptr;
      m_backend        = 0;
      m_epoch          = 0;
      m_active         = false;
      m_running        = false;
      m_reserved       = false;
      m_failed         = false;
      m_failurePending = false;
    }
    return false;
  }

  source.AudioState(m_device->Snapshot().status);
  return true;
}

void CAudioHub::Unbind(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  IAudioSource * source = nullptr;
  bool stop = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if ((!m_active && !m_failed && !m_reserved) ||
        m_backend != backend || m_epoch != epoch || !m_source)
      return;
    source           = m_source;
    stop             = m_running || m_reserved;
    m_active         = false;
    m_running        = false;
    m_reserved       = true;
    m_failed         = false;
    m_failurePending = false;
  }

  {
    // Serialize the request and barrier publication with state callbacks so
    // a fast inactive callback cannot pass before the barrier is visible.
    CSRWExclusiveLock callbackLock(m_callbackLock);
    // Start deactivation before asking the source to quiesce, then repeat it
    // after Stop so a target call already in flight cannot reactivate the
    // device on behalf of the departing source.
    m_device->SetActive(false);
    if (stop)
      source->Stop();
    const LGAudioDeviceSnapshot before = m_device->Snapshot();
    const AudioResult deactivate = m_device->SetActive(false);
    const LGAudioDeviceSnapshot after = m_device->Snapshot();
    const bool pending = deactivate == AudioResult::ACCEPTED &&
        (before.status.active || before.activeRequested ||
          after.status.active || after.activeRequested ||
          after.deactivating);
    {
      CSRWExclusiveLock lock(m_lock);
      m_deactivating = pending;
      m_deactivationGeneration = pending ?
        before.status.generation : 0;
    }
  }

  CSRWExclusiveLock lock(m_lock);
  if (m_source == source && m_backend == backend && m_epoch == epoch)
  {
    m_source         = nullptr;
    m_backend        = 0;
    m_epoch          = 0;
    m_active         = false;
    m_running        = false;
    m_reserved       = false;
    m_failed         = false;
    m_failurePending = false;
  }
}

bool CAudioHub::TakeFailure(SourceKey& source)
{
  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  if (!m_failurePending)
    return false;
  source = {};
  source.backend = m_backend;
  source.epoch   = m_epoch;
  m_failurePending = false;
  return true;
}

LGAudioDeviceSnapshot CAudioHub::Snapshot() const
{
  CSRWSharedLock lock(const_cast<CSRWLock&>(m_lock));
  return m_device ? m_device->Snapshot() : LGAudioDeviceSnapshot {};
}

void CAudioHub::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  IAudioSource * source = nullptr;
  bool stopSource = false;
  bool stopDevice = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;
    m_stopped        = true;
    source           = m_source;
    stopSource       = source && (m_running || m_reserved);
    stopDevice       = m_deviceStarted;
    m_active         = false;
    m_running        = false;
    m_reserved       = source != nullptr;
    m_failed         = false;
    m_failurePending = false;
  }

  if (m_device)
    m_device->SetActive(false);
  {
    CSRWExclusiveLock callbackLock(m_callbackLock);
    if (stopSource)
      source->Stop();
  }
  if (stopDevice)
    m_device->Stop();

  CSRWExclusiveLock lock(m_lock);
  m_source         = nullptr;
  m_backend        = 0;
  m_epoch          = 0;
  m_deviceStarted  = false;
  m_active         = false;
  m_running        = false;
  m_reserved       = false;
  m_failed         = false;
  m_failurePending = false;
  m_deactivating   = false;
  m_deactivationGeneration = 0;
}

AudioResult CAudioHub::SetAudioActive(bool active)
{
  IAudioSource * source = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_active || !m_running || m_failed ||
        !m_deviceStarted)
      return AudioResult::UNAVAILABLE;
    source = m_source;
  }
  const AudioResult result = m_device->SetActive(active);
  if (result == AudioResult::FAILED)
    MarkFailed(source);
  return result;
}

AudioResult CAudioHub::SendCapture(const AudioPacket& packet,
  const uint8_t * data, size_t size)
{
  IAudioSource * source = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_active || !m_running || m_failed ||
        !m_deviceStarted)
      return AudioResult::UNAVAILABLE;
    source = m_source;
  }
  const AudioResult result = m_device->SendCapture(packet, data, size);
  if (result == AudioResult::FAILED)
    MarkFailed(source);
  return result;
}

AudioResult CAudioHub::SetClockFeedback(
  const AudioClockFeedback& feedback)
{
  IAudioSource * source = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_active || !m_running || m_failed ||
        !m_deviceStarted)
      return AudioResult::UNAVAILABLE;
    source = m_source;
  }
  const AudioResult result = m_device->SetClockFeedback(feedback);
  if (result == AudioResult::FAILED)
    MarkFailed(source);
  return result;
}

void CAudioHub::PlaybackReceiveReady()
{
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_active || !m_running || m_failed ||
        !m_deviceStarted)
      return;
  }
  m_device->PlaybackReceiveReady();
}

void CAudioHub::AudioFailed()
{
  IAudioSource * source = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    source = m_source;
  }
  if (source)
    MarkFailed(source);
}

void CAudioHub::LGAudioState(const AudioEndpointStatus& state)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IAudioSource * source = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_deactivating && !state.active &&
        state.generation != m_deactivationGeneration)
    {
      m_deactivating = false;
      m_deactivationGeneration = 0;
    }
    if (!m_stopped && m_active && m_running && !m_failed)
      source = m_source;
  }
  if (source)
    source->AudioState(state);
}

AudioResult CAudioHub::LGAudioPlayback(const AudioPacket& packet,
  const uint8_t * data, size_t size)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IAudioSource * source = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    if (!m_stopped && m_active && m_running && !m_failed)
      source = m_source;
  }
  if (!source)
    return AudioResult::UNAVAILABLE;
  const AudioResult result = source->SendPlayback(packet, data, size);
  if (result == AudioResult::FAILED)
    MarkFailed(source);
  return result;
}

void CAudioHub::LGAudioCaptureReady()
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IAudioSource * source = nullptr;
  {
    CSRWSharedLock lock(m_lock);
    if (!m_stopped && m_active && m_running && !m_failed)
      source = m_source;
  }
  if (source)
    source->CaptureReceiveReady();
}
