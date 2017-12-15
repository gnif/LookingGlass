/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "Service.h"
#include "IVSHMEM.h"

#include "common/debug.h"
#include "common/KVMFR.h"

#include "CaptureFactory.h"

#if __MINGW32__
#define INTERLOCKED_AND8 __sync_and_and_fetch
#define INTERLOCKED_OR8 __sync_or_and_fetch
#else
#define INTERLOCKED_OR8 InterlockedOr8
#define INTERLOCKED_AND8 InterlockedAnd8
#endif

Service * Service::m_instance = NULL;

Service::Service() :
  m_initialized(false),
  m_memory(NULL),
  m_timer(NULL),
  m_capture(NULL),
  m_header(NULL),
  m_frameIndex(0),
  m_cursorDataSize(0),
  m_cursorData(NULL),
  m_shapePending(false)
{
  m_ivshmem = IVSHMEM::Get();
}

Service::~Service()
{
}

bool Service::Initialize(ICapture * captureDevice)
{
  if (m_initialized)
    DeInitialize();

  m_capture = captureDevice;
  if (!m_ivshmem->Initialize())
  {
    DEBUG_ERROR("IVSHMEM failed to initalize");
    DeInitialize();
    return false;
  }

  if (m_ivshmem->GetSize() < sizeof(KVMFRHeader))
  {
    DEBUG_ERROR("Shared memory is not large enough for the KVMFRHeader");
    DeInitialize();
    return false;
  }

  m_memory = static_cast<uint8_t*>(m_ivshmem->GetMemory());
  if (!m_memory)
  {
    DEBUG_ERROR("Failed to get IVSHMEM memory");
    DeInitialize();
    return false;
  }

  if (!InitPointers())
    return false;

  m_timer = CreateWaitableTimer(NULL, TRUE, NULL);
  if (!m_timer)
  {
    DEBUG_ERROR("Failed to create waitable timer");
    return false;
  }

  // update everything except for the hostID
  memcpy(m_header->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC));
  m_header->version     = KVMFR_HEADER_VERSION;
  m_header->guestID     = m_ivshmem->GetPeerID();
  m_header->updateCount = 0;

  // clear but retain the restart flag if it was set by the client
  INTERLOCKED_AND8((char *)&m_header->flags, KVMFR_HEADER_FLAG_RESTART);
  ZeroMemory(&m_header->frame , sizeof(KVMFRFrame ));
  ZeroMemory(&m_header->cursor, sizeof(KVMFRCursor));

  m_initialized = true;
  return true;
}

bool Service::InitPointers()
{
  m_header = reinterpret_cast<KVMFRHeader *>(m_memory);
  m_frame[0] = (uint8_t *)(((uintptr_t)m_memory + sizeof(KVMFRHeader *) + 0x7F) & ~0x7F);
  m_frameSize = ((m_ivshmem->GetSize() - (m_frame[0] - m_memory)) & ~0x7F) >> 1;
  m_frame[1] = m_frame[0] + m_frameSize;
  m_dataOffset[0] = m_frame[0] - m_memory;
  m_dataOffset[1] = m_frame[1] - m_memory;

  if (m_capture->GetMaxFrameSize() > m_frameSize)
  {
    DEBUG_ERROR("Frame can exceed buffer size!");
    DeInitialize();
    return false;
  }

  return true;
}

void Service::DeInitialize()
{
  if (m_timer)
  {
    CloseHandle(m_timer);
    m_timer = NULL;
  }

  m_shapePending = false;

  if (m_cursorData)
  {
    delete[] m_cursorData;
    m_cursorDataSize = 0;
    m_cursorData     = NULL;
  }

  m_header        = NULL;
  m_frame[0]      = NULL;
  m_frame[1]      = NULL;
  m_dataOffset[0] = 0;
  m_dataOffset[1] = 0;

  m_ivshmem->DeInitialize();

  if (m_capture)
  {
    m_capture->DeInitialize();
    m_capture = NULL;
  }

  m_memory = NULL;
  m_initialized = false;
}

bool Service::Process()
{
  if (!m_initialized)
    return false;

  bool restart = false;
  struct FrameInfo frame;
  ZeroMemory(&frame, sizeof(FrameInfo));
  frame.buffer      = m_frame[m_frameIndex];
  frame.bufferSize  = m_frameSize;

  volatile uint8_t *flags = &m_header->flags;

  // wait for the host to notify that is it is ready to proceed
  while (true)
  {
    const uint8_t f = *flags;

    // check if the client has flagged a restart
    if (f & KVMFR_HEADER_FLAG_RESTART)
    {
      m_header->updateCount = 0;
      INTERLOCKED_AND8((volatile char *)flags, ~(KVMFR_HEADER_FLAG_RESTART));
      restart = true;
      break;
    }

    // check if the client has flagged it's ready
    if (f & KVMFR_HEADER_FLAG_READY)
    {
      INTERLOCKED_AND8((volatile char *)flags, ~(KVMFR_HEADER_FLAG_READY));
      break;
    }

    // wait for 100ns before polling again
    LARGE_INTEGER timeout;
    timeout.QuadPart = -100;
    if (!SetWaitableTimer(m_timer, &timeout, 0, NULL, NULL, FALSE))
    {
      DEBUG_ERROR("Failed to set waitable timer");
      return false;
    }
    WaitForSingleObject(m_timer, INFINITE);
  }

  bool ok         = false;
  bool cursorOnly = false;
  if (m_shapePending)
  {
    ok         = true;
    cursorOnly = true;
  }
  else
    for(int i = 0; i < 2; ++i)
    {
      // capture a frame of data
      switch (m_capture->GrabFrame(frame))
      {
        case GRAB_STATUS_OK:
          ok = true;
          break;

        case GRAB_STATUS_CURSOR:
          ok         = true;
          cursorOnly = true;
          break;

        case GRAB_STATUS_ERROR:
          DEBUG_ERROR("Capture failed");
          return false;

        case GRAB_STATUS_REINIT:
          DEBUG_INFO("ReInitialize Requested");
          if (!m_capture->ReInitialize() || !InitPointers())
          {
            DEBUG_ERROR("ReInitialize Failed");
            return false;
          }
          continue;
      }

      if (ok)
        break;
    }

  if (!ok)
  {
    DEBUG_ERROR("Capture retry count exceeded");
    return false;
  }

  uint8_t updateFlags = 0;
  m_header->cursor.flags = 0;

  if (!cursorOnly)
  {
    // signal a frame update
    updateFlags            |= KVMFR_HEADER_FLAG_FRAME;
    m_header->frame.type    = m_capture->GetFrameType();
    m_header->frame.width   = frame.width;
    m_header->frame.height  = frame.height;
    m_header->frame.stride  = frame.stride;
    m_header->frame.dataPos = m_dataOffset[m_frameIndex];
    if (++m_frameIndex == 2)
      m_frameIndex = 0;
  }

  if (frame.cursor.hasPos || (m_cursor.hasPos && restart))
  {
    // remember the last state for client restart
    if (frame.cursor.hasPos)
    {
      m_cursor.hasPos  = true;
      m_cursor.visible = frame.cursor.visible;
      m_cursor.x       = frame.cursor.x;
      m_cursor.y       = frame.cursor.y;
    }

    // tell the host where the cursor is
    updateFlags            |= KVMFR_HEADER_FLAG_CURSOR;
    m_header->cursor.flags |= KVMFR_CURSOR_FLAG_POS;
    if (m_cursor.visible)
      m_header->cursor.flags |= KVMFR_CURSOR_FLAG_VISIBLE;
    m_header->cursor.x      = m_cursor.x;
    m_header->cursor.y      = m_cursor.y;
  }

  if (frame.cursor.hasShape || m_shapePending || (m_cursor.hasShape && restart))
  {
    if (!m_shapePending && !restart)
    {
      if (frame.cursor.dataSize > m_frameSize)
      {
        DEBUG_ERROR("Cursor size exceeds frame size! This should never happen unless your shared memory is WAY too small");
        return false;
      }        

      // take a copy of the shape information for client restarts or pending shape changes
      m_cursor.hasShape = frame.cursor.hasShape;
      m_cursor.type     = frame.cursor.type;
      m_cursor.w        = frame.cursor.w;
      m_cursor.h        = frame.cursor.h;
      m_cursor.pitch    = frame.cursor.pitch;
      m_cursor.dataSize = frame.cursor.dataSize;
      memcpy(&m_cursor, &frame.cursor, sizeof(CursorInfo));
      if (m_cursorDataSize < frame.cursor.dataSize)
      {
        delete[] m_cursorData;
        m_cursorData     = new uint8_t[frame.cursor.dataSize];
        m_cursorDataSize = frame.cursor.dataSize;
      }

      memcpy(m_cursorData, frame.cursor.shape, frame.cursor.dataSize);
    }

    // we can't send a frame with the cursor shape as we need the buffer location
    // flag it to send on the next packet
    if (updateFlags & KVMFR_HEADER_FLAG_FRAME)
      m_shapePending = true;
    else
    {
      // give the host the new cursor shape
      updateFlags             |= KVMFR_HEADER_FLAG_CURSOR;
      m_header->cursor.flags  |= KVMFR_CURSOR_FLAG_SHAPE;
      if (m_cursor.visible)
        m_header->cursor.flags |= KVMFR_CURSOR_FLAG_VISIBLE;

      m_header->cursor.type    = m_cursor.type;
      m_header->cursor.w       = m_cursor.w;
      m_header->cursor.h       = m_cursor.h;
      m_header->cursor.pitch   = m_cursor.pitch;
      m_header->cursor.dataPos = m_dataOffset[m_frameIndex];
      memcpy(m_frame[m_frameIndex], m_cursorData, m_cursor.dataSize);
      m_shapePending = false;

      if (++m_frameIndex == 2)
        m_frameIndex = 0;
    }
  }

  // update the flags
  INTERLOCKED_AND8((volatile char *)flags, KVMFR_HEADER_FLAG_RESTART);
  INTERLOCKED_OR8 ((volatile char *)flags, updateFlags);

  // increment the update count to resume the host
  ++m_header->updateCount;

  return true;
}