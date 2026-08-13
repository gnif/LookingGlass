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
static const uint32_t POINTER_POSITION_SIZE =
  (uint32_t)sizeof(KVMFRCursor);

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
        POINTER_POSITION_SIZE, &m_pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s",
        lgmpStatusString(status));
      return false;
    }
    memset(lgmpHostMemPtr(m_pointerMemory[i]), 0, POINTER_POSITION_SIZE);
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
  SetControlEvents(nullptr, {});
  for (int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&m_pointerMemory[i]);
  for (int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerShapeMemory[i]);
  for (int i = 0; i < COLOR_TRANSFORM_BUFFERS; ++i)
    lgmpHostMemFree(&m_pointerTransformMemory[i]);

  m_pointerQueue          = nullptr;
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

void CLGMPControl::SetControlEvents(
  IControlEvents * events, const ControlToken& token)
{
  CSRWExclusiveLock lock(m_eventLock);
  m_events = events;
  m_token  = events ? token : ControlToken {};
}

PLGMPMemory CLGMPControl::FindAvailable(
  PLGMPMemory * memory, int count, int& index) const
{
  for (int offset = 0; offset < count; ++offset)
  {
    const int candidate = (index + offset) % count;
    if (memory[candidate] &&
        !lgmpHostQueuePayloadPending(m_pointerQueue, memory[candidate]))
    {
      index = candidate;
      return memory[candidate];
    }
  }
  return nullptr;
}

ControlResult CLGMPControl::SendCursor(
  const IDARG_OUT_QUERY_HWCURSOR& info,
  const BYTE * data, size_t size, UINT sdrWhiteLevel)
{
  if (!m_pointerQueue)
    return ControlResult::FAILED;

  const bool hasShape = info.CursorShapeInfo.CursorType !=
    IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED;
  if (hasShape)
  {
    if (info.CursorShapeInfo.CursorType != IDDCX_CURSOR_SHAPE_TYPE_ALPHA &&
        info.CursorShapeInfo.CursorType !=
          IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR)
    {
      DEBUG_ERROR("Unsupported pointer shape type: %u",
        static_cast<unsigned>(info.CursorShapeInfo.CursorType));
      return ControlResult::FAILED;
    }

    if (info.CursorShapeInfo.Height &&
        info.CursorShapeInfo.Pitch > SIZE_MAX / info.CursorShapeInfo.Height)
    {
      DEBUG_ERROR("Pointer shape size overflow");
      return ControlResult::FAILED;
    }
    const size_t required = static_cast<size_t>(
      info.CursorShapeInfo.Height) * info.CursorShapeInfo.Pitch;
    if (required != size || (required && !data) ||
        required > MAX_POINTER_SIZE - sizeof(KVMFRCursor))
    {
      DEBUG_ERROR("Invalid pointer shape payload: %zu bytes", size);
      return ControlResult::FAILED;
    }
  }

  PLGMPMemory mem;
  int * index;
  int count;
  if (!hasShape)
  {
    index = &m_pointerMemoryIndex;
    count = LGMP_Q_POINTER_LEN;
    mem = FindAvailable(m_pointerMemory, count, *index);
  }
  else
  {
    index = &m_pointerShapeIndex;
    count = POINTER_SHAPE_BUFFERS;
    mem = FindAvailable(m_pointerShapeMemory, count, *index);
  }
  if (!mem)
    return ControlResult::RETRY;

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  cursor->sdrWhiteLevel = sdrWhiteLevel ?
    sdrWhiteLevel : KVMFR_SDR_WHITE_LEVEL_DEFAULT;

  uint32_t flags  = CURSOR_FLAG_VISIBLE_VALID;

  if (info.IsCursorVisible)
  {
    cursor->x = (int16_t)info.X;
    cursor->y = (int16_t)info.Y;
    flags |= CURSOR_FLAG_POSITION | CURSOR_FLAG_VISIBLE;
  }

  if (hasShape)
  {
    if (size)
      memcpy(cursor + 1, data, size);

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
  }

  const LGMP_STATUS status =
    lgmpHostQueuePost(m_pointerQueue, flags, mem);
  if (status == LGMP_OK)
  {
    *index = (*index + 1) % count;
    return ControlResult::APPLIED;
  }
  if (status == LGMP_ERR_QUEUE_FULL)
    return ControlResult::RETRY;

  DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s",
    lgmpStatusString(status));
  return ControlResult::FAILED;
}

ControlResult CLGMPControl::SetColorTransform(
  std::shared_ptr<const D12ColorTransform> transform)
{
  return SendColorTransform(transform);
}

ControlResult CLGMPControl::SendColorTransform(
  const std::shared_ptr<const D12ColorTransform>& transform)
{
  if (!m_pointerQueue || !m_pointerTransformMemory[0])
    return ControlResult::FAILED;

  PLGMPMemory mem = FindAvailable(m_pointerTransformMemory,
    COLOR_TRANSFORM_BUFFERS, m_pointerTransformIndex);
  if (!mem)
    return ControlResult::RETRY;

  KVMFRCursor * cursor = (KVMFRCursor *)lgmpHostMemPtr(mem);
  KVMFRColorTransform * output =
    (KVMFRColorTransform *)(cursor + 1);

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

  const LGMP_STATUS status = lgmpHostQueuePost(m_pointerQueue,
    CURSOR_FLAG_COLOR_TRANSFORM, mem);
  if (status == LGMP_OK)
  {
    m_pointerTransformIndex =
      (m_pointerTransformIndex + 1) % COLOR_TRANSFORM_BUFFERS;
    return ControlResult::APPLIED;
  }
  if (status == LGMP_ERR_QUEUE_FULL)
    return ControlResult::RETRY;

  DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer Transform): %s",
    lgmpStatusString(status));
  return ControlResult::FAILED;
}

void CLGMPControl::RequestReplay()
{
  CSRWSharedLock lock(m_eventLock);
  if (m_events)
    m_events->OnControlReplay(m_token);
}
