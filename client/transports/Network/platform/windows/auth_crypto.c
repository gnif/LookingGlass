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

#include "../../auth_crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <limits.h>

static const uint8_t EMPTY_INPUT = 0;

static PUCHAR inputBytes(const void * data, size_t size)
{
  return (PUCHAR)(size ? data : &EMPTY_INPUT);
}

static bool openHMACSHA256(BCRYPT_ALG_HANDLE * algorithm)
{
  return algorithm && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(algorithm,
    BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG));
}

bool networkAuthRandomBytes(void * data, size_t size)
{
  if (size && !data)
    return false;

  uint8_t *    output    = (uint8_t *)data;
  const size_t totalSize = size;
  while (size)
  {
    const ULONG chunk = size > ULONG_MAX ? ULONG_MAX : (ULONG)size;
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, output, chunk,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    {
      networkAuthSecureZero(data, totalSize);
      return false;
    }

    output += chunk;
    size   -= chunk;
  }

  return true;
}

bool networkAuthPBKDF2HMACSHA256(const void * password,
    size_t passwordSize, const void * salt, size_t saltSize,
    uint32_t iterations, void * key, size_t keySize)
{
  if ((passwordSize && !password) || (saltSize && !salt) || !iterations ||
      !key || !keySize || passwordSize > INT_MAX || saltSize > INT_MAX ||
      keySize > INT_MAX || iterations > INT_MAX)
    return false;

  BCRYPT_ALG_HANDLE algorithm = NULL;
  if (!openHMACSHA256(&algorithm))
  {
    networkAuthSecureZero(key, keySize);
    return false;
  }

  const NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm,
    inputBytes(password, passwordSize), (ULONG)passwordSize,
    inputBytes(salt, saltSize), (ULONG)saltSize, iterations,
    (PUCHAR)key, (ULONG)keySize, 0);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (BCRYPT_SUCCESS(status))
    return true;

  networkAuthSecureZero(key, keySize);
  return false;
}

bool networkAuthHMACSHA256(const void * key, size_t keySize,
    const void * data, size_t size,
    uint8_t output[NETWORK_AUTH_SHA256_SIZE])
{
  if ((keySize && !key) || (size && !data) || keySize > INT_MAX || !output)
    return false;

  bool                  result       = false;
  BCRYPT_ALG_HANDLE     algorithm    = NULL;
  BCRYPT_HASH_HANDLE    hash         = NULL;
  PUCHAR                hashObject   = NULL;
  ULONG                 hashSize     = 0;
  ULONG                 propertySize = 0;
  const uint8_t *       input        = (const uint8_t *)data;

  if (!openHMACSHA256(&algorithm) ||
      !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&hashSize, sizeof(hashSize), &propertySize, 0)) ||
      propertySize != sizeof(hashSize) || !hashSize)
    goto cleanup;

  hashObject = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, hashSize);
  if (!hashObject ||
      !BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, hashObject,
        hashSize, inputBytes(key, keySize), (ULONG)keySize, 0)))
    goto cleanup;

  while (size)
  {
    const ULONG chunk = size > ULONG_MAX ? ULONG_MAX : (ULONG)size;
    if (!BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)input, chunk, 0)))
      goto cleanup;

    input += chunk;
    size  -= chunk;
  }

  result = BCRYPT_SUCCESS(BCryptFinishHash(
    hash, output, NETWORK_AUTH_SHA256_SIZE, 0));

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
    networkAuthSecureZero(output, NETWORK_AUTH_SHA256_SIZE);
  return result;
}

bool networkAuthConstantTimeEqual(
    const void * left, const void * right, size_t size)
{
  if (size && (!left || !right))
    return false;

  const volatile uint8_t * leftBytes  = (const volatile uint8_t *)left;
  const volatile uint8_t * rightBytes = (const volatile uint8_t *)right;
  volatile uint8_t         difference = 0;
  for (size_t index = 0; index < size; ++index)
    difference |= leftBytes[index] ^ rightBytes[index];
  return difference == 0;
}

void networkAuthSecureZero(void * data, size_t size)
{
  if (data && size)
    SecureZeroMemory(data, size);
}
