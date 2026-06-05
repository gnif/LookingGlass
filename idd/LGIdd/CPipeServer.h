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
#include <wrl.h>
#include <vector>

#include "PipeMsg.h"

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

class CPipeServer
{
  private:
    HandleT<HANDLETraits>     m_pipe;
    HandleT<HANDLENullTraits> m_thread;
    HandleT<EventTraits>      m_signal;
    std::vector<LGPipeMsg>    m_queue;

    bool m_running   = false;
    bool m_connected = false;

    void _DeInit();

    static DWORD WINAPI _pipeThread(LPVOID lpParam) { ((CPipeServer*)lpParam)->Thread(); return 0; }
    void Thread();

    void WriteMsg(const LGPipeMsg & msg);

    void HandleReloadSettings();

  public:
    ~CPipeServer() { DeInit(); }

    bool Init();
    void DeInit();

    void SetCursorPos(uint32_t x, uint32_t y);
    void SetDisplayMode(uint32_t width, uint32_t height, uint32_t refresh);
    void SetGPUStatus(bool software);
};

extern CPipeServer g_pipe;