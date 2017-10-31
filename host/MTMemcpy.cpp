#include "MTMemcpy.h"

MTMemcpy::MTMemcpy() :
  m_initialized(false)
{

}

MTMemcpy::~MTMemcpy()
{
  DeInitialize();
}

DWORD WINAPI MTMemcpy::thread_copy_proc(LPVOID param)
{
  mt_cpy_t * p = (mt_cpy_t *)param;

  while (1)
  {
    WaitForSingleObject(p->s->hCopyStartSemaphores[p->ct], INFINITE);
    memcpy(p->dest, p->src, p->size);
    ReleaseSemaphore(p->s->hCopyStopSemaphores[p->ct], 1, NULL);
  }

  return 0;
}

bool MTMemcpy::Initialize()
{
  if (m_initialized)
    DeInitialize();

  for (int ctr = 0; ctr < NUM_CPY_THREADS; ctr++)
  {
    hCopyStartSemaphores[ctr] = CreateSemaphore(NULL, 0, 1, NULL);
    hCopyStopSemaphores[ctr] = CreateSemaphore(NULL, 0, 1, NULL);
    mtParamters[ctr].s  = this;
    mtParamters[ctr].ct = ctr;
    hCopyThreads[ctr] = CreateThread(0, 0, thread_copy_proc, &mtParamters[ctr], 0, NULL);
  }

  m_initialized = true;
  return true;
}

bool MTMemcpy::Copy(void * dest, void * src, size_t bytes)
{
  if (!m_initialized)
    return false;

  //set up parameters
  for (int ctr = 0; ctr < NUM_CPY_THREADS; ctr++)
  {
    mtParamters[ctr].dest = (char *)dest + ctr * bytes / NUM_CPY_THREADS;
    mtParamters[ctr].src = (char *)src + ctr * bytes / NUM_CPY_THREADS;
    mtParamters[ctr].size = (ctr + 1) * bytes / NUM_CPY_THREADS - ctr * bytes / NUM_CPY_THREADS;
    ReleaseSemaphore(hCopyStartSemaphores[ctr], 1, NULL);
  }

  //wait for all threads to finish
  WaitForMultipleObjects(NUM_CPY_THREADS, hCopyStopSemaphores, TRUE, INFINITE);

  return true;
}

void MTMemcpy::DeInitialize()
{
  if (!m_initialized)
    return;

  for (int ctr = 0; ctr < NUM_CPY_THREADS; ctr++)
  {
    TerminateThread(hCopyThreads[ctr], 0);
    CloseHandle(hCopyStartSemaphores[ctr]);
    CloseHandle(hCopyStopSemaphores[ctr]);
  }

  m_initialized = false;
}