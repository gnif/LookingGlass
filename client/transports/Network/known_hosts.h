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

#include "fingerprint.h"

#include "common/locking.h"

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_FINGERPRINT_TEXT_SIZE (NETWORK_FINGERPRINT_SIZE * 2U + 1U)

typedef enum NetworkKnownHostResult
{
  NETWORK_KNOWN_HOST_UNKNOWN,
  NETWORK_KNOWN_HOST_MATCH,
  NETWORK_KNOWN_HOST_CHANGED,
  NETWORK_KNOWN_HOST_ERROR,
}
NetworkKnownHostResult;

typedef struct NetworkKnownHosts
{
  LG_Lock   lock;
  char    * path;
}
NetworkKnownHosts;

bool networkKnownHostsInit(NetworkKnownHosts * knownHosts);
void networkKnownHostsFree(NetworkKnownHosts * knownHosts);
NetworkKnownHostResult networkKnownHostsCheck(NetworkKnownHosts * knownHosts,
    const char * endpoint, const char * fingerprint);
bool networkKnownHostsRemember(NetworkKnownHosts * knownHosts,
    const char * endpoint, const char * fingerprint);
bool networkKnownHostsForget(NetworkKnownHosts * knownHosts,
    const char * endpoint);

void networkFingerprintFormat(
    const uint8_t fingerprint[NETWORK_FINGERPRINT_SIZE],
    char text[NETWORK_FINGERPRINT_TEXT_SIZE]);
