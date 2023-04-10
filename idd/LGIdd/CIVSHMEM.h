#pragma once

#include <Windows.h>
#include <SetupAPI.h>
#include <vector>

class CIVSHMEM
{
private:
  struct IVSHMEMData
  {
    SP_DEVINFO_DATA devInfoData;
    DWORD64         busAddr;
  };

  std::vector<struct IVSHMEMData> m_devices;
  HANDLE m_handle = INVALID_HANDLE_VALUE;
  size_t m_size   = 0;
  void * m_mem    = nullptr;

public:
  CIVSHMEM();
  ~CIVSHMEM();

  bool Init();
  bool Open();
  void Close();

  size_t GetSize() { return m_size; }
  void * GetMem () { return m_mem;  }
};

