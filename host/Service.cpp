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

#include "common\debug.h"
#include "common\KVMFR.h"

#include "CaptureFactory.h"

Service * Service::m_instance = NULL;

Service::Service() :
  m_initialized(false),
  m_memory(NULL),
  m_readyEvent(INVALID_HANDLE_VALUE),
  m_capture(NULL),
  m_header(NULL),
  m_frameIndex(0)
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

  m_readyEvent = m_ivshmem->CreateVectorEvent(0);
  if (m_readyEvent == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR("Failed to get event for vector 0");
    DeInitialize();
    return false;
  }

  // we save this as it might actually be valid
  UINT16 hostID = m_header->hostID;

  ZeroMemory(m_header, sizeof(KVMFRHeader));
  memcpy(m_header->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC));

  m_header->version     = KVMFR_HEADER_VERSION;
  m_header->guestID     = m_ivshmem->GetPeerID();
  m_header->hostID      = hostID;
  m_header->updateCount = 0;

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
  if (m_readyEvent != INVALID_HANDLE_VALUE)
  {
    CloseHandle(m_readyEvent);
    m_readyEvent = INVALID_HANDLE_VALUE;
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

  struct FrameInfo frame;
  ZeroMemory(&frame, sizeof(FrameInfo));
  frame.buffer      = m_frame[m_frameIndex];
  frame.bufferSize  = m_frameSize;

  // wait for the host to notify that is it is ready to proceed
  bool eventDone = false;
  while (!eventDone)
  {
    switch (WaitForSingleObject(m_readyEvent, 1000))
    {
    case WAIT_ABANDONED:
      DEBUG_ERROR("Wait abandoned");
      return false;

    case WAIT_OBJECT_0:
      eventDone = true;
      break;

    case WAIT_TIMEOUT:
      continue;

    case WAIT_FAILED:
      DEBUG_ERROR("Wait failed");
      return false;

    default:
      DEBUG_ERROR("Unknown error");
      return false;
    }
  }
  ResetEvent(m_readyEvent);

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

  m_header->flags        = 0;
  m_header->cursor.flags = 0;

  if (!cursorOnly)
  {
    // signal a frame update
    m_header->flags        |= KVMFR_HEADER_FLAG_FRAME;
    m_header->frame.type    = m_capture->GetFrameType();
    m_header->frame.width   = frame.width;
    m_header->frame.height  = frame.height;
    m_header->frame.stride  = frame.stride;
    m_header->frame.dataPos = m_dataOffset[m_frameIndex];
    if (++m_frameIndex == 2)
      m_frameIndex = 0;
  }

  if (frame.cursor.hasPos)
  {
    // tell the host where the cursor is
    m_header->flags        |= KVMFR_HEADER_FLAG_CURSOR;
    m_header->cursor.flags |= KVMFR_CURSOR_FLAG_POS;
    m_header->cursor.x      = frame.cursor.x;
    m_header->cursor.y      = frame.cursor.y;
  }

  if (frame.cursor.hasShape)
  {
    // give the host the new cursor shape
    m_header->flags        |= KVMFR_HEADER_FLAG_CURSOR;
    m_header->cursor.flags |= KVMFR_CURSOR_FLAG_SHAPE;
    m_header->cursor.type   = frame.cursor.type;
    m_header->cursor.w      = frame.cursor.w;
    m_header->cursor.h      = frame.cursor.h;
    if (frame.cursor.dataSize > KVMFR_CURSOR_BUFFER)
    {
      DEBUG_ERROR("Cursor shape size exceeds buffer size");
      return false;
    }
    memcpy(m_header->cursor.shape, frame.cursor.shape, frame.cursor.dataSize);
  }

  // increment the update count to resume the host
  ++m_header->updateCount;

  return true;
}