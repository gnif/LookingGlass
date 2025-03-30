/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

#include "CPlatformInfo.h"
#include "CPipeServer.h"
#include "CDebug.h"
#include "VersionInfo.h"

#include <sstream>

static const struct LGMPQueueConfig FRAME_QUEUE_CONFIG =
{
  LGMP_Q_FRAME,       //queueID
  LGMP_Q_FRAME_LEN,   //numMessages
  1000                //subTimeout
};

static const struct LGMPQueueConfig POINTER_QUEUE_CONFIG =
{
  LGMP_Q_POINTER,     //queueID
  LGMP_Q_POINTER_LEN, //numMesages
  1000                //subTimeout
};

const DWORD DefaultDisplayModes[][3] =
{
  {7680, 4800, 120}, {7680, 4320, 120}, {6016, 3384, 120}, {5760, 3600, 120},
  {5760, 3240, 120}, {5120, 2800, 120}, {4096, 2560, 120}, {4096, 2304, 120},
  {3840, 2400, 120}, {3840, 2160, 120}, {3200, 2400, 120}, {3200, 1800, 120},
  {3008, 1692, 120}, {2880, 1800, 120}, {2880, 1620, 120}, {2560, 1600, 120},
  {2560, 1440, 120}, {1920, 1440, 120}, {1920, 1200, 120}, {1920, 1080, 120},
  {1600, 1200, 120}, {1600, 1024, 120}, {1600, 1050, 120}, {1600, 900 , 120},
  {1440, 900 , 120}, {1400, 1050, 120}, {1366, 768 , 120}, {1360, 768 , 120},
  {1280, 1024, 120}, {1280, 960 , 120}, {1280, 800 , 120}, {1280, 768 , 120},
  {1280, 720 , 120}, {1280, 600 , 120}, {1152, 864 , 120}, {1024, 768 , 120},
  {800 , 600 , 120}, {640 , 480 , 120}
};

static const BYTE EDID[] =
{
  0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x30,0xE8,0x34,0x12,0xC9,0x07,0xCC,0x00,
  0x01,0x21,0x01,0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,
  0x0B,0x50,0x54,0x00,0x02,0x00,0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
  0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,
  0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,0x00,0x4C,0x6F,0x6F,
  0x6B,0x69,0x6E,0x67,0x47,0x6C,0x61,0x73,0x73,0x0A,0x00,0x00,0x00,0xFC,0x00,0x4C,
  0x6F,0x6F,0x6B,0x69,0x6E,0x67,0x20,0x47,0x6C,0x61,0x73,0x73,0x00,0x00,0x00,0xFD,
  0x00,0x28,0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x4A
};

const DWORD DefaultPreferredDisplayMode = 19;

void CIndirectDeviceContext::PopulateDefaultModes(bool setDefaultMode)
{
  m_displayModes.reserve(m_displayModes.size() +
    ARRAYSIZE(DefaultDisplayModes));

  for (int i = 0; i < ARRAYSIZE(DefaultDisplayModes); ++i)
  {
    DisplayMode m;
    m.width     = DefaultDisplayModes[i][0];
    m.height    = DefaultDisplayModes[i][1];
    m.refresh   = DefaultDisplayModes[i][2];
    m.preferred = setDefaultMode && (i == DefaultPreferredDisplayMode);
    m_displayModes.push_back(m);
  }
}

void CIndirectDeviceContext::InitAdapter()
{
  if (!m_ivshmem.Init() || !m_ivshmem.Open())
    return;

  m_displayModes.clear();
  PopulateDefaultModes(true);

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);

  /**
   * For some reason if we do not set this flag sometimes windows will
   * refuse to enumerate our virtual monitor. Intel also noted in their
   * sources that if this is not set dynamic resolution changes from this
   * driver will not work. This behaviour is not documented by Microsoft.
   */
  caps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;

  caps.MaxMonitorsSupported = 1;

  caps.EndPointDiagnostics.Size             = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport     = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;

  caps.EndPointDiagnostics.pEndPointFriendlyName     = L"Looking Glass IDD Driver";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"Looking Glass";
  caps.EndPointDiagnostics.pEndPointModelName        = L"Looking Glass";

  IDDCX_ENDPOINT_VERSION ver = {};
  ver.Size     = sizeof(ver);
  ver.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &ver;
  caps.EndPointDiagnostics.pHardwareVersion = &ver;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectDeviceContextWrapper);

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice        = m_wdfDevice;
  init.pCaps            = &caps;
  init.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT initOut;
  NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxAdapterInitAsync Failed");
    return;
  }

  m_adapter = initOut.AdapterObject;

  // try to co-exist with the virtual video device by telling IddCx which adapter we prefer to render on
  IDXGIFactory * factory = NULL;
  IDXGIAdapter * dxgiAdapter;
  CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory);
  for (UINT i = 0; factory->EnumAdapters(i, &dxgiAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC adapterDesc;
    dxgiAdapter->GetDesc(&adapterDesc);
    dxgiAdapter->Release();

    if ((adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c) || // Microsoft Basic Render Driver
        (adapterDesc.VendorId == 0x1b36 && adapterDesc.DeviceId == 0x000d) || // QXL      
        (adapterDesc.VendorId == 0x1234 && adapterDesc.DeviceId == 0x1111))   // QEMU Standard VGA
      continue;

    IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
    args.PreferredRenderAdapter = adapterDesc.AdapterLuid;
    IddCxAdapterSetRenderAdapter(m_adapter, &args);
    break;
  }
  factory->Release();

  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(m_adapter);
  wrapper->context = this;  
}

void CIndirectDeviceContext::FinishInit(UINT connectorIndex)
{
  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectMonitorContextWrapper);

  IDDCX_MONITOR_INFO info = {};
  info.Size           = sizeof(info);
  info.MonitorType    = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  if (connectorIndex >= 1)
  {
    info.MonitorDescription.DataSize = 0;
    info.MonitorDescription.pData    = nullptr;
  }
  else
  {
    info.MonitorDescription.DataSize = sizeof(EDID);
    info.MonitorDescription.pData    = const_cast<BYTE*>(EDID);
  }

  CoCreateGuid(&info.MonitorContainerId);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut;
  NTSTATUS status = IddCxMonitorCreate(m_adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorCreate Failed");
    return;
  }

  m_monitor = createOut.MonitorObject;
  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(m_monitor);
  wrapper->context = new CIndirectMonitorContext(m_monitor, this);

  IDARG_OUT_MONITORARRIVAL out;
  status = IddCxMonitorArrival(m_monitor, &out);
  if (FAILED(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorArrival Failed");
    return;
  }
}

void CIndirectDeviceContext::ReplugMonitor()
{
  if (m_monitor == WDF_NO_HANDLE)
  {
    FinishInit(0);
    return;
  }

  if (m_replugMonitor)
    return;

  DEBUG_TRACE("ReplugMonitor");
  m_replugMonitor = true;
  NTSTATUS status = IddCxMonitorDeparture(m_monitor);
  if (!NT_SUCCESS(status))
  {
    m_replugMonitor = false;
    DEBUG_ERROR("IddCxMonitorDeparture Failed (0x%08x)", status);
    return;
  }
}

void CIndirectDeviceContext::OnAssignSwapChain()
{
  if (m_setCustomMode)
  {
    m_setCustomMode = false;
    g_pipe.SetDisplayMode(m_customMode.width, m_customMode.height);
  }
}

void CIndirectDeviceContext::OnUnassignedSwapChain()
{
  if (m_replugMonitor)
  {
    m_replugMonitor = false;
    FinishInit(0);
  }
}

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO & mode, DWORD width, DWORD height, DWORD vsync, bool monitorMode)
{
  mode.totalSize.cx = mode.activeSize.cx = width;
  mode.totalSize.cy = mode.activeSize.cy = height;

  mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  mode.AdditionalSignalInfo.videoStandard    = 255;
  
  mode.vSyncFreq.Numerator   = vsync;
  mode.vSyncFreq.Denominator = 1;
  mode.hSyncFreq.Numerator   = vsync * height;
  mode.hSyncFreq.Denominator = 1;

  mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  mode.pixelRate        = ((UINT64)vsync) * ((UINT64)width) * ((UINT64)height);
}

NTSTATUS CIndirectDeviceContext::ParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  outArgs->MonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->MonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorGetDefaultModes(
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs)
{
  outArgs->DefaultMonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->DefaultMonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->DefaultMonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pDefaultMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
      (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes(
  const IDARG_IN_QUERYTARGETMODES* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  outArgs->TargetModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->TargetModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pTargetModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_TARGET_MODE);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
  }

  return STATUS_SUCCESS;
}

void CIndirectDeviceContext::SetResolution(int width, int height)
{
  m_displayModes.clear();
  m_customMode.width     = width;
  m_customMode.height    = height;
  m_customMode.refresh   = 120;
  m_customMode.preferred = true;
  m_displayModes.push_back(m_customMode);
  PopulateDefaultModes(false);

  m_setCustomMode = true;

#if 1
  ReplugMonitor();
#else

  if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2))
  {
    IDDCX_TARGET_MODE2* modes = (IDDCX_TARGET_MODE2*)_malloca(
      m_displayModes.size() * sizeof(IDDCX_TARGET_MODE2));

    if (!modes)
    {
      DEBUG_ERROR("Failed to allocate memory for the mode list");
      return;
    }

    ZeroMemory(modes, m_displayModes.size() * sizeof(IDDCX_TARGET_MODE2));

    IDARG_IN_UPDATEMODES2 um = {};
    um.Reason          = IDDCX_UPDATE_REASON_OTHER;
    um.TargetModeCount = (UINT)m_displayModes.size();
    um.pTargetModes    = modes;
    auto* mode = modes;
    for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
    {      
      mode->Size                 = sizeof(IDDCX_TARGET_MODE2);
      mode->RequiredBandwidth    = (UINT64)(it->width * it->height * it->refresh * 32);
      mode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;

      FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, true);
    }

    NTSTATUS status = IddCxMonitorUpdateModes2(m_monitor, &um);
    if (!NT_SUCCESS(status))
      DEBUG_ERROR("IddCxMonitorUpdateModes2 Failed (0x%08x)", status);

    _freea(modes);
  }
  else
  {
    IDDCX_TARGET_MODE* modes = (IDDCX_TARGET_MODE*)_malloca(
      m_displayModes.size() * sizeof(IDDCX_TARGET_MODE));

    if (!modes)
    {
      DEBUG_ERROR("Failed to allocate memory for the mode list");
      return;
    }

    IDARG_IN_UPDATEMODES um = {};
    um.Reason          = IDDCX_UPDATE_REASON_OTHER;
    um.TargetModeCount = (UINT)m_displayModes.size();
    um.pTargetModes    = modes;
  
    auto* mode = modes;
    for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
    {
      mode->Size              = sizeof(IDDCX_TARGET_MODE);
      mode->RequiredBandwidth = (UINT64)(it->width * it->height * it->refresh * 32);

      FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, true);
    }

    NTSTATUS status = IddCxMonitorUpdateModes(m_monitor, &um);
    if (!NT_SUCCESS(status))
      DEBUG_ERROR("IddCxMonitorUpdateModes Failed (0x%08x)", status);

    _freea(modes);
  }
#endif
}

bool CIndirectDeviceContext::SetupLGMP(size_t alignSize)
{
  // this may get called multiple times as we need to delay calling it until
  // we can determine the required alignment from the GPU in use
  if (m_lgmp)
    return true;

  m_alignSize = alignSize;
  
  std::stringstream ss;
  {
    KVMFR kvmfr = {};
    memcpy_s(kvmfr.magic, sizeof(kvmfr.magic), KVMFR_MAGIC, sizeof(KVMFR_MAGIC) - 1);
    kvmfr.version  = KVMFR_VERSION;
    kvmfr.features =
      KVMFR_FEATURE_SETCURSORPOS |
      KVMFR_FEATURE_WINDOWSIZE;
    strncpy_s(kvmfr.hostver, LG_VERSION_STR, sizeof(kvmfr.hostver) - 1);
    ss.write(reinterpret_cast<const char *>(&kvmfr), sizeof(kvmfr));
  }

  {
    const std::string & model = CPlatformInfo::GetCPUModel();

    KVMFRRecord_VMInfo * vmInfo = static_cast<KVMFRRecord_VMInfo *>(calloc(1, sizeof(*vmInfo)));
    if (!vmInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_VMInfo");
      return false;
    }
    vmInfo->cpus    = static_cast<uint8_t>(CPlatformInfo::GetProcCount  ());
    vmInfo->cores   = static_cast<uint8_t>(CPlatformInfo::GetCoreCount  ());
    vmInfo->sockets = static_cast<uint8_t>(CPlatformInfo::GetSocketCount());

    const uint8_t * uuid = CPlatformInfo::GetUUID();
    memcpy_s (vmInfo->uuid, sizeof(vmInfo->uuid), uuid, 16);
    strncpy_s(vmInfo->capture, "Looking Glass IDD Driver", sizeof(vmInfo->capture));

    KVMFRRecord * record = static_cast<KVMFRRecord *>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_VMINFO;
    record->size = sizeof(*vmInfo) + (uint32_t)model.length() + 1;

    ss.write(reinterpret_cast<const char*>(record       ), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(vmInfo       ), sizeof(*vmInfo));
    ss.write(reinterpret_cast<const char*>(model.c_str()), model.length() + 1);
  }

  {
    KVMFRRecord_OSInfo * osInfo = static_cast<KVMFRRecord_OSInfo *>(calloc(1, sizeof(*osInfo)));
    if (!osInfo)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord_OSInfo");
      return false;
    }

    osInfo->os = KVMFR_OS_WINDOWS;

    const std::string & osName = CPlatformInfo::GetProductName();

    KVMFRRecord* record = static_cast<KVMFRRecord*>(calloc(1, sizeof(*record)));
    if (!record)
    {
      DEBUG_ERROR("Failed to allocate KVMFRRecord");
      return false;
    }

    record->type = KVMFR_RECORD_OSINFO;
    record->size = sizeof(*osInfo) + (uint32_t)osName.length() + 1;

    ss.write(reinterpret_cast<const char*>(record), sizeof(*record));
    ss.write(reinterpret_cast<const char*>(osInfo), sizeof(*osInfo));
    ss.write(reinterpret_cast<const char*>(osName.c_str()), osName.length() + 1);
  }

  LGMP_STATUS status;
  std::string udata = ss.str();

  if ((status = lgmpHostInit(m_ivshmem.GetMem(), (uint32_t)m_ivshmem.GetSize(),
    &m_lgmp, (uint32_t)udata.size(), (uint8_t*)&udata[0])) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, FRAME_QUEUE_CONFIG, &m_frameQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    return false;
  }

  if ((status = lgmpHostQueueNew(m_lgmp, POINTER_QUEUE_CONFIG, &m_pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Pointer): %s", lgmpStatusString(status));
    return false;
  }

  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
  {
    if ((status = lgmpHostMemAlloc(m_lgmp, MAX_POINTER_SIZE, &m_pointerShapeMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shapes): %s", lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerShapeMemory[i]), 0, MAX_POINTER_SIZE);
  }

  m_maxFrameSize = lgmpHostMemAvail(m_lgmp);
  m_maxFrameSize = (m_maxFrameSize -(m_alignSize - 1)) & ~(m_alignSize - 1);
  m_maxFrameSize /= LGMP_Q_FRAME_LEN;
  DEBUG_INFO("Max Frame Size: %u MiB", (unsigned int)(m_maxFrameSize / 1048576LL));

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(m_lgmp, (uint32_t)m_maxFrameSize,
        (uint32_t)m_alignSize, &m_frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned Failed (Frame): %s", lgmpStatusString(status));
      return false;
    }

    m_frame[i] = (KVMFRFrame *)lgmpHostMemPtr(m_frameMemory[i]);

    /**
     * put the framebuffer on the border of the next page, this is to allow for
     * aligned DMA tranfers by the reciever */
    const size_t alignOffset = alignSize - sizeof(FrameBuffer);
    m_frame[i]->offset = (uint32_t)alignOffset;
    m_frameBuffer[i] = (FrameBuffer*)(((uint8_t*)m_frame[i]) + alignOffset);
  }

  WDF_TIMER_CONFIG config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&config,
    [](WDFTIMER timer) -> void
    {
      WDFOBJECT parent = WdfTimerGetParentObject(timer);
      auto wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(parent);
      wrapper->context->LGMPTimer();
    },
    10);
  config.AutomaticSerialization = FALSE;

  /**
  * documentation states that Dispatch is not available under the UDMF, however...
  * using Passive returns a not supported error, and Dispatch works.
  */
  WDF_OBJECT_ATTRIBUTES attribs;
  WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
  attribs.ParentObject   = m_wdfDevice;
  attribs.ExecutionLevel = WdfExecutionLevelDispatch;

  NTSTATUS s = WdfTimerCreate(&config, &attribs, &m_lgmpTimer);
  if (!NT_SUCCESS(s))
  {
    DEBUG_ERROR_HR(s, "Timer creation failed");
    return false;
  }
  WdfTimerStart(m_lgmpTimer, WDF_REL_TIMEOUT_IN_MS(10));

  return true;
}

void CIndirectDeviceContext::DeInitLGMP()
{
  m_hasFrame = false;

  if (m_lgmp == nullptr)
    return;

  if (m_lgmpTimer)
  {
    WdfTimerStop(m_lgmpTimer, TRUE);
    m_lgmpTimer = nullptr;
  }

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&m_frameMemory[i]);
  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&m_pointerMemory[i]);
  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerShapeMemory[i]);
  lgmpHostFree(&m_lgmp);
}

void CIndirectDeviceContext::LGMPTimer()
{
  LGMP_STATUS status;
  if ((status = lgmpHostProcess(m_lgmp)) != LGMP_OK)
  {
    if (status == LGMP_ERR_CORRUPTED)
    {
      DEBUG_WARN("LGMP reported the shared memory has been corrupted, attempting to recover\n");
      //TODO: fixme - reinit
      return;
    }

    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    //TODO: fixme - shutdown
    return;
  }

  uint8_t data[LGMP_MSGS_SIZE];
  size_t  size;
  while ((status = lgmpHostReadData(m_pointerQueue, &data, &size)) == LGMP_OK)
  {
    KVMFRMessage * msg = (KVMFRMessage *)data;
    switch (msg->type)
    {
      case KVMFR_MESSAGE_SETCURSORPOS:
      {
        KVMFRSetCursorPos* sp = (KVMFRSetCursorPos*)msg;
        g_pipe.SetCursorPos(sp->x, sp->y);
        break;
      }

      case KVMFR_MESSAGE_WINDOWSIZE:
      {
        KVMFRWindowSize* ws = (KVMFRWindowSize*)msg;
        SetResolution(ws->w, ws->h);
      }
    }

    lgmpHostAckData(m_pointerQueue);
  }

  if (lgmpHostQueueNewSubs(m_frameQueue) && m_monitor)
  {
    if (m_hasFrame)
      lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[m_frameIndex]);
  }

  if (lgmpHostQueueNewSubs(m_pointerQueue))
    ResendCursor();
}

CIndirectDeviceContext::PreparedFrameBuffer CIndirectDeviceContext::PrepareFrameBuffer(int width, int height, int pitch, DXGI_FORMAT format)
{
  PreparedFrameBuffer result = {};

  if (!m_lgmp || !m_frameQueue)
    return result;

  if (m_width != width || m_height != height || m_pitch != pitch || m_format != format)
  {
    m_width  = width;
    m_height = height;
    m_format = format;
    m_pitch  = pitch;
    ++m_formatVer;
  }

  if (++m_frameIndex == LGMP_Q_FRAME_LEN)
    m_frameIndex = 0;

  KVMFRFrame * fi = m_frame[m_frameIndex];

  // wait until there is room in the queue
  while (lgmpHostQueuePending(m_frameQueue) == LGMP_Q_FRAME_LEN)
    Sleep(0);

  int bpp = 4;
  switch (format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM    : fi->type = FRAME_TYPE_BGRA   ; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM    : fi->type = FRAME_TYPE_RGBA   ; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM : fi->type = FRAME_TYPE_RGBA10 ; break;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      fi->type = FRAME_TYPE_RGBA16F;
      bpp = 8;
      break;

    default:
      DEBUG_ERROR("Unsuppoted DXGI format 0x%08x", format);
      return result;
  }

  fi->formatVer        = m_formatVer;
  fi->frameSerial      = m_frameSerial++;
  fi->screenWidth      = width;
  fi->screenHeight     = height;
  fi->dataWidth        = width;
  fi->dataHeight       = height;
  fi->frameWidth       = width;
  fi->frameHeight      = height;
  fi->stride           = width * bpp;
  fi->pitch            = pitch;
  // fi->offset is initialized at startup
  fi->flags            = 0;
  fi->rotation         = FRAME_ROT_0;
  fi->damageRectsCount = 0;

  FrameBuffer* fb = m_frameBuffer[m_frameIndex];  
  fb->wp = 0;

  lgmpHostQueuePost(m_frameQueue, 0, m_frameMemory[m_frameIndex]);

  result.frameIndex = m_frameIndex;
  result.mem        = fb->data;

  m_hasFrame = true;
  return result;
}

void CIndirectDeviceContext::WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos)
{
  FrameBuffer * fb = m_frameBuffer[frameIndex];

  memcpy(
    (void *)((uintptr_t)fb->data + offset),
    (void *)((uintptr_t)src + offset),
    len);

  if (setWritePos)
    fb->wp = (uint32_t)(offset + len);
}

void CIndirectDeviceContext::FinalizeFrameBuffer(unsigned frameIndex)
{
  FrameBuffer * fb = m_frameBuffer[frameIndex];
  fb->wp = m_height * m_pitch;
}

void CIndirectDeviceContext::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info, const BYTE * data)
{
  PLGMPMemory mem;
  if (info.CursorShapeInfo.CursorType == IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    mem = m_pointerMemory[m_pointerMemoryIndex];
    if (++m_pointerMemoryIndex == LGMP_Q_POINTER_LEN)
      m_pointerMemoryIndex = 0;
  }
  else
  {
    mem = m_pointerShapeMemory[m_pointerShapeIndex];
    if (++m_pointerShapeIndex == POINTER_SHAPE_BUFFERS)
      m_pointerShapeIndex = 0;
  }

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  
  m_cursorVisible = info.IsCursorVisible;
  uint32_t flags  = 0;

  if (info.IsCursorVisible)
  {
    m_cursorX       = info.X;
    m_cursorY       = info.Y;
    cursor->x = (int16_t)info.X;
    cursor->y = (int16_t)info.Y;
    flags |= CURSOR_FLAG_POSITION | CURSOR_FLAG_VISIBLE;
  }

  if (info.CursorShapeInfo.CursorType != IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    memcpy(cursor + 1, data,
      (size_t)info.CursorShapeInfo.Height * info.CursorShapeInfo.Pitch);

    cursor->hx     = (int8_t  )info.CursorShapeInfo.XHot;
    cursor->hy     = (int8_t  )info.CursorShapeInfo.YHot;
    cursor->width  = (uint32_t)info.CursorShapeInfo.Width;
    cursor->height = (uint32_t)info.CursorShapeInfo.Height;
    cursor->pitch  = (uint32_t)info.CursorShapeInfo.Pitch;

    switch (info.CursorShapeInfo.CursorType)
    {
      case IDDCX_CURSOR_SHAPE_TYPE_ALPHA:
        cursor->type = CURSOR_TYPE_COLOR;
        break;

      case IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR:
        cursor->type = CURSOR_TYPE_MASKED_COLOR;
        break;
    }

    flags |= CURSOR_FLAG_SHAPE;
    m_pointerShape = mem;
  }

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}

void CIndirectDeviceContext::ResendCursor()
{
  PLGMPMemory mem = m_pointerShape;
  if (!mem)
    return;

  KVMFRCursor* cursor = (KVMFRCursor*)lgmpHostMemPtr(mem);
  cursor->x = (int16_t)m_cursorX;
  cursor->y = (int16_t)m_cursorY;

  const uint32_t flags =
    CURSOR_FLAG_POSITION | CURSOR_FLAG_SHAPE |
    (m_cursorVisible ? CURSOR_FLAG_VISIBLE : 0);

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}