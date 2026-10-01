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

#include <LGProtocol/NetworkProtocol.h>

#include <stdint.h>
#include <string>

static constexpr uint16_t                    NETWORK_DEFAULT_MAX_CLIENTS  = 4;
static constexpr uint16_t                    NETWORK_MAX_CLIENTS          = 32;
static constexpr LGNetVideoChromaSubsampling NETWORK_DEFAULT_VIDEO_CHROMA =
  static_cast<LGNetVideoChromaSubsampling>(LG_NET_VIDEO_CHROMA_444);

struct NetworkSettings
{
  std::wstring                  bindAddress = L"*";
  uint16_t                      port        =
    static_cast<uint16_t>(LG_NET_DEFAULT_PORT);
  uint16_t                      maxClients  = NETWORK_DEFAULT_MAX_CLIENTS;
  LGNetAuthMode                 auth        = LG_NET_AUTH_MODE_DEFAULT;
  std::wstring                  password;
  LGNetVideoChromaSubsampling   videoChroma =
    NETWORK_DEFAULT_VIDEO_CHROMA;
};

// Settings use a semicolon-separated key=value list. Supported keys are
// bind, port, maxClients, auth, password, and videoChroma. videoChroma accepts
// 420 or 444 and defaults to 444. Password authentication requires a non-empty
// password. The default address requests a dual-stack wildcard listener from
// the eventual QUIC provider.
bool ParseNetworkSettings(const std::wstring& source,
  NetworkSettings& settings, std::wstring * error = nullptr);
