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
#include "CClipboardChannel.h"
#include "CSRWLock.h"
#include "PipeMsg.h"

class CDeviceContext;

struct LGIddAuthorityFileContext
{
  bool closing;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
  LGIddAuthorityFileContext, LGIddAuthorityGetFileContext)

class CPipeServer : private IPipeEndpointHandler,
  public IClipboardChannelDoorbell
{
  public:
    enum class RecoveryResult
    {
      NORMAL,
      ACTIVE,
      FAILED,
      NO_DISPLAY,
      HELPER_UNAVAILABLE,
    };

    enum class RecoveryDispatch
    {
      REJECTED,
      UNAVAILABLE,
      QUEUED,
      SENT,
    };

    using RecoveryHandler = void (*)(void * opaque,
      uint64_t route, uint64_t session, uint32_t serial, bool active,
      RecoveryResult result);

  private:
    CPipeEndpoint          m_endpoint;
    CClipboardChannel      m_clipboard;
    CSRWLock               m_queueLock;
    std::vector<LGPipeMsg> m_queue;
    bool      m_recoveryValid         = false;
    bool      m_recoveryPending       = false;
    bool      m_recoveryHelperActive  = false;
    bool      m_recoveryReplaySafe    = true;
    bool      m_recoveryActiveValid   = false;
    LGPipeMsg m_recoveryRequest       = {};
    LGPipeMsg m_recoveryActiveRequest = {};
    LGPipeMsg m_recoveryNewestRequest = {};
    uint64_t  m_recoveryActiveRoute   = 0;
    uint64_t  m_recoveryNewestRoute   = 0;

    CSRWLock         m_deviceContextLock;
    CDeviceContext * m_deviceContext = nullptr;

    CSRWLock        m_recoveryLock;
    RecoveryHandler m_recoveryHandler = nullptr;
    void *          m_recoveryOpaque  = nullptr;
    uint64_t        m_recoveryRoute   = 0;

    CSRWLock      m_authorityLock;
    WDFFILEOBJECT m_authorityOwner        = nullptr;
    HANDLE        m_authorityMapping      = nullptr;
    DWORD         m_authoritySession      = 0xFFFFFFFFU;
    uint64_t      m_authorityMappingId[2] = {};
    uint64_t      m_authorityId[2]        = {};

    PSECURITY_DESCRIPTOR m_pipeSecurityDescriptor  = nullptr;
    SECURITY_ATTRIBUTES  m_pipeSecurity            = {};
    HANDLE               m_pendingClipboardMapping = nullptr;
    uint64_t             m_pendingClipboardEpoch   = 0;
    uint64_t             m_clientAuthorityId[2]    = {};

    void WriteMsg(const LGPipeMsg & msg);
    void QueueMsgLocked(const LGPipeMsg & msg);

    void HandleReloadSettings();
    void HandleRecovery(const LGPipeMsg & msg);
    bool ClearClipboardAuthorityInternal(
      WDFFILEOBJECT owner, bool closing);

    void OnPipeConnected() override;
    void OnPipeDisconnected() override;
    bool PipeClientAuthenticationRequired() const override { return true; }
    bool AuthenticatePipeClient(HANDLE pipe,
      const void * message, size_t size) override;
    bool PipeClientStillAuthorized(HANDLE pipe) override;
    bool OnPipeMessage(const void * message, size_t size) override;

  public:
    ~CPipeServer() { DeInit(); }

    bool Init();
    void DeInit();

    void SetDeviceContext(CDeviceContext * context);
    void SetRecoveryHandler(RecoveryHandler handler, void * opaque);
    void ClearRecoveryHandler(void * opaque);

    bool RegisterClipboardAuthority(WDFFILEOBJECT owner, HANDLE mapping,
      DWORD session, const uint64_t (&mappingId)[2],
      const uint64_t (&authorityId)[2]);
    bool ClearClipboardAuthority(WDFFILEOBJECT owner = nullptr);
    void CloseClipboardAuthorityFile(WDFFILEOBJECT owner);

    bool SetCursorPos(int32_t x, int32_t y);
    void SetDisplayMode(
      uint32_t width, uint32_t height, uint32_t refresh100uHz);
    void SetGPUStatus(bool software);
    void ResolutionRejected(uint32_t width, uint32_t height,
      uint32_t requiredSizeMiB);
    RecoveryDispatch SetRecovery(void * owner, uint64_t route,
      uint64_t session, uint32_t serial, bool active,
      bool replayIfUnavailable = true);

    CClipboardChannel& Clipboard() { return m_clipboard; }
    bool ClipboardKick(uint64_t epoch) override;
    void ClipboardResetPeer(uint64_t epoch, uint32_t reason) override;
};

extern CPipeServer g_pipe;
