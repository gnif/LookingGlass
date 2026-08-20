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

#include "display/CMonitorManager.h"

#include "display/CMonitorContext.h"
#include "Atomic.h"
#include "CDebug.h"

bool CMonitorManager::Create(UINT connectorIndex, IDDCX_ADAPTER adapter,
  std::vector<BYTE> edid, CDeviceContext * owner)
{
  std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
  DEBUG_INFO("Creating monitor on connector %u", connectorIndex);

  // We support a single monitor; never create a second one if one already
  // exists (a replug must clear m_monitor via departure first).
  bool haveMonitor;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_enabled)
      return false;
    haveMonitor = m_monitor != WDF_NO_HANDLE;
  }
  if (haveMonitor)
  {
    DEBUG_WARN("FinishInit skipped: a monitor already exists");
    return false;
  }

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CMonitorContextWrapper);

  DEBUG_INFO("Using %llu-byte monitor EDID",
    (unsigned long long)edid.size());

  IDDCX_MONITOR_INFO info = {};
  info.Size           = sizeof(info);
  info.MonitorType    = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size     = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = (UINT)edid.size();
  info.MonitorDescription.pData    = edid.empty() ? nullptr : edid.data();

  HRESULT hr = CoCreateGuid(&info.MonitorContainerId);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the monitor container ID");
    return false;
  }

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut = {};
  NTSTATUS status = IddCxMonitorCreate(adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorCreate Failed");
    return false;
  }

  DEBUG_INFO("Monitor object created (%p)", createOut.MonitorObject);

  const IDDCX_MONITOR monitor = createOut.MonitorObject;

  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(monitor);
  wrapper->context = new CMonitorContext(monitor, owner);

  {
    CSRWExclusiveLock lock(m_lock);
    m_monitor         = monitor;
    m_monitorDeparted = false;
  }

  IDARG_OUT_MONITORARRIVAL out = {};
  status = IddCxMonitorArrival(monitor, &out);
  if (FAILED(status))
  {
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_monitor == monitor)
        m_monitor = nullptr;
    }
    WdfObjectDelete((WDFOBJECT)monitor);
    DEBUG_ERROR_HR(status, "IddCxMonitorArrival Failed");
    return false;
  }

  DEBUG_INFO("Monitor arrival reported successfully");
  return true;
}

bool CMonitorManager::Disable()
{
  std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
  IDDCX_MONITOR monitor                    = nullptr;
  bool          oldReplugMonitor           = false;
  bool          oldReplugPending           = false;
  bool          oldMonitorDeparted         = false;
  bool          oldWaitForSwapChainRelease = false;
  LONG          oldCreateQueued            = 0;
  LONG          oldReplugQueued            = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_enabled)
      return true;

    oldReplugMonitor           = m_replugMonitor;
    oldReplugPending           = m_replugPending;
    oldMonitorDeparted         = m_monitorDeparted;
    oldWaitForSwapChainRelease = m_waitForSwapChainRelease;
    oldCreateQueued = Atomic::Load(m_createQueued);
    oldReplugQueued = Atomic::Load(m_replugQueued);

    m_enabled       = false;
    m_replugMonitor = false;
    m_replugPending = false;
    Atomic::Store(m_createQueued, 0);
    Atomic::Store(m_replugQueued, 0);

    monitor = m_monitor;
    if (monitor == WDF_NO_HANDLE)
      return true;

    m_monitor                 = nullptr;
    m_monitorDeparted         = false;
    m_waitForSwapChainRelease = m_swapChainAssigned;
  }

  DEBUG_INFO("Disabling the virtual monitor for recovery");
  const NTSTATUS status = IddCxMonitorDeparture(monitor);
  if (!NT_SUCCESS(status))
  {
    CSRWExclusiveLock lock(m_lock);
    m_enabled                 = true;
    m_monitor                 = monitor;
    m_replugMonitor           = oldReplugMonitor;
    m_replugPending           = oldReplugPending;
    m_monitorDeparted         = oldMonitorDeparted;
    m_waitForSwapChainRelease = oldWaitForSwapChainRelease;
    Atomic::Store(m_createQueued, oldCreateQueued);
    Atomic::Store(m_replugQueued, oldReplugQueued);
    DEBUG_ERROR_HR(status,
      "Failed to disable the virtual monitor for recovery");
    return false;
  }

  {
    CSRWExclusiveLock lock(m_lock);
    m_monitorDeparted = true;
  }
  DEBUG_INFO("Virtual monitor disabled for recovery");
  return true;
}

void CMonitorManager::Enable()
{
  std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
  bool create = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_enabled)
      return;

    m_enabled = true;
    create = m_monitor == WDF_NO_HANDLE &&
      !m_waitForSwapChainRelease;
  }

  if (create)
    Atomic::Store(m_createQueued, 1);
}

CMonitorManager::ReplugAction CMonitorManager::Replug()
{
  std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
  IDDCX_MONITOR monitor;
  {
    CSRWExclusiveLock lock(m_lock);

    if (!m_enabled)
      return ReplugAction::NONE;

    if (m_waitForSwapChainRelease)
      return ReplugAction::NONE;

    if (m_replugMonitor || (m_swapChainAssigned && !m_swapChainReady))
    {
      // Coalesce changes received while a swap chain is being initialized,
      // the old one is draining, or its replacement is being initialized.
      m_replugPending = true;
      return ReplugAction::NONE;
    }

    monitor = m_monitor;
    if (monitor == WDF_NO_HANDLE)
    {
      m_replugMonitor   = true;
      m_monitorDeparted = true;
    }
    else
    {
      // Clear the handle before departing so nothing calls an IddCx monitor
      // API on a departing/destroyed handle. Create publishes the new one.
      m_replugMonitor           = true;
      m_monitorDeparted         = false;
      m_waitForSwapChainRelease = m_swapChainAssigned;
      m_monitor                 = nullptr;
    }
  }

  if (monitor == WDF_NO_HANDLE)
  {
    // Either no monitor yet, or one is already pending; build it now and
    // cancel any queued rebuild so we do not create two.
    Atomic::Store(m_createQueued, 0);
    return ReplugAction::CREATE;
  }

  DEBUG_TRACE("ReplugMonitor");
  NTSTATUS status = IddCxMonitorDeparture(monitor);
  if (!NT_SUCCESS(status))
  {
    {
      CSRWExclusiveLock lock(m_lock);
      m_replugMonitor           = false;
      m_replugPending           = false;
      m_monitorDeparted         = false;
      m_waitForSwapChainRelease = false;
      m_monitor                 = monitor;
    }
    DEBUG_ERROR("IddCxMonitorDeparture Failed (0x%08x)", status);
    return ReplugAction::NONE;
  }

  bool rebuild;
  {
    CSRWExclusiveLock departedLock(m_lock);
    m_monitorDeparted = true;
    rebuild = !m_waitForSwapChainRelease;
  }

  // If there was no swap chain there will be no unassign callback to queue
  // the rebuild. Otherwise OnSwapChainReleased does so after teardown drains.
  if (rebuild)
    Atomic::Store(m_createQueued, 1);

  return ReplugAction::NONE;
}

void CMonitorManager::RequestMode(const CSettings::DisplayMode& mode)
{
  CSRWExclusiveLock lock(m_lock);
  m_setMode   = mode;
  m_doSetMode = true;
}

void CMonitorManager::OnDestroyed(IDDCX_MONITOR monitor)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_monitor == monitor)
    m_monitor = nullptr;
}

void CMonitorManager::OnSwapChainAssigned()
{
  CSRWExclusiveLock lock(m_lock);
  m_swapChainAssigned = true;
  m_swapChainReady    = false;
}

void CMonitorManager::OnSwapChainReleased()
{
  bool rebuild = false;

  {
    CSRWExclusiveLock lock(m_lock);
    m_swapChainAssigned = false;
    m_swapChainReady    = false;
    if (m_waitForSwapChainRelease)
    {
      m_waitForSwapChainRelease = false;
      rebuild = m_enabled && m_monitorDeparted &&
        m_monitor == WDF_NO_HANDLE;
    }
  }

  if (rebuild)
    Atomic::Store(m_createQueued, 1);
}

CMonitorManager::ReadyAction CMonitorManager::OnSwapChainReady()
{
  ReadyAction action = {};
  bool replug = false;

  {
    CSRWExclusiveLock lock(m_lock);
    m_swapChainReady = true;
    if (!m_enabled || m_waitForSwapChainRelease)
      return action;

    if (m_replugMonitor)
    {
      m_replugMonitor   = false;
      m_monitorDeparted = false;
      if (m_replugPending)
      {
        m_replugPending = false;
        replug = true;
      }
    }
    else if (m_replugPending)
    {
      m_replugPending = false;
      replug = true;
    }

    // Do not consume the requested mode on an intermediate replacement swap
    // chain. The last coalesced replug must be the one that applies it.
    if (!replug && m_doSetMode)
    {
      action.mode    = m_setMode;
      m_doSetMode    = false;
      action.setMode = true;
    }
  }

  action.replug = replug;

  return action;
}

void CMonitorManager::QueueReplug()
{
  CSRWSharedLock lock(m_lock);
  if (!m_enabled)
    return;
  Atomic::Store(m_replugQueued, 1);
}

CMonitorManager::DeferredAction CMonitorManager::TakeDeferredAction()
{
  if (Atomic::Swap(m_createQueued, 0))
  {
    CSRWSharedLock lock(m_lock);
    if (m_enabled)
      return DeferredAction::CREATE;
  }

  if (Atomic::Swap(m_replugQueued, 0))
  {
    CSRWSharedLock lock(m_lock);
    if (m_enabled)
      return DeferredAction::REPLUG;
  }

  return DeferredAction::NONE;
}
