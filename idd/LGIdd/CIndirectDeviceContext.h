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

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>
#include <vector>

#include "CIVSHMEM.h"

extern "C" {
  #include "lgmp/host.h"
}

#include "common/KVMFR.h"
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

  int         m_width    = 0;
  int         m_height   = 0;
  int         m_pitch    = 0;
  DXGI_FORMAT m_format   = DXGI_FORMAT_UNKNOWN;
  bool        m_hasFrame = false;

  void DeInitLGMP();
  void LGMPTimer();
  void ResendCursor();

  struct DisplayMode
  {
    unsigned width;
    unsigned height;
    unsigned refresh;
    bool     preferred;
  };
  std::vector<DisplayMode> m_displayModes;
  DisplayMode m_customMode    = {};
  bool        m_setCustomMode = false;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext() { DeInitLGMP(); }

  bool SetupLGMP(size_t alignSize);

  void PopulateDefaultModes(bool setDefaultMode);
  void InitAdapter();
  void FinishInit(UINT connectorIndex);
  void ReplugMonitor();

  void OnAssignSwapChain();
  void OnUnassignedSwapChain();

  NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorGetDefaultModes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs);
  NTSTATUS MonitorQueryTargetModes(
    const IDARG_IN_QUERYTARGETMODES* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);

  void SetResolution(int width, int height);

  size_t GetAlignSize()    { return m_alignSize;    }
  size_t GetMaxFrameSize() { return m_maxFrameSize; }

  struct PreparedFrameBuffer
  {
    unsigned frameIndex;
    uint8_t* mem;
  };

  PreparedFrameBuffer PrepareFrameBuffer(int width, int height, int pitch, DXGI_FORMAT format);
  void WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos);
  void FinalizeFrameBuffer(unsigned frameIndex);

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR & info, const BYTE * data);

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