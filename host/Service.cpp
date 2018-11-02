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
#include "TraceUtil.h"

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

  m_tryTarget  = 0;
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

  // Create the cursor thread
  m_cursorThread = CreateThread(NULL, 0, _CursorThread, this, 0, NULL);
  m_cursorEvent  = CreateEvent (NULL, FALSE, FALSE, L"CursorEvent");
  InitializeCriticalSection(&m_cursorCS);

  // update everything except for the hostID
  memcpy(m_shmHeader->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC));
  m_shmHeader->version = KVMFR_HEADER_VERSION;

  // zero and tell the client we have restarted
  ZeroMemory(&(m_shmHeader->frame ), sizeof(KVMFRFrame ));
  ZeroMemory(&(m_shmHeader->cursor), sizeof(KVMFRCursor));
  m_shmHeader->flags &= ~KVMFR_HEADER_FLAG_RESTART;

  m_haveFrame   = false;
  m_initialized = true;
  m_running     = true;
  return true;
}

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)

bool Service::InitPointers()
{
  m_shmHeader      = reinterpret_cast<KVMFRHeader *>(m_memory);
  m_cursorData     = (uint8_t *)ALIGN_UP(m_memory + sizeof(KVMFRHeader));
  m_cursorDataSize = 1048576; // 1MB fixed for cursor size, should be more then enough
  m_cursorOffset   = m_cursorData - m_memory;

  uint8_t * m_frames = (uint8_t *)ALIGN_UP(m_cursorData + m_cursorDataSize);
  m_frameSize = ALIGN_DN((m_ivshmem->GetSize() - (m_frames - m_memory)) / MAX_FRAMES);

  DEBUG_INFO("Total Available : %3u MB", (unsigned int)(m_ivshmem->GetSize() / 1024 / 1024));
  DEBUG_INFO("Max Cursor Size : %3u MB", (unsigned int)(m_cursorDataSize / 1024 / 1024));
  DEBUG_INFO("Max Frame Size  : %3u MB", (unsigned int)(m_frameSize / 1024 / 1024));
  DEBUG_INFO("Cursor          : %p (0x%08x)", m_cursorData, (int)m_cursorOffset);

  for (int i = 0; i < MAX_FRAMES; ++i)
  {
    m_frame[i] = m_frames + i * m_frameSize;
    m_dataOffset[i] = m_frame[i] - m_memory;
    DEBUG_INFO("Frame %d         : %p (0x%08x)", i, m_frame[i], (int)m_dataOffset[i]);
  }

  return true;
}

void Service::DeInitialize()
{
  m_running = false;

  WaitForSingleObject(m_cursorThread, INFINITE);
  CloseHandle(m_cursorThread);
  CloseHandle(m_cursorEvent);

  m_shmHeader      = NULL;
  m_cursorData     = NULL;
  m_cursorDataSize = 0;
  m_cursorOffset   = 0;
  m_haveFrame      = false;

  for(int i = 0; i < MAX_FRAMES; ++i)
  {
    m_frame     [i] = NULL;
    m_dataOffset[i] = 0;
  }
  m_frameSize = 0;

  m_ivshmem->DeInitialize();

  if (m_capture)
  {
    m_capture->DeInitialize();
    m_capture = NULL;
  }

  m_memory = NULL;
  m_initialized = false;
}

bool Service::ReInit(volatile char * flags)
{
  DEBUG_INFO("ReInitialize Requested");

  INTERLOCKED_OR8(flags, KVMFR_HEADER_FLAG_PAUSED);
  if (WTSGetActiveConsoleSessionId() != m_consoleSessionID)
  {
    DEBUG_INFO("User switch detected, waiting to regain control");
    while (WTSGetActiveConsoleSessionId() != m_consoleSessionID)
      Sleep(100);
  }

  while (!m_capture->CanInitialize())
    Sleep(100);

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

  INTERLOCKED_AND8(flags, ~KVMFR_HEADER_FLAG_PAUSED);
  return true;
}

ProcessStatus Service::Process()
{
  if (!m_initialized)
    return PROCESS_STATUS_ERROR;

  volatile char * flags = (volatile char *)&(m_shmHeader->flags);

  // check if the client has flagged a restart
  if (*flags & KVMFR_HEADER_FLAG_RESTART)
  {
    DEBUG_INFO("Restart Requested");
    if (!m_capture->ReInitialize())
    {
      DEBUG_ERROR("ReInitialize Failed");
      return PROCESS_STATUS_ERROR;
    }

    if (m_capture->GetMaxFrameSize() > m_frameSize)
    {
      DEBUG_ERROR("Maximum frame size of %zd bytes exceeds maximum space available", m_capture->GetMaxFrameSize());
      return PROCESS_STATUS_ERROR;
    }

    INTERLOCKED_AND8(flags, ~(KVMFR_HEADER_FLAG_RESTART));
  }

  unsigned int status;
  bool notify = false;

  status = m_capture->Capture();
  if (status & GRAB_STATUS_ERROR)
  {
    DEBUG_WARN("Capture error, retrying");
    return PROCESS_STATUS_RETRY;
  }

  if (status & GRAB_STATUS_TIMEOUT)
  {
    // timeouts should not count towards a failure to capture
    if (!m_haveFrame)
      return PROCESS_STATUS_OK;

    notify = true;
  }

  if (status & GRAB_STATUS_REINIT)
  {
    if (!ReInit(flags))
      return PROCESS_STATUS_ERROR;

    // re-init request should not count towards a failure to capture
    return PROCESS_STATUS_OK;
  }

  if ((status & (GRAB_STATUS_OK | GRAB_STATUS_TIMEOUT)) == 0)
  {
    DEBUG_ERROR("Capture interface returned an unexpected result");
    return PROCESS_STATUS_ERROR;
  }

  if (status & GRAB_STATUS_CURSOR)
    SetEvent(m_cursorEvent);

  volatile KVMFRFrame * fi = &(m_shmHeader->frame);
  if (status & GRAB_STATUS_FRAME)
  { 
    FrameInfo frame  = { 0 };
    frame.buffer     = m_frame[m_frameIndex];
    frame.bufferSize = m_frameSize;

    GrabStatus result = m_capture->GetFrame(frame);
    if (result != GRAB_STATUS_OK)
    {
      if (result == GRAB_STATUS_REINIT)
      {
        if (!ReInit(flags))
          return PROCESS_STATUS_ERROR;

        // re-init request should not count towards a failure to capture
        return PROCESS_STATUS_OK;
      }

      DEBUG_INFO("GetFrame failed");
      return PROCESS_STATUS_ERROR;
    }

    /* don't touch the frame information until the client is done with it */
    while (fi->flags & KVMFR_FRAME_FLAG_UPDATE)
    {
      /* this generally never occurs */
      Sleep(1);
      if (*flags & KVMFR_HEADER_FLAG_RESTART)
        break;
    }

    fi->type    = m_capture->GetFrameType();
    fi->width   = frame.width;
    fi->height  = frame.height;
    fi->stride  = frame.stride;
    fi->pitch   = frame.pitch;
    fi->dataPos = m_dataOffset[m_frameIndex];

    if (++m_frameIndex == MAX_FRAMES)
      m_frameIndex = 0;

    // remember that we have a valid frame
    m_haveFrame = true;
    notify = true;
  }

  if (notify)
  {
    /* don't touch the frame inforamtion until the client is done with it */
    while (fi->flags & KVMFR_FRAME_FLAG_UPDATE)
    {
      if (*flags & KVMFR_HEADER_FLAG_RESTART)
        break;
    }
    // signal a frame update
    fi->flags |= KVMFR_FRAME_FLAG_UPDATE;
  }

  // update the flags
  INTERLOCKED_AND8(flags, KVMFR_HEADER_FLAG_RESTART);
  return PROCESS_STATUS_OK;
}

DWORD Service::CursorThread()
{
  while(m_running)
  {
    if (WaitForSingleObject(m_cursorEvent, 1000) != WAIT_OBJECT_0)
      continue;

    CursorInfo ci;
    while (m_capture->GetCursor(ci))
    {
      volatile KVMFRCursor * cursor = &(m_shmHeader->cursor);
      // wait until the client is ready
      while (cursor->flags != 0)
      {
        Sleep(1);
        if (!m_capture)
          return 0;
      }

      uint8_t flags = 0;

      if (ci.hasPos)
      {
        // tell the client where the cursor is
        flags |= KVMFR_CURSOR_FLAG_POS;
        cursor->x = ci.x;
        cursor->y = ci.y;
      }

      if (ci.hasShape)
      {
        if (ci.shape.pointerSize > m_cursorDataSize)
          DEBUG_ERROR("Cursor size exceeds allocated space");
        else
        {
          // give the client the new cursor shape
          flags |= KVMFR_CURSOR_FLAG_SHAPE;
          ++cursor->version;

          cursor->type    = ci.type;
          cursor->width   = ci.w;
          cursor->height  = ci.h;
          cursor->pitch   = ci.pitch;
          cursor->dataPos = m_cursorOffset;

          memcpy(m_cursorData, ci.shape.buffer, ci.shape.bufferSize);
        }
      }

      if (ci.visible)
        flags |= KVMFR_CURSOR_FLAG_VISIBLE;

      flags |= KVMFR_CURSOR_FLAG_UPDATE;
      cursor->flags = flags;
      m_capture->FreeCursor();
    }
  }

  return 0;
}