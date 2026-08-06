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

#include "CIVSHMEM.h"
#include "CSettings.h"
#include "CEdid.h"
#include "CPostProcessor.h"
#include "CFrameScheduler.h"

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
#define COLOR_TRANSFORM_BUFFERS 3
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
  IDDCX_ADAPTER m_adapter                    = nullptr;
  IDDCX_MONITOR m_monitor                    = nullptr;
  LUID          m_preferredRenderAdapter     = {};
  bool          m_havePreferredRenderAdapter = false;
  bool          m_replugMonitor              = false;
  bool          m_replugPending              = false;
  bool          m_monitorDeparted            = false;
  bool          m_swapChainAssigned          = false;
  bool          m_swapChainReady             = false;
  bool          m_waitForSwapChainRelease    = false;

  // Guards the adapter/monitor init handshake and the replug state machine
  // (monitor/replug/swap-chain state, m_doSetMode, m_setMode). These are
  // touched from the IddCx callback threads, swap-chain thread and LGMP timer.
  SRWLOCK m_stateLock = SRWLOCK_INIT;

  // Retry state for InitAdapter. At boot the IVSHMEM device may not have
  // enumerated yet; if so we re-attempt from a timer instead of giving up.
  WDFTIMER          m_initTimer       = nullptr;
  bool              m_ivshmemOpened   = false;
  std::atomic<LONG> m_initInProgress  = 0;

  CIVSHMEM m_ivshmem;

  PLGMPHost      m_lgmp            = nullptr;
  WDFTIMER       m_lgmpTimer       = nullptr;
  PLGMPHostQueue m_frameQueue      = nullptr;
  PLGMPHostQueue m_frameOwnerQueue[LGMP_Q_FRAME_LEN] = {};
  SRWLOCK        m_lgmpProcessLock = SRWLOCK_INIT;

  CFrameScheduler m_frameScheduler;

  PLGMPHostQueue m_pointerQueue = nullptr;
  PLGMPMemory    m_pointerMemory     [LGMP_Q_POINTER_LEN   ] = {};
  PLGMPMemory    m_pointerShapeMemory[POINTER_SHAPE_BUFFERS] = {};
  PLGMPMemory    m_pointerTransformMemory[COLOR_TRANSFORM_BUFFERS] = {};
  PLGMPMemory    m_pointerShape = nullptr;
  int m_pointerMemoryIndex = 0;
  int m_pointerShapeIndex  = 0;
  int m_pointerTransformIndex = 0;
  bool m_cursorVisible = false;
  int m_cursorX = 0, m_cursorY = 0;

  size_t            m_alignSize           = 0;
  size_t            m_frameMemoryOffset   = 0;
  size_t            m_maxFrameSize        = 0;
  // LGMP publication precedes copy completion, so only the ready index is safe
  // to retain and replay.
  std::atomic<LONG> m_submittedFrameIndex = -1;
  std::atomic<LONG> m_readyFrameIndex     = -1;
  std::atomic<bool> m_frameInFlight[LGMP_Q_FRAME_BUFFER_LEN] = {};
  SRWLOCK           m_framePublishLock     = SRWLOCK_INIT;
  uint64_t          m_framePublishSequence = 0;
  uint64_t          m_frameLastPublishSequence[LGMP_Q_FRAME_BUFFER_LEN] = {};

  enum SharedFramePostResult
  {
    SHARED_FRAME_FAILED,
    SHARED_FRAME_IDLE,
    SHARED_FRAME_POSTED,
  };

  struct FrameDelivery
  {
    uint64_t sharedOwnerToken    = 0;
    unsigned ownerQueueMask      = 0;
    uint32_t sharedOwnerClientID = 0;
    bool     sharedOwnerPending  = false;
    bool     sharedPending       = false;
  };

  struct OwnerDelivery
  {
    uint64_t token      = 0;
    uint32_t clientID   = 0;
    unsigned frameIndex = 0;
    bool     active     = false;
  };

  FrameDelivery m_frameDelivery[LGMP_Q_FRAME_BUFFER_LEN] = {};
  OwnerDelivery m_ownerDelivery[LGMP_Q_FRAME_LEN] = {};
  uint32_t      m_formatVer   = 0;
  uint32_t      m_frameSerial = 0;
  PLGMPMemory   m_frameMemory[LGMP_Q_FRAME_BUFFER_LEN] = {};
  KVMFRFrame  * m_frame      [LGMP_Q_FRAME_BUFFER_LEN] = {};
  FrameBuffer * m_frameBuffer[LGMP_Q_FRAME_BUFFER_LEN] = {};

  unsigned    m_width       = 0;
  unsigned    m_height      = 0;
  unsigned    m_frameWidth  = 0;
  unsigned    m_frameHeight = 0;
  unsigned    m_pitch       = 0;
  DXGI_FORMAT m_format      = DXGI_FORMAT_UNKNOWN;
  FrameType   m_frameType   = FRAME_TYPE_INVALID;

  UINT        m_iddCxVersion    = 0;
  bool        m_hasIddCx110DDIs = false;
  bool        m_canProcessFP16  = false;
  bool        m_softwareMode    = true;

  // Previous HDR metadata used to detect changes for formatVer bumps
  uint16_t m_lastHDRDisplayPrimary[3][2]      = {};
  uint16_t m_lastHDRWhitePoint[2]             = {};
  uint32_t m_lastHDRMaxDisplayLuminance       = 0;
  uint32_t m_lastHDRMinDisplayLuminance       = 0;
  uint32_t m_lastHDRMaxContentLightLevel      = 0;
  uint32_t m_lastHDRMaxFrameAverageLightLevel = 0;
  uint32_t m_lastSDRWhiteLevel                = 0;
  bool     m_lastHDRActive                    = false;
  bool     m_lastHDRMetadata                  = false;

  // Windows display calibration transform. The callback publishes immutable
  // snapshots so the swap-chain thread never observes a partially updated
  // matrix or LUT.
  mutable SRWLOCK m_colorTransformLock = SRWLOCK_INIT;
  std::shared_ptr<const D12ColorTransform> m_colorTransform;

  void QueryIddCxCapabilities();

  void ScheduleInitRetry();
  void StopInitRetry();

  bool InitializeLGMP();
  void DeInitLGMP();
  void LGMPTimer();
  void ProcessFrameDeliveries();
  int FindAvailableFrameBuffer() const;
  int FindAvailableOwnerQueue(unsigned preferredIndex) const;
  unsigned CountOwnerDeliveries(uint32_t clientID) const;
  bool HasMatchingOwnerDelivery(uint32_t clientID, unsigned frameIndex,
    uint64_t token) const;
  SharedFramePostResult PostSharedFrame(unsigned frameIndex,
    uint32_t excludeClientID, uint64_t now);
  bool PostSharedOwnerFrame(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule);
  void ResendCursor();
  void SendColorTransform();
  void InitializeEdid();
  bool GetResolutionMemoryRequirements(uint32_t width, uint32_t height,
    UINT64 alignment, UINT64& frameSize, UINT64& ivshmemSize) const;

  // Guards m_displayModes and m_edid. The mode list is rebuilt on the LGMP
  // timer thread (SetResolution) while IddCx concurrently enumerates it on its
  // own callback threads. The EDID is initialized once and remains immutable.
  // Never held across an IddCx API call - snapshot then call.
  mutable SRWLOCK m_modeLock = SRWLOCK_INIT;

  CSettings::DisplayModes m_displayModes;
  CEdid                   m_edid;

  CSettings::DisplayMode m_setMode = {};
  bool m_doSetMode = false;

  // Set by ReplugMonitor after a departure to rebuild the monitor from the LGMP
  // timer, off the IddCx callback thread.
  std::atomic<LONG> m_finishInitQueued = 0;
  std::atomic<LONG> m_replugQueued     = 0;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext() { DeInitLGMP(); }

  bool SetupLGMP(size_t alignSize);

  bool PopulateDefaultModes();
  void InitAdapter();
  void FinishAdapterInit(UINT connectorIndex);
  void FinishInit(UINT connectorIndex);
  void ReplugMonitor();

  void OnMonitorDestroyed(IDDCX_MONITOR monitor);
  void OnSwapChainAssigned();
  void OnSwapChainReleased();
  void OnSwapChainReady();

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

  void SetResolution(uint32_t width, uint32_t height);

  size_t GetAlignSize   () const { return m_alignSize     ; }
  size_t GetMaxFrameSize() const { return m_maxFrameSize  ; }
  bool   HasIddCx110DDIs() const { return m_hasIddCx110DDIs; }
  bool   CanProcessFP16 () const { return m_canProcessFP16; }
  bool   IsSoftwareMode () const { return m_softwareMode  ; }

  struct PreparedFrameBuffer
  {
    unsigned frameIndex;
    uint8_t* mem;
    bool     fullCopy;
  };

  bool FrameBufferAvailable(const CFrameScheduler::Schedule& schedule);
  bool HasPublishedFrame() const
  {
    return m_readyFrameIndex.load(std::memory_order_acquire) >= 0;
  }
  void ProcessFrameQueue();
  bool GetSharedFrameTarget(uint64_t now, uint64_t& target);
  bool ReplaySharedFrame(uint64_t now, bool& retry);
  PreparedFrameBuffer PrepareFrameBuffer(unsigned pitch, const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat, const RECT * dirtyRects, unsigned nbDirtyRects);
  bool PublishFrameBuffer(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule);
  bool RepublishFrameBuffer(const CFrameScheduler::Schedule& schedule);
  void CommitFrameBuffer(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule, bool periodic);
  void AbortFrameBuffer(unsigned frameIndex);
  void FailFrameBuffer(unsigned frameIndex);
  void CompleteFrameBuffer(unsigned frameIndex, bool succeeded);
  void SetFrameTiming(unsigned frameIndex, uint64_t captureTime,
    uint64_t postProcessTime, uint64_t copyTime, uint64_t readyTime);
  void WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const;
  void FinalizeFrameBuffer(unsigned frameIndex) const;

  void ObserveFrame(uint64_t now);
  void ForceFrame();
  bool GetPublishTarget(uint64_t now, uint64_t& target,
    CFrameScheduler::Schedule& schedule, bool& periodic, bool& republish);
  void FrameSuperseded();
  HANDLE GetFrameScheduleEvent() const
  {
    return m_frameScheduler.GetWakeEvent();
  }
  void RecordFrameTiming(uint64_t duration);

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR & info, const BYTE * data,
    UINT sdrWhiteLevel);

  void SetColorTransform(std::shared_ptr<const D12ColorTransform> transform);
  std::shared_ptr<const D12ColorTransform> GetColorTransform() const;

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
