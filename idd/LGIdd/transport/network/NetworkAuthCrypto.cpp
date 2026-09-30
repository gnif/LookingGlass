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

#include "transport/network/NetworkAuthCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <climits>

namespace
{
  static const std::uint8_t EMPTY_INPUT = 0;

  PUCHAR InputBytes(const void * data, std::size_t size) noexcept
  {
    return const_cast<PUCHAR>(static_cast<const UCHAR *>(
      size ? data : &EMPTY_INPUT));
  }

  bool OpenHMACSHA256(BCRYPT_ALG_HANDLE& algorithm) noexcept
  {
    return BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm,
      BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
  }
}

namespace NetworkAuthCrypto
{
  bool RandomBytes(void * data, std::size_t size) noexcept
  {
    if (size && !data)
      return false;

    auto *            output    = static_cast<std::uint8_t *>(data);
    const std::size_t totalSize = size;
    while (size)
    {
      const ULONG chunk = size > ULONG_MAX ? ULONG_MAX :
        static_cast<ULONG>(size);
      if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, output, chunk,
          BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
      {
        SecureZero(data, totalSize);
        return false;
      }

      output += chunk;
      size   -= chunk;
    }

    return true;
  }

  bool PBKDF2HMACSHA256(const void * password, std::size_t passwordSize,
    const void * salt, std::size_t saltSize, std::uint32_t iterations,
    void * key, std::size_t keySize) noexcept
  {
    if ((passwordSize && !password) || (saltSize && !salt) || !iterations ||
        !key || !keySize || passwordSize > INT_MAX || saltSize > INT_MAX ||
        keySize > INT_MAX || iterations > INT_MAX)
      return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!OpenHMACSHA256(algorithm))
    {
      SecureZero(key, keySize);
      return false;
    }

    const NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm,
      InputBytes(password, passwordSize), static_cast<ULONG>(passwordSize),
      InputBytes(salt, saltSize), static_cast<ULONG>(saltSize), iterations,
      static_cast<PUCHAR>(key), static_cast<ULONG>(keySize), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (BCRYPT_SUCCESS(status))
      return true;

    SecureZero(key, keySize);
    return false;
  }

  bool HMACSHA256(const void * key, std::size_t keySize,
    const void * data, std::size_t size,
    std::uint8_t output[SHA256_SIZE]) noexcept
  {
    if ((keySize && !key) || (size && !data) || keySize > INT_MAX || !output)
      return false;

    bool               result       = false;
    BCRYPT_ALG_HANDLE  algorithm    = nullptr;
    BCRYPT_HASH_HANDLE hash         = nullptr;
    PUCHAR             hashObject   = nullptr;
    ULONG              hashSize     = 0;
    ULONG              propertySize = 0;
    const auto *       input        = static_cast<const std::uint8_t *>(data);

    if (!OpenHMACSHA256(algorithm) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
          &propertySize, 0)) ||
        propertySize != sizeof(hashSize) || !hashSize)
      goto cleanup;

    hashObject = static_cast<PUCHAR>(
      HeapAlloc(GetProcessHeap(), 0, hashSize));
    if (!hashObject ||
        !BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hashObject,
          hashSize, InputBytes(key, keySize), static_cast<ULONG>(keySize), 0)))
      goto cleanup;

    while (size)
    {
      const ULONG chunk = size > ULONG_MAX ? ULONG_MAX :
        static_cast<ULONG>(size);
      if (!BCRYPT_SUCCESS(BCryptHashData(
          hash, const_cast<PUCHAR>(input), chunk, 0)))
        goto cleanup;

      input += chunk;
      size  -= chunk;
    }

    result = BCRYPT_SUCCESS(BCryptFinishHash(
      hash, output, static_cast<ULONG>(SHA256_SIZE), 0));

cleanup:
    if (hash)
      BCryptDestroyHash(hash);
    if (hashObject)
    {
      SecureZeroMemory(hashObject, hashSize);
      HeapFree(GetProcessHeap(), 0, hashObject);
    }
    if (algorithm)
      BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!result)
      SecureZero(output, SHA256_SIZE);
    return result;
  }

  bool ConstantTimeEqual(
    const void * left, const void * right, std::size_t size) noexcept
  {
    if (size && (!left || !right))
      return false;

    const volatile std::uint8_t * leftBytes  =
      static_cast<const volatile std::uint8_t *>(left);
    const volatile std::uint8_t * rightBytes =
      static_cast<const volatile std::uint8_t *>(right);
    volatile std::uint8_t         difference = 0;
    for (std::size_t index = 0; index < size; ++index)
    {
      difference = static_cast<std::uint8_t>(difference |
        static_cast<std::uint8_t>(leftBytes[index] ^ rightBytes[index]));
    }
    return difference == 0;
  }

  void SecureZero(void * data, std::size_t size) noexcept
  {
    if (data && size)
      SecureZeroMemory(data, size);
  }
}
