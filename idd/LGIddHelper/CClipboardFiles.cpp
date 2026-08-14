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

#include "CClipboardFiles.h"

#include <ShlObj.h>
#include <strsafe.h>
#include <WtsApi32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace
{
  static constexpr uint64_t WINDOWS_EPOCH_TICKS =
    UINT64_C(116444736000000000);
  static constexpr size_t COPY_BUFFER_BYTES =
    static_cast<size_t>(64U) * 1024U;

  class CThreadImpersonation final
  {
  private:
    HANDLE m_previousToken = nullptr;
    bool m_active           = false;
    DWORD m_error           = ERROR_SUCCESS;

    void ClosePreviousToken()
    {
      if (m_previousToken)
      {
        CloseHandle(m_previousToken);
        m_previousToken = nullptr;
      }
    }

    bool RevertInternal()
    {
      if (!m_active)
        return true;
      const bool restored = m_previousToken ?
        SetThreadToken(nullptr, m_previousToken) != FALSE :
        RevertToSelf() != FALSE;
      if (restored)
      {
        m_active = false;
        ClosePreviousToken();
        return true;
      }

      const DWORD error = GetLastError();
      if (!m_previousToken && SetThreadToken(nullptr, nullptr))
      {
        m_active = false;
        m_error = error;
        SetLastError(error);
        return false;
      }

      RevertToSelf();
      TerminateProcess(GetCurrentProcess(), error ? error :
        ERROR_ACCESS_DENIED);
      std::terminate();
    }

  public:
    explicit CThreadImpersonation(HANDLE token)
    {
      if (!token)
        return;

      if (!OpenThreadToken(GetCurrentThread(),
          TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &m_previousToken))
      {
        const DWORD error = GetLastError();
        if (error != ERROR_NO_TOKEN)
        {
          m_error = error;
          return;
        }
        SetLastError(ERROR_SUCCESS);
      }

      if (SetThreadToken(nullptr, token))
        m_active = true;
      else
      {
        const DWORD error = GetLastError();
        ClosePreviousToken();
        m_error = error;
        SetLastError(error);
      }
    }

    ~CThreadImpersonation()
    {
      const DWORD error = GetLastError();
      bool restored = true;
      if (m_active)
        restored = RevertInternal();
      ClosePreviousToken();
      if (restored)
        SetLastError(error);
    }

    bool Active() const
    {
      return m_active;
    }

    DWORD Error() const
    {
      return m_error;
    }

    bool Finish()
    {
      const DWORD error = GetLastError();
      if (!RevertInternal())
        return false;
      SetLastError(error);
      return true;
    }
  };

  KVMFRClipboardFileError TokenError(DWORD error)
  {
    switch (error)
    {
      case ERROR_NOT_ENOUGH_MEMORY:
      case ERROR_OUTOFMEMORY:
        return KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      default:
        return KVMFR_CLIPBOARD_FILE_ERROR_ACCESS;
    }
  }

  bool CaptureUserToken(HANDLE& token,
    KVMFRClipboardFileError& error, DWORD& winError)
  {
    token = nullptr;
    winError = ERROR_SUCCESS;
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId))
    {
      winError = GetLastError();
      error = TokenError(winError);
      return false;
    }

    HANDLE processToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE,
        &processToken))
    {
      winError = GetLastError();
      error = TokenError(winError);
      return false;
    }

    HANDLE brokerToken = nullptr;
    const bool brokerDuplicated = DuplicateTokenEx(processToken,
      TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr,
      SecurityImpersonation, TokenImpersonation, &brokerToken) != FALSE;
    const DWORD brokerError = brokerDuplicated ? ERROR_SUCCESS :
      GetLastError();
    CloseHandle(processToken);
    if (!brokerDuplicated)
    {
      winError = brokerError;
      error = TokenError(brokerError);
      return false;
    }

    LUID privilege = {};
    if (!LookupPrivilegeValueW(nullptr, SE_TCB_NAME, &privilege))
    {
      winError = GetLastError();
      CloseHandle(brokerToken);
      error = TokenError(winError);
      return false;
    }

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = privilege;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const bool adjusted = AdjustTokenPrivileges(brokerToken, FALSE,
      &privileges, 0, nullptr, nullptr) != FALSE;
    const DWORD adjustError = GetLastError();
    if (!adjusted || adjustError != ERROR_SUCCESS)
    {
      CloseHandle(brokerToken);
      winError = adjustError ? adjustError : ERROR_ACCESS_DENIED;
      error = TokenError(winError);
      return false;
    }

    CThreadImpersonation brokerImpersonation(brokerToken);
    if (!brokerImpersonation.Active())
    {
      const DWORD brokerImpersonationError = brokerImpersonation.Error();
      CloseHandle(brokerToken);
      winError = brokerImpersonationError;
      error = TokenError(winError);
      return false;
    }

    HANDLE sourceToken = nullptr;
    const bool queried = WTSQueryUserToken(sessionId, &sourceToken) != FALSE;
    const DWORD queryError = queried ? ERROR_SUCCESS : GetLastError();
    if (!brokerImpersonation.Finish())
    {
      const DWORD restoreError = brokerImpersonation.Error();
      if (sourceToken)
        CloseHandle(sourceToken);
      CloseHandle(brokerToken);
      winError = restoreError;
      error = TokenError(winError);
      return false;
    }
    CloseHandle(brokerToken);
    if (!queried || !sourceToken)
    {
      if (sourceToken)
        CloseHandle(sourceToken);
      winError = queryError ? queryError : ERROR_ACCESS_DENIED;
      error = TokenError(winError);
      return false;
    }

    const bool duplicated = DuplicateTokenEx(sourceToken,
      TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation,
      TokenImpersonation, &token) != FALSE;
    const DWORD duplicateError = duplicated ? ERROR_SUCCESS : GetLastError();
    CloseHandle(sourceToken);
    if (!duplicated)
    {
      winError = duplicateError;
      error = TokenError(duplicateError);
      return false;
    }
    return true;
  }

  uint64_t FileTimeToUnixNs(const FILETIME& time)
  {
    ULARGE_INTEGER value = {};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    if (value.QuadPart <= WINDOWS_EPOCH_TICKS)
      return 0;
    const uint64_t ticks = value.QuadPart - WINDOWS_EPOCH_TICKS;
    return ticks <= (std::numeric_limits<uint64_t>::max)() / 100U ?
      ticks * 100U : (std::numeric_limits<uint64_t>::max)();
  }

  FILETIME UnixNsToFileTime(uint64_t ns)
  {
    ULARGE_INTEGER value = {};
    const uint64_t ticks = ns / 100U;
    value.QuadPart = ticks <=
      (std::numeric_limits<uint64_t>::max)() - WINDOWS_EPOCH_TICKS ?
      ticks + WINDOWS_EPOCH_TICKS :
      (std::numeric_limits<uint64_t>::max)();
    FILETIME result = {};
    result.dwLowDateTime = value.LowPart;
    result.dwHighDateTime = value.HighPart;
    return result;
  }

  KVMFRClipboardFileError FileError(DWORD error)
  {
    switch (error)
    {
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND:
      case ERROR_INVALID_HANDLE:
        return KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND;
      case ERROR_ACCESS_DENIED:
      case ERROR_SHARING_VIOLATION:
      case ERROR_LOCK_VIOLATION:
        return KVMFR_CLIPBOARD_FILE_ERROR_ACCESS;
      case ERROR_DIRECTORY:
        return KVMFR_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;
      case ERROR_NOT_ENOUGH_MEMORY:
      case ERROR_OUTOFMEMORY:
        return KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      case ERROR_DISK_FULL:
      case ERROR_HANDLE_DISK_FULL:
        return KVMFR_CLIPBOARD_FILE_ERROR_NO_SPACE;
      case ERROR_DEVICE_NOT_CONNECTED:
      case ERROR_BROKEN_PIPE:
        return KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
      case ERROR_OPERATION_ABORTED:
      case ERROR_CANCELLED:
        return KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED;
      case ERROR_NOT_SUPPORTED:
        return KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
      case ERROR_INVALID_DATA:
      case ERROR_INVALID_NAME:
      case ERROR_INVALID_PARAMETER:
        return KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
      default:
        return KVMFR_CLIPBOARD_FILE_ERROR_IO;
    }
  }

  bool SameIdentity(const CLocalClipboardFiles::Node& node,
    const BY_HANDLE_FILE_INFORMATION& information)
  {
    const bool directory =
      (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const uint64_t size = directory ? 0 :
      (static_cast<uint64_t>(information.nFileSizeHigh) << 32) |
        information.nFileSizeLow;
    return !(information.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) &&
      directory ==
        (node.type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY) &&
      size == node.size &&
      FileTimeToUnixNs(information.ftCreationTime) == node.createdNs &&
      FileTimeToUnixNs(information.ftLastWriteTime) == node.modifiedNs &&
      information.dwVolumeSerialNumber == node.volumeSerial &&
      information.nFileIndexHigh == node.fileIndexHigh &&
      information.nFileIndexLow == node.fileIndexLow;
  }

  std::wstring BaseName(std::wstring path)
  {
    while (path.size() > 1 &&
        (path.back() == L'\\' || path.back() == L'/'))
    {
      if (path.size() == 3 && path[1] == L':')
        break;
      path.pop_back();
    }
    size_t end = path.size();
    while (end && (path[end - 1] == L'\\' || path[end - 1] == L'/'))
      --end;
    const size_t separator = end ? path.find_last_of(L"\\/", end - 1) :
      std::wstring::npos;
    std::wstring name = path.substr(
      separator == std::wstring::npos ? 0 : separator + 1,
      end - (separator == std::wstring::npos ? 0 : separator + 1));
    if (name.size() == 2 && name[1] == L':')
      name.resize(1);
    return name;
  }

  std::wstring JoinPath(const std::wstring& parent,
    const std::wstring& child)
  {
    std::wstring result = parent;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/')
      result.push_back(L'\\');
    result += child;
    return result;
  }

  bool HasDotComponent(const std::wstring& path, size_t offset)
  {
    while (offset < path.size())
    {
      const size_t end = path.find(L'\\', offset);
      const size_t length =
        (end == std::wstring::npos ? path.size() : end) - offset;
      if ((length == 1 && path[offset] == L'.') ||
          (length == 2 && path[offset] == L'.' &&
            path[offset + 1] == L'.'))
        return true;
      if (end == std::wstring::npos)
        break;
      offset = end + 1U;
    }
    return false;
  }

  bool SourcePath(const std::wstring& input, std::wstring& output)
  {
    if (input.empty())
    {
      SetLastError(ERROR_INVALID_NAME);
      return false;
    }

    try
    {
      std::wstring path = input;
      for (wchar_t& value : path)
        if (value == L'/')
          value = L'\\';

      const bool drive = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
      const bool unc = path.size() >= 3 && path[0] == L'\\' &&
        path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
      const bool extendedDrive = path.size() >= 7 &&
        path.compare(0, 4, L"\\\\?\\") == 0 &&
        ((path[4] >= L'A' && path[4] <= L'Z') ||
         (path[4] >= L'a' && path[4] <= L'z')) &&
        path[5] == L':' && path[6] == L'\\';
      const bool extendedUNC = path.size() >= 8 &&
        path.compare(0, 4, L"\\\\?\\") == 0 &&
        (path[4] == L'U' || path[4] == L'u') &&
        (path[5] == L'N' || path[5] == L'n') &&
        (path[6] == L'C' || path[6] == L'c') && path[7] == L'\\';

      const size_t componentOffset = drive ? 3U : unc ? 2U :
        extendedDrive ? 7U : extendedUNC ? 8U : 0U;
      if (!componentOffset || HasDotComponent(path, componentOffset))
      {
        SetLastError(ERROR_INVALID_NAME);
        return false;
      }

      if (extendedDrive)
        output = std::move(path);
      else if (extendedUNC)
      {
        path[4] = L'U';
        path[5] = L'N';
        path[6] = L'C';
        output = std::move(path);
      }
      else if (drive)
        output = L"\\\\?\\" + path;
      else if (unc)
        output = L"\\\\?\\UNC\\" + path.substr(2);
      else
      {
        SetLastError(ERROR_INVALID_NAME);
        return false;
      }
    }
    catch (const std::bad_alloc&)
    {
      SetLastError(ERROR_OUTOFMEMORY);
      return false;
    }
    return true;
  }

  bool ToUTF8(const std::wstring& text, std::vector<uint8_t>& output)
  {
    if (text.empty() || text.size() >
        static_cast<size_t>((std::numeric_limits<int>::max)()))
    {
      SetLastError(ERROR_INVALID_DATA);
      return false;
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
      return false;
    try
    {
      const size_t offset = output.size();
      output.resize(offset + static_cast<size_t>(count));
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
          text.data(), static_cast<int>(text.size()),
          reinterpret_cast<char *>(output.data() + offset), count,
          nullptr, nullptr) != count)
      {
        output.resize(offset);
        return false;
      }
    }
    catch (const std::bad_alloc&)
    {
      SetLastError(ERROR_OUTOFMEMORY);
      return false;
    }
    return true;
  }

  bool FromUTF8(const uint8_t * data, size_t length, std::wstring& output)
  {
    if (!data || !length || length >
        static_cast<size_t>((std::numeric_limits<int>::max)()))
      return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      reinterpret_cast<const char *>(data), static_cast<int>(length),
      nullptr, 0);
    if (count <= 0)
      return false;
    try
    {
      output.resize(static_cast<size_t>(count));
    }
    catch (const std::bad_alloc&)
    {
      return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      reinterpret_cast<const char *>(data), static_cast<int>(length),
      output.data(), count) == count;
  }

  bool ValidComponent(const std::wstring& name)
  {
    if (name.empty() || name.size() > 255 || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ')
      return false;
    for (wchar_t value : name)
      if (value < 32 || value == L'\\' || value == L'/' || value == L'<' ||
          value == L'>' || value == L':' || value == L'"' || value == L'|' ||
          value == L'?' || value == L'*')
        return false;

    std::wstring base = name.substr(0, name.find(L'.'));
    for (wchar_t& value : base)
      if (value >= L'a' && value <= L'z')
        value = static_cast<wchar_t>(value - (L'a' - L'A'));
    if (base == L"CON" || base == L"PRN" || base == L"AUX" ||
        base == L"NUL")
      return false;
    if (base.size() == 4 &&
        (base.compare(0, 3, L"COM") == 0 ||
         base.compare(0, 3, L"LPT") == 0))
    {
      const wchar_t suffix = base[3];
      if ((suffix >= L'1' && suffix <= L'9') || suffix == L'\u00b9' ||
          suffix == L'\u00b2' || suffix == L'\u00b3')
        return false;
    }
    return true;
  }

  int CompareComponent(const std::wstring& left,
    const std::wstring& right)
  {
    const int result = CompareStringOrdinal(left.c_str(),
      static_cast<int>(left.size()), right.c_str(),
      static_cast<int>(right.size()), TRUE);
    return result ? result :
      (left < right ? CSTR_LESS_THAN :
        (left == right ? CSTR_EQUAL : CSTR_GREATER_THAN));
  }

  struct ComponentLess
  {
    bool operator()(const std::wstring& left,
      const std::wstring& right) const
    {
      return CompareComponent(left, right) == CSTR_LESS_THAN;
    }
  };

  bool SameComponent(const std::wstring& left, const std::wstring& right)
  {
    return CompareComponent(left, right) == CSTR_EQUAL;
  }

  class CClipboardFileDatasetLease final
  {
  public:
    const uint64_t dataset;
    const uint64_t acquisition;
    const std::shared_ptr<IRemoteClipboardFileProvider> provider;

    CClipboardFileDatasetLease(uint64_t dataset, uint64_t acquisition,
      std::shared_ptr<IRemoteClipboardFileProvider> provider) :
      dataset(dataset), acquisition(acquisition), provider(std::move(provider))
    {
    }

    ~CClipboardFileDatasetLease()
    {
      provider->ReleaseClipboardFileDataset(dataset, acquisition);
    }
  };

  class CClipboardFileStream final : public IStream
  {
  private:
    std::atomic<ULONG> m_refs { 1 };
    ClipboardRemoteFileEntry m_entry;
    std::shared_ptr<CClipboardFileDatasetLease> m_lease;
    std::mutex m_lock;
    uint64_t m_offset = 0;

  public:
    CClipboardFileStream(
      const ClipboardRemoteFileEntry& entry,
      std::shared_ptr<CClipboardFileDatasetLease> lease,
      uint64_t offset = 0) :
      m_entry(entry), m_lease(std::move(lease)), m_offset(offset)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void ** object)
      override
    {
      if (!object)
        return E_POINTER;
      *object = nullptr;
      if (iid == IID_IUnknown || iid == IID_ISequentialStream ||
          iid == IID_IStream)
      {
        *object = static_cast<IStream *>(this);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
      return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
      const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (!refs)
        delete this;
      return refs;
    }

    HRESULT STDMETHODCALLTYPE Read(void * data, ULONG length,
      ULONG * read) override
    {
      if (read)
        *read = 0;
      if (!data && length)
        return STG_E_INVALIDPOINTER;
      std::lock_guard<std::mutex> lock(m_lock);
      if (!length)
        return S_OK;
      if (m_offset >= m_entry.size)
        return S_FALSE;
      const ULONG requested = static_cast<ULONG>((std::min<uint64_t>)(
        length, m_entry.size - m_offset));
      ULONG actual = 0;
      const HRESULT result = m_lease->provider->ReadClipboardFile(
        m_lease->dataset, m_lease->acquisition,
        m_entry.node, m_offset, data, requested, actual);
      if (FAILED(result))
        return result;
      if (actual != requested)
        return STG_E_READFAULT;
      m_offset += actual;
      if (read)
        *read = actual;
      return requested == length ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override
    {
      return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
      ULARGE_INTEGER * position) override
    {
      std::lock_guard<std::mutex> lock(m_lock);
      uint64_t base = 0;
      switch (origin)
      {
        case STREAM_SEEK_SET:
          base = 0;
          break;
        case STREAM_SEEK_CUR:
          base = m_offset;
          break;
        case STREAM_SEEK_END:
          base = m_entry.size;
          break;
        default:
          return STG_E_INVALIDFUNCTION;
      }

      uint64_t next = 0;
      if (move.QuadPart < 0)
      {
        const uint64_t amount = static_cast<uint64_t>(-(move.QuadPart + 1)) +
          1U;
        if (amount > base)
          return STG_E_INVALIDFUNCTION;
        next = base - amount;
      }
      else
      {
        const uint64_t amount = static_cast<uint64_t>(move.QuadPart);
        if (amount > (std::numeric_limits<uint64_t>::max)() - base)
          return STG_E_INVALIDFUNCTION;
        next = base + amount;
      }
      m_offset = next;
      if (position)
        position->QuadPart = next;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
    {
      return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream * stream, ULARGE_INTEGER count,
      ULARGE_INTEGER * readTotal, ULARGE_INTEGER * writtenTotal) override
    {
      if (!stream)
        return STG_E_INVALIDPOINTER;
      if (readTotal)
        readTotal->QuadPart = 0;
      if (writtenTotal)
        writtenTotal->QuadPart = 0;
      std::vector<uint8_t> buffer;
      try
      {
        buffer.resize(COPY_BUFFER_BYTES);
      }
      catch (const std::bad_alloc&)
      {
        return E_OUTOFMEMORY;
      }
      catch (const std::length_error&)
      {
        return E_OUTOFMEMORY;
      }

      uint64_t remaining = count.QuadPart;
      while (remaining)
      {
        const ULONG wanted = static_cast<ULONG>((std::min<uint64_t>)(
          remaining, buffer.size()));
        ULONG got = 0;
        HRESULT result = Read(buffer.data(), wanted, &got);
        if (FAILED(result))
          return result;
        if (!got)
          return S_FALSE;
        ULONG written = 0;
        result = stream->Write(buffer.data(), got, &written);
        if (readTotal)
          readTotal->QuadPart += got;
        if (writtenTotal)
          writtenTotal->QuadPart += written;
        if (FAILED(result) || written != got)
          return FAILED(result) ? result : STG_E_WRITEFAULT;
        remaining -= got;
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_REVERTED; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
      DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
      DWORD) override { return STG_E_INVALIDFUNCTION; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG * stat, DWORD flags) override
    {
      if (!stat)
        return STG_E_INVALIDPOINTER;
      *stat = {};
      stat->type = STGTY_STREAM;
      stat->cbSize.QuadPart = m_entry.size;
      stat->grfMode = STGM_READ;
      stat->ctime = UnixNsToFileTime(m_entry.createdNs);
      stat->mtime = UnixNsToFileTime(m_entry.modifiedNs);
      if (!(flags & STATFLAG_NONAME))
      {
        const size_t bytes = (m_entry.name.size() + 1U) * sizeof(wchar_t);
        stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
        if (!stat->pwcsName)
          return E_OUTOFMEMORY;
        memcpy(stat->pwcsName, m_entry.name.c_str(), bytes);
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream ** stream) override
    {
      if (!stream)
        return E_POINTER;
      *stream = nullptr;
      std::lock_guard<std::mutex> lock(m_lock);
      try
      {
        CClipboardFileStream * clone = new (std::nothrow)
          CClipboardFileStream(m_entry, m_lease, m_offset);
        if (!clone)
          return E_OUTOFMEMORY;
        *stream = clone;
        return S_OK;
      }
      catch (const std::bad_alloc&)
      {
        return E_OUTOFMEMORY;
      }
      catch (const std::length_error&)
      {
        return E_OUTOFMEMORY;
      }
    }
  };

  class CFormatEnumerator final : public IEnumFORMATETC
  {
  private:
    std::atomic<ULONG> m_refs { 1 };
    std::vector<FORMATETC> m_formats;
    size_t m_index = 0;

  public:
    CFormatEnumerator(std::vector<FORMATETC> formats, size_t index = 0) :
      m_formats(std::move(formats)), m_index(index)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void ** object)
      override
    {
      if (!object)
        return E_POINTER;
      *object = nullptr;
      if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC)
      {
        *object = static_cast<IEnumFORMATETC *>(this);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
      return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
      const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (!refs)
        delete this;
      return refs;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC * formats,
      ULONG * fetched) override
    {
      if (fetched)
        *fetched = 0;
      if (!formats || (count != 1 && !fetched))
        return E_INVALIDARG;
      ULONG copied = 0;
      while (copied < count && m_index < m_formats.size())
        formats[copied++] = m_formats[m_index++];
      if (fetched)
        *fetched = copied;
      return copied == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
    {
      const size_t available = m_formats.size() - m_index;
      const size_t skipped = (std::min<size_t>)(available, count);
      m_index += skipped;
      return skipped == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
      m_index = 0;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC ** clone) override
    {
      if (!clone)
        return E_POINTER;
      *clone = nullptr;
      try
      {
        *clone = new (std::nothrow) CFormatEnumerator(m_formats, m_index);
        return *clone ? S_OK : E_OUTOFMEMORY;
      }
      catch (const std::bad_alloc&)
      {
        return E_OUTOFMEMORY;
      }
      catch (const std::length_error&)
      {
        return E_OUTOFMEMORY;
      }
    }
  };

  class CClipboardFileDataObject final : public IDataObject
  {
  private:
    std::atomic<ULONG> m_refs { 1 };
    std::vector<ClipboardRemoteFileEntry> m_entries;
    std::vector<FILEDESCRIPTORW> m_descriptors;
    std::shared_ptr<CClipboardFileDatasetLease> m_lease;
    UINT m_fileDescriptor = 0;
    UINT m_fileContents = 0;
    UINT m_preferredDropEffect = 0;

    bool Matches(const FORMATETC& format, UINT clipboardFormat,
      DWORD tymed) const
    {
      return format.cfFormat == clipboardFormat &&
        format.dwAspect == DVASPECT_CONTENT && format.lindex >= -1 &&
        (format.tymed & tymed);
    }

  public:
    CClipboardFileDataObject(
      std::vector<ClipboardRemoteFileEntry>&& entries,
      std::vector<FILEDESCRIPTORW>&& descriptors,
      UINT fileDescriptor, UINT fileContents,
      UINT preferredDropEffect) noexcept :
      m_entries(std::move(entries)),
      m_descriptors(std::move(descriptors)),
      m_fileDescriptor(fileDescriptor), m_fileContents(fileContents),
      m_preferredDropEffect(preferredDropEffect)
    {
    }

    void SetLease(std::shared_ptr<CClipboardFileDatasetLease> lease) noexcept
    {
      m_lease = std::move(lease);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void ** object)
      override
    {
      if (!object)
        return E_POINTER;
      *object = nullptr;
      if (iid == IID_IUnknown || iid == IID_IDataObject)
      {
        *object = static_cast<IDataObject *>(this);
        AddRef();
        return S_OK;
      }
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
      return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
      const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (!refs)
        delete this;
      return refs;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC * format,
      STGMEDIUM * medium) override
    {
      if (!format || !medium)
        return E_POINTER;
      *medium = {};

      if (Matches(*format, m_fileDescriptor, TYMED_HGLOBAL) &&
          format->lindex == -1)
      {
        if (m_descriptors.size() >
            (std::numeric_limits<UINT>::max)() ||
            m_descriptors.size() > ((std::numeric_limits<SIZE_T>::max)() -
              offsetof(FILEGROUPDESCRIPTORW, fgd)) /
              sizeof(FILEDESCRIPTORW))
          return E_OUTOFMEMORY;
        const SIZE_T bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) +
          m_descriptors.size() * sizeof(FILEDESCRIPTORW);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!memory)
          return E_OUTOFMEMORY;
        FILEGROUPDESCRIPTORW * group =
          static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(memory));
        if (!group)
        {
          GlobalFree(memory);
          return E_OUTOFMEMORY;
        }
        group->cItems = static_cast<UINT>(m_descriptors.size());
        if (!m_descriptors.empty())
          memcpy(group->fgd, m_descriptors.data(),
            m_descriptors.size() * sizeof(FILEDESCRIPTORW));
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
      }

      if (Matches(*format, m_fileContents, TYMED_ISTREAM))
      {
        if (format->lindex < 0 ||
            static_cast<size_t>(format->lindex) >= m_entries.size())
          return DV_E_LINDEX;
        const ClipboardRemoteFileEntry& entry =
          m_entries[static_cast<size_t>(format->lindex)];
        if (entry.type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
          return DV_E_LINDEX;
        try
        {
          CClipboardFileStream * stream = new (std::nothrow)
            CClipboardFileStream(entry, m_lease);
          if (!stream)
            return E_OUTOFMEMORY;
          medium->tymed = TYMED_ISTREAM;
          medium->pstm = stream;
          return S_OK;
        }
        catch (const std::bad_alloc&)
        {
          return E_OUTOFMEMORY;
        }
        catch (const std::length_error&)
        {
          return E_OUTOFMEMORY;
        }
      }

      if (Matches(*format, m_preferredDropEffect, TYMED_HGLOBAL) &&
          format->lindex == -1)
      {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (!memory)
          return E_OUTOFMEMORY;
        DWORD * effect = static_cast<DWORD *>(GlobalLock(memory));
        if (!effect)
        {
          GlobalFree(memory);
          return E_OUTOFMEMORY;
        }
        *effect = DROPEFFECT_COPY;
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
      }
      return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override
    {
      return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC * format) override
    {
      if (!format)
        return E_POINTER;
      if (Matches(*format, m_fileDescriptor, TYMED_HGLOBAL) &&
          format->lindex == -1)
        return S_OK;
      if (Matches(*format, m_preferredDropEffect, TYMED_HGLOBAL) &&
          format->lindex == -1)
        return S_OK;
      if (Matches(*format, m_fileContents, TYMED_ISTREAM))
      {
        if (format->lindex == -1)
          for (const ClipboardRemoteFileEntry& entry : m_entries)
            if (entry.type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
              return S_OK;
        if (format->lindex >= 0 &&
            static_cast<size_t>(format->lindex) < m_entries.size() &&
            m_entries[static_cast<size_t>(format->lindex)].type ==
              KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
          return S_OK;
      }
      return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC * input,
      FORMATETC * output) override
    {
      if (!input || !output)
        return E_POINTER;
      *output = *input;
      output->ptd = nullptr;
      return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL)
      override
    {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,
      IEnumFORMATETC ** enumerator) override
    {
      if (!enumerator)
        return E_POINTER;
      *enumerator = nullptr;
      if (direction != DATADIR_GET)
        return E_NOTIMPL;
      try
      {
        std::vector<FORMATETC> formats;
        formats.reserve(3U);
        formats.push_back({ static_cast<CLIPFORMAT>(m_fileDescriptor),
          nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
        for (const ClipboardRemoteFileEntry& entry : m_entries)
          if (entry.type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
          {
            formats.push_back({ static_cast<CLIPFORMAT>(m_fileContents),
              nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM });
            break;
          }
        formats.push_back({
          static_cast<CLIPFORMAT>(m_preferredDropEffect), nullptr,
          DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
        *enumerator = new (std::nothrow)
          CFormatEnumerator(std::move(formats));
        return *enumerator ? S_OK : E_OUTOFMEMORY;
      }
      catch (const std::bad_alloc&)
      {
        return E_OUTOFMEMORY;
      }
      catch (const std::length_error&)
      {
        return E_OUTOFMEMORY;
      }
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *,
      DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
      { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override
      { return OLE_E_ADVISENOTSUPPORTED; }
  };
}

class CClipboardUserImpersonation::Impl
{
public:
  HANDLE token;
  CThreadImpersonation impersonation;

  explicit Impl(HANDLE token) :
    token(token), impersonation(token)
  {
  }

  ~Impl()
  {
    impersonation.Finish();
    CloseHandle(token);
  }
};

CClipboardUserImpersonation::CClipboardUserImpersonation(
  KVMFRClipboardFileError& error)
{
  HANDLE token = nullptr;
  if (!CaptureUserToken(token, error, m_error))
    return;
  try
  {
    m_impl = std::make_unique<Impl>(token);
  }
  catch (const std::bad_alloc&)
  {
    CloseHandle(token);
    m_error = ERROR_OUTOFMEMORY;
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return;
  }
  if (!m_impl->impersonation.Active())
  {
    m_error = m_impl->impersonation.Error();
    m_impl.reset();
    error = TokenError(m_error);
  }
}

CClipboardUserImpersonation::~CClipboardUserImpersonation() = default;

bool CClipboardUserImpersonation::Active() const
{
  return m_impl && m_impl->impersonation.Active();
}

DWORD CClipboardUserImpersonation::Error() const
{
  return m_error;
}

CLocalClipboardFiles::CLocalClipboardFiles(HANDLE userToken) :
  m_userToken(userToken)
{
}

CLocalClipboardFiles::~CLocalClipboardFiles()
{
  if (m_userToken)
    CloseHandle(m_userToken);
}

std::shared_ptr<CLocalClipboardFiles> CLocalClipboardFiles::Capture(
  HDROP drop, KVMFRClipboardFileError& error)
{
  error = KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
  if (!drop)
  {
    SetLastError(ERROR_INVALID_HANDLE);
    return nullptr;
  }
  std::vector<std::wstring> roots;
  HANDLE userToken = nullptr;
  std::shared_ptr<CLocalClipboardFiles> dataset;
  try
  {
    DWORD tokenError = ERROR_SUCCESS;
    if (!CaptureUserToken(userToken, error, tokenError))
      return nullptr;
    CLocalClipboardFiles * raw = new CLocalClipboardFiles(userToken);
    userToken = nullptr;
    dataset = std::shared_ptr<CLocalClipboardFiles>(raw);

    const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
    if (!count)
    {
      SetLastError(ERROR_INVALID_DATA);
      return nullptr;
    }
    roots.reserve(count);
    for (UINT index = 0; index < count; ++index)
    {
      const UINT length = DragQueryFileW(drop, index, nullptr, 0);
      if (!length)
      {
        SetLastError(ERROR_INVALID_DATA);
        return nullptr;
      }
      std::vector<wchar_t> path(static_cast<size_t>(length) + 1U);
      std::wstring source;
      if (DragQueryFileW(drop, index, path.data(), length + 1U) != length)
      {
        SetLastError(ERROR_INVALID_DATA);
        error = KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
        return nullptr;
      }
      if (!SourcePath(path.data(), source))
      {
        error = FileError(GetLastError());
        return nullptr;
      }
      roots.emplace_back(std::move(source));
    }

    CThreadImpersonation impersonation(dataset->m_userToken);
    if (!impersonation.Active())
    {
      error = TokenError(impersonation.Error());
      return nullptr;
    }
    for (const std::wstring& root : roots)
      if (!dataset->AddRoot(root, error))
        return nullptr;
    if (!impersonation.Finish())
    {
      error = TokenError(impersonation.Error());
      return nullptr;
    }
  }
  catch (const std::bad_alloc&)
  {
    if (userToken)
      CloseHandle(userToken);
    SetLastError(ERROR_OUTOFMEMORY);
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return nullptr;
  }
  error = KVMFR_CLIPBOARD_FILE_ERROR_NONE;
  return dataset;
}

size_t CLocalClipboardFiles::RootCount() const
{
  // Roots are immutable after Capture publishes the dataset.
  return m_roots.size();
}

bool CLocalClipboardFiles::AddRoot(const std::wstring& path,
  KVMFRClipboardFileError& error)
{
  Node node;
  try
  {
    node.path = path;
    node.name = BaseName(path);
  }
  catch (const std::bad_alloc&)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return false;
  }
  if (!ValidComponent(node.name))
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
    return false;
  }
  if (!CaptureIdentity(node, error))
    return false;
  for (uint64_t root : m_roots)
    if (SameComponent(m_nodes[static_cast<size_t>(root - 1U)].name,
        node.name))
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
      return false;
    }

  node.id = static_cast<uint64_t>(m_nodes.size()) + 1U;
  m_nodes.emplace_back(std::move(node));
  m_roots.push_back(static_cast<uint64_t>(m_nodes.size()));
  return true;
}

bool CLocalClipboardFiles::RefreshRoots(KVMFRClipboardFileError& error)
{
  std::vector<Node> refreshed;
  try
  {
    refreshed.reserve(m_roots.size());
    for (uint64_t id : m_roots)
    {
      const Node& source = m_nodes[static_cast<size_t>(id - 1U)];
      Node node;
      node.id = source.id;
      node.path = source.path;
      node.name = source.name;
      if (!CaptureIdentity(node, error))
        return false;
      refreshed.emplace_back(std::move(node));
    }
  }
  catch (const std::bad_alloc&)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return false;
  }

  for (size_t index = 0; index < refreshed.size(); ++index)
  {
    Node& target = m_nodes[static_cast<size_t>(m_roots[index] - 1U)];
    Node& source = refreshed[index];
    target.size = source.size;
    target.createdNs = source.createdNs;
    target.modifiedNs = source.modifiedNs;
    target.type = source.type;
    target.volumeSerial = source.volumeSerial;
    target.fileIndexHigh = source.fileIndexHigh;
    target.fileIndexLow = source.fileIndexLow;
  }
  return true;
}

bool CLocalClipboardFiles::CaptureIdentity(Node& node,
  KVMFRClipboardFileError& error) const
{
  const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
    FILE_FLAG_BACKUP_SEMANTICS;
  HANDLE file = CreateFileW(node.path.c_str(), FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
    OPEN_EXISTING, flags, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    error = FileError(GetLastError());
    return false;
  }
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  BY_HANDLE_FILE_INFORMATION information = {};
  bool success = GetFileInformationByHandle(file, &information) != FALSE;
  if (success &&
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    success = GetFileInformationByHandleEx(file,
      FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE;
  const DWORD winError = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!success)
  {
    error = FileError(winError);
    return false;
  }
  const DWORD attributes = tag.FileAttributes |
    information.dwFileAttributes;
  if ((attributes & FILE_ATTRIBUTE_DEVICE) ||
      ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
       IsReparseTagNameSurrogate(tag.ReparseTag)))
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
    return false;
  }
  node.type = information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ?
    KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY :
    KVMFR_CLIPBOARD_FILE_TYPE_REGULAR;
  node.size = node.type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR ?
    (static_cast<uint64_t>(information.nFileSizeHigh) << 32) |
      information.nFileSizeLow : 0;
  node.createdNs = FileTimeToUnixNs(information.ftCreationTime);
  node.modifiedNs = FileTimeToUnixNs(information.ftLastWriteTime);
  node.volumeSerial = information.dwVolumeSerialNumber;
  node.fileIndexHigh = information.nFileIndexHigh;
  node.fileIndexLow = information.nFileIndexLow;
  return true;
}

bool CLocalClipboardFiles::CheckIdentity(const Node& node,
  KVMFRClipboardFileError& error) const
{
  Node current;
  try
  {
    current.path = node.path;
  }
  catch (const std::bad_alloc&)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return false;
  }
  if (!CaptureIdentity(current, error))
    return false;
  if (current.volumeSerial != node.volumeSerial ||
      current.fileIndexHigh != node.fileIndexHigh ||
      current.fileIndexLow != node.fileIndexLow ||
      current.type != node.type || current.size != node.size ||
      current.createdNs != node.createdNs ||
      current.modifiedNs != node.modifiedNs)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_STALE;
    return false;
  }
  return true;
}

bool CLocalClipboardFiles::LoadChildren(size_t index,
  KVMFRClipboardFileError& error)
{
  if (index >= m_nodes.size())
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND;
    return false;
  }
  if (m_nodes[index].type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;
    return false;
  }
  if (m_nodes[index].childrenLoaded)
    return true;

  if (!CheckIdentity(m_nodes[index], error))
    return false;

  HANDLE search = INVALID_HANDLE_VALUE;
  try
  {
    const std::wstring parent = m_nodes[index].path;
    const std::wstring pattern = JoinPath(parent, L"*");
    WIN32_FIND_DATAW found = {};
    search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
      &found, FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE)
    {
      const DWORD winError = GetLastError();
      if (winError == ERROR_FILE_NOT_FOUND &&
          CheckIdentity(m_nodes[index], error))
      {
        m_nodes[index].childrenLoaded = true;
        return true;
      }
      if (!error)
        error = FileError(winError);
      return false;
    }

    std::vector<Node> children;
    do
    {
      if (!wcscmp(found.cFileName, L".") ||
          !wcscmp(found.cFileName, L".."))
        continue;
      if (!ValidComponent(found.cFileName))
      {
        FindClose(search);
        search = INVALID_HANDLE_VALUE;
        error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
        return false;
      }
      Node child;
      child.path = JoinPath(parent, found.cFileName);
      child.name = found.cFileName;
      if (!CaptureIdentity(child, error))
      {
        FindClose(search);
        search = INVALID_HANDLE_VALUE;
        return false;
      }
      children.emplace_back(std::move(child));
    }
    while (FindNextFileW(search, &found));
    const DWORD enumerationError = GetLastError();
    FindClose(search);
    search = INVALID_HANDLE_VALUE;
    if (enumerationError != ERROR_NO_MORE_FILES)
    {
      error = FileError(enumerationError);
      return false;
    }
    if (!CheckIdentity(m_nodes[index], error))
      return false;

    const size_t first = m_nodes.size();
    if (children.size() > m_nodes.max_size() - first)
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      return false;
    }
    m_nodes.reserve(first + children.size());
    std::vector<uint64_t>& ids = m_nodes[index].children;
    ids.reserve(children.size());
    for (Node& child : children)
    {
      child.id = static_cast<uint64_t>(m_nodes.size()) + 1U;
      ids.push_back(child.id);
      m_nodes.emplace_back(std::move(child));
    }
    m_nodes[index].childrenLoaded = true;
  }
  catch (const std::bad_alloc&)
  {
    if (search != INVALID_HANDLE_VALUE)
      FindClose(search);
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return false;
  }
  return true;
}

bool CLocalClipboardFiles::List(uint64_t node, std::vector<uint8_t>& data,
  KVMFRClipboardFileError& error)
{
  std::lock_guard<std::mutex> lock(m_lock);
  error = KVMFR_CLIPBOARD_FILE_ERROR_NONE;
  const std::vector<uint64_t> * children = nullptr;
  if (node == KVMFR_CLIPBOARD_FILE_ROOT_NODE)
  {
    if (!m_published)
    {
      CThreadImpersonation impersonation(m_userToken);
      if (!impersonation.Active())
      {
        error = TokenError(impersonation.Error());
        return false;
      }
      if (!RefreshRoots(error))
        return false;
      if (!impersonation.Finish())
      {
        error = TokenError(impersonation.Error());
        return false;
      }
      m_published = true;
    }
    children = &m_roots;
  }
  else
  {
    if (!m_published)
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
      return false;
    }
    if (node > m_nodes.size())
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND;
      return false;
    }
    const size_t index = static_cast<size_t>(node - 1U);
    CThreadImpersonation impersonation(m_userToken);
    if (!impersonation.Active())
    {
      error = TokenError(impersonation.Error());
      return false;
    }
    if (!LoadChildren(index, error))
      return false;
    if (!impersonation.Finish())
    {
      error = TokenError(impersonation.Error());
      return false;
    }
    children = &m_nodes[index].children;
  }

  data.clear();
  try
  {
    for (uint64_t id : *children)
    {
      const Node& child = m_nodes[static_cast<size_t>(id - 1U)];
      std::vector<uint8_t> name;
      if (!ToUTF8(child.name, name))
      {
        error = FileError(GetLastError());
        return false;
      }
      if (name.size() > (std::numeric_limits<uint32_t>::max)())
      {
        error = KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
        return false;
      }
      const uint64_t recordBytes = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(
        name.size());
      if (recordBytes > (std::numeric_limits<size_t>::max)() ||
          data.size() > (std::numeric_limits<size_t>::max)() -
            static_cast<size_t>(recordBytes))
      {
        error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
        return false;
      }
      const size_t offset = data.size();
      data.resize(offset + static_cast<size_t>(recordBytes), 0);
      KVMFRClipboardFileEntry entry = {};
      entry.node = child.id;
      entry.size = child.size;
      entry.createdNs = child.createdNs;
      entry.modifiedNs = child.modifiedNs;
      entry.type = child.type;
      entry.nameLength = static_cast<uint32_t>(name.size());
      memcpy(data.data() + offset, &entry, sizeof(entry));
      memcpy(data.data() + offset + sizeof(entry), name.data(), name.size());
    }
  }
  catch (const std::bad_alloc&)
  {
    data.clear();
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    return false;
  }
  return true;
}

bool CLocalClipboardFiles::Read(uint64_t node, uint64_t offset,
  uint32_t length, std::vector<uint8_t>& data,
  KVMFRClipboardFileError& error)
{
  error = KVMFR_CLIPBOARD_FILE_ERROR_NONE;
  std::wstring path;
  Node expected;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_published || !node || node > m_nodes.size())
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND;
      return false;
    }
    const Node& source = m_nodes[static_cast<size_t>(node - 1U)];
    if (source.type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_IS_DIRECTORY;
      return false;
    }
    try
    {
      path = source.path;
      expected.id = source.id;
      expected.size = source.size;
      expected.createdNs = source.createdNs;
      expected.modifiedNs = source.modifiedNs;
      expected.type = source.type;
      expected.volumeSerial = source.volumeSerial;
      expected.fileIndexHigh = source.fileIndexHigh;
      expected.fileIndexLow = source.fileIndexLow;
    }
    catch (const std::bad_alloc&)
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
      return false;
    }
  }
  if (length > KVMFR_CLIPBOARD_FILE_READ_BYTES ||
      offset > static_cast<uint64_t>(
        (std::numeric_limits<LONGLONG>::max)()) ||
      offset > (std::numeric_limits<uint64_t>::max)() - length)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
    return false;
  }

  CThreadImpersonation impersonation(m_userToken);
  if (!impersonation.Active())
  {
    error = TokenError(impersonation.Error());
    return false;
  }

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    error = FileError(GetLastError());
    return false;
  }
  FILE_STANDARD_INFO standard = {};
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  BY_HANDLE_FILE_INFORMATION identity = {};
  if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
      sizeof(standard)) ||
      !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag,
        sizeof(tag)) ||
      !GetFileInformationByHandle(file, &identity))
  {
    error = FileError(GetLastError());
    CloseHandle(file);
    return false;
  }
  if (standard.Directory || (tag.FileAttributes & FILE_ATTRIBUTE_DEVICE) ||
      ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
       IsReparseTagNameSurrogate(tag.ReparseTag)))
  {
    error = standard.Directory ? KVMFR_CLIPBOARD_FILE_ERROR_IS_DIRECTORY :
      KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
    CloseHandle(file);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!node || node > m_nodes.size())
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_STALE;
      CloseHandle(file);
      return false;
    }
    const Node& source = m_nodes[static_cast<size_t>(node - 1U)];
    if (!SameIdentity(source, identity))
    {
      error = KVMFR_CLIPBOARD_FILE_ERROR_STALE;
      CloseHandle(file);
      return false;
    }
  }
  if (standard.EndOfFile.QuadPart < 0)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_IO;
    CloseHandle(file);
    return false;
  }
  const uint64_t actualSize = static_cast<uint64_t>(
    standard.EndOfFile.QuadPart);
  if (!SameIdentity(expected, identity) || actualSize != expected.size)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_STALE;
    CloseHandle(file);
    return false;
  }

  const uint32_t wanted = offset >= actualSize ? 0 :
    static_cast<uint32_t>((std::min<uint64_t>)(length, actualSize - offset));
  try
  {
    data.resize(wanted);
  }
  catch (const std::bad_alloc&)
  {
    error = KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    CloseHandle(file);
    return false;
  }
  LARGE_INTEGER position = {};
  position.QuadPart = offset;
  DWORD read = 0;
  bool success = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
    (!wanted || ReadFile(file, data.data(), wanted, &read, nullptr));
  DWORD ioError = success ? ERROR_SUCCESS : GetLastError();
  BY_HANDLE_FILE_INFORMATION after = {};
  if (success && !GetFileInformationByHandle(file, &after))
  {
    success = false;
    ioError = GetLastError();
  }
  if (success && !SameIdentity(expected, after))
  {
    success = false;
    error = KVMFR_CLIPBOARD_FILE_ERROR_STALE;
  }
  CloseHandle(file);
  if (!success || read != wanted)
  {
    data.clear();
    if (error == KVMFR_CLIPBOARD_FILE_ERROR_NONE)
      error = FileError(ioError ? ioError : ERROR_READ_FAULT);
    return false;
  }
  if (!impersonation.Finish())
  {
    data.clear();
    error = TokenError(impersonation.Error());
    return false;
  }
  return true;
}

HRESULT ParseClipboardFileList(uint64_t parent, const uint8_t * data,
  size_t length, std::vector<ClipboardRemoteFileEntry>& entries)
{
  if ((!data && length) || (length && length < sizeof(KVMFRClipboardFileEntry)))
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  size_t offset = 0;
  try
  {
    while (offset < length)
    {
      if (length - offset < sizeof(KVMFRClipboardFileEntry))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      KVMFRClipboardFileEntry wire = {};
      memcpy(&wire, data + offset, sizeof(wire));
      const uint64_t bytes = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(wire.nameLength);
      if (!wire.node || !wire.nameLength ||
          (wire.type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR &&
           wire.type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY) ||
          bytes > length - offset ||
          wire.nameLength > bytes - sizeof(wire))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      std::wstring name;
      if (!FromUTF8(data + offset + sizeof(wire), wire.nameLength, name) ||
          !ValidComponent(name))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      for (size_t pad = sizeof(wire) + wire.nameLength;
           pad < static_cast<size_t>(bytes); ++pad)
        if (data[offset + pad])
          return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      ClipboardRemoteFileEntry entry;
      entry.node = wire.node;
      entry.parent = parent;
      entry.size = wire.type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR ?
        wire.size : 0;
      entry.createdNs = wire.createdNs;
      entry.modifiedNs = wire.modifiedNs;
      entry.type = wire.type;
      entry.name = std::move(name);
      entries.emplace_back(std::move(entry));
      offset += static_cast<size_t>(bytes);
    }
  }
  catch (const std::bad_alloc&)
  {
    return E_OUTOFMEMORY;
  }
  catch (const std::length_error&)
  {
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

HRESULT CreateClipboardFileDataObject(uint64_t dataset, uint64_t acquisition,
  std::vector<ClipboardRemoteFileEntry>&& entries,
  std::shared_ptr<IRemoteClipboardFileProvider> provider,
  IDataObject ** object)
{
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (!dataset || !acquisition || !provider || entries.empty())
    return E_INVALIDARG;

  try
  {
    if (entries.size() >
          static_cast<size_t>((std::numeric_limits<LONG>::max)()) ||
        entries.size() >
          static_cast<size_t>((std::numeric_limits<UINT>::max)()))
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

    std::unordered_map<uint64_t, size_t> byID;
    std::map<uint64_t, std::set<std::wstring, ComponentLess>> siblings;
    byID.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
    {
      const ClipboardRemoteFileEntry& entry = entries[index];
      if (!entry.node || !ValidComponent(entry.name) ||
          !byID.emplace(entry.node, index).second ||
          !siblings[entry.parent].insert(entry.name).second)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    std::vector<std::wstring> paths(entries.size());
    std::vector<uint8_t> state(entries.size(), 0);
    std::vector<size_t> chain;
    chain.reserve((std::min<size_t>)(entries.size(), MAX_PATH));
    for (size_t start = 0; start < entries.size(); ++start)
    {
      if (state[start] == 2)
        continue;
      chain.clear();
      size_t current = start;
      while (state[current] != 2)
      {
        if (state[current] == 1 || chain.size() >= MAX_PATH)
          return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        state[current] = 1;
        chain.push_back(current);
        const ClipboardRemoteFileEntry& entry = entries[current];
        if (!entry.parent)
          break;
        const auto parent = byID.find(entry.parent);
        if (parent == byID.end() ||
            entries[parent->second].type !=
              KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
          return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        current = parent->second;
      }
      while (!chain.empty())
      {
        const size_t index = chain.back();
        chain.pop_back();
        const ClipboardRemoteFileEntry& entry = entries[index];
        if (!entry.parent)
          paths[index] = entry.name;
        else
        {
          const size_t parent = byID.find(entry.parent)->second;
          if (state[parent] != 2)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
          paths[index] = paths[parent] + L"\\" + entry.name;
        }
        if (paths[index].size() >= MAX_PATH)
          return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        state[index] = 2;
      }
    }

    std::vector<FILEDESCRIPTORW> descriptors;
    if (entries.size() > descriptors.max_size())
      return E_OUTOFMEMORY;
    descriptors.resize(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
    {
      const ClipboardRemoteFileEntry& entry = entries[index];
      FILEDESCRIPTORW& descriptor = descriptors[index];
      descriptor.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI;
      descriptor.dwFileAttributes = entry.type ==
        KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY :
        FILE_ATTRIBUTE_NORMAL;
      if (entry.createdNs)
      {
        descriptor.dwFlags |= FD_CREATETIME;
        descriptor.ftCreationTime = UnixNsToFileTime(entry.createdNs);
      }
      if (entry.modifiedNs)
      {
        descriptor.dwFlags |= FD_WRITESTIME;
        descriptor.ftLastWriteTime = UnixNsToFileTime(entry.modifiedNs);
      }
      if (entry.type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
      {
        descriptor.dwFlags |= FD_FILESIZE;
        descriptor.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
        descriptor.nFileSizeLow = static_cast<DWORD>(entry.size);
      }
      if (FAILED(StringCchCopyW(descriptor.cFileName,
          ARRAYSIZE(descriptor.cFileName), paths[index].c_str())))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }

    const UINT fileDescriptor =
      RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    const UINT fileContents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    const UINT preferredDropEffect =
      RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!fileDescriptor || !fileContents || !preferredDropEffect)
      return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() :
        ERROR_INVALID_DATA);

    CClipboardFileDataObject * result = new (std::nothrow)
      CClipboardFileDataObject(std::move(entries),
        std::move(descriptors), fileDescriptor, fileContents,
        preferredDropEffect);
    if (!result)
      return E_OUTOFMEMORY;
    try
    {
      std::shared_ptr<CClipboardFileDatasetLease> lease =
        std::make_shared<CClipboardFileDatasetLease>(dataset, acquisition,
          std::move(provider));
      result->SetLease(std::move(lease));
    }
    catch (const std::bad_alloc&)
    {
      result->Release();
      return E_OUTOFMEMORY;
    }
    *object = result;
    return S_OK;
  }
  catch (const std::bad_alloc&)
  {
    return E_OUTOFMEMORY;
  }
  catch (const std::length_error&)
  {
    return E_OUTOFMEMORY;
  }
}
