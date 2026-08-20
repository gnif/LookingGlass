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

#include "ipc/CPipeServer.h"
#include "CClipboardRing.h"
#include "CDebug.h"
#include "CSRWLock.h"
#include "display/CDeviceContext.h"

#include <sddl.h>
#include <vector>

CPipeServer g_pipe;

namespace
{
  static constexpr DWORD NO_CONSOLE_SESSION = 0xFFFFFFFFU;
}

bool CPipeServer::Init()
{
  DeInit();

  // Only the driver identities may create/manage the endpoint. Interactive
  // users receive client read/write access, then the first HELLO is matched
  // against the SYSTEM service's device-bound clipboard authority.
  static constexpr wchar_t PIPE_SECURITY[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;UD)"
    L"(A;;GRGW;;;IU)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      PIPE_SECURITY, SDDL_REVISION_1,
      &m_pipeSecurityDescriptor, nullptr))
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create named pipe security descriptor");
    return false;
  }
  m_pipeSecurity.nLength              = sizeof(m_pipeSecurity);
  m_pipeSecurity.lpSecurityDescriptor = m_pipeSecurityDescriptor;
  m_pipeSecurity.bInheritHandle       = FALSE;

  m_endpoint.SetHandler(this);
  if (m_endpoint.Start(
    LG_PIPE_NAME,
    CPipeEndpoint::Mode::Server,
    sizeof(LGPipeMsg),
    &m_pipeSecurity,
    FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_REJECT_REMOTE_CLIENTS))
    return true;

  LocalFree(m_pipeSecurityDescriptor);
  m_pipeSecurityDescriptor = nullptr;
  m_pipeSecurity = {};
  return false;
}

void CPipeServer::DeInit()
{
  m_endpoint.Stop();
  ClearClipboardAuthority();
  if (m_pipeSecurityDescriptor)
  {
    LocalFree(m_pipeSecurityDescriptor);
    m_pipeSecurityDescriptor = nullptr;
    m_pipeSecurity = {};
  }
}

bool CPipeServer::RegisterClipboardAuthority(WDFFILEOBJECT owner,
  HANDLE mapping, DWORD session, const uint64_t (&mappingId)[2],
  const uint64_t (&authorityId)[2])
{
  if (!owner || !mapping || mapping == INVALID_HANDLE_VALUE ||
      session == NO_CONSOLE_SESSION || !session ||
      !mappingId[0] || !mappingId[1] ||
      !authorityId[0] || !authorityId[1])
  {
    DEBUG_WARN(
      "Rejected invalid clipboard authority registration");
    return false;
  }

  const ClipboardMapping * view = static_cast<const ClipboardMapping *>(
    MapViewOfFileFromApp(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0,
      sizeof(ClipboardMapping)));
  if (!view)
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error,
      "Failed to map the clipboard authority section");
    return false;
  }
  if (!UnmapViewOfFile(view))
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error,
      "Failed to unmap the clipboard authority section");
    return false;
  }

  CSRWExclusiveLock lock(m_authorityLock);
  const LGIddAuthorityFileContext * fileContext =
    LGIddAuthorityGetFileContext(owner);
  if (fileContext->closing)
  {
    DEBUG_WARN(
      "Rejected clipboard authority registration for a closing file");
    return false;
  }
  if (m_authorityOwner || m_authorityMapping)
  {
    DEBUG_WARN(
      "Clipboard authority is already registered");
    return false;
  }

  m_authorityOwner        = owner;
  m_authorityMapping      = mapping;
  m_authoritySession      = session;
  m_authorityMappingId[0] = mappingId[0];
  m_authorityMappingId[1] = mappingId[1];
  m_authorityId[0]        = authorityId[0];
  m_authorityId[1]        = authorityId[1];
  DEBUG_INFO("Registered clipboard authority for session %lu", session);
  return true;
}

bool CPipeServer::ClearClipboardAuthority(WDFFILEOBJECT owner)
{
  return ClearClipboardAuthorityInternal(owner, false);
}

void CPipeServer::CloseClipboardAuthorityFile(WDFFILEOBJECT owner)
{
  (void) ClearClipboardAuthorityInternal(owner, true);
}

bool CPipeServer::ClearClipboardAuthorityInternal(
  WDFFILEOBJECT owner, bool closing)
{
  HANDLE authority = nullptr;
  HANDLE pending   = nullptr;
  {
    CSRWExclusiveLock lock(m_authorityLock);
    if (closing)
    {
      if (!owner)
      {
        DEBUG_WARN(
          "Cannot close an unspecified clipboard authority file");
        return false;
      }
      LGIddAuthorityGetFileContext(owner)->closing = true;
      if (m_authorityOwner != owner)
        return true;
    }

    if (owner && m_authorityOwner && m_authorityOwner != owner)
    {
      DEBUG_WARN(
        "Rejected clipboard authority clear from a different file object");
      return false;
    }

    authority = m_authorityMapping;
    pending   = m_pendingClipboardMapping;

    m_authorityOwner          = nullptr;
    m_authorityMapping        = nullptr;
    m_authoritySession        = NO_CONSOLE_SESSION;
    m_authorityMappingId[0]   = 0;
    m_authorityMappingId[1]   = 0;
    m_authorityId[0]          = 0;
    m_authorityId[1]          = 0;
    m_pendingClipboardMapping = nullptr;
    m_pendingClipboardEpoch   = 0;
    m_clientAuthorityId[0]    = 0;
    m_clientAuthorityId[1]    = 0;

    m_endpoint.DisconnectClient();
    m_clipboard.Detach();
  }

  if (pending)
    CloseHandle(pending);
  if (authority)
    CloseHandle(authority);
  if (authority || pending)
    DEBUG_INFO("Cleared clipboard authority");
  return true;
}

bool CPipeServer::AuthenticatePipeClient(
  HANDLE pipe, const void * message, size_t size)
{
  (void)pipe;
  if (size != sizeof(LGPipeMsg))
  {
    DEBUG_WARN(
      "Rejected Helper HELLO frame with %llu bytes, expected %llu",
      static_cast<unsigned long long>(size),
      static_cast<unsigned long long>(sizeof(LGPipeMsg)));
    return false;
  }
  const LGPipeMsg& hello = *static_cast<const LGPipeMsg *>(message);
  if (hello.size != sizeof(hello) || hello.type != LGPipeMsg::HELLO ||
      hello.hello.version != LGPipeMsg::PROTOCOL_VERSION ||
      !hello.hello.authorityId[0] || !hello.hello.authorityId[1])
  {
    DEBUG_WARN(
      "Rejected malformed Helper HELLO: size=%u type=%u version=%u",
      hello.size, static_cast<unsigned>(hello.type), hello.hello.version);
    return false;
  }

  CSRWExclusiveLock lock(m_authorityLock);
  if (!m_authorityOwner || !m_authorityMapping ||
      m_authorityId[0] != hello.hello.authorityId[0] ||
      m_authorityId[1] != hello.hello.authorityId[1])
  {
    DEBUG_WARN(
      "Named pipe client does not match the clipboard authority");
    return false;
  }

  HANDLE mapping = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), m_authorityMapping,
      GetCurrentProcess(), &mapping,
      SECTION_MAP_READ | SECTION_MAP_WRITE, FALSE, 0))
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error,
      "Failed to duplicate the authorized clipboard mapping");
    return false;
  }

  const ClipboardMapping * view = static_cast<const ClipboardMapping *>(
    MapViewOfFileFromApp(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0,
      sizeof(ClipboardMapping)));
  if (!view)
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error,
      "Failed to validate the registered clipboard mapping");
    CloseHandle(mapping);
    return false;
  }

  const uint64_t epoch = view->epoch;
  const bool valid = CClipboardRing::Valid(*view, epoch);
  if (!UnmapViewOfFile(view))
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error,
      "Failed to unmap the validated clipboard mapping");
    CloseHandle(mapping);
    return false;
  }
  if (!valid || !epoch)
  {
    DEBUG_WARN(
      "Rejected an uninitialized registered clipboard mapping");
    CloseHandle(mapping);
    return false;
  }

  if (m_pendingClipboardMapping)
    CloseHandle(m_pendingClipboardMapping);
  m_pendingClipboardMapping = mapping;
  m_pendingClipboardEpoch   = epoch;
  m_clientAuthorityId[0]    = hello.hello.authorityId[0];
  m_clientAuthorityId[1]    = hello.hello.authorityId[1];
  DEBUG_INFO("Authenticated clipboard Helper for session %lu",
    m_authoritySession);
  return true;
}

bool CPipeServer::PipeClientStillAuthorized(HANDLE pipe)
{
  (void)pipe;
  CSRWSharedLock lock(m_authorityLock);
  if (!m_authorityOwner || !m_authorityMapping ||
      !m_clientAuthorityId[0] || !m_clientAuthorityId[1])
  {
    DEBUG_WARN(
      "Named pipe client authorization state is incomplete");
    return false;
  }
  if (m_clientAuthorityId[0] != m_authorityId[0] ||
      m_clientAuthorityId[1] != m_authorityId[1])
  {
    DEBUG_WARN(
      "Named pipe client session authorization expired");
    return false;
  }
  return true;
}

void CPipeServer::OnPipeConnected()
{
  if (!PipeClientStillAuthorized(m_endpoint.NativeHandle()))
  {
    DEBUG_WARN("Named pipe client authorization expired before activation");
    return;
  }

  uint64_t epoch = 0;
  bool ready     = false;
  {
    CSRWExclusiveLock lock(m_authorityLock);
    HANDLE mapping = m_pendingClipboardMapping;
    epoch = m_pendingClipboardEpoch;
    m_pendingClipboardMapping = nullptr;
    m_pendingClipboardEpoch   = 0;

    const bool authorityMatches = m_authorityOwner &&
      m_authorityMapping &&
      m_authorityId[0] == m_clientAuthorityId[0] &&
      m_authorityId[1] == m_clientAuthorityId[1];
    if (authorityMatches && mapping)
      ready = m_clipboard.Attach(mapping, epoch, false, *this);
    else
    {
      if (mapping)
        CloseHandle(mapping);
      DEBUG_ERROR("Authenticated clipboard mapping is missing or expired");
    }
  }

  LGPipeMsg clipboardReady = {};
  clipboardReady.size                  = sizeof(clipboardReady);
  clipboardReady.type                  = LGPipeMsg::CLIPBOARD_READY;
  clipboardReady.clipboardReady.epoch  = epoch;
  clipboardReady.clipboardReady.status = ready ?
    ERROR_SUCCESS : ERROR_INVALID_DATA;
  m_endpoint.Send(&clipboardReady, sizeof(clipboardReady));

  CSRWExclusiveLock lock(m_queueLock);
  std::vector<LGPipeMsg> queued;
  queued.swap(m_queue);

  for (size_t i = 0; i < queued.size(); ++i)
    if (!m_endpoint.Send(&queued[i], sizeof(queued[i])))
    {
      for (; i < queued.size(); ++i)
        QueueMsgLocked(queued[i]);
      break;
    }

  // Recovery is latched state rather than a one-shot command. Reapply the
  // latest request whenever the helper reconnects so a helper restart cannot
  // silently restore the IDD-only topology while recovery is active.
  if (m_recoveryValid)
    m_recoveryPending = m_endpoint.Send(
      &m_recoveryRequest, sizeof(m_recoveryRequest));
}

void CPipeServer::OnPipeDisconnected()
{
  {
    CSRWExclusiveLock lock(m_authorityLock);
    m_clipboard.Detach();
    if (m_pendingClipboardMapping)
      CloseHandle(m_pendingClipboardMapping);
    m_pendingClipboardMapping = nullptr;
    m_pendingClipboardEpoch   = 0;
    m_clientAuthorityId[0]    = 0;
    m_clientAuthorityId[1]    = 0;
  }

  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWSharedLock recoveryLock(m_recoveryLock);
  if (m_recoveryValid &&
      (m_recoveryPending || m_recoveryHelperActive) &&
      m_recoveryHandler)
  {
    const uint64_t route   = m_recoveryRoute;
    const uint64_t session = m_recoveryRequest.recovery.session;
    const uint32_t serial  = m_recoveryRequest.recovery.request &
      ~LGPipeMsg::RECOVERY_ACTIVE;
    const bool active = (m_recoveryRequest.recovery.request &
      LGPipeMsg::RECOVERY_ACTIVE) != 0;

    // If NORMAL was sent while Helper still owned the recovery topology,
    // do not replay it after a disconnect until the IDD monitor is present.
    // Preserve the preceding ACTIVE command for reconnect in the meantime.
    if (!active && !m_recoveryReplaySafe && m_recoveryActiveValid)
    {
      m_recoveryValid     = true;
      m_recoveryRoute     = m_recoveryActiveRoute;
      m_recoveryRequest   = m_recoveryActiveRequest;
      m_recoveryReplaySafe = true;
    }

    m_recoveryPending      = false;
    m_recoveryHelperActive = false;
    queueLock.Unlock();
    m_recoveryHandler(m_recoveryOpaque, route, session, serial, active,
      RecoveryResult::HELPER_UNAVAILABLE);
  }
}

bool CPipeServer::OnPipeMessage(const void * message, size_t size)
{
  if (size != sizeof(LGPipeMsg))
    return false;

  const LGPipeMsg & msg = *static_cast<const LGPipeMsg *>(message);
  if (msg.size != sizeof(msg))
    return false;

  switch (msg.type)
  {
    case LGPipeMsg::RELOADSETTINGS:
      HandleReloadSettings();
      return true;

    case LGPipeMsg::RECOVERY_OFF:
    case LGPipeMsg::RECOVERY_ON:
    case LGPipeMsg::RECOVERY_FAILED:
    case LGPipeMsg::RECOVERY_NO_DISPLAY:
      HandleRecovery(msg);
      return true;

    case LGPipeMsg::CLIPBOARD_SETUP:
      // Legacy SETUP names a handle in this process. Never interpret a value
      // supplied by an unprivileged client as a local driver handle.
      DEBUG_WARN("Rejected legacy clipboard mapping setup");
      return false;

    case LGPipeMsg::CLIPBOARD_READY:
      // READY normally travels IDD to Helper. A failure in the reverse
      // direction reports that Helper activation failed after IDD attach.
      if (msg.clipboardReady.status != ERROR_SUCCESS)
      {
        CSRWExclusiveLock lock(m_authorityLock);
        if (msg.clipboardReady.epoch == m_clipboard.Epoch())
        {
          m_clipboard.Reset(
            msg.clipboardReady.epoch, msg.clipboardReady.status);
          m_clipboard.Detach();
        }
      }
      return true;

    case LGPipeMsg::CLIPBOARD_KICK:
      m_clipboard.Kick(msg.clipboardKick.epoch);
      return true;

    case LGPipeMsg::CLIPBOARD_RESET:
      m_clipboard.Reset(
        msg.clipboardReset.epoch, msg.clipboardReset.reason);
      return true;

    default:
      DEBUG_ERROR("Unknown message type %d", msg.type);
      return false;
  }
}

bool CPipeServer::ClipboardKick(uint64_t epoch)
{
  LGPipeMsg msg = {};
  msg.size                = sizeof(msg);
  msg.type                = LGPipeMsg::CLIPBOARD_KICK;
  msg.clipboardKick.epoch = epoch;
  msg.clipboardKick.rings = 0;
  return m_endpoint.Send(&msg, sizeof(msg));
}

void CPipeServer::ClipboardResetPeer(uint64_t epoch, uint32_t reason)
{
  LGPipeMsg msg = {};
  msg.size                  = sizeof(msg);
  msg.type                  = LGPipeMsg::CLIPBOARD_RESET;
  msg.clipboardReset.epoch  = epoch;
  msg.clipboardReset.reason = reason;
  m_endpoint.Send(&msg, sizeof(msg));
}

void CPipeServer::QueueMsgLocked(const LGPipeMsg & msg)
{
  for (LGPipeMsg & queued : m_queue)
    if (queued.type == msg.type)
    {
      queued = msg;
      return;
    }

  m_queue.push_back(msg);
}

void CPipeServer::WriteMsg(const LGPipeMsg & msg)
{
  CSRWExclusiveLock lock(m_queueLock);
  if (!m_endpoint.Send(&msg, sizeof(msg)))
    QueueMsgLocked(msg);
}

void CPipeServer::HandleReloadSettings()
{
  DEBUG_INFO("Reloading settings");

  CSRWSharedLock lock(m_deviceContextLock);
  if (m_deviceContext)
    m_deviceContext->ReloadSettings();
}

void CPipeServer::HandleRecovery(const LGPipeMsg & msg)
{
  CSRWExclusiveLock queueLock(m_queueLock);
  if (!m_recoveryValid ||
      msg.recovery.session != m_recoveryRequest.recovery.session ||
      msg.recovery.request != m_recoveryRequest.recovery.request)
  {
    DEBUG_WARN("Ignoring stale recovery status");
    return;
  }

  const uint32_t serial =
    msg.recovery.request & ~LGPipeMsg::RECOVERY_ACTIVE;
  const bool active =
    (msg.recovery.request & LGPipeMsg::RECOVERY_ACTIVE) != 0;
  const uint64_t route = m_recoveryRoute;
  RecoveryResult result;
  switch (msg.type)
  {
    case LGPipeMsg::RECOVERY_OFF:
      result = RecoveryResult::NORMAL;
      m_recoveryHelperActive  = false;
      m_recoveryReplaySafe    = true;
      m_recoveryActiveValid   = false;
      m_recoveryActiveRoute   = 0;
      m_recoveryActiveRequest = {};
      break;

    case LGPipeMsg::RECOVERY_ON:
      result = RecoveryResult::ACTIVE;
      m_recoveryHelperActive = true;
      break;

    case LGPipeMsg::RECOVERY_FAILED:
      result = RecoveryResult::FAILED;
      break;

    case LGPipeMsg::RECOVERY_NO_DISPLAY:
      result = RecoveryResult::NO_DISPLAY;
      break;

    default:
      return;
  }
  m_recoveryPending = false;

  CSRWSharedLock recoveryLock(m_recoveryLock);
  queueLock.Unlock();
  if (m_recoveryHandler)
    m_recoveryHandler(m_recoveryOpaque,
      route, msg.recovery.session, serial, active, result);
}

void CPipeServer::SetDeviceContext(CDeviceContext * context)
{
  CSRWExclusiveLock lock(m_deviceContextLock);
  m_deviceContext = context;
}

void CPipeServer::SetRecoveryHandler(
  RecoveryHandler handler, void * opaque)
{
  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWExclusiveLock recoveryLock(m_recoveryLock);
  m_recoveryRoute         = 0;
  m_recoveryValid         = false;
  m_recoveryPending       = false;
  m_recoveryHelperActive  = false;
  m_recoveryReplaySafe    = true;
  m_recoveryActiveValid   = false;
  m_recoveryRequest       = {};
  m_recoveryActiveRequest = {};
  m_recoveryNewestRequest = {};
  m_recoveryActiveRoute   = 0;
  m_recoveryNewestRoute   = 0;
  m_recoveryHandler       = handler;
  m_recoveryOpaque        = opaque;
}

void CPipeServer::ClearRecoveryHandler(void * opaque)
{
  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWExclusiveLock recoveryLock(m_recoveryLock);
  if (m_recoveryOpaque != opaque)
    return;

  m_recoveryRoute         = 0;
  m_recoveryValid         = false;
  m_recoveryPending       = false;
  m_recoveryHelperActive  = false;
  m_recoveryReplaySafe    = true;
  m_recoveryActiveValid   = false;
  m_recoveryRequest       = {};
  m_recoveryActiveRequest = {};
  m_recoveryNewestRequest = {};
  m_recoveryActiveRoute   = 0;
  m_recoveryNewestRoute   = 0;
  m_recoveryHandler       = nullptr;
  m_recoveryOpaque        = nullptr;
}

bool CPipeServer::SetCursorPos(int32_t x, int32_t y)
{
  // do not send cursor messages if we are not connected or they will end up queued
  if (!m_endpoint.IsConnected())
    return false;

  LGPipeMsg msg = {};
  msg.size       = sizeof(msg);
  msg.type       = LGPipeMsg::SETCURSORPOS;
  msg.curorPos.x = x;
  msg.curorPos.y = y;
  // Cursor position is transient. If the connection is lost during this
  // write, drop it instead of replaying stale coordinates after reconnect.
  return m_endpoint.Send(&msg, sizeof(msg));
}

void CPipeServer::SetDisplayMode(
  uint32_t width, uint32_t height, uint32_t refresh100uHz)
{
  LGPipeMsg msg = {};
  msg.size                       = sizeof(msg);
  msg.type                       = LGPipeMsg::SETDISPLAYMODE;
  msg.displayMode.width          = width;
  msg.displayMode.height         = height;
  msg.displayMode.refresh100uHz = refresh100uHz;
  WriteMsg(msg);
}

void CPipeServer::SetGPUStatus(bool software)
{
  LGPipeMsg msg = {};
  msg.size               = sizeof(msg);
  msg.type               = LGPipeMsg::GPUSTATUS;
  msg.gpuStatus.software = software;
  WriteMsg(msg);
}

void CPipeServer::ResolutionRejected(uint32_t width, uint32_t height,
  uint32_t requiredSizeMiB)
{
  LGPipeMsg msg = {};
  msg.size                               = sizeof(msg);
  msg.type                               = LGPipeMsg::RESOLUTIONREJECTED;
  msg.resolutionRejected.width           = width;
  msg.resolutionRejected.height          = height;
  msg.resolutionRejected.requiredSizeMiB = requiredSizeMiB;
  WriteMsg(msg);
}

CPipeServer::RecoveryDispatch CPipeServer::SetRecovery(
  void * owner, uint64_t route,
  uint64_t session, uint32_t serial, bool active,
  bool replayIfUnavailable)
{
  if (!route || !session || !serial ||
      (serial & LGPipeMsg::RECOVERY_ACTIVE))
  {
    DEBUG_ERROR("Invalid recovery request correlation");
    return RecoveryDispatch::REJECTED;
  }

  LGPipeMsg msg = {};
  msg.size             = sizeof(msg);
  msg.type             = LGPipeMsg::SET_RECOVERY;
  msg.recovery.session = session;
  msg.recovery.request = serial |
    (active ? LGPipeMsg::RECOVERY_ACTIVE : 0U);

  CSRWExclusiveLock queueLock(m_queueLock);
  CSRWExclusiveLock recoveryLock(m_recoveryLock);
  if (!m_recoveryHandler || m_recoveryOpaque != owner)
    return RecoveryDispatch::REJECTED;

  // RecoveryHub routes increase for the lifetime of this handler. Keep a
  // watermark separate from the replay request so restoring retained ACTIVE
  // state cannot let a late, older NORMAL overwrite newer work.
  if (m_recoveryNewestRoute)
  {
    if (route < m_recoveryNewestRoute)
      return RecoveryDispatch::REJECTED;
    if (route == m_recoveryNewestRoute &&
        (msg.recovery.session !=
           m_recoveryNewestRequest.recovery.session ||
         msg.recovery.request !=
           m_recoveryNewestRequest.recovery.request))
      return RecoveryDispatch::REJECTED;
  }
  if (route > m_recoveryNewestRoute)
  {
    m_recoveryNewestRoute   = route;
    m_recoveryNewestRequest = msg;
  }

  const bool      previousValid      = m_recoveryValid;
  const bool      previousPending    = m_recoveryPending;
  const bool      previousReplaySafe = m_recoveryReplaySafe;
  const uint64_t  previousRoute      = m_recoveryRoute;
  const LGPipeMsg previousRequest    = m_recoveryRequest;

  m_recoveryValid      = true;
  m_recoveryRoute      = route;
  m_recoveryRequest    = msg;
  m_recoveryReplaySafe = active || replayIfUnavailable;
  if (active)
  {
    m_recoveryActiveValid   = true;
    m_recoveryActiveRoute   = route;
    m_recoveryActiveRequest = msg;
  }
  m_recoveryPending = m_endpoint.Send(&msg, sizeof(msg));
  if (m_recoveryPending)
    return RecoveryDispatch::SENT;

  if (replayIfUnavailable)
    return RecoveryDispatch::QUEUED;

  // A NORMAL request cannot be replayed while the IDD monitor is locally
  // absent. Retain the prior ACTIVE request until the monitor has re-arrived.
  m_recoveryValid      = previousValid;
  m_recoveryPending    = previousPending;
  m_recoveryReplaySafe = previousReplaySafe;
  m_recoveryRoute      = previousRoute;
  m_recoveryRequest    = previousRequest;
  return RecoveryDispatch::UNAVAILABLE;
}
