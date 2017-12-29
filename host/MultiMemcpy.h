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

#define MULTIMEMCPY_THREADS 4

#include <windows.h>
#include <stdint.h>

#include "Util.h"

#pragma once
class MultiMemcpy
{
public:
  MultiMemcpy();
  ~MultiMemcpy();

  // preempt the copy and wake up the threads early
  inline void Wake()
  {
    if (m_awake)
      return;

    for (int i = 0; i < MULTIMEMCPY_THREADS; ++i)
      ReleaseSemaphore(m_workers[i].start, 1, NULL);

    m_awake = true;
  }

  // abort a pre-empted copy
  inline void Abort()
  {
    if (!m_awake)
      return;

    for (int i = 0; i < MULTIMEMCPY_THREADS; ++i)
      m_workers[i].abort = true;

    INTERLOCKED_OR8(&m_running, (1 << MULTIMEMCPY_THREADS) - 1);
    while (m_running) {}

    m_awake = false;
  }


  void Copy(void * dst, void * src, size_t size);
private:
  struct Worker
  {
    unsigned int   id;
    volatile char *running;
    bool           abort;

    HANDLE  start;
    HANDLE  thread;
    void  * dst;
    void  * src;
    size_t  size;
  };

  bool m_awake;
  volatile char m_running;
  struct Worker m_workers[MULTIMEMCPY_THREADS];
  static DWORD WINAPI WorkerFunction(LPVOID param);
};

