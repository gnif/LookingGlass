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

#include <type_traits>

namespace Seq
{
  template<typename T>
  T Next(T value)
  {
    static_assert(std::is_integral<T>::value &&
      std::is_unsigned<T>::value && !std::is_same<T, bool>::value,
      "sequence type must be an unsigned integer");

    const T next = value + static_cast<T>(1);
    return next ? next : static_cast<T>(1);
  }

  template<typename T>
  T Inc(T& value)
  {
    value = Next(value);
    return value;
  }

  template<typename T>
  T Take(T& value)
  {
    if (!value)
      value = static_cast<T>(1);
    const T result = value;
    value = Next(value);
    return result;
  }
}
