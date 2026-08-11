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

#pragma once

#include <windows.h>
#include <wdf.h>
#include <stdint.h>
#include <vector>

#include "CPipeEndpoint.h"
#include "CSRWLock.h"
#include "PipeMsg.h"

class CDeviceContext;

class CPipeServer : private IPipeEndpointHandler
{
  private:
    CPipeEndpoint          m_endpoint;
    CSRWLock               m_queueLock;
    std::vector<LGPipeMsg> m_queue;

    CSRWLock         m_deviceContextLock;
    CDeviceContext * m_deviceContext     = nullptr;

    void WriteMsg(const LGPipeMsg & msg);
    void QueueMsgLocked(const LGPipeMsg & msg);

    void HandleReloadSettings();

    void OnPipeConnected() override;
    bool OnPipeMessage(const void * message, size_t size) override;

  public:
    ~CPipeServer() { DeInit(); }

    bool Init();
    void DeInit();

    void SetDeviceContext(CDeviceContext * context);

    void SetCursorPos(uint32_t x, uint32_t y);
    void SetDisplayMode(
      uint32_t width, uint32_t height, uint32_t refreshMilliHz);
    void SetGPUStatus(bool software);
    void ResolutionRejected(uint32_t width, uint32_t height,
      uint32_t requiredSizeMiB);
};

extern CPipeServer g_pipe;
