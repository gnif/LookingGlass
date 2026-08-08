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

#include "transport/ITransport.h"
#include "transport/lgmp/CIVSHMEM.h"
#include "transport/lgmp/CLGMPControl.h"
#include "transport/lgmp/CLGMPFrameTransport.h"
#include "transport/lgmp/CLGMPHost.h"
#include "transport/lgmp/CLGMPInputTransport.h"

class CLGMPTransport final : public ITransport
{
private:
  // Keep this declaration order. Destruction must release frame and control
  // allocations before the LGMP host and its IVSHMEM mapping are destroyed.
  CIVSHMEM            m_ivshmem;
  CLGMPHost           m_host;
  CLGMPControl        m_control;
  CLGMPFrameTransport m_frames;
  CLGMPInputTransport m_input;

public:
  CLGMPTransport();
  ~CLGMPTransport() override = default;

  CLGMPTransport(const CLGMPTransport&) = delete;
  CLGMPTransport& operator=(const CLGMPTransport&) = delete;

  OpenResult Open() override;
  bool Initialize() override;
  bool Setup(size_t alignment) override;
  void Process(ITransportEvents& events) override;

  FrameMemoryLimits GetMemoryLimits() const override;
  DirectFrameBufferMemory GetDirectMemory() const override;

  IFrameTransport& Frames() override { return m_frames; }
  IControlTransport& Control() override { return m_control; }
  IInputTransport * Input() override { return &m_input; }
};
