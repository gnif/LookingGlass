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

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <deque>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "display/CDisplayConfiguration.h"
#include "display/CMonitorManager.h"
#include "transport/CTransportManager.h"

class CDeviceContext : private ITransportActions
{
private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter                    = nullptr;
  LUID          m_preferredRenderAdapter     = {};
  bool          m_havePreferredRenderAdapter = false;

  // At boot the selected transport may not be available yet. The retry timer
  // and atomic gate keep adapter creation single-threaded until it is ready.
  WDFTIMER          m_initTimer      = nullptr;
  std::atomic<LONG> m_initInProgress = 0;

  struct DisplayRect
  {
    int32_t  x      = 0;
    int32_t  y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
  };

  CSRWLock    m_displayRectLock;
  DisplayRect m_desktopRect;

  struct Head
  {
    std::unique_ptr<CTransportManager> transport;
    CDisplayConfiguration              displayConfiguration;
    CMonitorManager                    monitorManager;
    const UINT                         index;
    bool                               transportOpened = false;
    DisplayRect                        displayRect;

    Head(UINT connectorIndex, std::unique_ptr<CTransportManager> manager,
      CSettings& settings);
    Head(const Head&) = delete;
    Head& operator=(const Head&) = delete;
  };

  std::vector<std::unique_ptr<Head>> m_heads;

  WDFTIMER m_transportTimer     = nullptr;
  bool     m_recoveryHandlerSet = false;

  std::mutex                 m_recoveryTransitionMutex;
  CSRWLock                   m_localRecoveryLock;
  std::deque<RecoveryAction> m_localRecoveryQueue;
  RecoveryAction             m_localRecoveryArrivalAction = {};
  RecoveryAction             m_latestRecoveryAction       = {};
  RecoveryAction             m_helperRecoveryAction       = {};
  bool                       m_localRecoveryArrival        = false;
  bool                       m_latestRecoveryValid         = false;
  bool                       m_helperRecoveryComplete      = false;
  bool                       m_recoveryActive              = false;
  bool                       m_recoveryMonitorDisabled     = false;
  bool                       m_adapterReady                = false;

  UINT m_iddCxVersion    = 0;
  bool m_hasIddCx110DDIs = false;
  bool m_canProcessFP16  = false;
  bool m_softwareMode    = true;

  Head& PrimaryHead() { return *m_heads[0]; }

  Head& HeadAt(UINT head)
  {
    if (head >= m_heads.size())
      head = 0;
    return *m_heads[head];
  }

  UINT HeadForBackend(BackendId backend) const;
  CSettings::DisplayModes MonitorModes(bool * hdrEnabled) const;

  void QueryIddCxCapabilities();

  void ScheduleInitRetry();
  void StopInitRetry();

  bool InitializeTransport();
  void TransportTimer();
  static bool SameRecoveryAction(
    const RecoveryAction& left, const RecoveryAction& right);
  bool QueueLocalRecovery(const RecoveryAction& action);
  void RemoveLocalRecovery(const RecoveryAction& action);
  void ProcessLocalRecovery();
  void CompleteRecoveryArrival(bool arrived);
  InteractionResult OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) override;
  InteractionResult OnSetResolution(const SourceKey& source,
    uint32_t width, uint32_t height) override;
  bool OnRecoveryAction(const RecoveryAction& action) override;
  InteractionResult SetResolution(
    UINT head, uint32_t width, uint32_t height);

public:
  explicit CDeviceContext(_In_ WDFDEVICE wdfDevice);
  ~CDeviceContext();

  CDeviceContext(const CDeviceContext&) = delete;
  CDeviceContext& operator=(const CDeviceContext&) = delete;

  bool SetupTransport(UINT head, size_t alignSize);

  void InitAdapter();
  void FinishAdapterInit(UINT connectorIndex);
  void FinishInit(UINT connectorIndex);
  void ReloadSettings();
  void ReplugMonitor(UINT head);
  void SetDisplayRect(uint32_t connector, int32_t x, int32_t y,
    uint32_t width, uint32_t height);

  void OnMonitorDestroyed(UINT head, IDDCX_MONITOR monitor);
  void OnSwapChainAssigned(UINT head);
  void OnSwapChainReleased(UINT head);
  void OnSwapChainReady(UINT head);

  NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION * inArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION * outArgs) const;
#ifdef HAS_IDDCX_110
  NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2 * inArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION * outArgs) const;
#endif

  bool HasIddCx110DDIs() const { return m_hasIddCx110DDIs; }
  bool CanProcessFP16 () const { return m_canProcessFP16;  }
  bool IsSoftwareMode () const { return m_softwareMode;    }

  CTransportManager& GetTransport()
  {
    return *PrimaryHead().transport;
  }

  CTransportManager& GetTransport(UINT head)
  {
    if (head >= m_heads.size())
      head = 0;
    return *m_heads[head]->transport;
  }

  CDisplayConfiguration& GetDisplayConfiguration(UINT head)
  {
    if (head >= m_heads.size())
      head = 0;
    return m_heads[head]->displayConfiguration;
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
