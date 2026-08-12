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

#include <stdint.h>
#include <string>
#include <vector>

using BackendId = uint32_t;

enum TransportService : uint32_t
{
  TRANSPORT_SERVICE_FRAME   = 1U << 0,
  TRANSPORT_SERVICE_CONTROL = 1U << 1,
  TRANSPORT_SERVICE_INPUT   = 1U << 2,
  TRANSPORT_SERVICE_ALL     = TRANSPORT_SERVICE_FRAME |
    TRANSPORT_SERVICE_CONTROL | TRANSPORT_SERVICE_INPUT,
};

struct TransportInstance
{
  BackendId   id       = 0;
  std::wstring kind;
  bool         enabled  = true;
  bool         required = false;
  uint32_t     services = TRANSPORT_SERVICE_ALL;
  int32_t      priority = 0;
  std::wstring settings;
};

using TransportInstances = std::vector<TransportInstance>;

static const unsigned TRANSPORT_MAX_INSTANCES = 8;

struct TransportKind
{
  const wchar_t * name;
  unsigned        activeLimit;
};

struct ResolvedTransportInstance
{
  TransportInstance config;
  unsigned          kindIndex = 0;
  bool              primary   = false;
};

using ResolvedTransportInstances =
  std::vector<ResolvedTransportInstance>;

TransportInstances DefaultTransportInstances();
bool ParseTransportInstances(const std::vector<std::wstring>& entries,
  TransportInstances& instances);
bool ResolveTransportInstances(const TransportInstances& configured,
  const TransportKind * kinds, unsigned kindCount,
  ResolvedTransportInstances& resolved, bool& usedDefaults);
