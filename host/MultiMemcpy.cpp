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
    m_workers[i].id      = (1 << i);
    m_workers[i].running = &m_running;
    m_workers[i].start   = CreateSemaphore(NULL, 0, 1, NULL);

    m_workers[i].thread = CreateThread(0, 0, WorkerFunction, &m_workers[i], 0, NULL);
  }
}

MultiMemcpy::~MultiMemcpy()
{
  for(int i = 0; i < MULTIMEMCPY_THREADS; ++i)
  {
    TerminateThread(m_workers[i].thread, 0);
    CloseHandle(m_workers[i].start);
  }
}

void MultiMemcpy::Copy(void * dst, void * src, size_t size)
{
  if (!m_awake)
    Wake();

  const size_t block = size / MULTIMEMCPY_THREADS;
  for (int i = 0; i < MULTIMEMCPY_THREADS; ++i)
  {
    m_workers[i].dst  = (uint8_t *)dst + i * block;
    m_workers[i].src  = (uint8_t *)src + i * block;
    m_workers[i].size = (i + 1) * block - i * block;
  }

  INTERLOCKED_OR8(&m_running, (1 << MULTIMEMCPY_THREADS) - 1);
  while(m_running) {}

  m_awake = false;
}

DWORD WINAPI MultiMemcpy::WorkerFunction(LPVOID param)
{
  struct Worker * w = (struct Worker *)param;
  
  for(;;)
  {
    WaitForSingleObject(w->start, INFINITE);
    while(!(*w->running & w->id)) {}

    memcpySSE(w->dst, w->src, w->size);
    INTERLOCKED_AND8(w->running, ~w->id);
  }
}