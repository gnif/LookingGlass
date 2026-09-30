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

#include "../../fingerprint.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <limits.h>

bool networkFingerprintSHA256(const uint8_t * data, size_t size,
    uint8_t fingerprint[NETWORK_FINGERPRINT_SIZE])
{
  if (size > ULONG_MAX)
    return false;

  DWORD outputSize = NETWORK_FINGERPRINT_SIZE;
  return CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, NULL,
      data, (DWORD)size, fingerprint, &outputSize) &&
    outputSize == NETWORK_FINGERPRINT_SIZE;
}
