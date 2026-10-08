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

#include <LGProtocol/NetworkServices.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum NetworkPyroWaveResult
{
  NETWORK_PYROWAVE_OK,
  NETWORK_PYROWAVE_INVALID,
  NETWORK_PYROWAVE_UNSUPPORTED,
}
NetworkPyroWaveResult;

typedef struct NetworkPyroWaveLayout
{
  uint8_t          sequence;
  bool             chromaLeft;
  bool             delta;
  LGNetVideoDamage damage;
}
NetworkPyroWaveLayout;

bool networkPyroWaveProfileValid(const LGNetVideoStreamConfig * config);

NetworkPyroWaveResult networkPyroWaveParse(
    const LGNetVideoStreamConfig * config, const void * data, size_t size,
    uint32_t expectedBlocks, uint32_t * offsets, uint32_t offsetCount,
    NetworkPyroWaveLayout * layout);
