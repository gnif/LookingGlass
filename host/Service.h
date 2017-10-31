#pragma once

#define W32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdbool.h>

#include "ivshmem.h"
#include "ICapture.h"

class Service
{
public:
  static Service * Get()
  {
    if (!m_instance)
      m_instance = new Service();
    return m_instance;
  }

  bool Initialize();
  void DeInitialize();
  bool Process(HANDLE stopEvent);

private:
  static Service * m_instance;

  Service();
  ~Service();

  bool       m_initialized;
  IVSHMEM  * m_ivshmem;
  HANDLE     m_readyEvent;
  ICapture * m_capture;
  void     * m_memory;
};