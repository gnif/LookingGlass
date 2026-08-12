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
#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "display/CDisplayConfiguration.h"
#include "display/CMonitorManager.h"
#include "transport/CTransportManager.h"

class CDeviceContext : private ITransportEvents
{
private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter                    = nullptr;
  LUID          m_preferredRenderAdapter     = {};
  bool          m_havePreferredRenderAdapter = false;

  // At boot the selected transport may not be available yet. The retry timer
  // and atomic gate keep adapter creation single-threaded until it is ready.
  WDFTIMER          m_initTimer       = nullptr;
  bool              m_transportOpened = false;
  std::atomic<LONG> m_initInProgress  = 0;

  std::unique_ptr<CTransportManager> m_transport;
  CDisplayConfiguration       m_displayConfiguration;
  CMonitorManager             m_monitorManager;

  WDFTIMER m_transportTimer     = nullptr;
  bool     m_recoveryHandlerSet = false;
  CSRWLock m_recoveryPublishLock;
  CSRWLock m_recoveryRouteLock;
  uint64_t m_nextRecoveryRoute  = 1;
  uint64_t m_recoveryRoute      = 0;
  SourceKey m_recoverySource;
  uint64_t  m_recoverySession   = 0;
  uint32_t  m_recoverySerial    = 0;
  bool      m_recoveryActive    = false;

  UINT m_iddCxVersion    = 0;
  bool m_hasIddCx110DDIs = false;
  bool m_canProcessFP16  = false;
  bool m_softwareMode    = true;

  void QueryIddCxCapabilities();

  void ScheduleInitRetry();
  void StopInitRetry();

  bool InitializeTransport();
  void TransportTimer();
  void OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) override;
  void OnSetResolution(const SourceKey& source,
    uint32_t width, uint32_t height) override;
  void OnRecoveryRequest(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active) override;
  void SetResolution(uint32_t width, uint32_t height);

public:
  explicit CDeviceContext(_In_ WDFDEVICE wdfDevice);
  ~CDeviceContext();

  CDeviceContext(const CDeviceContext&) = delete;
  CDeviceContext& operator=(const CDeviceContext&) = delete;

  bool SetupTransport(size_t alignSize);

  void InitAdapter();
  void FinishAdapterInit(UINT connectorIndex);
  void FinishInit(UINT connectorIndex);
  void ReloadSettings();
  void ReplugMonitor();

  void OnMonitorDestroyed(IDDCX_MONITOR monitor);
  void OnSwapChainAssigned();
  void OnSwapChainReleased();
  void OnSwapChainReady();

  bool HasIddCx110DDIs() const { return m_hasIddCx110DDIs; }
  bool CanProcessFP16 () const { return m_canProcessFP16;  }
  bool IsSoftwareMode () const { return m_softwareMode;    }

  CTransportManager& GetTransport()
  {
    return *m_transport;
  }

  CDisplayConfiguration& GetDisplayConfiguration()
  {
    return m_displayConfiguration;
  }
};

struct CDeviceContextWrapper
{
  CDeviceContext * context;

  void Cleanup()
  {
    delete context;
    context = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE(CDeviceContextWrapper);
