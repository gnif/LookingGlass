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

#include "Seq.h"

#include <Windows.h>

#include <atomic>
#include <stdint.h>

namespace Atomic
{
  namespace Detail
  {
    inline volatile LONG * Ptr(uint32_t& value)
    {
      static_assert(sizeof(value) == sizeof(LONG),
        "atomic value must match the Windows interlocked width");
      return reinterpret_cast<volatile LONG *>(&value);
    }
  }

  template<typename T>
  T Load(const std::atomic<T>& value,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return value.load(order);
  }

  template<typename T, typename U>
  void Store(std::atomic<T>& value, U data,
    std::memory_order order = std::memory_order_seq_cst)
  {
    value.store(static_cast<T>(data), order);
  }

  template<typename T, typename U>
  T Swap(std::atomic<T>& value, U data,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return value.exchange(static_cast<T>(data), order);
  }

  template<typename T, typename U>
  T FetchAdd(std::atomic<T>& value, U data,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return value.fetch_add(static_cast<T>(data), order);
  }

  template<typename T, typename U>
  T FetchSub(std::atomic<T>& value, U data,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return value.fetch_sub(static_cast<T>(data), order);
  }

  template<typename T, typename U>
  bool CAS(std::atomic<T>& value, T& expected, U data,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return value.compare_exchange_strong(
      expected, static_cast<T>(data), order);
  }

  template<typename T, typename U>
  bool CAS(std::atomic<T>& value, T& expected, U data,
    std::memory_order success, std::memory_order failure)
  {
    return value.compare_exchange_strong(
      expected, static_cast<T>(data), success, failure);
  }

  template<typename T, typename U>
  bool CASWeak(std::atomic<T>& value, T& expected, U data,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return value.compare_exchange_weak(
      expected, static_cast<T>(data), order);
  }

  template<typename T, typename U>
  bool CASWeak(std::atomic<T>& value, T& expected, U data,
    std::memory_order success, std::memory_order failure)
  {
    return value.compare_exchange_weak(
      expected, static_cast<T>(data), success, failure);
  }

  template<typename T>
  T Inc(std::atomic<T>& value,
    std::memory_order order = std::memory_order_seq_cst)
  {
    return FetchAdd(value, static_cast<T>(1), order) + 1;
  }

  template<typename T>
  T Next(std::atomic<T>& value,
    std::memory_order order = std::memory_order_relaxed)
  {
    T current = Load(value, std::memory_order_relaxed);
    for (;;)
    {
      const T next = Seq::Next(current);
      if (CASWeak(value, current, next, order))
        return next;
    }
  }

  inline uint32_t Load(uint32_t& value)
  {
    return static_cast<uint32_t>(InterlockedCompareExchange(
      Detail::Ptr(value), 0, 0));
  }

  inline void Store(uint32_t& value, uint32_t data)
  {
    InterlockedExchange(Detail::Ptr(value), static_cast<LONG>(data));
  }

  inline uint32_t Swap(uint32_t& value, uint32_t data)
  {
    return static_cast<uint32_t>(
      InterlockedExchange(Detail::Ptr(value), static_cast<LONG>(data)));
  }

  inline uint32_t FetchAdd(uint32_t& value, uint32_t data)
  {
    return static_cast<uint32_t>(
      InterlockedExchangeAdd(Detail::Ptr(value), static_cast<LONG>(data)));
  }

  inline uint32_t FetchSub(uint32_t& value, uint32_t data)
  {
    return static_cast<uint32_t>(InterlockedExchangeAdd(
      Detail::Ptr(value), static_cast<LONG>(0U - data)));
  }

  inline bool CAS(uint32_t& value, uint32_t expected, uint32_t data)
  {
    const uint32_t actual = static_cast<uint32_t>(InterlockedCompareExchange(
      Detail::Ptr(value), static_cast<LONG>(data),
      static_cast<LONG>(expected)));
    return actual == expected;
  }

  inline bool CASWeak(uint32_t& value, uint32_t expected, uint32_t data)
  {
    return CAS(value, expected, data);
  }

  inline uint32_t Inc(uint32_t& value)
  {
    return static_cast<uint32_t>(InterlockedIncrement(Detail::Ptr(value)));
  }

  inline uint32_t Next(uint32_t& value, uint32_t step = 1)
  {
    uint32_t result = FetchAdd(value, step) + step;
    if (!result)
      result = FetchAdd(value, step) + step;
    return result;
  }

  inline void Fence()
  {
    MemoryBarrier();
  }
}
