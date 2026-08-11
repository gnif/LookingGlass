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

class CSRWSharedLock;
class CSRWExclusiveLock;

class CSRWLock
{
private:
  friend class CSRWSharedLock;
  friend class CSRWExclusiveLock;

  SRWLOCK m_lock = SRWLOCK_INIT;

public:
  CSRWLock() = default;

  CSRWLock(const CSRWLock&) = delete;
  CSRWLock& operator=(const CSRWLock&) = delete;
};

class CSRWSharedLock
{
private:
  SRWLOCK * m_lock;

public:
  explicit CSRWSharedLock(CSRWLock& lock) : m_lock(&lock.m_lock)
  {
    AcquireSRWLockShared(m_lock);
  }

  ~CSRWSharedLock()
  {
    Unlock();
  }

  void Unlock()
  {
    if (!m_lock)
      return;

    ReleaseSRWLockShared(m_lock);
    m_lock = nullptr;
  }

  CSRWSharedLock(const CSRWSharedLock&) = delete;
  CSRWSharedLock& operator=(const CSRWSharedLock&) = delete;

  CSRWSharedLock(CSRWSharedLock&& other) noexcept : m_lock(other.m_lock)
  {
    other.m_lock = nullptr;
  }

  CSRWSharedLock& operator=(CSRWSharedLock&&) = delete;
};

class CSRWExclusiveLock
{
private:
  struct TryTag {};

  SRWLOCK * m_lock;

  CSRWExclusiveLock(CSRWLock& lock, TryTag) :
    m_lock(TryAcquireSRWLockExclusive(&lock.m_lock) ? &lock.m_lock : nullptr)
  {
  }

public:
  explicit CSRWExclusiveLock(CSRWLock& lock) : m_lock(&lock.m_lock)
  {
    AcquireSRWLockExclusive(m_lock);
  }

  ~CSRWExclusiveLock()
  {
    Unlock();
  }

  static CSRWExclusiveLock Try(CSRWLock& lock)
  {
    return CSRWExclusiveLock(lock, TryTag {});
  }

  explicit operator bool() const
  {
    return m_lock != nullptr;
  }

  void Unlock()
  {
    if (!m_lock)
      return;

    ReleaseSRWLockExclusive(m_lock);
    m_lock = nullptr;
  }

  CSRWExclusiveLock(const CSRWExclusiveLock&) = delete;
  CSRWExclusiveLock& operator=(const CSRWExclusiveLock&) = delete;

  CSRWExclusiveLock(CSRWExclusiveLock&& other) noexcept :
    m_lock(other.m_lock)
  {
    other.m_lock = nullptr;
  }

  CSRWExclusiveLock& operator=(CSRWExclusiveLock&&) = delete;
};
