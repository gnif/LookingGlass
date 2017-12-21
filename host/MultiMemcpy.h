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

#pragma once
class MultiMemcpy
{
public:
  MultiMemcpy();
  ~MultiMemcpy();

  void Copy(void * dst, void * src, size_t size);
private:
  struct Worker
  {
    HANDLE  start;
    HANDLE  stop;
    HANDLE  thread;
    void  * dst;
    void  * src;
    size_t  size;
  };

  HANDLE m_semaphores[MULTIMEMCPY_THREADS];
  struct Worker m_workers[MULTIMEMCPY_THREADS];
  static DWORD WINAPI WorkerFunction(LPVOID param);
};

