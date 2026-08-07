/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#pragma once

#include <Windows.h>

#include <stddef.h>
#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
}

class CIVSHMEM;

class CLGMPHost
{
private:
  PLGMPHost m_host        = nullptr;
  SRWLOCK   m_processLock = SRWLOCK_INIT;

public:
  CLGMPHost() = default;
  ~CLGMPHost();

  CLGMPHost(const CLGMPHost&) = delete;
  CLGMPHost& operator=(const CLGMPHost&) = delete;

  bool Initialize(CIVSHMEM& ivshmem);
  void DeInit();

  bool IsInitialized() const { return m_host != nullptr; }
  LGMP_STATUS Process();

  LGMP_STATUS CreateQueue(const struct LGMPQueueConfig& config,
    PLGMPHostQueue * queue);
  LGMP_STATUS Allocate(uint32_t size, PLGMPMemory * memory);
  LGMP_STATUS AllocateAligned(uint32_t size, uint32_t alignment,
    PLGMPMemory * memory);
  size_t Available() const;
};
