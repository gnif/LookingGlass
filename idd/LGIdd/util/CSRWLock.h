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

class CSRWSharedLock
{
private:
  SRWLOCK * m_lock;

public:
  explicit CSRWSharedLock(SRWLOCK * lock) : m_lock(lock)
  {
    AcquireSRWLockShared(m_lock);
  }

  ~CSRWSharedLock()
  {
    ReleaseSRWLockShared(m_lock);
  }

  CSRWSharedLock(const CSRWSharedLock&) = delete;
  CSRWSharedLock& operator=(const CSRWSharedLock&) = delete;
};

class CSRWExclusiveLock
{
private:
  SRWLOCK * m_lock;

public:
  explicit CSRWExclusiveLock(SRWLOCK * lock) : m_lock(lock)
  {
    AcquireSRWLockExclusive(m_lock);
  }

  ~CSRWExclusiveLock()
  {
    ReleaseSRWLockExclusive(m_lock);
  }

  CSRWExclusiveLock(const CSRWExclusiveLock&) = delete;
  CSRWExclusiveLock& operator=(const CSRWExclusiveLock&) = delete;
};
