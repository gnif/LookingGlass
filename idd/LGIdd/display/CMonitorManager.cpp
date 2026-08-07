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

#include "display/monitor/Context.h"
#include "CDebug.h"

void CMonitorManager::Create(UINT connectorIndex, IDDCX_ADAPTER adapter,
  std::vector<BYTE> edid, CDeviceContext * owner)
{
  DEBUG_INFO("Creating monitor on connector %u", connectorIndex);

  // We support a single monitor; never create a second one if one already
  // exists (a replug must clear m_monitor via departure first).
  AcquireSRWLockExclusive(&m_lock);
  const bool haveMonitor = m_monitor != WDF_NO_HANDLE;
  ReleaseSRWLockExclusive(&m_lock);
  if (haveMonitor)
  {
    DEBUG_WARN("FinishInit skipped: a monitor already exists");
    return;
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
    return;
  }

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut = {};
  NTSTATUS status = IddCxMonitorCreate(adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorCreate Failed");
    return;
  }

  DEBUG_INFO("Monitor object created (%p)", createOut.MonitorObject);

  AcquireSRWLockExclusive(&m_lock);
  m_monitor = createOut.MonitorObject;
  ReleaseSRWLockExclusive(&m_lock);

  auto * wrapper = WdfObjectGet_CMonitorContextWrapper(m_monitor);
  wrapper->context = new CMonitorContext(m_monitor, owner);

  IDARG_OUT_MONITORARRIVAL out = {};
  status = IddCxMonitorArrival(m_monitor, &out);
  if (FAILED(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorArrival Failed");
    return;
  }

  DEBUG_INFO("Monitor arrival reported successfully");
}

CMonitorManager::ReplugAction CMonitorManager::Replug()
{
  AcquireSRWLockExclusive(&m_lock);

  if (m_replugMonitor || (m_swapChainAssigned && !m_swapChainReady))
  {
    // Coalesce changes received while a swap chain is being initialized, the
    // old one is draining, or its replacement is being initialized.
    m_replugPending = true;
    ReleaseSRWLockExclusive(&m_lock);
    return ReplugAction::NONE;
  }

  IDDCX_MONITOR monitor = m_monitor;
  if (monitor == WDF_NO_HANDLE)
  {
    m_replugMonitor   = true;
    m_monitorDeparted = true;
    ReleaseSRWLockExclusive(&m_lock);
    // Either no monitor yet, or one is already pending; build it now and
    // cancel any queued rebuild so we do not create two.
    m_createQueued.store(0);
    return ReplugAction::CREATE;
  }

  // Clear the handle before departing so nothing calls an IddCx monitor API
  // on a departing/destroyed handle. Create publishes the new one.
  m_replugMonitor           = true;
  m_monitorDeparted         = false;
  m_waitForSwapChainRelease = m_swapChainAssigned;
  m_monitor                 = nullptr;
  ReleaseSRWLockExclusive(&m_lock);

  DEBUG_TRACE("ReplugMonitor");
  NTSTATUS status = IddCxMonitorDeparture(monitor);
  if (!NT_SUCCESS(status))
  {
    AcquireSRWLockExclusive(&m_lock);
    m_replugMonitor           = false;
    m_replugPending           = false;
    m_monitorDeparted         = false;
    m_waitForSwapChainRelease = false;
    m_monitor                 = monitor;
    ReleaseSRWLockExclusive(&m_lock);
    DEBUG_ERROR("IddCxMonitorDeparture Failed (0x%08x)", status);
    return ReplugAction::NONE;
  }

  AcquireSRWLockExclusive(&m_lock);
  m_monitorDeparted = true;
  const bool rebuild = !m_waitForSwapChainRelease;
  ReleaseSRWLockExclusive(&m_lock);

  // If there was no swap chain there will be no unassign callback to queue
  // the rebuild. Otherwise OnSwapChainReleased does so after teardown drains.
  if (rebuild)
    m_createQueued.store(1);

  return ReplugAction::NONE;
}

void CMonitorManager::RequestMode(const CSettings::DisplayMode& mode)
{
  AcquireSRWLockExclusive(&m_lock);
  m_setMode   = mode;
  m_doSetMode = true;
  ReleaseSRWLockExclusive(&m_lock);
}

void CMonitorManager::OnDestroyed(IDDCX_MONITOR monitor)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_monitor == monitor)
    m_monitor = nullptr;
  ReleaseSRWLockExclusive(&m_lock);
}

void CMonitorManager::OnSwapChainAssigned()
{
  AcquireSRWLockExclusive(&m_lock);
  m_swapChainAssigned = true;
  m_swapChainReady    = false;
  ReleaseSRWLockExclusive(&m_lock);
}

void CMonitorManager::OnSwapChainReleased()
{
  bool rebuild = false;

  AcquireSRWLockExclusive(&m_lock);
  m_swapChainAssigned = false;
  m_swapChainReady    = false;
  if (m_replugMonitor && m_waitForSwapChainRelease)
  {
    m_waitForSwapChainRelease = false;
    rebuild = m_monitorDeparted;
  }
  ReleaseSRWLockExclusive(&m_lock);

  if (rebuild)
    m_createQueued.store(1);
}

CMonitorManager::ReadyAction CMonitorManager::OnSwapChainReady()
{
  ReadyAction action = {};
  bool replug = false;

  AcquireSRWLockExclusive(&m_lock);
  m_swapChainReady = true;
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
  ReleaseSRWLockExclusive(&m_lock);

  action.replug = replug;

  return action;
}

void CMonitorManager::QueueReplug()
{
  m_replugQueued.store(1);
}

CMonitorManager::DeferredAction CMonitorManager::TakeDeferredAction()
{
  if (m_createQueued.exchange(0))
    return DeferredAction::CREATE;

  if (m_replugQueued.exchange(0))
    return DeferredAction::REPLUG;

  return DeferredAction::NONE;
}
