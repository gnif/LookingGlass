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

/*
 * The packet layout and block ordering validated here are adapted from the
 * PyroWave bitstream specification and reference parser at commit
 * 89f7e47d4abbf650c91fae766728af866c5e32a0.
 *
 * Copyright (c) 2025 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "pyrowave.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_PYROWAVE_HEADER_SIZE          8U
#define NETWORK_PYROWAVE_DECOMPOSITION_LEVELS 5U
#define NETWORK_PYROWAVE_ALIGNMENT            \
  (1U << NETWORK_PYROWAVE_DECOMPOSITION_LEVELS)
#define NETWORK_PYROWAVE_MINIMUM_SIZE         \
  (4U << NETWORK_PYROWAVE_DECOMPOSITION_LEVELS)
#define NETWORK_PYROWAVE_SEQUENCE_MASK        7U

static uint32_t networkPyroWaveReadU32(const uint8_t * data)
{
  return (uint32_t)data[0]        |
    (uint32_t)data[1] << 8        |
    (uint32_t)data[2] << 16       |
    (uint32_t)data[3] << 24;
}

static uint16_t networkPyroWaveReadU16(const uint8_t * data)
{
  return (uint16_t)((uint16_t)data[0] | (uint16_t)data[1] << 8);
}

static unsigned networkPyroWaveBitCount(uint32_t value)
{
  unsigned count = 0;
  while (value)
  {
    count += value & 1U;
    value >>= 1;
  }
  return count;
}

static bool networkPyroWavePacketValid(
    const uint8_t * packet, size_t packetSize, uint16_t ballot)
{
  const unsigned groups      = networkPyroWaveBitCount(ballot);
  const size_t   controlBase = NETWORK_PYROWAVE_HEADER_SIZE;
  const size_t   scaleBase   = controlBase + groups * sizeof(uint16_t);
  const size_t   planeBase   = scaleBase + groups;
  if (planeBase > packetSize)
    return false;

  size_t   planeOffset = planeBase;
  unsigned signs       = 0;
  for (unsigned group = 0; group < 16U; ++group)
  {
    if (!(ballot & (UINT32_C(1) << group)))
      continue;

    const unsigned compact = networkPyroWaveBitCount(
        ballot & ((UINT32_C(1) << group) - 1U));
    const uint16_t control = networkPyroWaveReadU16(
        packet + controlBase + compact * sizeof(uint16_t));
    const uint8_t  qBits   = packet[scaleBase + compact] & 15U;

    for (unsigned subblock = 0; subblock < 8U; ++subblock)
    {
      const unsigned planes = qBits +
        ((control >> (2U * subblock)) & 3U);
      if (planes > packetSize - planeOffset)
        return false;

      uint8_t nonzero = 0;
      for (unsigned plane = 0; plane < planes; ++plane)
        nonzero |= packet[planeOffset++];
      signs += networkPyroWaveBitCount(nonzero);
    }
  }

  const size_t signBytes = (signs + 7U) / 8U;
  if (signBytes > packetSize - planeOffset)
    return false;

  /* payload_words rounds the packet to a u32 boundary. No other trailing
   * payload is defined by version 1. */
  return packetSize - planeOffset - signBytes < sizeof(uint32_t);
}

static uint32_t networkPyroWaveAlign(uint32_t value, uint32_t alignment)
{
  return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool networkPyroWaveBlockCapacity(
    const LGNetVideoStreamConfig * config, uint32_t * capacity)
{
  if (!config || !capacity)
    return false;

  uint32_t width  = networkPyroWaveAlign(
      config->width, NETWORK_PYROWAVE_ALIGNMENT);
  uint32_t height = networkPyroWaveAlign(
      config->height, NETWORK_PYROWAVE_ALIGNMENT);
  if (width < NETWORK_PYROWAVE_MINIMUM_SIZE)
    width = NETWORK_PYROWAVE_MINIMUM_SIZE;
  if (height < NETWORK_PYROWAVE_MINIMUM_SIZE)
    height = NETWORK_PYROWAVE_MINIMUM_SIZE;

  uint64_t blocks = 0;
  for (int level = NETWORK_PYROWAVE_DECOMPOSITION_LEVELS - 1;
       level >= 0; --level)
  {
    const uint32_t levelWidth  = width  >> (level + 1);
    const uint32_t levelHeight = height >> (level + 1);
    const uint32_t blocksWide  = (levelWidth  + 31U) / 32U;
    const uint32_t blocksHigh  = (levelHeight + 31U) / 32U;
    const uint32_t bands       =
      level == NETWORK_PYROWAVE_DECOMPOSITION_LEVELS - 1 ? 4U : 3U;
    uint32_t       components  = 3U;
    if (!level && config->chromaSubsampling == LG_NET_VIDEO_CHROMA_420)
      components = 1U;

    blocks += (uint64_t)blocksWide * blocksHigh * bands * components;
  }

  if (!blocks || blocks > LG_NET_VIDEO_MAX_BLOCKS)
    return false;

  *capacity = (uint32_t)blocks;
  return true;
}

bool networkPyroWaveProfileValid(const LGNetVideoStreamConfig * config)
{
  if (!config || !lgNetVideoStreamConfigValid(config) ||
      config->codec != LG_NET_VIDEO_CODEC_PYROWAVE ||
      config->codecVersion < LG_NET_VIDEO_PYROWAVE_CODEC_VERSION_MIN ||
      config->codecVersion > LG_NET_VIDEO_PYROWAVE_CODEC_VERSION_MAX)
    return false;

  if (config->flags & LG_NET_VIDEO_STREAM_RESERVED_ALPHA)
    return false;

  const bool hdr =
    (config->flags & LG_NET_VIDEO_STREAM_HDR) != 0;
  if (hdr ?
      (config->colorPrimaries != LG_NET_COLOR_PRIMARIES_BT2020 ||
        config->colorTransfer != LG_NET_COLOR_TRANSFER_PQ ||
        config->colorMatrix != LG_NET_COLOR_MATRIX_BT2020) :
      (config->colorPrimaries != LG_NET_COLOR_PRIMARIES_BT709 ||
        config->colorTransfer != LG_NET_COLOR_TRANSFER_BT709 ||
        config->colorMatrix != LG_NET_COLOR_MATRIX_BT709))
    return false;

  uint32_t capacity;
  return networkPyroWaveBlockCapacity(config, &capacity);
}

static bool networkPyroWaveSequenceProfile(
    const LGNetVideoStreamConfig * config, uint32_t word)
{
  const uint32_t chroma          = (word >> 26) & 1U;
  const uint32_t primaries       = (word >> 27) & 1U;
  const uint32_t transfer        = (word >> 28) & 1U;
  const uint32_t transform       = (word >> 29) & 1U;
  const uint32_t range           = (word >> 30) & 1U;
  const uint32_t wireChroma      =
    config->chromaSubsampling == LG_NET_VIDEO_CHROMA_444 ? 1U : 0U;
  const uint32_t wirePrimaries   =
    config->colorPrimaries == LG_NET_COLOR_PRIMARIES_BT2020 ? 1U : 0U;
  const uint32_t wireTransfer    =
    config->colorTransfer == LG_NET_COLOR_TRANSFER_PQ ? 1U : 0U;
  const uint32_t wireTransform   =
    config->colorMatrix == LG_NET_COLOR_MATRIX_BT2020 ? 1U : 0U;
  const uint32_t wireRange       =
    config->colorRange == LG_NET_COLOR_RANGE_LIMITED ? 1U : 0U;

  return chroma    == wireChroma     &&
    primaries      == wirePrimaries &&
    transfer       == wireTransfer  &&
    transform      == wireTransform &&
    range          == wireRange;
}

NetworkPyroWaveResult networkPyroWaveParse(
    const LGNetVideoStreamConfig * config, const void * data, size_t size,
    uint32_t expectedBlocks, uint32_t * offsets, uint32_t offsetCount,
    NetworkPyroWaveLayout * layout)
{
  if (!networkPyroWaveProfileValid(config))
    return NETWORK_PYROWAVE_UNSUPPORTED;
  if (!data || size < NETWORK_PYROWAVE_HEADER_SIZE ||
      size > config->maxFrameLength || !expectedBlocks || !offsets || !layout)
    return NETWORK_PYROWAVE_INVALID;

  uint32_t capacity;
  if (!networkPyroWaveBlockCapacity(config, &capacity) ||
      expectedBlocks > capacity || offsetCount != capacity)
    return NETWORK_PYROWAVE_INVALID;

  memset(offsets, 0xff, (size_t)capacity * sizeof(*offsets));
  *layout = (NetworkPyroWaveLayout){0};

  const uint8_t * bytes          = data;
  size_t          cursor         = 0;
  uint32_t        uniqueBlocks   = 0;
  uint32_t        declaredBlocks = 0;
  uint32_t        headerSecond   = 0;
  uint8_t         frameSequence  = 0;
  bool            haveSequence   = false;
  bool            haveHeader     = false;
  bool            haveDamage     = false;
  bool            haveClear      = false;
  bool            valid          = true;

  while (valid && size - cursor >= NETWORK_PYROWAVE_HEADER_SIZE)
  {
    const uint32_t first          = networkPyroWaveReadU32(bytes + cursor);
    const uint32_t second         = networkPyroWaveReadU32(
        bytes + cursor + sizeof(uint32_t));
    const uint8_t  packetSequence =
      (uint8_t)((first >> 28) & NETWORK_PYROWAVE_SEQUENCE_MASK);
    const bool     extended       = (first >> 31) != 0;

    if (haveSequence && packetSequence != frameSequence)
    {
      valid = false;
      break;
    }
    haveSequence  = true;
    frameSequence = packetSequence;

    if (extended)
    {
      const uint32_t code   =
        (second >> LG_NET_VIDEO_PYROWAVE_MODE_SHIFT) &
          LG_NET_VIDEO_PYROWAVE_MODE_MASK;
      const uint32_t width  = (first & UINT32_C(0x00003FFF)) + 1U;
      const uint32_t height = ((first >> 14) & UINT32_C(0x00003FFF)) + 1U;
      const uint32_t blocks = second & UINT32_C(0x00FFFFFF);
      if ((code != LG_NET_VIDEO_PYROWAVE_MODE_FULL &&
            code != LG_NET_VIDEO_PYROWAVE_MODE_DELTA) ||
          width != config->width || height != config->height ||
          !blocks || blocks > capacity ||
          !networkPyroWaveSequenceProfile(config, second) ||
          haveHeader)
      {
        valid = false;
        break;
      }

      declaredBlocks = blocks;
      headerSecond   = second;
      haveHeader     = true;
      cursor         += NETWORK_PYROWAVE_HEADER_SIZE;

      if (code == LG_NET_VIDEO_PYROWAVE_MODE_DELTA)
      {
        size_t damageSize = 0;
        if (lgNetVideoDamageDecode(&layout->damage,
              bytes + cursor, size - cursor,
              config->width, config->height, &damageSize) != LG_NET_PARSE_OK)
        {
          valid = false;
          break;
        }
        cursor     += damageSize;
        haveDamage  = true;
      }
      continue;
    }

    if (!haveHeader)
    {
      valid = false;
      break;
    }

    const uint32_t ballot       = first & UINT32_C(0x0000FFFF);
    const uint32_t payloadWords = (first >> 16) & UINT32_C(0x00000FFF);
    const uint32_t blockIndex   = second >> 8;
    const size_t   packetSize   = (size_t)payloadWords * sizeof(uint32_t);
    const bool     clear        = ballot == 0;
    if (payloadWords < 2U || packetSize > size - cursor ||
        blockIndex >= capacity || offsets[blockIndex] != UINT32_MAX ||
        (clear ? payloadWords != LG_NET_VIDEO_PYROWAVE_CLEAR_PACKET_WORDS :
          !networkPyroWavePacketValid(
            bytes + cursor, packetSize, (uint16_t)ballot)))
    {
      valid = false;
      break;
    }

    offsets[blockIndex] = (uint32_t)(cursor / sizeof(uint32_t));
    if (clear)
      haveClear = true;
    ++uniqueBlocks;
    cursor += packetSize;
  }

  const bool delta =
    ((headerSecond >> LG_NET_VIDEO_PYROWAVE_MODE_SHIFT) &
      LG_NET_VIDEO_PYROWAVE_MODE_MASK) ==
        LG_NET_VIDEO_PYROWAVE_MODE_DELTA;
  valid = valid && cursor == size && haveHeader && haveSequence &&
    haveDamage == delta &&
    declaredBlocks == uniqueBlocks && expectedBlocks == uniqueBlocks &&
    (!haveClear || delta);
  if (!valid)
    return NETWORK_PYROWAVE_INVALID;

  layout->sequence   = frameSequence;
  layout->chromaLeft = (headerSecond >> 31) != 0;
  layout->delta      = delta;
  return NETWORK_PYROWAVE_OK;
}
