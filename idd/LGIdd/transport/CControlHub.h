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
#include "transport/IControlTransport.h"
#include "transport/ITransport.h"

#include <memory>
#include <vector>

class CControlHub final : public IControlTransport
{
private:
  struct Sink
  {
    BackendId          backend;
    uint32_t           epoch;
    CSRWLock           lock;
    IControlTransport * control;
  };

  mutable CSRWLock                   m_sinkLock;
  std::vector<std::shared_ptr<Sink>> m_sinks;

  mutable CSRWLock m_stateLock;
  std::shared_ptr<const D12ColorTransform> m_colorTransform;
  IDARG_OUT_QUERY_HWCURSOR m_cursor = {};
  std::vector<BYTE>        m_cursorData;
  UINT                     m_sdrWhiteLevel = 0;
  bool                     m_cursorValid   = false;
  bool                     m_shapeValid    = false;

  std::vector<std::shared_ptr<Sink>> Snapshot() const;
  void Replay(const std::shared_ptr<Sink>& sink);

public:
  bool Add(BackendId backend, uint32_t epoch, IControlTransport& control);
  void Remove(BackendId backend, uint32_t epoch);

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
    const BYTE * data, UINT sdrWhiteLevel) override;
  void SetColorTransform(
    std::shared_ptr<const D12ColorTransform> transform) override;
  std::shared_ptr<const D12ColorTransform>
    GetColorTransform() const override;
};
