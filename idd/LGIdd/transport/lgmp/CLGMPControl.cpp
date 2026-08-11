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

#include "transport/lgmp/CLGMPControl.h"

#include "CDebug.h"

#include <string.h>

#include <utility>

static const uint32_t MAX_POINTER_SIZE =
  (uint32_t)(sizeof(KVMFRCursor) + (512 * 512 * 4));

static const struct LGMPQueueConfig POINTER_QUEUE_CONFIG =
{
  LGMP_Q_POINTER,     //queueID
  LGMP_Q_POINTER_LEN, //numMesages
  1000                //subTimeout
};

CLGMPControl::~CLGMPControl()
{
  DeInit();
}

bool CLGMPControl::Initialize()
{
  if (m_pointerQueue)
    return true;

  LGMP_STATUS status;
  if ((status = m_host.CreateQueue(
      POINTER_QUEUE_CONFIG, &m_pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Pointer): %s",
      lgmpStatusString(status));
    return false;
  }

  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = m_host.Allocate(
        MAX_POINTER_SIZE, &m_pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s",
        lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
  {
    if ((status = m_host.Allocate(
        MAX_POINTER_SIZE, &m_pointerShapeMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shapes): %s",
        lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerShapeMemory[i]), 0, MAX_POINTER_SIZE);
  }

  for (int i = 0; i < COLOR_TRANSFORM_BUFFERS; ++i)
  {
    if ((status = m_host.Allocate(
        sizeof(KVMFRCursor) + sizeof(KVMFRColorTransform),
        &m_pointerTransformMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Transform): %s",
        lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerTransformMemory[i]), 0,
      sizeof(KVMFRCursor) + sizeof(KVMFRColorTransform));
  }

  return true;
}

void CLGMPControl::DeInit()
{
  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&m_pointerMemory[i]);
  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerShapeMemory[i]);
  for (int i = 0; i < COLOR_TRANSFORM_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerTransformMemory[i]);

  m_pointerQueue          = nullptr;
  m_pointerShape          = nullptr;
  m_pointerMemoryIndex    = 0;
  m_pointerShapeIndex     = 0;
  m_pointerTransformIndex = 0;
}

LGMP_STATUS CLGMPControl::ReadDataWithSource(void * data, size_t * size,
  uint32_t * sourceClientID)
{
  return lgmpHostReadDataWithSource(
    m_pointerQueue, data, size, sourceClientID);
}

LGMP_STATUS CLGMPControl::AckData()
{
  return lgmpHostAckData(m_pointerQueue);
}

bool CLGMPControl::HasNewSubscribers()
{
  return lgmpHostQueueNewSubs(m_pointerQueue) != 0;
}

void CLGMPControl::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
  const BYTE * data, UINT sdrWhiteLevel)
{
  PLGMPMemory mem;
  if (info.CursorShapeInfo.CursorType == IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    mem = m_pointerMemory[m_pointerMemoryIndex];
    if (++m_pointerMemoryIndex == LGMP_Q_POINTER_LEN)
      m_pointerMemoryIndex = 0;
  }
  else
  {
    mem = m_pointerShapeMemory[m_pointerShapeIndex];
    if (++m_pointerShapeIndex == POINTER_SHAPE_BUFFERS)
      m_pointerShapeIndex = 0;
  }

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  cursor->sdrWhiteLevel = sdrWhiteLevel ?
    sdrWhiteLevel : KVMFR_SDR_WHITE_LEVEL_DEFAULT;

  m_cursorVisible = info.IsCursorVisible;
  uint32_t flags  = CURSOR_FLAG_VISIBLE_VALID;

  if (info.IsCursorVisible)
  {
    m_cursorX       = info.X;
    m_cursorY       = info.Y;
    cursor->x = (int16_t)info.X;
    cursor->y = (int16_t)info.Y;
    flags |= CURSOR_FLAG_POSITION | CURSOR_FLAG_VISIBLE;
  }

  if (info.CursorShapeInfo.CursorType != IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED)
  {
    memcpy(cursor + 1, data,
      (size_t)info.CursorShapeInfo.Height * info.CursorShapeInfo.Pitch);

    cursor->hx     = (int8_t  )info.CursorShapeInfo.XHot;
    cursor->hy     = (int8_t  )info.CursorShapeInfo.YHot;
    cursor->width  = (uint32_t)info.CursorShapeInfo.Width;
    cursor->height = (uint32_t)info.CursorShapeInfo.Height;
    cursor->pitch  = (uint32_t)info.CursorShapeInfo.Pitch;

    switch (info.CursorShapeInfo.CursorType)
    {
      case IDDCX_CURSOR_SHAPE_TYPE_ALPHA:
        cursor->type = CURSOR_TYPE_COLOR;
        break;

      case IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR:
        cursor->type = CURSOR_TYPE_MASKED_COLOR;
        break;
    }

    flags |= CURSOR_FLAG_SHAPE;
    m_pointerShape = mem;
  }

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(
      m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s",
      lgmpStatusString(status));
    break;
  }
}

void CLGMPControl::SetColorTransform(
  std::shared_ptr<const D12ColorTransform> transform)
{
  {
    CSRWExclusiveLock lock(m_colorTransformLock);
    m_colorTransform = std::move(transform);
  }
  SendColorTransform();
}

std::shared_ptr<const D12ColorTransform>
CLGMPControl::GetColorTransform() const
{
  CSRWSharedLock lock(m_colorTransformLock);
  std::shared_ptr<const D12ColorTransform> transform = m_colorTransform;
  return transform;
}

void CLGMPControl::SendColorTransform()
{
  if (!m_pointerQueue || !m_pointerTransformMemory[0])
    return;

  PLGMPMemory mem = m_pointerTransformMemory[m_pointerTransformIndex];
  if (++m_pointerTransformIndex == COLOR_TRANSFORM_BUFFERS)
    m_pointerTransformIndex = 0;

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  KVMFRColorTransform * output =
    (KVMFRColorTransform *)(cursor + 1);
  const auto transform = GetColorTransform();

  output->flags = 0;
  if (transform)
  {
    if (transform->matrixEnabled)
      output->flags |= KVMFR_COLOR_TRANSFORM_MATRIX;
    if (transform->lutEnabled)
      output->flags |= KVMFR_COLOR_TRANSFORM_LUT;
    memcpy(output->matrix, transform->matrix, sizeof(output->matrix));
    output->scalar = transform->scalar;
    memcpy(output->lut, transform->lut, sizeof(output->lut));
  }

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(m_pointerQueue,
      CURSOR_FLAG_COLOR_TRANSFORM, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer Transform): %s",
      lgmpStatusString(status));
    break;
  }
}

void CLGMPControl::ResendCursor()
{
  PLGMPMemory mem = m_pointerShape;
  if (!mem)
    return;

  KVMFRCursor* cursor = (KVMFRCursor*)lgmpHostMemPtr(mem);
  cursor->x = (int16_t)m_cursorX;
  cursor->y = (int16_t)m_cursorY;

  const uint32_t flags =
    CURSOR_FLAG_POSITION | CURSOR_FLAG_SHAPE | CURSOR_FLAG_VISIBLE_VALID |
    (m_cursorVisible ? CURSOR_FLAG_VISIBLE : 0);

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(
      m_pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      Sleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s",
      lgmpStatusString(status));
    break;
  }
}

void CLGMPControl::ResendState()
{
  ResendCursor();
  SendColorTransform();
}
