/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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