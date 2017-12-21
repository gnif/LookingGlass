/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "MultiMemcpy.h"
#include "Util.h"
#include "common/memcpySSE.h"

MultiMemcpy::MultiMemcpy()
{
  for (int i = 0; i < MULTIMEMCPY_THREADS; ++i)
  {
    m_workers[i].start  = CreateSemaphore(NULL, 0, 1, NULL);
    m_workers[i].stop   = CreateSemaphore(NULL, 0, 1, NULL);
    m_semaphores[i]     = m_workers[i].stop;

    m_workers[i].thread = CreateThread(0, 0, WorkerFunction, &m_workers[i], 0, NULL);
  }
}

MultiMemcpy::~MultiMemcpy()
{
  for(int i = 0; i < MULTIMEMCPY_THREADS; ++i)
  {
    TerminateThread(m_workers[i].thread, 0);
    CloseHandle(m_workers[i].start);
    CloseHandle(m_workers[i].stop );
  }
}

void MultiMemcpy::Copy(void * dst, void * src, size_t size)
{
  const size_t block = size / MULTIMEMCPY_THREADS;
  for (int i = 0; i < MULTIMEMCPY_THREADS; ++i)
  {
    m_workers[i].dst  = (uint8_t *)dst + i * block;
    m_workers[i].src  = (uint8_t *)src + i * block;
    m_workers[i].size = (i + 1) * block - i * block;
    ReleaseSemaphore(m_workers[i].start, 1, NULL);
  }
  
  WaitForMultipleObjects(MULTIMEMCPY_THREADS, m_semaphores, TRUE, INFINITE);
}

DWORD WINAPI MultiMemcpy::WorkerFunction(LPVOID param)
{
  struct Worker * w = (struct Worker *)param;
  
  for(;;)
  {
    WaitForSingleObject(w->start, INFINITE);
    memcpySSE(w->dst, w->src, w->size);
    ReleaseSemaphore(w->stop, 1, NULL);
  }
}