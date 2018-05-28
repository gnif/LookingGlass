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
  {
    DeInitialize();
    return false;
  }

  if (m_capture->GetMaxFrameSize() > m_frameSize)
  {
    DEBUG_ERROR("Maximum frame size of %zu bytes excceds maximum space available", m_capture->GetMaxFrameSize());
    DeInitialize();
    return false;
  }

  m_timer = CreateWaitableTimer(NULL, TRUE, NULL);
  if (!m_timer)
  {
    DEBUG_ERROR("Failed to create waitable timer");
    return false;
  }

  // Create the cursor thread
  m_cursorThread = CreateThread(NULL, 0, _CursorThread, this, 0, NULL);
  m_cursorEvent  = CreateEvent (NULL, FALSE, FALSE, L"CursorEvent");
  InitializeCriticalSection(&m_cursorCS);

  // update everything except for the hostID
  memcpy(m_shmHeader->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC));
  m_shmHeader->version = KVMFR_HEADER_VERSION;

  // zero and tell the client we have restarted
  ZeroMemory(&m_shmHeader->frame , sizeof(KVMFRFrame ));
  ZeroMemory(&m_shmHeader->cursor, sizeof(KVMFRCursor));
  m_shmHeader->flags &= ~KVMFR_HEADER_FLAG_RESTART;

  m_initialized = true;
  return true;
}

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)

bool Service::InitPointers()
{
  m_shmHeader      = reinterpret_cast<KVMFRHeader *>(m_memory);
  m_cursorData     = (uint8_t *)ALIGN_UP(m_memory + sizeof(KVMFRHeader));
  m_cursorDataSize = 1048576; // 1MB fixed for cursor size, should be more then enough
  m_frame[0]       = (uint8_t *)ALIGN_UP(m_cursorData + m_cursorDataSize);
  m_frameSize      = ALIGN_DN((m_ivshmem->GetSize() - (m_frame[0] - m_memory)) >> 1);
  m_frame[1]       = (uint8_t *)ALIGN_DN(m_frame[0] + m_frameSize);


  m_cursorOffset  = m_cursorData - m_memory;
  m_dataOffset[0] = m_frame[0]   - m_memory;
  m_dataOffset[1] = m_frame[1]   - m_memory;

  DEBUG_INFO("Total Available : %3u MB", (unsigned int)(m_ivshmem->GetSize() / 1024 / 1024));
  DEBUG_INFO("Max Cursor Size : %3u MB", (unsigned int)(m_cursorDataSize / 1024 / 1024));
  DEBUG_INFO("Max Frame Size  : %3u MB", (unsigned int)(m_frameSize / 1024 / 1024));
  DEBUG_INFO("Cursor          : %p (0x%08x)", m_cursorData, (int)m_cursorOffset );
  DEBUG_INFO("Frame 1         : %p (0x%08x)", m_frame[0]  , (int)m_dataOffset[0]);
  DEBUG_INFO("Frame 2         : %p (0x%08x)", m_frame[1]  , (int)m_dataOffset[1]);

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

  WaitForSingleObject(m_cursorThread, INFINITE);
  CloseHandle(m_cursorThread);
  CloseHandle(m_cursorEvent );

  m_memory = NULL;
  m_initialized = false;
}

bool Service::Process()
{
  if (!m_initialized)
    return false;

  volatile uint8_t *flags = &m_shmHeader->flags;

  // wait for the host to notify that is it is ready to proceed
  while (true)
  {
    const uint8_t f = *flags;

    // check if the client has flagged a restart
    if (f & KVMFR_HEADER_FLAG_RESTART)
    {
      DEBUG_INFO("Restart Requested");
      if (!m_capture->ReInitialize())
      {
        DEBUG_ERROR("ReInitialize Failed");
        return false;
      }

      if (m_capture->GetMaxFrameSize() > m_frameSize)
      {
        DEBUG_ERROR("Maximum frame size of %zd bytes excceds maximum space available", m_capture->GetMaxFrameSize());
        return false;
      }

      INTERLOCKED_AND8((volatile char *)flags, ~(KVMFR_HEADER_FLAG_RESTART));
      break;
    }

    // check if the client has flagged it's ready for a frame
    if (!(m_shmHeader->frame.flags & KVMFR_FRAME_FLAG_UPDATE))
      break;

    // wait for 2ms before polling again
    Sleep(2);
  }

  struct FrameInfo  frame;
  struct CursorInfo cursor;
  ZeroMemory(&frame , sizeof(struct FrameInfo ));
  ZeroMemory(&cursor, sizeof(struct CursorInfo));
  frame.buffer     = m_frame[m_frameIndex];
  frame.bufferSize = m_frameSize;

  bool ok         = false;
  bool cursorOnly = false;
  for(int i = 0; i < 2; ++i)
  {
    // capture a frame of data
    switch (m_capture->GrabFrame(frame, cursor))
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
          *flags |= KVMFR_HEADER_FLAG_PAUSED;
          while (WTSGetActiveConsoleSessionId() != m_consoleSessionID)
            Sleep(100);
          *flags &= ~KVMFR_HEADER_FLAG_PAUSED;
        }

        if (!m_capture->ReInitialize())
        {
          DEBUG_ERROR("ReInitialize Failed");
          return false;
        }

        if (m_capture->GetMaxFrameSize() > m_frameSize)
        {
          DEBUG_ERROR("Maximum frame size of %zd bytes excceds maximum space available", m_capture->GetMaxFrameSize());
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

  if (cursor.updated)
  {
    EnterCriticalSection(&m_cursorCS);
    if (cursor.hasPos)
    {
      m_cursorInfo.hasPos = true;
      m_cursorInfo.x      = cursor.x;
      m_cursorInfo.y      = cursor.y;
    }

    if (cursor.hasShape)
    {
      m_cursorInfo.hasShape = true;
      m_cursorInfo.dataSize = cursor.dataSize;
      m_cursorInfo.type     = cursor.type;
      m_cursorInfo.w        = cursor.w;
      m_cursorInfo.h        = cursor.h;
      m_cursorInfo.pitch    = cursor.pitch;
      m_cursorInfo.shape    = cursor.shape;
    }

    m_cursorInfo.visible = cursor.visible;
    LeaveCriticalSection(&m_cursorCS);
    SetEvent(m_cursorEvent);
  }

  if (!cursorOnly)
  {
    KVMFRFrame * fi = &m_shmHeader->frame;

    fi->type    = m_capture->GetFrameType();
    fi->width   = frame.width;
    fi->height  = frame.height;
    fi->stride  = frame.stride;
    fi->pitch   = frame.pitch;
    fi->dataPos = m_dataOffset[m_frameIndex];
    if (++m_frameIndex == 2)
      m_frameIndex = 0;

    // signal a frame update
    fi->flags |= KVMFR_FRAME_FLAG_UPDATE;
  }

  // update the flags
  INTERLOCKED_AND8((volatile char *)flags, KVMFR_HEADER_FLAG_RESTART);
//  _mm_sfence();

  return true;
}

DWORD Service::CursorThread()
{
  while(m_capture)
  {
    if (WaitForSingleObject(m_cursorEvent, 1000) != WAIT_OBJECT_0)
      continue;

    KVMFRCursor * cursor = &m_shmHeader->cursor;
    // wait until the client is ready
    while (cursor->flags != 0)
    {
      Sleep(2);
      if (!m_capture)
        return 0;
    }

    EnterCriticalSection(&m_cursorCS);
    if (m_cursorInfo.hasPos)
    {
      m_cursorInfo.hasPos = false;

      // tell the client where the cursor is
      cursor->flags |= KVMFR_CURSOR_FLAG_POS;
      cursor->x = m_cursorInfo.x;
      cursor->y = m_cursorInfo.y;

      if (m_cursorInfo.visible)
        cursor->flags |= KVMFR_CURSOR_FLAG_VISIBLE;
      else
        cursor->flags &= ~KVMFR_CURSOR_FLAG_VISIBLE;
    }

    if (m_cursorInfo.hasShape)
    {
      m_cursorInfo.hasShape = false;
      if (m_cursorInfo.dataSize > m_cursorDataSize)
        DEBUG_ERROR("Cursor size exceeds allocated space");
      else
      {
        // give the client the new cursor shape
        cursor->flags |= KVMFR_CURSOR_FLAG_SHAPE;
        ++cursor->version;

        cursor->type    = m_cursorInfo.type;
        cursor->width   = m_cursorInfo.w;
        cursor->height  = m_cursorInfo.h;
        cursor->pitch   = m_cursorInfo.pitch;
        cursor->dataPos = m_cursorOffset;

        memcpy(m_cursorData, m_cursorInfo.shape, m_cursorInfo.dataSize);
      }
    }

    LeaveCriticalSection(&m_cursorCS);
    cursor->flags |= KVMFR_CURSOR_FLAG_UPDATE;
  }

  return 0;
}