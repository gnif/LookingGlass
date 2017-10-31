#pragma once

#define W32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdbool.h>

class IVSHMEM
{
public:
  static IVSHMEM * Get()
  {
    if (!m_instance)
      m_instance = new IVSHMEM();
    return m_instance;
  }

  bool Initialize();
  void DeInitialize();
  bool IsInitialized();

  UINT64 GetSize();
  UINT16 GetPeerID();
  UINT16 GetVectors();
  void * GetMemory();
  HANDLE CreateVectorEvent(UINT16 vector);

protected:


private:
  static IVSHMEM * m_instance;

  IVSHMEM();
  ~IVSHMEM();

  bool   m_initialized;
  HANDLE m_handle;

  UINT64 m_size   ; bool m_gotSize  ;
  UINT16 m_peerID ; bool m_gotPeerID;
  void * m_memory ; bool m_gotMemory;
  UINT16 m_vectors; bool m_gotVectors;
};