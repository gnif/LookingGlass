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
#include <stdint.h>

#include "CPipeEndpoint.h"
#include "CClipboardChannel.h"
#include "CSRWLock.h"
#include "PipeMsg.h"

class CPipeClient : private IPipeEndpointHandler,
  public IClipboardChannelDoorbell
{
private:
  CPipeEndpoint      m_endpoint;
  CClipboardChannel  m_clipboard;
  CSRWLock           m_clipboardSetupLock;
  HANDLE             m_clipboardMapping      = nullptr;
  uint64_t           m_clipboardEpoch        = 0;
  uint64_t           m_clipboardEpochCounter = 0;
  bool               m_clipboardEnabled      = false;
  CSRWLock           m_displayLock;

  bool      m_recoveryActive    = false;
  bool      m_hasRecoveryStatus = false;
  LGPipeMsg m_recoveryStatus    = {};

  void WriteMsg(const LGPipeMsg& msg);

  void SetActiveDesktop();

  bool EnsureOnlyDisplayLocked();
  uint32_t RestoreSavedTopologyLocked() const;
  uint32_t RestoreLGTopologyLocked();

  void HandleSetCursorPos(const LGPipeMsg& msg);
  void HandleSetDisplayMode(const LGPipeMsg& msg);
  void HandleGPUStatus(const LGPipeMsg& msg);
  void HandleResolutionRejected(const LGPipeMsg& msg);
  void HandleSetRecovery(const LGPipeMsg& msg);
  void ResetClipboardSetupLocked();

  void OnPipeConnected() override;
  void OnPipeDisconnected() override;
  bool ShouldReconnect() override;
  bool OnPipeMessage(const void * message, size_t size) override;

public:
  ~CPipeClient() { DeInit(); }

  static bool IsLGIddDeviceAttached();

  bool Init();
  void DeInit();
  bool IsRunning() { return m_endpoint.IsRunning(); }

  CClipboardChannel& Clipboard() { return m_clipboard; }
  void EnableClipboard() { m_clipboardEnabled = true; }
  bool ClipboardKick(uint64_t epoch) override;
  void ClipboardResetPeer(uint64_t epoch, uint32_t reason) override;

  void ReloadSettings();
  bool EnsureOnlyDisplay();
};

extern CPipeClient g_pipe;
