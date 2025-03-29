/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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
#include <stdint.h>
#include <wrl.h>

#include "PipeMsg.h"

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

class CPipeClient
{
private:
  HandleT<HANDLENullTraits> m_pipe;
  HandleT<HANDLENullTraits> m_thread;
  bool m_running = false;
  bool m_connected = false;

  static DWORD WINAPI _pipeThread(LPVOID lpParam) { ((CPipeClient*)lpParam)->Thread(); return 0; }
  void Thread();

  void WriteMsg(const LGPipeMsg& msg);

  void SetActiveDesktop();
  
  void HandleSetCursorPos(const LGPipeMsg& msg);
  void HandleSetDisplayMode(const LGPipeMsg& msg);

public:
  ~CPipeClient() { DeInit(); }

  static bool IsLGIddDeviceAttached();

  bool Init();
  void DeInit();
  bool IsRunning() { return m_running; }
};

extern CPipeClient g_pipe;