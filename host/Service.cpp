#include "Service.h"
#include "IVSHMEM.h"

#include "common\debug.h"
#include "common\KVMGFXHeader.h"

#include "CaptureFactory.h"

Service * Service::m_instance = NULL;

Service::Service() :
  m_initialized(false),
  m_readyEvent(INVALID_HANDLE_VALUE),
  m_capture(NULL),
  m_memory(NULL)
{
  m_ivshmem = IVSHMEM::Get();
}

Service::~Service()
{
}

bool Service::Initialize()
{
  if (m_initialized)
    DeInitialize();

  m_capture = CaptureFactory::GetCaptureDevice();
  if (!m_capture || !m_capture->Initialize())
  {
    DEBUG_ERROR("Failed to initialize capture interface");
    DeInitialize();
    return false;
  }

  if (!m_ivshmem->Initialize())
  {
    DEBUG_ERROR("IVSHMEM failed to initalize");
    DeInitialize();
    return false;
  }

  if (m_ivshmem->GetSize() < sizeof(KVMGFXHeader))
  {
    DEBUG_ERROR("Shared memory is not large enough for the KVMGFXHeader");
    DeInitialize();
    return false;
  }

  m_memory = m_ivshmem->GetMemory();
  if (!m_memory)
  {
    DEBUG_ERROR("Failed to get IVSHMEM memory");
    DeInitialize();
    return false;
  }

  m_readyEvent = m_ivshmem->CreateVectorEvent(0);
  if (m_readyEvent == INVALID_HANDLE_VALUE)
  {
    DEBUG_ERROR("Failed to get event for vector 0");
    DeInitialize();
    return false;
  }

  KVMGFXHeader * header = static_cast<KVMGFXHeader*>(m_memory);

  // we save this as it might actually be valid
  UINT16 hostID = header->hostID;

  ZeroMemory(header, sizeof(KVMGFXHeader));
  memcpy(header->magic, KVMGFX_HEADER_MAGIC, sizeof(KVMGFX_HEADER_MAGIC));

  header->version   = 2;
  header->guestID   = m_ivshmem->GetPeerID();
  header->hostID    = hostID;

  m_initialized = true;
  return true;
}

void Service::DeInitialize()
{
  if (m_readyEvent != INVALID_HANDLE_VALUE)
    CloseHandle(m_readyEvent);

  m_memory = NULL;
  m_ivshmem->DeInitialize();

  if (m_capture)
  {
    m_capture->DeInitialize();
    m_capture = NULL;
  }

  m_initialized = false;
}

bool Service::Process(HANDLE stopEvent)
{
  if (!m_initialized)
    return false;

  KVMGFXHeader * header  = static_cast<KVMGFXHeader *>(m_memory  );
  void         * data    = static_cast<void         *>(header + 1);
  const size_t available = m_ivshmem->GetSize() - sizeof(KVMGFXHeader);
  if (m_capture->GetMaxFrameSize() > available)
  {
    DEBUG_ERROR("Frame could exceed buffer size!");
    return false;
  }

  // setup the header
  header->frameType = m_capture->GetFrameType();
  header->compType  = m_capture->GetFrameCompression();
  header->dataLen   = 0;

  FrameInfo frame;
  frame.buffer     = data;
  frame.bufferSize = m_ivshmem->GetSize() - sizeof(KVMGFXHeader);

  // capture a frame of data
  if (!m_capture->GrabFrame(frame))
  {
    header->dataLen = 0;
    DEBUG_ERROR("Capture failed");
    return false;
  }

  // copy the frame details into the header
  header->width   = frame.width;
  header->height  = frame.height;
  header->stride  = frame.stride;
  header->dataLen = frame.outSize;

  // tell the host where the cursor is
  POINT cursorPos;
  GetCursorPos(&cursorPos);
  header->mouseX = cursorPos.x;
  header->mouseY = cursorPos.y;

  // wait for the host to notify that is it is ready to proceed
  ResetEvent(m_readyEvent);
  while(
    stopEvent == INVALID_HANDLE_VALUE ||
    (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
  )
  {
    if (!m_ivshmem->RingDoorbell(header->hostID, 0))
    {
      DEBUG_ERROR("Failed to ring doorbell");
      return false;
    }

    switch (WaitForSingleObject(m_readyEvent, 1000))
    {
      case WAIT_ABANDONED:
        DEBUG_ERROR("Wait abandoned");
        return false;

      case WAIT_OBJECT_0:
        return true;

      // if we timed out we just continue to ring until we get an answer or we are stopped
      case WAIT_TIMEOUT:
        break;

      case WAIT_FAILED:
        DEBUG_ERROR("Wait failed");
        return false;

      default:
        DEBUG_ERROR("Unknown error");
        return false;
    }
  }
  
  return true;
}
