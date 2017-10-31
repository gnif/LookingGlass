#pragma once

#define W32_LEAN_AND_MEAN
#include <Windows.h>

#define NUM_CPY_THREADS 4

class MTMemcpy
{
public:
  bool MTMemcpy::Initialize();
  void MTMemcpy::DeInitialize();
  bool MTMemcpy::Copy(void * dest, void * src, size_t bytes);

  MTMemcpy();
  ~MTMemcpy();

private:
  bool m_initialized;
  static DWORD WINAPI MTMemcpy::thread_copy_proc(LPVOID param);

  typedef struct
  {
    MTMemcpy * s;
    int        ct;
    void     * src;
    void     * dest;
    size_t     size;
  }
  mt_cpy_t;

  HANDLE hCopyThreads[NUM_CPY_THREADS] = { 0 };
  HANDLE hCopyStartSemaphores[NUM_CPY_THREADS] = { 0 };
  HANDLE hCopyStopSemaphores[NUM_CPY_THREADS] = { 0 };

  mt_cpy_t mtParamters[NUM_CPY_THREADS] = { 0 };
};