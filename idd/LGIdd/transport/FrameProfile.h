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
#include <stdint.h>

static const unsigned FRAME_PROFILE_MAX = 16;

enum class FrameStorage : uint8_t
{
  D3D12_TEXTURE,
  D3D11_TEXTURE,
};

enum class GpuMode : uint8_t
{
  HARDWARE,
  SOFTWARE,
};

enum class FramePixel : uint8_t
{
  BGRA8,
  RGBA8,
  RGB10A2,
  RGBA16F,
};

enum class FrameSignal : uint8_t
{
  SRGB,
  PQ_BT2020,
  SCRGB_LINEAR,
};

struct FrameProfile
{
  // Transport frame inputs are always Direct3D textures.
  FrameStorage storage = FrameStorage::D3D12_TEXTURE;
  FramePixel   pixel   = FramePixel::BGRA8;
  FrameSignal  signal  = FrameSignal::SRGB;
};

struct FrameCfg
{
  // Software mode still provides a Direct3D texture to the transport.
  GpuMode     mode    = GpuMode::HARDWARE;
  LUID        adapter = {};
  unsigned    width   = 0;
  unsigned    height  = 0;
  FrameProfile profile;
};

namespace Frame
{
  inline bool Same(const LUID& left, const LUID& right)
  {
    return left.LowPart  == right.LowPart &&
           left.HighPart == right.HighPart;
  }

  inline bool Same(const FrameProfile& left, const FrameProfile& right)
  {
    return left.storage == right.storage &&
           left.pixel   == right.pixel   &&
           left.signal  == right.signal;
  }

  inline bool Same(const FrameCfg& left, const FrameCfg& right)
  {
    return
      left.mode    == right.mode                         &&
      Same(left.adapter, right.adapter)                  &&
      left.width   == right.width                        &&
      left.height  == right.height                       &&
      Same(left.profile, right.profile);
  }

  inline bool Valid(GpuMode mode)
  {
    return mode == GpuMode::HARDWARE || mode == GpuMode::SOFTWARE;
  }

  inline bool Valid(FrameStorage storage)
  {
    return storage == FrameStorage::D3D12_TEXTURE ||
           storage == FrameStorage::D3D11_TEXTURE;
  }

  inline bool Valid(FramePixel pixel, FrameSignal signal)
  {
    switch (signal)
    {
      case FrameSignal::SRGB:
        return pixel == FramePixel::BGRA8 || pixel == FramePixel::RGBA8;

      case FrameSignal::SCRGB_LINEAR:
        return pixel == FramePixel::RGBA16F;

      case FrameSignal::PQ_BT2020:
        return pixel == FramePixel::RGB10A2;
    }
    return false;
  }

  inline bool Valid(const FrameProfile& profile)
  {
    return Valid(profile.storage) && Valid(profile.pixel, profile.signal);
  }

  inline bool Can(FrameSignal source, FrameSignal target)
  {
    switch (source)
    {
      case FrameSignal::SRGB:
        return target == FrameSignal::SRGB;

      case FrameSignal::SCRGB_LINEAR:
        return target == FrameSignal::SCRGB_LINEAR ||
               target == FrameSignal::PQ_BT2020;

      case FrameSignal::PQ_BT2020:
        return target == FrameSignal::PQ_BT2020;
    }
    return false;
  }

  inline FrameProfile Store(
    const FrameProfile& profile, FrameStorage storage)
  {
    FrameProfile result = profile;
    result.storage = storage;
    return result;
  }
}

enum class CfgResult : uint8_t
{
  // The proposed route may be prepared or committed.
  ACCEPTED,
  // The transport declined this profile and permits another candidate.
  NEXT,
  // The route is unavailable without affecting sibling routes.
  REJECTED,
  // Preserve the active route and offer the proposal again later.
  RETRY,
  // The frame component failed; required policy decides escalation.
  FAILED,
};
