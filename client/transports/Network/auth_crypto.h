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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_AUTH_SHA256_SIZE 32U

bool networkAuthRandomBytes(void * data, size_t size);
bool networkAuthPBKDF2HMACSHA256(const void * password,
    size_t passwordSize, const void * salt, size_t saltSize,
    uint32_t iterations, void * key, size_t keySize);
bool networkAuthHMACSHA256(const void * key, size_t keySize,
    const void * data, size_t size,
    uint8_t output[NETWORK_AUTH_SHA256_SIZE]);
bool networkAuthConstantTimeEqual(
    const void * left, const void * right, size_t size);
void networkAuthSecureZero(void * data, size_t size);
