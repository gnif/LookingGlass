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

#include "postprocess/D12FrameFormat.h"

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <memory>
#include <stddef.h>
#include <stdint.h>

struct ControlToken
{
  uint32_t backend = 0;
  uint32_t epoch   = 0;
};

enum class ControlResult
{
  APPLIED,
  RETRY,
  FAILED,
};

class IControlEvents
{
public:
  virtual ~IControlEvents() = default;

  virtual void OnControlReplay(const ControlToken& token) = 0;
};

class IControlSink
{
public:
  virtual ~IControlSink() = default;

  // Passing nullptr is a callback-quiescence barrier. Once this returns, no
  // callback using the previous events pointer may still be running.
  virtual void SetControlEvents(
    IControlEvents * events, const ControlToken& token) = 0;

  // These calls perform at most one delivery attempt.
  virtual ControlResult SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
    const BYTE * data, size_t size, UINT sdrWhiteLevel) = 0;
  virtual ControlResult SetColorTransform(
    std::shared_ptr<const D12ColorTransform> transform) = 0;
};
