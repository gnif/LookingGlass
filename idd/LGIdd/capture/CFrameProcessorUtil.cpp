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

#include "capture/CFrameProcessorUtil.h"
#include "d3d/CInteropResource.h"
#include "postprocess/CPostProcessor.h"

#include <cstring>

bool CFrameProcessorUtil::FrameMetadataChanged(
  const D12FrameFormat& previous, const D12FrameFormat& current)
{
  return
    previous.hdrMetadata   != current.hdrMetadata   ||
    previous.sdrWhiteLevel != current.sdrWhiteLevel ||
    (current.hdrMetadata &&
      (memcmp(previous.displayPrimary, current.displayPrimary,
         sizeof(current.displayPrimary)) != 0 ||
       memcmp(previous.whitePoint, current.whitePoint,
         sizeof(current.whitePoint)) != 0 ||
       previous.maxDisplayLuminance       != current.maxDisplayLuminance       ||
       previous.minDisplayLuminance       != current.minDisplayLuminance       ||
       previous.maxContentLightLevel      != current.maxContentLightLevel      ||
       previous.maxFrameAverageLightLevel != current.maxFrameAverageLightLevel));
}

FrameType CFrameProcessorUtil::GetFrameType(DXGI_FORMAT format)
{
  switch (format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM    : return FRAME_TYPE_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM    : return FRAME_TYPE_RGBA;
    case DXGI_FORMAT_R10G10B10A2_UNORM : return FRAME_TYPE_RGBA10;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FRAME_TYPE_RGBA16F;
    default                            : return FRAME_TYPE_INVALID;
  }
}

bool CFrameProcessorUtil::ResourceDescMatches(
  const D3D12_RESOURCE_DESC& left, const D3D12_RESOURCE_DESC& right,
  bool compareAlignment)
{
  // GetDesc may report a resolved alignment when resource creation requested
  // automatic alignment, so callers comparing creation descriptors can omit
  // this allocation metadata.
  return
    left.Dimension          == right.Dimension          &&
    (!compareAlignment || left.Alignment == right.Alignment) &&
    left.Width              == right.Width              &&
    left.Height             == right.Height             &&
    left.DepthOrArraySize   == right.DepthOrArraySize   &&
    left.MipLevels          == right.MipLevels          &&
    left.Format             == right.Format             &&
    left.SampleDesc.Count   == right.SampleDesc.Count   &&
    left.SampleDesc.Quality == right.SampleDesc.Quality &&
    left.Layout             == right.Layout             &&
    left.Flags              == right.Flags;
}

static bool IsFullDamage(const RECT * dirtyRects, unsigned nbDirtyRects,
  unsigned width, unsigned height)
{
  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
    if (rect->left   == 0            &&
        rect->top    == 0            &&
        rect->right  == (LONG)width  &&
        rect->bottom == (LONG)height)
      return true;

  return false;
}

static bool DirtyRectContains(const RECT& outer, const RECT& inner)
{
  return outer.left   <= inner.left  &&
         outer.top    <= inner.top   &&
         outer.right  >= inner.right &&
         outer.bottom >= inner.bottom;
}

static bool DirtyRectsTouchOrIntersect(const RECT& a, const RECT& b)
{
  return a.left <= b.right  && a.right  >= b.left &&
         a.top  <= b.bottom && a.bottom >= b.top;
}

static RECT MergeDirtyRects(const RECT& a, const RECT& b)
{
  RECT result;
  result.left   = min(a.left  , b.left  );
  result.top    = min(a.top   , b.top   );
  result.right  = max(a.right , b.right );
  result.bottom = max(a.bottom, b.bottom);
  return result;
}

static uint64_t DirtyRectArea(const RECT& rect)
{
  const uint64_t width  = (uint64_t)((int64_t)rect.right  - rect.left);
  const uint64_t height = (uint64_t)((int64_t)rect.bottom - rect.top );
  return width * height;
}

static bool AddCopyDirtyRect(RECT dirtyRects[], unsigned capacity,
  unsigned * nbDirtyRects, const RECT& dirtyRect)
{
  RECT candidate = dirtyRect;
  for (unsigned i = 0; i < *nbDirtyRects;)
  {
    if (DirtyRectContains(dirtyRects[i], candidate))
      return true;

    const RECT merged = MergeDirtyRects(dirtyRects[i], candidate);
    if (DirtyRectContains(candidate, dirtyRects[i]) ||
        (DirtyRectsTouchOrIntersect(dirtyRects[i], candidate) &&
          DirtyRectArea(merged) <=
            DirtyRectArea(dirtyRects[i]) + DirtyRectArea(candidate)))
    {
      candidate = merged;
      --(*nbDirtyRects);
      dirtyRects[i] = dirtyRects[*nbDirtyRects];
      i = 0;
      continue;
    }

    ++i;
  }

  if (*nbDirtyRects >= capacity)
    return false;

  dirtyRects[(*nbDirtyRects)++] = candidate;
  return true;
}

static bool CopyAreaCoversFrame(const RECT * dirtyRects,
  unsigned nbDirtyRects, unsigned width, unsigned height)
{
  const uint64_t frameArea = (uint64_t)width * height;
  uint64_t       copyArea  = 0;

  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
  {
    const uint64_t area = DirtyRectArea(*rect);
    if (area >= frameArea - copyArea)
      return true;
    copyArea += area;
  }

  return false;
}

static bool ClipDirtyRect(RECT& rect, unsigned width, unsigned height)
{
  const LONG maxRight  = (LONG)width;
  const LONG maxBottom = (LONG)height;

  if (rect.left   < 0        ) rect.left   = 0;
  if (rect.top    < 0        ) rect.top    = 0;
  if (rect.right  > maxRight ) rect.right  = maxRight;
  if (rect.bottom > maxBottom) rect.bottom = maxBottom;

  return rect.left < rect.right && rect.top < rect.bottom;
}

void CFrameProcessorUtil::ClipDirtyRects(
  RECT dirtyRects[], unsigned * nbDirtyRects,
  unsigned width, unsigned height)
{
  unsigned out = 0;
  for (unsigned i = 0; i < *nbDirtyRects; ++i)
  {
    RECT rect = dirtyRects[i];
    if (ClipDirtyRect(rect, width, height))
      dirtyRects[out++] = rect;
  }
  *nbDirtyRects = out;
}

bool CFrameProcessorUtil::BuildCopyDamage(
  const CPostProcessor& postProcessor, bool destinationNeedsFullCopy,
  const RECT previousDirtyRects[], unsigned nbPreviousDirtyRects,
  const RECT currentDirtyRects[], unsigned nbCurrentDirtyRects,
  unsigned width, unsigned height,
  RECT copyDirtyRects[], unsigned * nbCopyDirtyRects)
{
  *nbCopyDirtyRects = 0;
  bool fullCopy = destinationNeedsFullCopy ||
    nbCurrentDirtyRects == 0 || nbPreviousDirtyRects == 0;

  if (fullCopy)
    return true;

  for (const RECT * rect = previousDirtyRects;
       rect < previousDirtyRects + nbPreviousDirtyRects && !fullCopy;
       ++rect)
  {
    RECT clipped = *rect;
    if (ClipDirtyRect(clipped, width, height) &&
        !AddCopyDirtyRect(copyDirtyRects, LG_MAX_DIRTY_RECTS * 2,
          nbCopyDirtyRects, clipped))
      fullCopy = true;
  }

  for (const RECT * rect = currentDirtyRects;
       rect < currentDirtyRects + nbCurrentDirtyRects && !fullCopy;
       ++rect)
    if (!AddCopyDirtyRect(copyDirtyRects, LG_MAX_DIRTY_RECTS * 2,
        nbCopyDirtyRects, *rect))
      fullCopy = true;

  if (!fullCopy)
    fullCopy = IsFullDamage(copyDirtyRects, *nbCopyDirtyRects,
        width, height) ||
      CopyAreaCoversFrame(copyDirtyRects, *nbCopyDirtyRects,
        width, height);

  if (!fullCopy)
    fullCopy = postProcessor.ShouldCopyFully(
      copyDirtyRects, *nbCopyDirtyRects);

  return fullCopy;
}

void CFrameProcessorUtil::AccumulateDamage(
  RECT pendingDirtyRects[], unsigned * nbPendingDirtyRects,
  bool * hasPendingDamage, const RECT dirtyRects[],
  unsigned nbDirtyRects)
{
  if (nbDirtyRects > LG_MAX_DIRTY_RECTS)
    nbDirtyRects = 0;

  if (!*hasPendingDamage)
  {
    *hasPendingDamage    = true;
    *nbPendingDirtyRects = nbDirtyRects;
    if (nbDirtyRects)
      memcpy(pendingDirtyRects, dirtyRects,
        nbDirtyRects * sizeof(*pendingDirtyRects));
    return;
  }

  if (*nbPendingDirtyRects == 0 || nbDirtyRects == 0 ||
      *nbPendingDirtyRects + nbDirtyRects > LG_MAX_DIRTY_RECTS)
  {
    *nbPendingDirtyRects = 0;
    return;
  }

  memcpy(pendingDirtyRects + *nbPendingDirtyRects, dirtyRects,
    nbDirtyRects * sizeof(*pendingDirtyRects));
  *nbPendingDirtyRects += nbDirtyRects;
}
