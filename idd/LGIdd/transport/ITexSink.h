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

#include "capture/CFrameTex.h"
#include "d3d/CInteropPool.h"
#include "transport/FrameIn.h"

class ITexSink
{
public:
  virtual ~ITexSink() = default;

  // Push is bounded and nonblocking. ACCEPTED transfers the self-contained
  // frame and lease to the transport; both remain retained until its
  // asynchronous GPU use completes. Other results retain neither object and
  // leave sibling routes and providers untouched. A later executor must make
  // the next accepted product full-damage after BUSY or another dropped
  // partial update.
  virtual PushResult Push(FrameIn frame, TexLease lease) noexcept = 0;
  virtual PushResult Push(FrameIn frame, D11Lease lease) noexcept = 0;
};
