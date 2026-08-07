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

#include "capture/CSwapChainProcessor.h"

#include "display/IddCxCompat.h"
#include "display/device/CDeviceContext.h"
#include "transport/IControlTransport.h"

#include "CDebug.h"

DWORD CALLBACK CSwapChainProcessor::_CursorThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor*>(arg)->CursorThread();
  return 0;
}

bool CSwapChainProcessor::QueryHWCursor()
{
  IDARG_IN_QUERY_HWCURSOR in = {};
  in.LastShapeId            = m_lastShapeId;
  in.pShapeBuffer           = m_shapeBuffer;
  in.ShapeBufferSizeInBytes = 512 * 512 * 4;

  IDARG_OUT_QUERY_HWCURSOR out = {};
  UINT cursorWhiteLevel = m_sdrWhiteLevel.load(std::memory_order_relaxed);
  NTSTATUS status;
#ifdef HAS_IDDCX_110
  if (m_devContext->HasIddCx110DDIs())
  {
    IDARG_OUT_QUERY_HWCURSOR3 out3 = {};
    status = IddCxMonitorQueryHardwareCursor3(m_monitor, &in, &out3);
    out.IsCursorVisible      = out3.IsCursorVisible;
    out.X                    = out3.X;
    out.Y                    = out3.Y;
    out.IsCursorShapeUpdated = out3.IsCursorShapeUpdated;
    out.CursorShapeInfo      = out3.CursorShapeInfo;
    if (out3.SdrWhiteLevel)
      cursorWhiteLevel = out3.SdrWhiteLevel;
  }
  else
#endif
  {
    status = IddCxMonitorQueryHardwareCursor(m_monitor, &in, &out);
  }

  if (FAILED(status))
  {
    // this occurs if the display went away (ie, screen blanking or disabled)
    if (status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY)
    {
      SetEvent(m_terminateEvent.Get());
      return false;
    }

    DEBUG_ERROR("IddCxMonitorQueryHardwareCursor failed (0x%08x)", status);
    return false;
  }

  if (out.IsCursorShapeUpdated)
    m_lastShapeId = out.CursorShapeInfo.ShapeId;

  m_control.SendCursor(out, m_shapeBuffer, cursorWhiteLevel);
  return true;
}

void CSwapChainProcessor::CursorThread()
{
  HRESULT hr = 0;
  bool running = true;

  while (running)
  {
    HANDLE waitHandles[] =
    {
      m_cursorDataEvent.Get(),
      m_terminateEvent.Get()
    };

    DWORD waitResult = WaitForMultipleObjects(
      ARRAYSIZE(waitHandles), waitHandles, FALSE, 100);

    switch (waitResult)
    {
    case WAIT_TIMEOUT:
      continue;

      // cursorDataEvent
    case WAIT_OBJECT_0:
      if (!QueryHWCursor())
        return;
      continue;

      // terminateEvent
    case WAIT_OBJECT_0 + 1:
      running = false;
      continue;

    default:
      hr = HRESULT_FROM_WIN32(waitResult);
      DEBUG_ERROR_HR(hr, "WaitForMultipleObjects");
      return;
    }
  }
}
