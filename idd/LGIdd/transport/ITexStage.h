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

#include "transport/FrameProfile.h"

class CFrameGraph;

// The future graph executor stages every fallible pool and interop resource
// here. Commit and Abort are infallible so transport configuration and graph
// activation remain one transaction. A non-ACCEPTED Prep leaves no pending
// state; an ACCEPTED Prep remains dormant until Commit. Commit publishes the
// new producer only after texture admission has opened. Abort may resume the
// old producer because the old admission set is reopened first.
class ITexStage
{
public:
  virtual ~ITexStage() = default;
  virtual CfgResult Prep(const CFrameGraph& graph) noexcept = 0;
  virtual void Commit() noexcept = 0;
  virtual void Abort() noexcept = 0;
};
