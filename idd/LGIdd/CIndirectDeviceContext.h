/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#include "CIVSHMEM.h"

extern "C" {
  #include "lgmp/host.h"
}

#include "common/KVMFR.h"
#define MAX_POINTER_SIZE (sizeof(KVMFRCursor) + (512 * 512 * 4))
#define POINTER_SHAPE_BUFFERS 3

class CIndirectDeviceContext
{
private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter = nullptr;
  IDDCX_MONITOR m_monitor = nullptr;

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

  size_t         m_maxFrameSize = 0;
  int            m_frameIndex   = 0;
  uint32_t       m_formatVer    = 0;
  uint32_t       m_frameSerial  = 0;
  PLGMPMemory    m_frameMemory[LGMP_Q_FRAME_LEN] = {};

  int         m_width  = 0;
  int         m_height = 0;
  int         m_pitch  = 0;
  DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

  bool SetupLGMP();
  void LGMPTimer();
  void ResendCursor();  

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext();

  void InitAdapter();

  void FinishInit(UINT connectorIndex);

  void SendFrame(int width, int height, int pitch, DXGI_FORMAT format, void* data);
  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR & info, const BYTE * data);
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