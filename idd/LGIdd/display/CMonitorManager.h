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

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>
#include <atomic>
#include <vector>

#include "config/CSettings.h"

class CDeviceContext;

class CMonitorManager
{
public:
  enum class ReplugAction
  {
    NONE,
    CREATE,
  };

  enum class DeferredAction
  {
    NONE,
    CREATE,
    REPLUG,
  };

  struct ReadyAction
  {
    CSettings::DisplayMode mode    = {};
    bool                   setMode = false;
    bool                   replug  = false;
  };

private:
  IDDCX_MONITOR m_monitor = nullptr;

  // Guards the monitor/replug/swap-chain state. These values are touched by
  // IddCx callback threads, the swap-chain thread, and the transport timer.
  SRWLOCK m_lock = SRWLOCK_INIT;

  bool m_replugMonitor           = false;
  bool m_replugPending           = false;
  bool m_monitorDeparted         = false;
  bool m_swapChainAssigned       = false;
  bool m_swapChainReady          = false;
  bool m_waitForSwapChainRelease = false;

  CSettings::DisplayMode m_setMode  = {};
  bool                   m_doSetMode = false;

  std::atomic<LONG> m_createQueued = 0;
  std::atomic<LONG> m_replugQueued = 0;

public:
  void Create(UINT connectorIndex, IDDCX_ADAPTER adapter,
    std::vector<BYTE> edid, CDeviceContext * owner);
  ReplugAction Replug();
  void RequestMode(const CSettings::DisplayMode& mode);

  void OnDestroyed(IDDCX_MONITOR monitor);
  void OnSwapChainAssigned();
  void OnSwapChainReleased();
  ReadyAction OnSwapChainReady();
  void QueueReplug();

  DeferredAction TakeDeferredAction();
};
