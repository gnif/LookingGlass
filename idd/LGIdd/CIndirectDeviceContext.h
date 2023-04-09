#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>
#include "CIVSHMEM.h"

class CIndirectDeviceContext
{
private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter = nullptr;
  CIVSHMEM      m_ivshmem;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext() {};

  void InitAdapter();

  void FinishInit(UINT connectorIndex);
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