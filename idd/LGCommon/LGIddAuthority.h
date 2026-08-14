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
#include <winioctl.h>
#include <stdint.h>

static const GUID GUID_DEVINTERFACE_LGIdd =
  { 0x997b0b66, 0xb74c, 0x4017,
    { 0x9a, 0x89, 0xe4, 0xaa, 0xd4, 0x1d, 0x37, 0x80 } };

static constexpr uint32_t LG_IDD_AUTHORITY_VERSION = 1U;

static constexpr DWORD IOCTL_LG_IDD_AUTHORITY_GET_HOST =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);
static constexpr DWORD IOCTL_LG_IDD_AUTHORITY_REGISTER =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);
static constexpr DWORD IOCTL_LG_IDD_AUTHORITY_CLEAR    =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED,
    FILE_READ_ACCESS | FILE_WRITE_ACCESS);

struct LGIddAuthorityHost
{
  uint32_t size;
  uint32_t version;
  uint32_t processId;
  uint32_t reserved;
  uint64_t processCreated;
  uint64_t instanceId[2];
};

struct LGIddAuthorityRegistration
{
  uint32_t size;
  uint32_t version;
  uint32_t session;
  uint32_t reserved;
  uint64_t instanceId[2];
  uint64_t mappingId[2];
  uint64_t mappingHandle;
};

struct LGIddAuthorityClear
{
  uint32_t size;
  uint32_t version;
  uint64_t instanceId[2];
};

static_assert(sizeof(LGIddAuthorityHost) == 40,
  "LGIdd authority host layout changed");
static_assert(sizeof(LGIddAuthorityRegistration) == 56,
  "LGIdd authority registration layout changed");
static_assert(sizeof(LGIddAuthorityClear) == 24,
  "LGIdd authority clear layout changed");
