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

#include "Util.h"
#include "CaptureFactory.h"

Service * Service::m_instance = NULL;

Service::Service() :
  m_initialized(false),
  m_memory(NULL),
  m_timer(NULL),
  m_capture(NULL),
  m_shmHeader(NULL),
  m_frameIndex(0),
  m_cursorDataSize(0),
  m_cursorData(NULL)
{
  m_consoleSessionID = WTSGetActiveConsoleSessionId();
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
  memcpy(m_shmHeader->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC));
  m_shmHeader->version = KVMFR_HEADER_VERSION;

  // clear but retain the restart flag if it was set by the client
  INTERLOCKED_AND8((char *)&m_shmHeader->flags, KVMFR_HEADER_FLAG_RESTART);
  ZeroMemory(&m_shmHeader->detail, sizeof(KVMFRDetail));

  m_initialized = true;
  return true;
}

bool Service::InitPointers()
{
  m_shmHeader      = reinterpret_cast<KVMFRHeader *>(m_memory);
  m_cursorData     = (uint8_t *)(((uintptr_t)m_memory + sizeof(KVMFRHeader *) + 0x7F) & ~0x7F);
  m_cursorDataSize = (128 * 128 * 4);
  m_frame[0]       = m_cursorData + (128*128*4);
  m_frameSize      = ((m_ivshmem->GetSize() - (m_frame[0] - m_memory)) & ~0x7F) >> 1;
  m_frame[1]       = m_frame[0] + m_frameSize;

  m_cursorOffset  = m_cursorData - m_memory;
  m_dataOffset[0] = m_frame[0]   - m_memory;
  m_dataOffset[1] = m_frame[1]   - m_memory;

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

  m_shmHeader      = NULL;
  m_cursorData     = NULL;
  m_frame[0]       = NULL;
  m_frame[1]       = NULL;
  m_cursorOffset   = 0;
  m_dataOffset[0]  = 0;
  m_dataOffset[1]  = 0;
  m_cursorDataSize = 0;
  m_frameSize      = 0;

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

  struct FrameInfo frame;
  ZeroMemory(&frame, sizeof(FrameInfo));
  frame.buffer      = m_frame[m_frameIndex];
  frame.bufferSize  = m_frameSize;

  volatile uint8_t *flags = &m_shmHeader->flags;

  // wait for the host to notify that is it is ready to proceed
  while (true)
  {
    const uint8_t f = *flags;

    // check if the client has flagged a restart
    if (f & KVMFR_HEADER_FLAG_RESTART)
    {
      INTERLOCKED_AND8((volatile char *)flags, ~(KVMFR_HEADER_FLAG_RESTART));
      break;
    }

    // check if the client has flagged it's ready
    if (!(f & KVMFR_HEADER_FLAG_FRAME))
      break;

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
        if(WTSGetActiveConsoleSessionId() != m_consoleSessionID)
        {
          DEBUG_INFO("User switch detected, waiting to regain control");
          while (WTSGetActiveConsoleSessionId() != m_consoleSessionID)
            Sleep(100);
        }

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

  if (!cursorOnly)
  {
    // signal a frame update
    updateFlags            |= KVMFR_HEADER_FLAG_FRAME;
    m_detail.frame.type    = m_capture->GetFrameType();
    m_detail.frame.width   = frame.width;
    m_detail.frame.height  = frame.height;
    m_detail.frame.stride  = frame.stride;
    m_detail.frame.pitch   = frame.pitch;
    m_detail.frame.dataPos = m_dataOffset[m_frameIndex];
    if (++m_frameIndex == 2)
      m_frameIndex = 0;
  }

  if (frame.cursor.hasPos)
  {
    // tell the host where the cursor is
    updateFlags |= KVMFR_HEADER_FLAG_CURSOR;
    m_detail.cursor.flags |= KVMFR_CURSOR_FLAG_POS;
    m_detail.cursor.x = frame.cursor.x;
    m_detail.cursor.y = frame.cursor.y;

    if (frame.cursor.visible)
      m_detail.cursor.flags |= KVMFR_CURSOR_FLAG_VISIBLE;
    else
      m_detail.cursor.flags &= ~KVMFR_CURSOR_FLAG_VISIBLE;
  }

  if (frame.cursor.hasShape)
  {
    if (frame.cursor.dataSize > m_cursorDataSize)
    {
      DEBUG_ERROR("Cursor size exceeds allocated space");
      return false;
    }        

    // give the host the new cursor shape
    updateFlags |= KVMFR_HEADER_FLAG_CURSOR;
    m_detail.cursor.flags |= KVMFR_CURSOR_FLAG_SHAPE;
    ++m_detail.cursor.version;
    
    m_detail.cursor.type    = frame.cursor.type;
    m_detail.cursor.width   = frame.cursor.w;
    m_detail.cursor.height  = frame.cursor.h;
    m_detail.cursor.pitch   = frame.cursor.pitch;
    m_detail.cursor.dataPos = m_cursorOffset;
    memcpy(m_cursorData, frame.cursor.shape, frame.cursor.dataSize);
  }

  // update the shared details only
  memcpy(&m_shmHeader->detail, &m_detail, sizeof(KVMFRDetail));

  // update the flags
  INTERLOCKED_AND8((volatile char *)flags, KVMFR_HEADER_FLAG_RESTART);
  INTERLOCKED_OR8 ((volatile char *)flags, updateFlags);
  _mm_sfence();

  return true;
}