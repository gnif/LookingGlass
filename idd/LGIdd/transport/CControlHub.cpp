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

#include "transport/CControlHub.h"

#include <cstring>
#include <new>
#include <utility>

std::vector<std::shared_ptr<CControlHub::Sink>> CControlHub::Snapshot() const
{
  CSRWSharedLock lock(m_sinkLock);
  return m_sinks;
}

bool CControlHub::Add(
  BackendId backend, uint32_t epoch, IControlTransport& control)
{
  std::shared_ptr<Sink> sink(new (std::nothrow) Sink);
  if (!sink)
    return false;

  sink->backend = backend;
  sink->epoch   = epoch;
  sink->control = &control;
  {
    CSRWExclusiveLock lock(m_sinkLock);
    for (const auto& current : m_sinks)
      if (current->backend == backend && current->epoch == epoch)
        return false;
    m_sinks.push_back(sink);
  }

  Replay(sink);
  return true;
}

void CControlHub::Remove(BackendId backend, uint32_t epoch)
{
  std::shared_ptr<Sink> removed;
  {
    CSRWExclusiveLock lock(m_sinkLock);
    for (auto current = m_sinks.begin(); current != m_sinks.end(); ++current)
      if ((*current)->backend == backend && (*current)->epoch == epoch)
      {
        removed = *current;
        m_sinks.erase(current);
        break;
      }
  }

  if (removed)
  {
    CSRWExclusiveLock lock(removed->lock);
    removed->control = nullptr;
  }
}

void CControlHub::Replay(const std::shared_ptr<Sink>& sink)
{
  IDARG_OUT_QUERY_HWCURSOR cursor = {};
  std::vector<BYTE> cursorData;
  UINT sdrWhiteLevel;
  bool cursorValid;
  std::shared_ptr<const D12ColorTransform> transform;
  {
    CSRWSharedLock lock(m_stateLock);
    cursor         = m_cursor;
    cursorData     = m_cursorData;
    sdrWhiteLevel  = m_sdrWhiteLevel;
    cursorValid    = m_cursorValid;
    transform      = m_colorTransform;
  }

  CSRWSharedLock lock(sink->lock);
  if (!sink->control)
    return;
  sink->control->SetColorTransform(std::move(transform));
  if (cursorValid)
    sink->control->SendCursor(
      cursor, cursorData.empty() ? nullptr : cursorData.data(),
      sdrWhiteLevel);
}

void CControlHub::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
  const BYTE * data, UINT sdrWhiteLevel)
{
  {
    CSRWExclusiveLock lock(m_stateLock);
    m_cursor.IsCursorVisible = info.IsCursorVisible;
    m_cursor.X               = info.X;
    m_cursor.Y               = info.Y;
    m_cursorValid            = true;
    m_sdrWhiteLevel          = sdrWhiteLevel;

    if (info.CursorShapeInfo.CursorType !=
        IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
    {
      const size_t size =
        (size_t)info.CursorShapeInfo.Height * info.CursorShapeInfo.Pitch;
      m_cursor.IsCursorShapeUpdated = info.IsCursorShapeUpdated;
      m_cursor.CursorShapeInfo      = info.CursorShapeInfo;
      m_cursorData.resize(size);
      if (size)
        memcpy(m_cursorData.data(), data, size);
      m_shapeValid = true;
    }
    else if (!m_shapeValid)
      m_cursor.CursorShapeInfo.CursorType =
        IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED;
  }

  const auto sinks = Snapshot();
  for (const auto& sink : sinks)
  {
    CSRWSharedLock lock(sink->lock);
    if (sink->control)
      sink->control->SendCursor(info, data, sdrWhiteLevel);
  }
}

void CControlHub::SetColorTransform(
  std::shared_ptr<const D12ColorTransform> transform)
{
  {
    CSRWExclusiveLock lock(m_stateLock);
    m_colorTransform = transform;
  }

  const auto sinks = Snapshot();
  for (const auto& sink : sinks)
  {
    CSRWSharedLock lock(sink->lock);
    if (sink->control)
      sink->control->SetColorTransform(transform);
  }
}

std::shared_ptr<const D12ColorTransform>
CControlHub::GetColorTransform() const
{
  CSRWSharedLock lock(m_stateLock);
  return m_colorTransform;
}
