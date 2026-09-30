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

#include <cstddef>
#include <cstdint>

namespace NetworkAuthCrypto
{
  static constexpr std::size_t SHA256_SIZE = 32;

  bool RandomBytes(void * data, std::size_t size) noexcept;
  bool PBKDF2HMACSHA256(const void * password, std::size_t passwordSize,
    const void * salt, std::size_t saltSize, std::uint32_t iterations,
    void * key, std::size_t keySize) noexcept;
  bool HMACSHA256(const void * key, std::size_t keySize,
    const void * data, std::size_t size,
    std::uint8_t output[SHA256_SIZE]) noexcept;
  bool ConstantTimeEqual(
    const void * left, const void * right, std::size_t size) noexcept;
  void SecureZero(void * data, std::size_t size) noexcept;
}
