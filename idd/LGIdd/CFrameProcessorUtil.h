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

#include "CInteropResource.h"
#include "CPostProcessor.h"

class CFrameProcessorSharedLock
{
private:
  SRWLOCK * m_lock;

public:
  explicit CFrameProcessorSharedLock(SRWLOCK * lock) : m_lock(lock)
  {
    AcquireSRWLockShared(m_lock);
  }

  ~CFrameProcessorSharedLock()
  {
    ReleaseSRWLockShared(m_lock);
  }
};

class CFrameProcessorUtil
{
public:
  static bool FrameMetadataChanged(const D12FrameFormat& previous,
    const D12FrameFormat& current);
  static FrameType GetFrameType(DXGI_FORMAT format);
  static bool ResourceDescMatches(const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right, bool compareAlignment = true);
  static void ClipDirtyRects(RECT dirtyRects[], unsigned * nbDirtyRects,
    unsigned width, unsigned height);
  static bool BuildCopyDamage(const CPostProcessor& postProcessor,
    bool destinationNeedsFullCopy,
    const RECT previousDirtyRects[], unsigned nbPreviousDirtyRects,
    const RECT currentDirtyRects[], unsigned nbCurrentDirtyRects,
    unsigned width, unsigned height,
    RECT copyDirtyRects[], unsigned * nbCopyDirtyRects);
  static void AccumulateDamage(
    RECT pendingDirtyRects[], unsigned * nbPendingDirtyRects,
    bool * hasPendingDamage, const RECT dirtyRects[],
    unsigned nbDirtyRects);
};
