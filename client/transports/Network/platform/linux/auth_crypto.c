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

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <limits.h>

static const uint8_t EMPTY_INPUT = 0;

static const uint8_t * inputBytes(const void * data, size_t size)
{
  return size ? (const uint8_t *)data : &EMPTY_INPUT;
}

bool networkAuthRandomBytes(void * data, size_t size)
{
  if (size && !data)
    return false;

  uint8_t *    output    = (uint8_t *)data;
  const size_t totalSize = size;
  while (size)
  {
    const int chunk = size > INT_MAX ? INT_MAX : (int)size;
    if (RAND_bytes(output, chunk) != 1)
    {
      networkAuthSecureZero(data, totalSize);
      return false;
    }

    output += chunk;
    size   -= (size_t)chunk;
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

  const int result = PKCS5_PBKDF2_HMAC(
    (const char *)inputBytes(password, passwordSize), (int)passwordSize,
    inputBytes(salt, saltSize), (int)saltSize, (int)iterations,
    EVP_sha256(), (int)keySize, (uint8_t *)key);
  if (result == 1)
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

  unsigned int outputSize = 0;
  if (HMAC(EVP_sha256(), inputBytes(key, keySize), (int)keySize,
      inputBytes(data, size), size, output, &outputSize) &&
      outputSize == NETWORK_AUTH_SHA256_SIZE)
    return true;

  networkAuthSecureZero(output, NETWORK_AUTH_SHA256_SIZE);
  return false;
}

bool networkAuthConstantTimeEqual(
    const void * left, const void * right, size_t size)
{
  if (size && (!left || !right))
    return false;
  if (!size)
    return true;

  return CRYPTO_memcmp(left, right, size) == 0;
}

void networkAuthSecureZero(void * data, size_t size)
{
  if (data && size)
    OPENSSL_cleanse(data, size);
}
