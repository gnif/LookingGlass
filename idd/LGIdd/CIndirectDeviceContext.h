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
#include <vector>

#include "CIVSHMEM.h"
#include "CSettings.h"
#include "CEdid.h"
#include "CPostProcessor.h"

extern "C" {
  #include "lgmp/host.h"
}

#include "common/KVMFR.h"

// IddCx 1.10 HDR/WCG types are only visible when the WDK targets
// (NTDDI >= 0x0A000005) *and* the build flags IDDCX_VERSION_MAJOR/MINOR are set to >= 1.10.
#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10)) && \
  NTDDI_VERSION >= 0x0A000005
  #define HAS_IDDCX_110
#endif

#define MAX_POINTER_SIZE (sizeof(KVMFRCursor) + (512 * 512 * 4))
#define POINTER_SHAPE_BUFFERS 3

//FIXME: this should not really be done here, this is a hack
#pragma warning(push)
#pragma warning(disable: 4200)
struct FrameBuffer
{
  volatile uint32_t wp;
  uint8_t data[0];
};
#pragma warning(pop)

class CIndirectDeviceContext
{
private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter       = nullptr;
  IDDCX_MONITOR m_monitor       = nullptr;
  bool          m_replugMonitor = false;

  // Guards the adapter/monitor init handshake and the replug state machine
  // (m_monitor, m_replugMonitor, m_doSetMode, m_setMode). These are touched
  // from the IddCx callback threads and the LGMP timer thread.
  SRWLOCK m_stateLock = SRWLOCK_INIT;

  // Retry state for InitAdapter. At boot the IVSHMEM device may not have
  // enumerated yet; if so we re-attempt from a timer instead of giving up.
  WDFTIMER      m_initTimer     = nullptr;
  bool          m_ivshmemOpened = false;
  volatile LONG m_initInProgress = 0;

  CIVSHMEM m_ivshmem;

  PLGMPHost      m_lgmp       = nullptr;
  WDFTIMER       m_lgmpTimer  = nullptr;
  PLGMPHostQueue m_frameQueue = nullptr;

  PLGMPHostQueue m_pointerQueue = nullptr;
  PLGMPMemory    m_pointerMemory     [LGMP_Q_POINTER_LEN   ] = {};
  PLGMPMemory    m_pointerShapeMemory[POINTER_SHAPE_BUFFERS] = {};
  PLGMPMemory    m_pointerShape = nullptr;
  int m_pointerMemoryIndex = 0;
  int m_pointerShapeIndex  = 0;
  bool m_cursorVisible = false;
  int m_cursorX = 0, m_cursorY = 0;

  size_t         m_alignSize    = 0;
  size_t         m_maxFrameSize = 0;
  int            m_frameIndex   = 0;
  uint32_t       m_formatVer    = 0;
  uint32_t       m_frameSerial  = 0;
  PLGMPMemory    m_frameMemory[LGMP_Q_FRAME_LEN] = {};
  KVMFRFrame   * m_frame      [LGMP_Q_FRAME_LEN] = {};
  FrameBuffer  * m_frameBuffer[LGMP_Q_FRAME_LEN] = {};

  unsigned    m_width    = 0;
  unsigned    m_height   = 0;
  unsigned    m_pitch    = 0;
  DXGI_FORMAT m_format   = DXGI_FORMAT_UNKNOWN;
  FrameType   m_frameType = FRAME_TYPE_INVALID;
  bool        m_hasFrame = false;

  UINT m_iddCxVersion = 0;
  bool m_canProcessFP16 = false;

  // HDR state from EvtIddCxMonitorSetDefaultHdrMetadata (IddCx 1.10+)
  // Protected by m_hdrLock - accessed from both the IDD callback thread
  // (SetHDRActive) and the swap-chain thread (PrepareFrameBuffer, GetHDRMetadata).
  mutable SRWLOCK m_hdrLock               = SRWLOCK_INIT;
  bool     m_hdrActive                    = false;
  uint16_t m_hdrDisplayPrimary[3][2]      = {};
  uint16_t m_hdrWhitePoint[2]             = {};
  uint32_t m_hdrMaxDisplayLuminance       = 0;
  uint32_t m_hdrMinDisplayLuminance       = 0;
  uint32_t m_hdrMaxContentLightLevel      = 0;
  uint32_t m_hdrMaxFrameAverageLightLevel = 0;

  // Previous HDR metadata used to detect changes for formatVer bumps
  uint16_t m_lastHDRDisplayPrimary[3][2]      = {};
  uint16_t m_lastHDRWhitePoint[2]             = {};
  uint32_t m_lastHDRMaxDisplayLuminance       = 0;
  uint32_t m_lastHDRMinDisplayLuminance       = 0;
  uint32_t m_lastHDRMaxContentLightLevel      = 0;
  uint32_t m_lastHDRMaxFrameAverageLightLevel = 0;
  uint32_t m_lastSDRWhiteLevel                = 0;
  bool     m_lastHDRActive                    = false;

  void QueryIddCxCapabilities();
  bool CanUseIddCx110DDIs() const { return m_canProcessFP16; }

  void ScheduleInitRetry();
  void StopInitRetry();

  void DeInitLGMP();
  void LGMPTimer();
  void ResendCursor() const;

  // Guards m_displayModes and m_edid. The mode list is rebuilt on the LGMP
  // timer thread (SetResolution) while IddCx concurrently enumerates it on its
  // own callback threads (ParseMonitorDescription / MonitorQueryTargetModes /
  // FinishInit). Never held across an IddCx API call - snapshot then call.
  mutable SRWLOCK m_modeLock = SRWLOCK_INIT;

  CSettings::DisplayModes m_displayModes;
  CEdid                   m_edid;

  CSettings::DisplayMode m_setMode = {};
  bool m_doSetMode = false;

  // Debounce for resolution changes. A window resize streams many WINDOWSIZE
  // requests in quick succession; rather than replug the monitor for each, we
  // (re)start a timer and only apply the last requested resolution once it has
  // remained unchanged for m_resChangeDelayMs. Guarded by m_stateLock.
  static const ULONG     m_resChangeDelayMs = 1000;
  WDFTIMER               m_resChangeTimer   = nullptr;
  CSettings::DisplayMode m_pendingMode      = {};
  bool                   m_resChangePending = false;

  // Set by ReplugMonitor after a departure to rebuild the monitor from the LGMP
  // timer, off the IddCx callback thread.
  volatile LONG m_finishInitQueued = 0;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext() { DeInitLGMP(); }

  bool SetupLGMP(size_t alignSize);

  void PopulateDefaultModes();
  void InitAdapter();
  void FinishInit(UINT connectorIndex);
  void ReplugMonitor();

  void OnMonitorDestroyed(IDDCX_MONITOR monitor);
  void OnAssignSwapChain();

  NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorGetDefaultModes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs);
  NTSTATUS MonitorQueryTargetModes(
    const IDARG_IN_QUERYTARGETMODES* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);

#ifdef HAS_IDDCX_110
  NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorQueryTargetModes2(
    const IDARG_IN_QUERYTARGETMODES2* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);
#endif

  void SetResolution(int width, int height);
  void ApplyResolution();

  size_t GetAlignSize   () const { return m_alignSize     ; }
  size_t GetMaxFrameSize() const { return m_maxFrameSize  ; }
  bool   CanProcessFP16 () const { return m_canProcessFP16; }

  struct PreparedFrameBuffer
  {
    unsigned frameIndex;
    uint8_t* mem;
  };

  PreparedFrameBuffer PrepareFrameBuffer(unsigned pitch, const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat, const RECT * dirtyRects, unsigned nbDirtyRects);
  void WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const;
  void FinalizeFrameBuffer(unsigned frameIndex) const;

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR & info, const BYTE * data);

  // Tracks HDR state from EvtIddCxMonitorSetDefaultHdrMetadata
  void SetHDRActive(const struct IDDCX_HDR10_METADATA * hdrMeta);

  // Returns true if the display is currently in HDR mode
  bool IsHDRActive() const
  {
    AcquireSRWLockShared(&m_hdrLock);
    bool result = m_hdrActive;
    ReleaseSRWLockShared(&m_hdrLock);
    return result;
  }

  // Copies current HDR metadata into the provided D12FrameFormat.
  // Returns true if HDR is active and metadata was copied.
  bool GetHDRMetadata(D12FrameFormat & format) const;

  CIVSHMEM &GetIVSHMEM() { return m_ivshmem; }
};

struct CIndirectDeviceContextWrapper
{
  CIndirectDeviceContext* context;

  void Cleanup()
  {
    delete context;
    context = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE(CIndirectDeviceContextWrapper);
