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

#include "transport/lgmp/CLGMPTransport.h"

#include "Atomic.h"
#include "CDebug.h"
#include "common/KVMFR.h"
#include "common/KVMFRRecovery.h"

static bool TranslateFrameScheduleFlags(
  uint32_t source, uint32_t& destination)
{
  static const uint32_t validFlags =
    KVMFR_FRAME_SCHEDULE_ACTIVE    |
    KVMFR_FRAME_SCHEDULE_RELEASE   |
    KVMFR_FRAME_SCHEDULE_RESET     |
    KVMFR_FRAME_SCHEDULE_IMMEDIATE;

  if (source & ~validFlags)
    return false;

  destination = 0;
  if (source & KVMFR_FRAME_SCHEDULE_ACTIVE)
    destination |= FRAME_SCHEDULE_ACTIVE;
  if (source & KVMFR_FRAME_SCHEDULE_RELEASE)
    destination |= FRAME_SCHEDULE_RELEASE;
  if (source & KVMFR_FRAME_SCHEDULE_RESET)
    destination |= FRAME_SCHEDULE_RESET;
  if (source & KVMFR_FRAME_SCHEDULE_IMMEDIATE)
    destination |= FRAME_SCHEDULE_IMMEDIATE;
  return true;
}

static const FrameProfile FRAME_PROFILES[] =
{
  {
    FrameStorage::D3D12_TEXTURE,
    FramePixel::BGRA8,
    FrameSignal::SRGB,
  },
  {
    FrameStorage::D3D12_TEXTURE,
    FramePixel::RGB10A2,
    FrameSignal::PQ_BT2020,
  },
};

CLGMPTransport::CLGMPTransport(const TransportInstance& config) :
  m_config(config),
  m_control(m_host),
  m_frames(m_host, m_ivshmem),
  m_input(m_host),
  m_clipboard(m_host)
{
}

ITransport::OpenResult CLGMPTransport::Open()
{
  if (m_ivshmem.GetMem())
    return OpenResult::SUCCESS;

  if (!m_ivshmem.Init() || !m_ivshmem.Open())
    return OpenResult::RETRY;

  return OpenResult::SUCCESS;
}

bool CLGMPTransport::Initialize()
{
  if (!m_recovery.Initialize(m_ivshmem))
    return false;

  if (!m_host.Initialize(m_ivshmem))
    return false;

  // Preserve the existing shared-memory layout by appending input and then
  // clipboard after the frame/pointer queues and retained pointer state.
  if (!m_frames.Initialize() || !m_control.Initialize() ||
      !m_input.Initialize() || !m_clipboard.Initialize())
    return false;

  m_frames.SealMemoryLayout();
  return true;
}

bool CLGMPTransport::Setup(size_t alignment)
{
  if (!m_frames.Setup(alignment))
    return false;

  Atomic::Store(m_ready, true, std::memory_order_release);
  return true;
}

ITransport::ProcessResult CLGMPTransport::Process(ITransportEvents& events)
{
  const CRecovery::Request recovery = m_recovery.Process();
  if (recovery.valid)
  {
    const RecoveryAdmission admission = events.OnRecoveryRequest(
      SourceKey(), recovery.session, recovery.serial, recovery.active);
    if (admission.complete)
      RecoveryStatus(SourceKey(), recovery.session, recovery.serial,
        recovery.active, admission.state, admission.error);
  }

  // Before the swap chain establishes the frame-buffer alignment, service
  // only the protocol-independent recovery channel. This preserves the old
  // transport startup boundary while keeping recovery available immediately.
  if (!Atomic::Load(m_ready, std::memory_order_acquire))
    return ProcessResult::OK;

  const LGMP_STATUS processStatus = m_host.Process();
  if (processStatus != LGMP_OK)
  {
    if (processStatus == LGMP_ERR_CORRUPTED)
    {
      DEBUG_WARN(
        "LGMP reported the shared memory has been corrupted, attempting to recover\n");
      return ProcessResult::RETRY;
    }

    DEBUG_ERROR("lgmpHostProcess Failed: %s",
      lgmpStatusString(processStatus));
    return ProcessResult::FAILURE;
  }

  m_input.FlushStatus();

  const uint64_t now = CFrameScheduler::Nanotime();

  // Take the frame subscriber snapshot before processing scheduling messages,
  // then publish both updates together just as the original timer did.
  const CLGMPFrameTransport::SubscriberSnapshot subscribers =
    m_frames.SnapshotSubscribers();

  uint8_t  data[LGMP_MSGS_SIZE];
  size_t   size;
  uint32_t sourceClientID;
  LGMP_STATUS status;
  while ((status = m_control.ReadDataWithSource(
      data, &size, &sourceClientID)) == LGMP_OK)
  {
    if (size < sizeof(KVMFRMessage))
    {
      DEBUG_WARN("Ignoring invalid KVMFR message");
      m_control.AckData();
      continue;
    }

    KVMFRMessage * msg = reinterpret_cast<KVMFRMessage *>(data);
    switch (msg->type)
    {
      case KVMFR_MESSAGE_SETCURSORPOS:
      {
        if (size == sizeof(KVMFRSetCursorPos))
        {
          SourceKey source;
          source.client = sourceClientID;
          const KVMFRSetCursorPos * position =
            reinterpret_cast<const KVMFRSetCursorPos *>(msg);
          events.OnSetCursorPos(source, position->x, position->y);
        }
        else
          DEBUG_WARN("Ignoring invalid KVMFR cursor position");
        break;
      }

      case KVMFR_MESSAGE_WINDOWSIZE:
      {
        if (size == sizeof(KVMFRWindowSize))
        {
          SourceKey source;
          source.client = sourceClientID;
          const KVMFRWindowSize * window =
            reinterpret_cast<const KVMFRWindowSize *>(msg);
          events.OnSetResolution(source, window->w, window->h);
        }
        else
          DEBUG_WARN("Ignoring invalid KVMFR window size");
        break;
      }

      case KVMFR_MESSAGE_FRAME_SCHEDULE:
      {
        const KVMFRFrameSchedule * schedule =
          reinterpret_cast<KVMFRFrameSchedule *>(msg);
        uint32_t translatedFlags = 0;
        bool valid = size == sizeof(*schedule) &&
          TranslateFrameScheduleFlags(schedule->flags, translatedFlags);
        if (valid)
        {
          FrameScheduleUpdate update = {};
          update.clientID               = schedule->clientID;
          update.generation             = schedule->generation;
          update.flags                  = translatedFlags;
          update.period                 = schedule->period;
          update.targetSlack            = schedule->targetSlack;
          update.phaseError             = schedule->phaseError;
          update.feedbackFrameSerial    = schedule->feedbackFrameSerial;
          update.feedbackScheduleEpoch  = schedule->feedbackScheduleEpoch;
          update.feedbackDeadlineSerial = schedule->feedbackDeadlineSerial;
          update.lease                  = schedule->lease;
          valid = m_frames.UpdateSchedule(sourceClientID, update, now);
        }
        if (!valid)
          DEBUG_WARN("Ignoring invalid KVMFR frame schedule");
        break;
      }
    }

    m_control.AckData();
  }

  m_frames.FinalizeSubscribers(subscribers, now);

  if (m_control.HasNewSubscribers())
    m_control.RequestReplay();

  return ProcessResult::OK;
}

void CLGMPTransport::Stop()
{
  Atomic::Store(m_ready, false, std::memory_order_release);
  Abort();
  m_hasActive = false;
  m_clipboard.Stop();
  m_input.Stop();
}

void CLGMPTransport::SyncRecovery()
{
  m_recovery.Sync();
}

void CLGMPTransport::RecoveryStatus(const SourceKey& source,
  uint64_t session, uint32_t serial, bool active,
  Recovery state, uint32_t error)
{
  UNREFERENCED_PARAMETER(source);
  uint32_t wireState = KVMFR_R_STATE_FAILED;
  uint32_t wireError = KVMFR_R_ERR_NONE;
  switch (state)
  {
    case Recovery::NORMAL:
      wireState = KVMFR_R_STATE_NORMAL;
      break;

    case Recovery::ACTIVE:
      wireState = KVMFR_R_STATE_ACTIVE;
      break;

    case Recovery::FAILED:
      wireState = KVMFR_R_STATE_FAILED;
      switch (error)
      {
        case ERROR_NOT_FOUND:
          wireError = KVMFR_R_ERR_NO_FALLBACK_DISPLAY;
          break;

        case ERROR_BUSY:
          wireError = KVMFR_R_ERR_BUSY;
          break;

        case ERROR_NOT_ENOUGH_QUOTA:
          wireError = KVMFR_R_ERR_CAPACITY;
          break;

        case ERROR_TIMEOUT:
        case RPC_S_SERVER_UNAVAILABLE:
          wireError = KVMFR_R_ERR_HELPER_UNAVAILABLE;
          break;

        default:
          wireError = KVMFR_R_ERR_TOPOLOGY_FAILED;
          break;
      }
      break;
  }

  m_recovery.SetStatus(
    session, serial, active, wireState, wireError);
}

const FrameProfile * CLGMPTransport::Profiles(unsigned& count) const
{
  count = ARRAYSIZE(FRAME_PROFILES);
  return FRAME_PROFILES;
}

CfgResult CLGMPTransport::Probe(const FrameCfg& cfg) const
{
  if (!cfg.width || !cfg.height)
    return CfgResult::REJECTED;

  switch (cfg.mode)
  {
    case GpuMode::HARDWARE:
      for (const FrameProfile& profile : FRAME_PROFILES)
        if (Frame::Same(cfg.profile, profile))
          return CfgResult::ACCEPTED;
      break;

    case GpuMode::SOFTWARE:
      if (Frame::Same(cfg.profile, FRAME_PROFILES[0]))
        return CfgResult::ACCEPTED;
      break;

    default:
      return CfgResult::REJECTED;
  }
  return CfgResult::NEXT;
}

CfgResult CLGMPTransport::Prepare(const FrameCfg& cfg)
{
  m_hasPending = false;
  const CfgResult result = Probe(cfg);
  if (result != CfgResult::ACCEPTED)
    return result;

  m_pendingCfg = cfg;
  m_hasPending = true;
  return CfgResult::ACCEPTED;
}

void CLGMPTransport::Commit()
{
  if (!m_hasPending)
    return;
  m_activeCfg  = m_pendingCfg;
  m_hasActive  = true;
  m_hasPending = false;
}

void CLGMPTransport::Abort()
{
  m_hasPending = false;
}

std::shared_ptr<const FrameCaps> CLGMPTransport::GetFrameCaps() const
{
  return m_frames.GetFrameCaps();
}

DirectFrameBufferMemory CLGMPTransport::GetDirectMemory() const
{
  return {m_ivshmem.GetMem(), m_ivshmem.GetFullSize()};
}
