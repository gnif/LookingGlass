#include "CIndirectMonitorContext.h"
#include "Direct3DDevice.h"
#include "Debug.h"

CIndirectMonitorContext::CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device) :
  m_monitor(monitor),
  m_devContext(device)
{
  m_terminateEvent .Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_cursorDataEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_thread.Attach(CreateThread(nullptr, 0, _CursorThread, this, 0, nullptr));
  m_shapeBuffer = new BYTE[512 * 512 * 4];
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  m_swapChain.reset();
  SetEvent(m_terminateEvent.Get());
  delete[] m_shapeBuffer;
}

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  m_swapChain.reset();
  auto device = std::make_shared<Direct3DDevice>(renderAdapter);
  if (FAILED(device->Init()))
  {
    WdfObjectDelete(swapChain);
    return;
  }

  m_swapChain.reset(new CSwapChainProcessor(m_devContext, swapChain, device, newFrameEvent));

  IDARG_IN_SETUP_HWCURSOR c = {};
  c.CursorInfo.Size                  = sizeof(c.CursorInfo);
  c.CursorInfo.AlphaCursorSupport    = TRUE;
  c.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
  c.CursorInfo.MaxX                  = 512;
  c.CursorInfo.MaxY                  = 512;
  c.hNewCursorDataAvailable          = m_cursorDataEvent.Get();
  NTSTATUS status = IddCxMonitorSetupHardwareCursor(m_monitor, &c);
  if (!NT_SUCCESS(status))
    DBGPRINT("IddCxMonitorSetupHardwareCursor Failed: %08x", status);
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  m_swapChain.reset();
}

DWORD CALLBACK CIndirectMonitorContext::_CursorThread(LPVOID arg)
{
  reinterpret_cast<CIndirectMonitorContext*>(arg)->CursorThread();
  return 0;
}

void CIndirectMonitorContext::CursorThread()
{
  HRESULT hr = 0;

  for (;;)
  {
    HANDLE waitHandles[] =
    {
      m_cursorDataEvent.Get(),
      m_terminateEvent.Get()
    };
    DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 100);
    if (waitResult == WAIT_TIMEOUT)
      continue;
    else if (waitResult == WAIT_OBJECT_0 + 1)
      break;
    else if (waitResult != WAIT_OBJECT_0)
    {
      hr = HRESULT_FROM_WIN32(waitResult);
      DBGPRINT("WaitForMultipleObjects: %08", hr);
      return;
    }

    IDARG_IN_QUERY_HWCURSOR in  = {};
    in.LastShapeId            = m_lastShapeId;
    in.pShapeBuffer           = m_shapeBuffer;
    in.ShapeBufferSizeInBytes = 512 * 512 * 4;

    IDARG_OUT_QUERY_HWCURSOR out = {};
    NTSTATUS status = IddCxMonitorQueryHardwareCursor(m_monitor, &in, &out);
    if (FAILED(status))
    {
      DBGPRINT("IddCxMonitorQueryHardwareCursor failed: %08x", status);
      return;
    }

    m_devContext->SendCursor(out, m_shapeBuffer);
  }
}