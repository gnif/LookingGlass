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

#include "Atomic.h"
#include "transport/ITransport.h"
#include "transport/lgmp/CIVSHMEM.h"
#include "transport/lgmp/CLGMPControl.h"
#include "transport/lgmp/CLGMPFrameTransport.h"
#include "transport/lgmp/CLGMPHost.h"
#include "transport/lgmp/CLGMPInputTransport.h"
#include "transport/lgmp/CRecovery.h"

class CLGMPTransport final : public ITransport
{
private:
  TransportInstance    m_config;
  // Keep this declaration order. Destruction must release frame and control
  // allocations before the LGMP host and its IVSHMEM mapping are destroyed.
  CIVSHMEM            m_ivshmem;
  CLGMPHost           m_host;
  CLGMPControl        m_control;
  CLGMPFrameTransport m_frames;
  CLGMPInputTransport m_input;
  CRecovery           m_recovery;
  std::atomic<bool>   m_ready = false;
  FrameCfg            m_activeCfg;
  FrameCfg            m_pendingCfg;
  bool                m_hasActive  = false;
  bool                m_hasPending = false;

public:
  explicit CLGMPTransport(const TransportInstance& config);
  ~CLGMPTransport() override = default;

  CLGMPTransport(const CLGMPTransport&) = delete;
  CLGMPTransport& operator=(const CLGMPTransport&) = delete;

  OpenResult Open() override;
  bool Initialize() override;
  bool Setup(size_t alignment) override;
  ProcessResult Process(ITransportEvents& events) override;
  void Stop() override;
  void SyncRecovery() override;
  void RecoveryStatus(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active,
    Recovery state, uint32_t error) override;

  const FrameProfile * Profiles(unsigned& count) const override;
  CfgResult Probe(const FrameCfg& cfg) const override;
  CfgResult Prepare(const FrameCfg& cfg) override;
  void Commit() override;
  void Abort() override;

  std::shared_ptr<const FrameCaps> GetFrameCaps() const override;
  DirectFrameBufferMemory GetDirectMemory() const override;

  IFrameSink * FrameSink() override { return &m_frames; }
  IControlSink * Control() override { return &m_control; }
  IInputSource * Input() override { return &m_input; }
};
