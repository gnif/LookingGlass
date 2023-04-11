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
  CIVSHMEM      m_ivshmem;

  PLGMPHost      m_lgmp         = nullptr;
  PLGMPHostQueue m_frameQueue   = nullptr;

  PLGMPHostQueue m_pointerQueue = nullptr;
  PLGMPMemory    m_pointerMemory     [LGMP_Q_POINTER_LEN   ] = {};
  PLGMPMemory    m_pointerShapeMemory[POINTER_SHAPE_BUFFERS] = {};

  size_t         m_maxFrameSize = 0;
  int            m_frameIndex   = 0;
  uint32_t       m_formatVer    = 0;
  uint32_t       m_frameSerial  = 0;
  PLGMPMemory    m_frameMemory[LGMP_Q_FRAME_LEN] = {};

  int         m_width  = 0;
  int         m_height = 0;
  DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

  bool SetupLGMP();

  void LGMPTimer();

  WDFTIMER m_lgmpTimer = nullptr;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext();

  void InitAdapter();

  void FinishInit(UINT connectorIndex);

  void SendFrame(int width, int height, int pitch, DXGI_FORMAT format, void* data);
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