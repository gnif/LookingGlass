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

#include <Windows.h>
#include <Ole2.h>
#include <wrl.h>
#include <UserEnv.h>
#include <WtsApi32.h>
#include <bcrypt.h>
#include <setupapi.h>

#include <array>
#include <cstddef>
#include <cinttypes>
#include <cstring>
#include <vector>
#include <string>

using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

#include "CDebug.h"
#include "VersionInfo.h"
#include "CPipeClient.h"
#include "CNotifyWindow.h"
#include "CConfigWindow.h"
#include "ClipboardRing.h"
#include "LGIddAuthority.h"

#include "common/array.h"

static constexpr wchar_t SVCNAME[] = L"Looking Glass (IDD Helper)";

static constexpr DWORD NO_CONSOLE_SESSION = 0xFFFFFFFFu;

static SERVICE_STATUS_HANDLE l_svcStatusHandle;
static SERVICE_STATUS        l_svcStatus;
static HandleT<EventTraits>  l_svcStopEvent;
static HandleT<EventTraits>  l_svcSessionChangeEvent;

bool HandleService();
static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv);
static DWORD WINAPI SvcCtrlHandler(DWORD dwControl, DWORD dwEventType,
  LPVOID lpEventData, LPVOID lpContext);
static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);

static std::wstring              l_executable;
static HandleT<HANDLENullTraits> l_process;
static HandleT<EventTraits>      l_childStopEvent;
static HandleT<EventTraits>      l_childActivationEvent;
static HandleT<HANDLENullTraits> l_childLifetimeMutex;
static HandleT<HANDLENullTraits> l_childClipboardMapping;
static HANDLE                    l_authorityDevice = INVALID_HANDLE_VALUE;
static LGIddAuthorityHost        l_authorityHost   = {};
static DWORD                     l_desiredSession  = NO_CONSOLE_SESSION;
static DWORD                     l_childSession    = NO_CONSOLE_SESSION;

struct OleScope
{
  ~OleScope() { OleUninitialize(); }
};

static bool Launch(DWORD sessionId);
static bool StopChild();
static void CloseChildLifetimeMutex();
static bool RegisterClipboardAuthority(DWORD session,
  const uint64_t (&mappingId)[2], HANDLE mapping);
static bool VerifyClipboardAuthority();
static void ClearClipboardAuthority();

static void CALLBACK DestroyNotifyWindow(PVOID lpParam, BOOLEAN bTimedOut)
{
  (void) bTimedOut;
  DEBUG_INFO("Helper shutdown requested, exiting...");
  CNotifyWindow *window = (CNotifyWindow *)lpParam;
  window->close();
}

struct LifetimeWaitContext
{
  HANDLE          lifetime;
  HANDLE          childStop;
  HANDLE          cancel;
  CNotifyWindow * window;
};

static DWORD WINAPI LifetimeWaitProc(void * opaque)
{
  const LifetimeWaitContext * context =
    static_cast<const LifetimeWaitContext *>(opaque);
  const HANDLE handles[] =
    { context->lifetime, context->childStop, context->cancel };
  const DWORD result = WaitForMultipleObjects(
    ARRAY_LENGTH(handles), handles, FALSE, INFINITE);
  if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED_0 ||
      result == WAIT_OBJECT_0 + 1)
    DestroyNotifyWindow(context->window, FALSE);
  else if (result == WAIT_FAILED)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error, "Failed to wait for the service lifetime mutex");
    DestroyNotifyWindow(context->window, FALSE);
  }
  return 0;
}

static bool ParseMappingId(
  const std::wstring& value, uint64_t (&mappingId)[2])
{
  if (value.size() != 32)
    return false;
  mappingId[0] = 0;
  mappingId[1] = 0;
  for (size_t i = 0; i < value.size(); ++i)
  {
    const wchar_t c = value[i];
    uint64_t nibble;
    if (c >= L'0' && c <= L'9')
      nibble = static_cast<uint64_t>(c - L'0');
    else if (c >= L'a' && c <= L'f')
      nibble = static_cast<uint64_t>(c - L'a' + 10);
    else if (c >= L'A' && c <= L'F')
      nibble = static_cast<uint64_t>(c - L'A' + 10);
    else
      return false;
    uint64_t& part = mappingId[i / 16];
    part = (part << 4) | nibble;
  }
  return mappingId[0] && mappingId[1];
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
  wchar_t buffer[MAX_PATH];
  DWORD result = GetModuleFileName(NULL, buffer, MAX_PATH);
  if (result == 0)
  {
    DEBUG_ERROR("Failed to get the executable path");
    return EXIT_FAILURE;
  }  
  l_executable = buffer;

  int argc = 0;
  LPWSTR * wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  args.reserve(argc);
  for (int i = 0; i < argc; ++i)
    args.emplace_back(wargv[i]);
  LocalFree(wargv);

  if (argc == 1)
  {
    g_debug.Init(L"looking-glass-idd-service");
    DEBUG_INFO("Looking Glass IDD Helper Service (" LG_VERSION_STR ")");
    if (!HandleService())
      return EXIT_FAILURE;
    return EXIT_SUCCESS;
  }

  if (argc != 5)
    return EXIT_FAILURE;

  // child process
  g_debug.Init(L"looking-glass-idd-helper", CDebug::Location::LocalAppData);
  DEBUG_INFO("Looking Glass IDD Helper Process (" LG_VERSION_STR ")");

  HandleT<HANDLENullTraits> hLifetime(
    OpenMutexW(SYNCHRONIZE, FALSE, args[1].c_str()));
  if (!hLifetime.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to open the service lifetime mutex");
    return EXIT_FAILURE;
  }

  HandleT<EventTraits> hStop(
    OpenEventW(SYNCHRONIZE, FALSE, args[2].c_str()));
  if (!hStop.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to open the child stop event");
    return EXIT_FAILURE;
  }

  HandleT<EventTraits> hActivation(
    OpenEventW(SYNCHRONIZE, FALSE, args[3].c_str()));
  if (!hActivation.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to open the child activation event");
    return EXIT_FAILURE;
  }

  const HANDLE activationHandles[] =
    { hLifetime.Get(), hStop.Get(), hActivation.Get() };
  const DWORD activationResult = WaitForMultipleObjects(
    ARRAY_LENGTH(activationHandles), activationHandles, FALSE, INFINITE);
  if (activationResult == WAIT_OBJECT_0 ||
      activationResult == WAIT_ABANDONED_0 ||
      activationResult == WAIT_OBJECT_0 + 1)
  {
    DEBUG_INFO("Helper activation cancelled by the service");
    return EXIT_SUCCESS;
  }
  if (activationResult != WAIT_OBJECT_0 + 2)
  {
    if (activationResult == WAIT_FAILED)
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error, "Failed to wait for Helper activation");
    }
    else
      DEBUG_ERROR_HR(ERROR_INVALID_STATE,
        "Helper activation wait returned an unexpected result");
    return EXIT_FAILURE;
  }

  uint64_t mappingId[2];
  if (!ParseMappingId(args[4], mappingId))
  {
    DEBUG_ERROR("Invalid service-owned clipboard mapping identifier");
    return EXIT_FAILURE;
  }
  g_pipe.SetClipboardMappingId(mappingId[0], mappingId[1]);

  const HRESULT ole = OleInitialize(nullptr);
  if (FAILED(ole))
  {
    DEBUG_ERROR_HR(ole, "Failed to initialize OLE");
    return EXIT_FAILURE;
  }
  const OleScope oleScope;

  if (!CNotifyWindow::registerClass())
  {
    DEBUG_ERROR("Failed to register message window class");
    return EXIT_FAILURE;
  }

  if (!CConfigWindow::registerClass())
  {
    DEBUG_ERROR("Failed to register config window class");
    return EXIT_FAILURE;
  }

  CNotifyWindow& window = CNotifyWindow::instance();

  if (!window.initClipboard(g_pipe.Clipboard()))
    DEBUG_ERROR("Failed to initialize clipboard synchronization");
  else
    g_pipe.EnableClipboard();

  // the pipe must be initialized after the CNotifyWindow
  // has been created to avoid a potential race
  if (!g_pipe.Init())
    return EXIT_FAILURE;

  window.onSettingChange([]() {
    g_pipe.ReloadSettings();
  });

  window.onEnsureOnlyDisplay([]() {
    return g_pipe.EnsureOnlyDisplay();
  });

  HandleT<EventTraits> lifetimeThreadStop(
    CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!lifetimeThreadStop.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create the service lifetime thread event");
    g_pipe.DeInit();
    return EXIT_FAILURE;
  }

  LifetimeWaitContext lifetimeContext =
    { hLifetime.Get(), hStop.Get(), lifetimeThreadStop.Get(), &window };
  HandleT<HANDLENullTraits> lifetimeThread(CreateThread(
    nullptr, 0, LifetimeWaitProc, &lifetimeContext, 0, nullptr));
  if (!lifetimeThread.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create the service lifetime wait thread");
    g_pipe.DeInit();
    return EXIT_FAILURE;
  }

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0) > 0)
  {
    HWND hDlg = window.hwndDialog();
    if (!hDlg || !IsDialogMessage(hDlg, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  if (!SetEvent(lifetimeThreadStop.Get()))
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to stop the service lifetime wait thread");
  const DWORD lifetimeWait =
    WaitForSingleObject(lifetimeThread.Get(), INFINITE);
  if (lifetimeWait == WAIT_FAILED)
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to join the service lifetime wait thread");

  DEBUG_INFO("Helper window destroyed.");
  g_pipe.DeInit();
  return EXIT_SUCCESS;
}

bool HandleService()
{  
  SERVICE_TABLE_ENTRY DispatchTable[] =
  {
    { (LPWSTR) SVCNAME, SvcMain },
    { NULL, NULL }
  };

  if (StartServiceCtrlDispatcher(DispatchTable) == FALSE)
  {
    DEBUG_ERROR_HR(GetLastError(), "StartServiceCtrlDispatcher Failed");
    return false;
  }

  return true;
}

static DWORD WINAPI SvcCtrlHandler(DWORD dwControl, DWORD dwEventType,
  LPVOID lpEventData, LPVOID lpContext)
{
  (void) dwEventType;
  (void) lpEventData;
  (void) lpContext;

  switch (dwControl)
  {
    case SERVICE_CONTROL_STOP:
      ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
      SetEvent(l_svcStopEvent.Get());
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      if (l_svcSessionChangeEvent.IsValid())
        SetEvent(l_svcSessionChangeEvent.Get());
      return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
      ReportSvcStatus(l_svcStatus.dwCurrentState, NO_ERROR, 0);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

static void WINAPI SvcMain(DWORD dwArgc, LPTSTR* lpszArgv)
{
  l_svcStatus.dwServiceType   = SERVICE_WIN32_OWN_PROCESS;
  l_svcStatus.dwWin32ExitCode = 0;
  l_desiredSession = NO_CONSOLE_SESSION;
  l_childSession   = NO_CONSOLE_SESSION;

  l_svcStatusHandle = RegisterServiceCtrlHandlerExW(SVCNAME,
    SvcCtrlHandler, NULL);
  if (!l_svcStatusHandle)
  {
    DEBUG_ERROR_HR(GetLastError(), "RegisterServiceCtrlHandlerExW Failed");
    return;
  }

  if (!CPipeClient::IsLGIddDeviceAttached())
  {
    DEBUG_INFO("Looking Glass Indirect Display Device not found, not starting.");
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 0);
  l_svcStopEvent.Attach(CreateEvent(NULL, TRUE, FALSE, NULL));
  if (!l_svcStopEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateEvent Failed");
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  l_svcSessionChangeEvent.Attach(CreateEvent(NULL, FALSE, FALSE, NULL));
  if (!l_svcSessionChangeEvent.IsValid())
  {
    DEBUG_ERROR_HR(GetLastError(), "CreateEvent Failed");
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
  }

  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
  bool running = true;
  ULONGLONG nextLaunch = 0;
  while (running)
  {
    if (WaitForSingleObject(l_svcStopEvent.Get(), 0) == WAIT_OBJECT_0)
      break;

    DWORD interactiveSession = WTSGetActiveConsoleSessionId();
    if (l_desiredSession != interactiveSession)
    {
      if (interactiveSession == NO_CONSOLE_SESSION)
        DEBUG_INFO("No active console session");
      else
        DEBUG_INFO("Active console session changed to %lu", interactiveSession);

      l_desiredSession = interactiveSession;
      nextLaunch = 0;
    }

    if (l_process.IsValid() && l_childSession != l_desiredSession)
    {
      if (!StopChild())
      {
        running = false;
        break;
      }

      // Re-evaluate both the stop event and the active console session before
      // launching a replacement child.
      continue;
    }

    if (l_process.IsValid() && !VerifyClipboardAuthority())
    {
      DEBUG_WARN("LGIdd clipboard authority was lost, restarting Helper");
      if (!StopChild())
      {
        running = false;
        break;
      }
      nextLaunch = GetTickCount64() + 1000;
      continue;
    }

    if (!l_process.IsValid() &&
        l_desiredSession != NO_CONSOLE_SESSION &&
        GetTickCount64() >= nextLaunch)
    {
      if (!CPipeClient::IsLGIddDeviceAttached())
      {
        DEBUG_INFO("Looking Glass Indirect Display Device has gone away");
        running = false;
        break;
      }

      if (!Launch(l_desiredSession))
        nextLaunch = GetTickCount64() + 1000;
    }

    HANDLE waitOn[] =
    {
      l_svcStopEvent.Get(),
      l_svcSessionChangeEvent.Get(),
      l_process.Get()
    };
    DWORD count     = 3;
    DWORD duration  = 1000;

    if (!l_process.IsValid())
      count    = 2;

    switch (WaitForMultipleObjects(count, waitOn, FALSE, duration))
    {
      // stop requested by the service manager
      case WAIT_OBJECT_0:
        running = false;
        break;

      // active console session may have changed
      case WAIT_OBJECT_0 + 1:
        break;

      // child application exited
      case WAIT_OBJECT_0 + 2:
      {
        ClearClipboardAuthority();
        DWORD code;
        if (!GetExitCodeProcess(l_process.Get(), &code))
          DEBUG_ERROR_HR(GetLastError(), "GetExitCodeProcess Failed");
        else
          DEBUG_INFO("Child process exited with code 0x%lx", code);

        l_process.Close();
        l_childStopEvent.Close();
        CloseChildLifetimeMutex();
        l_childSession = NO_CONSOLE_SESSION;
        nextLaunch = GetTickCount64() + 1000;
        break;
      }

      case WAIT_FAILED:
        DEBUG_ERROR_HR(GetLastError(), "Failed to WaitForMultipleObjects");
        running = false;
        break;
    }
  }

  (void) StopChild();
  ClearClipboardAuthority();
  ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

static void ReportSvcStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
  static DWORD dwCheckPoint   = 0;
  l_svcStatus.dwCurrentState  = dwCurrentState;
  l_svcStatus.dwWin32ExitCode = dwWin32ExitCode;
  l_svcStatus.dwWaitHint      = dwWaitHint;

  if (dwCurrentState == SERVICE_RUNNING)
    l_svcStatus.dwControlsAccepted =
      SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SESSIONCHANGE;
  else
    l_svcStatus.dwControlsAccepted = 0;

  if ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED))
    l_svcStatus.dwCheckPoint = 0;
  else
    l_svcStatus.dwCheckPoint = ++dwCheckPoint;

  SetServiceStatus(l_svcStatusHandle, &l_svcStatus);
}

//static void 

static bool EnablePriv(LPCWSTR name)
{
  TOKEN_PRIVILEGES tp = { 0 };
  LUID luid;
  HandleT<HANDLENullTraits> hToken;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
    hToken.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "OpenProcessToken");
    return false;
  }

  if (!LookupPrivilegeValue(NULL, name, &luid))
  {
    DEBUG_ERROR_HR(GetLastError(), "LookupPrivilegeValue %ls", name);
    return false;
  }

  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  SetLastError(ERROR_SUCCESS);
  if (!AdjustTokenPrivileges(hToken.Get(), FALSE, &tp, sizeof(tp), NULL, NULL))
  {
    DEBUG_ERROR_HR(GetLastError(), "AdjustTokenPrivileges %ls", name);
    return false;
  }

  const DWORD error = GetLastError();
  if (error != ERROR_SUCCESS)
  {
    DEBUG_ERROR_HR(error, "AdjustTokenPrivileges %ls", name);
    return false;
  }

  return true;
}

static void DisablePriv(LPCWSTR name)
{
  TOKEN_PRIVILEGES tp = {0};
  LUID luid;
  HandleT<HANDLENullTraits> hToken;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
    hToken.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(), "OpenProcessToken");
    return;
  }

  if (!LookupPrivilegeValue(NULL, name, &luid))
  {
    DEBUG_ERROR_HR(GetLastError(), "LookupPrivilegeValue %ls", name);
    return;
  }

  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = 0;

  if (!AdjustTokenPrivileges(hToken.Get(), FALSE, &tp, sizeof(tp), NULL, NULL))
    DEBUG_ERROR_HR(GetLastError(), "AdjustTokenPrivileges %ls", name);
}

namespace
{
  bool TokenFailure(DWORD error, const char * stage)
  {
    DEBUG_ERROR_HR(error, "%s", stage);
    SetLastError(error);
    return false;
  }

  bool GetTokenData(HANDLE token, TOKEN_INFORMATION_CLASS type,
    std::vector<uint8_t>& data)
  {
    DWORD bytes = 0;
    const BOOL sized =
      GetTokenInformation(token, type, nullptr, 0, &bytes);
    const DWORD sizeError = sized ? ERROR_SUCCESS : GetLastError();
    if (sized || sizeError != ERROR_INSUFFICIENT_BUFFER || !bytes)
    {
      SetLastError(sized || !bytes ? ERROR_INVALID_DATA : sizeError);
      return false;
    }

    try
    {
      data.resize(bytes);
    }
    catch (...)
    {
      SetLastError(ERROR_OUTOFMEMORY);
      return false;
    }
    if (!GetTokenInformation(token, type, data.data(), bytes, &bytes))
    {
      const DWORD error = GetLastError();
      SetLastError(error);
      return false;
    }
    return true;
  }

  bool TokenHasEnabledGroup(HANDLE token, WELL_KNOWN_SID_TYPE type)
  {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> sidBuffer = {};
    DWORD sidBytes = static_cast<DWORD>(sidBuffer.size());
    if (!CreateWellKnownSid(type, nullptr, sidBuffer.data(), &sidBytes))
      return false;

    std::vector<uint8_t> data;
    if (!GetTokenData(token, TokenGroups, data))
      return false;
    const TOKEN_GROUPS * groups =
      reinterpret_cast<const TOKEN_GROUPS *>(data.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i)
      if (EqualSid(groups->Groups[i].Sid, sidBuffer.data()) &&
          (groups->Groups[i].Attributes & SE_GROUP_ENABLED) &&
          !(groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY))
        return true;
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
  }

  bool GetLogonSid(HANDLE token, std::vector<uint8_t>& sid)
  {
    std::vector<uint8_t> data;
    if (!GetTokenData(token, TokenGroups, data))
      return false;
    const TOKEN_GROUPS * groups =
      reinterpret_cast<const TOKEN_GROUPS *>(data.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i)
      if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) ==
          SE_GROUP_LOGON_ID)
      {
        const DWORD bytes = GetLengthSid(groups->Groups[i].Sid);
        try
        {
          sid.resize(bytes);
        }
        catch (...)
        {
          SetLastError(ERROR_OUTOFMEMORY);
          return false;
        }
        if (!CopySid(bytes, sid.data(), groups->Groups[i].Sid))
          return false;
        return true;
      }
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
  }

  bool ValidateLaunchToken(HANDLE token, DWORD expectedSession,
    std::vector<uint8_t>& logonSid)
  {
    DWORD bytes = 0;
    TOKEN_TYPE tokenType = TokenImpersonation;
    if (!GetTokenInformation(token, TokenType, &tokenType,
        sizeof(tokenType), &bytes))
      return TokenFailure(GetLastError(),
        "Failed to query interactive token type");
    if (tokenType != TokenPrimary)
      return TokenFailure(ERROR_ACCESS_DENIED,
        "Interactive token is not a primary token");

    DWORD session = NO_CONSOLE_SESSION;
    if (!GetTokenInformation(token, TokenSessionId, &session,
        sizeof(session), &bytes))
      return TokenFailure(GetLastError(),
        "Failed to query interactive token session");
    if (session != expectedSession)
      return TokenFailure(ERROR_ACCESS_DENIED,
        "Interactive token belongs to the wrong session");

    DWORD appContainer = 0;
    if (!GetTokenInformation(token, TokenIsAppContainer, &appContainer,
        sizeof(appContainer), &bytes))
      return TokenFailure(GetLastError(),
        "Failed to query interactive token AppContainer state");
    if (appContainer)
      return TokenFailure(ERROR_ACCESS_DENIED,
        "Interactive token is an AppContainer token");

    TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
    if (!GetTokenInformation(token, TokenElevationType, &elevation,
        sizeof(elevation), &bytes))
      return TokenFailure(GetLastError(),
        "Failed to query interactive token elevation");
    if (elevation == TokenElevationTypeFull)
      return TokenFailure(ERROR_ACCESS_DENIED,
        "Interactive token is an unfiltered elevated token");

    std::vector<uint8_t> userData;
    if (!GetTokenData(token, TokenUser, userData))
      return TokenFailure(GetLastError(),
        "Failed to query interactive token user SID");
    const PSID user =
      reinterpret_cast<const TOKEN_USER *>(userData.data())->User.Sid;
    if (IsWellKnownSid(user, WinLocalSystemSid) ||
        IsWellKnownSid(user, WinLocalServiceSid) ||
        IsWellKnownSid(user, WinNetworkServiceSid))
    {
      return TokenFailure(ERROR_ACCESS_DENIED,
        "Interactive token belongs to a service identity");
    }

    std::vector<uint8_t> labelData;
    if (!GetTokenData(token, TokenIntegrityLevel, labelData))
      return TokenFailure(GetLastError(),
        "Failed to query interactive token integrity level");
    const PSID label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(
      labelData.data())->Label.Sid;
    if (!IsValidSid(label) || *GetSidSubAuthorityCount(label) == 0)
    {
      return TokenFailure(ERROR_INVALID_DATA,
        "Interactive token has an invalid integrity label");
    }
    const DWORD integrity = *GetSidSubAuthority(label,
      *GetSidSubAuthorityCount(label) - 1);
    if (integrity < SECURITY_MANDATORY_MEDIUM_RID ||
        integrity >= SECURITY_MANDATORY_SYSTEM_RID ||
        (integrity >= SECURITY_MANDATORY_HIGH_RID &&
         elevation != TokenElevationTypeDefault))
    {
      return TokenFailure(ERROR_ACCESS_DENIED,
        "Interactive token integrity level is not permitted");
    }

    if (!TokenHasEnabledGroup(token, WinInteractiveSid))
      return TokenFailure(GetLastError(),
        "Interactive token lacks the enabled INTERACTIVE group");
    if (!GetLogonSid(token, logonSid))
      return TokenFailure(GetLastError(),
        "Interactive token lacks a logon SID");
    return true;
  }

  class CObjectSecurity
  {
  private:
    BYTE m_systemSid[SECURITY_MAX_SID_SIZE] = {};
    BYTE m_mediumSid[SECURITY_MAX_SID_SIZE] = {};
    std::vector<uint8_t> m_acl;
    std::vector<uint8_t> m_sacl;
    SECURITY_DESCRIPTOR m_descriptor = {};
    SECURITY_ATTRIBUTES m_attributes = {};

  public:
    bool Init(PSID logonSid, DWORD systemAccess, DWORD userAccess,
      bool mediumIntegrity = false)
    {
      DWORD systemSidBytes = sizeof(m_systemSid);
      if (!CreateWellKnownSid(WinLocalSystemSid, nullptr,
          m_systemSid, &systemSidBytes))
        return TokenFailure(GetLastError(),
          "Failed to create SYSTEM SID for shared object security");

      const DWORD systemAceBytes = sizeof(ACCESS_ALLOWED_ACE) -
        sizeof(DWORD) + GetLengthSid(m_systemSid);
      const DWORD userAceBytes = logonSid ?
        sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
          GetLengthSid(logonSid) : 0;
      try
      {
        m_acl.resize(sizeof(ACL) + systemAceBytes + userAceBytes);
      }
      catch (...)
      {
        return TokenFailure(ERROR_OUTOFMEMORY,
          "Failed to allocate shared object DACL");
      }

      PACL acl = reinterpret_cast<PACL>(m_acl.data());
      if (!InitializeAcl(acl, static_cast<DWORD>(m_acl.size()),
          ACL_REVISION))
        return TokenFailure(GetLastError(),
          "Failed to initialize shared object DACL");
      if (!AddAccessAllowedAceEx(acl, ACL_REVISION, 0,
          systemAccess, m_systemSid))
        return TokenFailure(GetLastError(),
          "Failed to grant SYSTEM access to shared object");
      if (logonSid && !AddAccessAllowedAceEx(acl, ACL_REVISION, 0,
          userAccess, logonSid))
        return TokenFailure(GetLastError(),
          "Failed to grant logon SID access to shared object");
      if (!InitializeSecurityDescriptor(
          &m_descriptor, SECURITY_DESCRIPTOR_REVISION))
        return TokenFailure(GetLastError(),
          "Failed to initialize shared object security descriptor");
      if (!SetSecurityDescriptorDacl(
          &m_descriptor, TRUE, acl, FALSE))
        return TokenFailure(GetLastError(),
          "Failed to set shared object DACL");

      if (mediumIntegrity)
      {
        DWORD mediumSidBytes = sizeof(m_mediumSid);
        if (!CreateWellKnownSid(WinMediumLabelSid, nullptr,
            m_mediumSid, &mediumSidBytes))
          return TokenFailure(GetLastError(),
            "Failed to create medium mandatory label SID");
        const DWORD mandatoryAceBytes =
          sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) +
            GetLengthSid(m_mediumSid);
        try
        {
          m_sacl.resize(sizeof(ACL) + mandatoryAceBytes);
        }
        catch (...)
        {
          return TokenFailure(ERROR_OUTOFMEMORY,
            "Failed to allocate shared object mandatory label ACL");
        }
        PACL sacl = reinterpret_cast<PACL>(m_sacl.data());
        if (!InitializeAcl(sacl, static_cast<DWORD>(m_sacl.size()),
            ACL_REVISION))
          return TokenFailure(GetLastError(),
            "Failed to initialize shared object mandatory label ACL");
        if (!AddMandatoryAce(sacl, ACL_REVISION, 0,
            SYSTEM_MANDATORY_LABEL_NO_WRITE_UP |
              SYSTEM_MANDATORY_LABEL_NO_READ_UP,
            m_mediumSid))
          return TokenFailure(GetLastError(),
            "Failed to add shared object medium mandatory label");
        if (!SetSecurityDescriptorSacl(
            &m_descriptor, TRUE, sacl, FALSE))
          return TokenFailure(GetLastError(),
            "Failed to set shared object mandatory label");
      }

      const SECURITY_DESCRIPTOR_CONTROL protectedParts =
        static_cast<SECURITY_DESCRIPTOR_CONTROL>(SE_DACL_PROTECTED |
          (mediumIntegrity ? SE_SACL_PROTECTED : 0));
      if (!SetSecurityDescriptorControl(
          &m_descriptor, protectedParts, protectedParts))
        return TokenFailure(GetLastError(),
          "Failed to protect the shared object security descriptor");

      m_attributes.nLength              = sizeof(m_attributes);
      m_attributes.lpSecurityDescriptor = &m_descriptor;
      m_attributes.bInheritHandle       = FALSE;
      return true;
    }

    SECURITY_ATTRIBUTES * Get() { return &m_attributes; }
  };

  bool GenerateRandomId(uint64_t (&random)[2], const char * stage)
  {
    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
      const NTSTATUS status = BCryptGenRandom(nullptr,
        reinterpret_cast<PUCHAR>(random), sizeof(random),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      if (status < 0)
      {
        DEBUG_ERROR_HR(HRESULT_FROM_NT(status), "%s", stage);
        return false;
      }
      if (random[0] && random[1])
        return true;
    }
    DEBUG_ERROR_HR(ERROR_INVALID_DATA, "%s", stage);
    return false;
  }

  bool GenerateChildObjectNames(
    wchar_t * lifetimeName, size_t lifetimeCount,
    wchar_t * stopName, size_t stopCount,
    wchar_t * activationName, size_t activationCount)
  {
    uint64_t random[2];
    if (!GenerateRandomId(random, "Failed to generate child object names"))
      return false;

    const int lifetimeResult = _snwprintf_s(
      lifetimeName, lifetimeCount, _TRUNCATE,
      L"Global\\LookingGlassIDDHelperLifetime-%016llx%016llx",
      static_cast<unsigned long long>(random[0]),
      static_cast<unsigned long long>(random[1]));
    const int stopResult = _snwprintf_s(
      stopName, stopCount, _TRUNCATE,
      L"Global\\LookingGlassIDDHelperStop-%016llx%016llx",
      static_cast<unsigned long long>(random[0]),
      static_cast<unsigned long long>(random[1]));
    const int activationResult = _snwprintf_s(
      activationName, activationCount, _TRUNCATE,
      L"Global\\LookingGlassIDDHelperActivate-%016llx%016llx",
      static_cast<unsigned long long>(random[0]),
      static_cast<unsigned long long>(random[1]));
    if (lifetimeResult < 0 || stopResult < 0 || activationResult < 0)
    {
      DEBUG_ERROR_HR(ERROR_INSUFFICIENT_BUFFER,
        "Failed to format child object names");
      return false;
    }
    return true;
  }
}

static uint64_t FileTimeValue(const FILETIME& value)
{
  ULARGE_INTEGER result = {};
  result.LowPart  = value.dwLowDateTime;
  result.HighPart = value.dwHighDateTime;
  return result.QuadPart;
}

static bool QueryAuthorityHost(HANDLE device, LGIddAuthorityHost& host)
{
  ZeroMemory(&host, sizeof(host));
  DWORD bytes = 0;
  if (!DeviceIoControl(device, IOCTL_LG_IDD_AUTHORITY_GET_HOST,
      nullptr, 0, &host, sizeof(host), &bytes, nullptr))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error, "Failed to query the LGIdd authority host");
    return false;
  }
  if (bytes != sizeof(host) || host.size != sizeof(host) ||
      host.version != LG_IDD_AUTHORITY_VERSION || host.reserved ||
      !host.processId || !host.processCreated ||
      !host.instanceId[0] || !host.instanceId[1])
  {
    DEBUG_ERROR_HR(ERROR_INVALID_DATA,
      "LGIdd returned invalid authority host information");
    return false;
  }
  return true;
}

static HANDLE OpenAuthorityDevice(LGIddAuthorityHost& host)
{
  HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_LGIdd,
    nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devices == INVALID_HANDLE_VALUE)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to enumerate the LGIdd authority interface");
    return INVALID_HANDLE_VALUE;
  }

  HANDLE device = INVALID_HANDLE_VALUE;
  for (DWORD index = 0; ; ++index)
  {
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    if (!SetupDiEnumDeviceInterfaces(devices, nullptr,
        &GUID_DEVINTERFACE_LGIdd, index, &interfaceData))
    {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_ITEMS)
        DEBUG_ERROR_HR(error,
          "Failed to enumerate an LGIdd authority interface");
      break;
    }

    DWORD detailBytes = 0;
    const BOOL sized = SetupDiGetDeviceInterfaceDetailW(
      devices, &interfaceData, nullptr, 0, &detailBytes, nullptr);
    const DWORD detailSizeError = sized ? ERROR_SUCCESS : GetLastError();
    if (sized || detailSizeError != ERROR_INSUFFICIENT_BUFFER ||
        detailBytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
    {
      DEBUG_ERROR_HR(detailSizeError ? detailSizeError : ERROR_INVALID_DATA,
        "Failed to size the LGIdd authority interface path");
      continue;
    }

    std::vector<uint8_t> detailStorage;
    try
    {
      detailStorage.resize(detailBytes);
    }
    catch (...)
    {
      DEBUG_ERROR_HR(ERROR_OUTOFMEMORY,
        "Failed to allocate the LGIdd authority interface path");
      break;
    }
    SP_DEVICE_INTERFACE_DETAIL_DATA_W * detail =
      reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(
        detailStorage.data());
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData,
        detail, detailBytes, nullptr, nullptr))
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to read the LGIdd authority interface path");
      continue;
    }

    device = CreateFileW(detail->DevicePath,
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE)
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error, "Failed to open the LGIdd authority interface");
      continue;
    }
    if (QueryAuthorityHost(device, host))
      break;

    CloseHandle(device);
    device = INVALID_HANDLE_VALUE;
  }

  if (!SetupDiDestroyDeviceInfoList(devices))
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error,
      "Failed to release the LGIdd authority interface list");
  }
  if (device == INVALID_HANDLE_VALUE)
    DEBUG_ERROR_HR(ERROR_NOT_FOUND,
      "No usable LGIdd authority interface was found");
  return device;
}

static bool RegisterClipboardAuthority(DWORD session,
  const uint64_t (&mappingId)[2], HANDLE mapping)
{
  ClearClipboardAuthority();

  LGIddAuthorityHost host = {};
  HANDLE device = OpenAuthorityDevice(host);
  if (device == INVALID_HANDLE_VALUE)
    return false;

  HANDLE hostProcess = OpenProcess(PROCESS_DUP_HANDLE |
    PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
    FALSE, host.processId);
  if (!hostProcess)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error, "Failed to open the LGIdd authority host process");
    CloseHandle(device);
    return false;
  }

  FILETIME created = {};
  FILETIME exited  = {};
  FILETIME kernel  = {};
  FILETIME user    = {};
  if (!GetProcessTimes(hostProcess, &created, &exited, &kernel, &user))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to verify the LGIdd authority host process");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }
  if (FileTimeValue(created) != host.processCreated)
  {
    DEBUG_ERROR_HR(ERROR_ACCESS_DENIED,
      "LGIdd authority host process identity changed");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }
  const DWORD hostWait = WaitForSingleObject(hostProcess, 0);
  if (hostWait == WAIT_FAILED)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to check the LGIdd authority host process");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }
  if (hostWait != WAIT_TIMEOUT)
  {
    DEBUG_ERROR_HR(ERROR_PROCESS_ABORTED,
      "LGIdd authority host process exited during registration");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }

  HANDLE remoteMapping = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), mapping,
      hostProcess, &remoteMapping,
      SECTION_MAP_READ | SECTION_MAP_WRITE, FALSE, 0))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to duplicate the clipboard mapping into LGIdd");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }

  LGIddAuthorityRegistration registration = {};
  registration.size          = sizeof(registration);
  registration.version       = LG_IDD_AUTHORITY_VERSION;
  registration.session       = session;
  registration.instanceId[0] = host.instanceId[0];
  registration.instanceId[1] = host.instanceId[1];
  registration.mappingId[0]  = mappingId[0];
  registration.mappingId[1]  = mappingId[1];
  registration.mappingHandle = reinterpret_cast<uintptr_t>(remoteMapping);

  LGIddAuthorityHost confirmedHost = {};
  if (!QueryAuthorityHost(device, confirmedHost) ||
      memcmp(&confirmedHost, &host, sizeof(host)) != 0)
  {
    if (confirmedHost.size)
      DEBUG_ERROR_HR(ERROR_ACCESS_DENIED,
        "LGIdd authority host changed before registration");
    DEBUG_WARN("The unclaimed LGIdd mapping handle will be reclaimed when "
      "the driver host exits");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }
  const DWORD confirmedWait = WaitForSingleObject(hostProcess, 0);
  if (confirmedWait == WAIT_FAILED)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to recheck the LGIdd authority host process");
    DEBUG_WARN("The unclaimed LGIdd mapping handle will be reclaimed when "
      "the driver host exits");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }
  if (confirmedWait != WAIT_TIMEOUT)
  {
    DEBUG_ERROR_HR(ERROR_PROCESS_ABORTED,
      "LGIdd authority host exited before registration");
    CloseHandle(hostProcess);
    CloseHandle(device);
    return false;
  }

  DWORD bytes = 0;
  const BOOL registered = DeviceIoControl(device,
    IOCTL_LG_IDD_AUTHORITY_REGISTER,
    &registration, sizeof(registration), nullptr, 0, &bytes, nullptr);
  const DWORD registerError = registered ? ERROR_SUCCESS : GetLastError();
  CloseHandle(hostProcess);
  if (!registered || bytes)
  {
    DEBUG_ERROR_HR(registered ? ERROR_INVALID_DATA : registerError,
      "Failed to register clipboard authority with LGIdd");
    DEBUG_WARN("The unclaimed LGIdd mapping handle will be reclaimed when "
      "the driver host exits");
    CloseHandle(device);
    return false;
  }

  l_authorityDevice = device;
  l_authorityHost   = host;
  DEBUG_INFO("Registered clipboard authority with LGIdd process %lu",
    host.processId);
  return true;
}

static bool VerifyClipboardAuthority()
{
  if (l_authorityDevice == INVALID_HANDLE_VALUE)
    return false;

  LGIddAuthorityHost current = {};
  if (!QueryAuthorityHost(l_authorityDevice, current))
    return false;
  if (memcmp(&current, &l_authorityHost, sizeof(current)) != 0)
  {
    DEBUG_ERROR_HR(ERROR_ACCESS_DENIED,
      "LGIdd authority host identity changed");
    return false;
  }
  return true;
}

static void ClearClipboardAuthority()
{
  if (l_authorityDevice == INVALID_HANDLE_VALUE)
    return;

  LGIddAuthorityClear clear = {};
  clear.size          = sizeof(clear);
  clear.version       = LG_IDD_AUTHORITY_VERSION;
  clear.instanceId[0] = l_authorityHost.instanceId[0];
  clear.instanceId[1] = l_authorityHost.instanceId[1];

  DWORD bytes = 0;
  if (!DeviceIoControl(l_authorityDevice,
      IOCTL_LG_IDD_AUTHORITY_CLEAR,
      &clear, sizeof(clear), nullptr, 0, &bytes, nullptr))
  {
    const DWORD error = GetLastError();
    DEBUG_WARN_HR(error, "Failed to clear clipboard authority in LGIdd");
  }
  else if (bytes)
    DEBUG_WARN(
      "LGIdd returned unexpected clipboard authority clear data");

  CloseHandle(l_authorityDevice);
  l_authorityDevice = INVALID_HANDLE_VALUE;
  ZeroMemory(&l_authorityHost, sizeof(l_authorityHost));
}

static bool Launch(DWORD sessionId)
{
  if (l_process.IsValid())
    return false;

  ClearClipboardAuthority();

  if (sessionId == NO_CONSOLE_SESSION || sessionId == 0 ||
      WTSGetActiveConsoleSessionId() != sessionId)
  {
    DEBUG_WARN("Refusing to launch outside the active console session");
    return false;
  }

  if (!EnablePriv(SE_TCB_NAME))
  {
    DEBUG_ERROR("Failed to enable %ls", SE_TCB_NAME);
    return false;
  }

  HANDLE queriedRaw = nullptr;
  const bool queried = WTSQueryUserToken(sessionId, &queriedRaw) != FALSE;
  const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
  DisablePriv(SE_TCB_NAME);
  HandleT<HANDLENullTraits> queriedToken(queriedRaw);
  if (!queried || !queriedToken.IsValid())
  {
    DEBUG_ERROR_HR(queryError ? queryError : ERROR_NO_TOKEN,
      "WTSQueryUserToken failed");
    return false;
  }

  HandleT<HANDLENullTraits> linkedToken;
  DWORD returnedLen = 0;
  TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
  if (!GetTokenInformation(queriedToken.Get(), TokenElevationType,
      &elevation, sizeof(elevation), &returnedLen))
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to inspect the interactive user token elevation");
    return false;
  }
  HANDLE sourceToken = queriedToken.Get();
  if (elevation == TokenElevationTypeFull)
  {
    TOKEN_LINKED_TOKEN linked = {};
    if (!GetTokenInformation(queriedToken.Get(), TokenLinkedToken,
        &linked, sizeof(linked), &returnedLen))
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to query the limited linked interactive token");
      return false;
    }
    if (!linked.LinkedToken)
    {
      DEBUG_ERROR_HR(ERROR_NO_TOKEN,
        "Elevated interactive token has no limited linked token");
      return false;
    }
    linkedToken.Attach(linked.LinkedToken);
    sourceToken = linkedToken.Get();
  }

  HandleT<HANDLENullTraits> token;
  const DWORD tokenAccess = TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
    TOKEN_QUERY;
  if (!DuplicateTokenEx(sourceToken, tokenAccess, nullptr,
      SecurityImpersonation, TokenPrimary, token.GetAddressOf()))
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to duplicate the interactive user token");
    return false;
  }

  std::vector<uint8_t> logonSid;
  if (!ValidateLaunchToken(token.Get(), sessionId, logonSid))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Rejected the interactive user token");
    return false;
  }

  CObjectSecurity lifetimeSecurity;
  CObjectSecurity stopSecurity;
  if (!lifetimeSecurity.Init(logonSid.data(), MUTEX_ALL_ACCESS,
        SYNCHRONIZE) ||
      !stopSecurity.Init(logonSid.data(), EVENT_ALL_ACCESS, SYNCHRONIZE))
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create child lifecycle security descriptors");
    return false;
  }

  wchar_t lifetimeName[128];
  wchar_t stopEventName[128];
  wchar_t activationName[128];
  if (!GenerateChildObjectNames(lifetimeName, ARRAY_LENGTH(lifetimeName),
      stopEventName, ARRAY_LENGTH(stopEventName),
      activationName, ARRAY_LENGTH(activationName)))
    return false;

  SetLastError(ERROR_SUCCESS);
  const HANDLE lifetime = CreateMutexW(
    lifetimeSecurity.Get(), TRUE, lifetimeName);
  const DWORD lifetimeError = GetLastError();
  if (!lifetime)
  {
    DEBUG_ERROR_HR(lifetimeError,
      "Failed to create the child lifetime mutex");
    return false;
  }
  if (lifetimeError == ERROR_ALREADY_EXISTS)
  {
    CloseHandle(lifetime);
    DEBUG_ERROR("The child lifetime mutex already exists");
    return false;
  }
  l_childLifetimeMutex.Attach(lifetime);

  SetLastError(ERROR_SUCCESS);
  const HANDLE stopEvent = CreateEventW(
    stopSecurity.Get(), TRUE, FALSE, stopEventName);
  const DWORD stopError = GetLastError();
  if (!stopEvent)
  {
    DEBUG_ERROR_HR(stopError,
      "Failed to create the child stop event");
    CloseChildLifetimeMutex();
    return false;
  }
  if (stopError == ERROR_ALREADY_EXISTS)
  {
    CloseHandle(stopEvent);
    DEBUG_ERROR("The child stop event already exists");
    CloseChildLifetimeMutex();
    return false;
  }
  l_childStopEvent.Attach(stopEvent);

  SetLastError(ERROR_SUCCESS);
  const HANDLE activationEvent = CreateEventW(
    stopSecurity.Get(), TRUE, FALSE, activationName);
  const DWORD activationError = GetLastError();
  if (!activationEvent)
  {
    DEBUG_ERROR_HR(activationError,
      "Failed to create the child activation event");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }
  if (activationError == ERROR_ALREADY_EXISTS)
  {
    CloseHandle(activationEvent);
    DEBUG_ERROR("The child activation event already exists");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }
  l_childActivationEvent.Attach(activationEvent);

  CObjectSecurity mappingSecurity;
  if (!mappingSecurity.Init(logonSid.data(), SECTION_ALL_ACCESS,
      SECTION_MAP_READ | SECTION_MAP_WRITE, true))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create clipboard mapping security descriptor");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  uint64_t mappingId[2];
  if (!GenerateRandomId(mappingId,
      "Failed to generate clipboard mapping identifier"))
  {
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  uint64_t authorityId[2];
  if (!GenerateRandomId(authorityId,
      "Failed to generate clipboard authority identifier"))
  {
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  wchar_t mappingName[128];
  const int mappingNameResult = _snwprintf_s(
    mappingName, ARRAY_LENGTH(mappingName), _TRUNCATE,
    L"Global\\LookingGlassIDDClipboard-%016llx%016llx",
    static_cast<unsigned long long>(mappingId[0]),
    static_cast<unsigned long long>(mappingId[1]));
  if (mappingNameResult < 0)
  {
    DEBUG_ERROR_HR(ERROR_INSUFFICIENT_BUFFER,
      "Failed to format clipboard mapping name");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  LARGE_INTEGER mappingBytes = {};
  mappingBytes.QuadPart = sizeof(ClipboardMapping);

  const bool globalEnabled   = EnablePriv(SE_CREATE_GLOBAL_NAME);
  const bool securityEnabled = globalEnabled && EnablePriv(SE_SECURITY_NAME);
  if (!globalEnabled || !securityEnabled)
  {
    DEBUG_ERROR("Failed to enable clipboard mapping privileges");
    if (globalEnabled)
      DisablePriv(SE_CREATE_GLOBAL_NAME);
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  SetLastError(ERROR_SUCCESS);
  const HANDLE clipboardMapping = CreateFileMappingW(INVALID_HANDLE_VALUE,
    mappingSecurity.Get(), PAGE_READWRITE, mappingBytes.HighPart,
    mappingBytes.LowPart, mappingName);
  const DWORD mappingError = GetLastError();
  DisablePriv(SE_SECURITY_NAME);
  DisablePriv(SE_CREATE_GLOBAL_NAME);
  if (!clipboardMapping)
  {
    DEBUG_ERROR_HR(mappingError,
      "Failed to create service-owned clipboard mapping");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }
  if (mappingError == ERROR_ALREADY_EXISTS)
  {
    CloseHandle(clipboardMapping);
    DEBUG_ERROR("The service-owned clipboard mapping already exists");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }
  l_childClipboardMapping.Attach(clipboardMapping);

  ClipboardMapping * initialMapping = static_cast<ClipboardMapping *>(
    MapViewOfFile(clipboardMapping, FILE_MAP_READ | FILE_MAP_WRITE,
      0, 0, sizeof(ClipboardMapping)));
  if (!initialMapping)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to initialize the clipboard authority identifier");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  ZeroMemory(initialMapping, sizeof(*initialMapping));
  initialMapping->authorityId[0] = authorityId[0];
  initialMapping->authorityId[1] = authorityId[1];
  if (!UnmapViewOfFile(initialMapping))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to unmap the initialized clipboard authority section");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  LPVOID env = nullptr;
  if (!CreateEnvironmentBlock(&env, token.Get(), FALSE))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error, "CreateEnvironmentBlock failed");
    l_childClipboardMapping.Close();
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  bool quotaEnabled = EnablePriv(SE_INCREASE_QUOTA_NAME);
  bool assignEnabled = quotaEnabled && EnablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
  if (!quotaEnabled || !assignEnabled)
  {
    DEBUG_ERROR("Failed to enable process-launch privileges");
    if (quotaEnabled)
      DisablePriv(SE_INCREASE_QUOTA_NAME);
    DestroyEnvironmentBlock(env);
    l_childClipboardMapping.Close();
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  PROCESS_INFORMATION pi = {};
  STARTUPINFO si = {};
  si.cb          = sizeof(si);
  si.dwFlags     = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOW;
  si.lpDesktop   = const_cast<LPWSTR>(L"WinSta0\\Default");

  wchar_t cmdBuf[608];
  const int commandResult = _snwprintf_s(
    cmdBuf, ARRAY_LENGTH(cmdBuf), _TRUNCATE,
    L"\"LGIddHelper.exe\" %s %s %s %016llx%016llx",
    lifetimeName, stopEventName, activationName,
    static_cast<unsigned long long>(mappingId[0]),
    static_cast<unsigned long long>(mappingId[1]));
  if (commandResult < 0)
  {
    DEBUG_ERROR_HR(ERROR_INSUFFICIENT_BUFFER,
      "Failed to build the child command line");
    DisablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
    DisablePriv(SE_INCREASE_QUOTA_NAME);
    DestroyEnvironmentBlock(env);
    l_childClipboardMapping.Close();
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  if (WTSGetActiveConsoleSessionId() != sessionId)
  {
    DEBUG_WARN("Active console session changed before child launch");
    DisablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
    DisablePriv(SE_INCREASE_QUOTA_NAME);
    DestroyEnvironmentBlock(env);
    l_childClipboardMapping.Close();
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  if (!RegisterClipboardAuthority(sessionId, mappingId, clipboardMapping))
  {
    DisablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
    DisablePriv(SE_INCREASE_QUOTA_NAME);
    DestroyEnvironmentBlock(env);
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  const bool created = CreateProcessAsUserW(
    token.Get(),
    l_executable.c_str(),
    cmdBuf,
    NULL,
    NULL,
    FALSE,
    DETACHED_PROCESS | HIGH_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
    env,
    NULL,
    &si,
    &pi
  );
  const DWORD createError = created ? ERROR_SUCCESS : GetLastError();

  DisablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
  DisablePriv(SE_INCREASE_QUOTA_NAME);
  DestroyEnvironmentBlock(env);

  if (!created)
  {
    ClearClipboardAuthority();
    DEBUG_ERROR_HR(createError, "CreateProcessAsUser failed");
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    return false;
  }

  const auto discardGatedChild = [&pi]()
  {
    ClearClipboardAuthority();
    if (!SetEvent(l_childStopEvent.Get()))
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to cancel the gated child process");
    }

    DWORD waitResult = WaitForSingleObject(pi.hProcess, 5000);
    if (waitResult == WAIT_FAILED)
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to wait for the gated child process to stop");
    }
    else if (waitResult == WAIT_TIMEOUT)
    {
      if (!TerminateProcess(pi.hProcess, EXIT_FAILURE))
      {
        const DWORD error = GetLastError();
        DEBUG_ERROR_HR(error,
          "Failed to terminate the gated child process");
      }
      else
      {
        waitResult = WaitForSingleObject(pi.hProcess, 1000);
        if (waitResult == WAIT_FAILED)
        {
          const DWORD error = GetLastError();
          DEBUG_ERROR_HR(error,
            "Failed to wait for the terminated gated child process");
        }
        else if (waitResult != WAIT_OBJECT_0)
          DEBUG_ERROR_HR(ERROR_TIMEOUT,
            "Terminated gated child process did not exit in time");
      }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ZeroMemory(&pi, sizeof(pi));
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
  };

  DWORD finalTokenSession = NO_CONSOLE_SESSION;
  DWORD finalTokenBytes   = 0;
  if (!GetTokenInformation(token.Get(), TokenSessionId,
      &finalTokenSession, sizeof(finalTokenSession), &finalTokenBytes))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to recheck the child token session before activation");
    discardGatedChild();
    return false;
  }
  if (finalTokenSession != sessionId)
  {
    DEBUG_WARN(
      "Child token session changed before activation");
    discardGatedChild();
    return false;
  }

  const DWORD finalActiveSession = WTSGetActiveConsoleSessionId();
  if (finalActiveSession != sessionId)
  {
    DEBUG_WARN(
      "Active console session changed before child activation");
    discardGatedChild();
    return false;
  }

  const DWORD stopResult = WaitForSingleObject(l_svcStopEvent.Get(), 0);
  if (stopResult == WAIT_FAILED)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to check the service stop event before child activation");
    discardGatedChild();
    return false;
  }
  if (stopResult != WAIT_TIMEOUT)
  {
    DEBUG_WARN(
      "Service stop requested before child activation");
    discardGatedChild();
    return false;
  }

  if (!SetEvent(l_childActivationEvent.Get()))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error, "Failed to activate the child process");
    discardGatedChild();
    return false;
  }

  l_process.Attach(pi.hProcess);
  CloseHandle(pi.hThread);
  l_childSession = sessionId;
  DEBUG_INFO("Started child process %lu in session %lu",
    pi.dwProcessId, sessionId);
  return true;
}

static void CloseChildLifetimeMutex()
{
  l_childClipboardMapping.Close();
  l_childActivationEvent.Close();
  if (!l_childLifetimeMutex.IsValid())
    return;

  if (!ReleaseMutex(l_childLifetimeMutex.Get()))
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error, "Failed to release the child lifetime mutex");
  }
  l_childLifetimeMutex.Close();
}

static bool StopChild()
{
  ClearClipboardAuthority();
  if (!l_process.IsValid())
  {
    l_childStopEvent.Close();
    CloseChildLifetimeMutex();
    l_childSession = NO_CONSOLE_SESSION;
    return true;
  }

  DEBUG_INFO("Stopping child process in session %lu", l_childSession);
  if (l_childStopEvent.IsValid() && !SetEvent(l_childStopEvent.Get()))
    DEBUG_ERROR_HR(GetLastError(), "Failed to signal the child stop event");

  DWORD result = WaitForSingleObject(l_process.Get(), 5000);
  if (result == WAIT_TIMEOUT)
  {
    DEBUG_WARN("Child process did not stop in time, terminating it");
    if (!TerminateProcess(l_process.Get(), EXIT_FAILURE))
    {
      DEBUG_ERROR_HR(GetLastError(), "Failed to terminate child process");
      return false;
    }
    else
    {
      result = WaitForSingleObject(l_process.Get(), 1000);
      if (result == WAIT_FAILED)
      {
        const DWORD error = GetLastError();
        DEBUG_ERROR_HR(error,
          "Failed to wait for the terminated child process");
        return false;
      }
    }
  }
  else if (result == WAIT_FAILED)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to wait for child process");
    return false;
  }

  if (result != WAIT_OBJECT_0)
  {
    DEBUG_ERROR("Child process did not terminate");
    return false;
  }

  l_process.Close();
  l_childStopEvent.Close();
  CloseChildLifetimeMutex();
  l_childSession = NO_CONSOLE_SESSION;
  return true;
}
