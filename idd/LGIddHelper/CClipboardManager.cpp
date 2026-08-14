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

#include "CClipboardManager.h"

#include <CDebug.h>

#include <algorithm>
#include <limits>
#include <new>
#include <string.h>
#include <strsafe.h>
#include <utility>

namespace
{
  static constexpr size_t MEMORY_SPOOL_LIMIT = 1024U * 1024U;
  static constexpr size_t TEXT_CONVERSION_CHUNK = 16384;
  static constexpr uint64_t MAX_SPOOL_BYTES =
    UINT64_C(512) * 1024U * 1024U;
  static constexpr DWORD CHANNEL_RETRY_MS = 10;
  static constexpr DWORD REMOTE_RETRY_MS = 250;
  static constexpr DWORD REMOTE_RETRY_TIMEOUT_MS = 5000;
  static constexpr UINT_PTR REMOTE_RETRY_TIMER = 0x4c47;
  static constexpr uint32_t ORIGIN_MAGIC = 0x4c47434fU;
  // BI_ALPHABITFIELDS is a serialized DIB value, but desktop SDKs omit it.
  static constexpr DWORD DIB_ALPHA_BITFIELDS = 6U;

  struct ClipboardOrigin
  {
    uint32_t magic;
    uint32_t reserved;
    uint64_t epoch;
    uint64_t generation;
  };

  bool WriteAll(HANDLE file, const void * data, size_t size)
  {
    const uint8_t * current = static_cast<const uint8_t *>(data);
    while (size)
    {
      const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(
        size, (std::numeric_limits<DWORD>::max)()));
      DWORD written = 0;
      if (!WriteFile(file, current, chunk, &written, nullptr))
        return false;
      if (written != chunk)
      {
        SetLastError(ERROR_WRITE_FAULT);
        return false;
      }
      current += written;
      size -= written;
    }
    return true;
  }

  bool ReadAll(HANDLE file, void * data, size_t size)
  {
    uint8_t * current = static_cast<uint8_t *>(data);
    while (size)
    {
      const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(
        size, (std::numeric_limits<DWORD>::max)()));
      DWORD read = 0;
      if (!ReadFile(file, current, chunk, &read, nullptr))
        return false;
      if (read != chunk)
      {
        SetLastError(ERROR_HANDLE_EOF);
        return false;
      }
      current += read;
      size -= read;
    }
    return true;
  }

  void AppendUTF8(std::vector<uint8_t>& output, uint32_t codepoint)
  {
    if (codepoint <= 0x7f)
      output.push_back(static_cast<uint8_t>(codepoint));
    else if (codepoint <= 0x7ff)
    {
      output.push_back(static_cast<uint8_t>(0xc0 | (codepoint >> 6)));
      output.push_back(static_cast<uint8_t>(0x80 | (codepoint & 0x3f)));
    }
    else if (codepoint <= 0xffff)
    {
      output.push_back(static_cast<uint8_t>(0xe0 | (codepoint >> 12)));
      output.push_back(static_cast<uint8_t>(0x80 |
        ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<uint8_t>(0x80 | (codepoint & 0x3f)));
    }
    else
    {
      output.push_back(static_cast<uint8_t>(0xf0 | (codepoint >> 18)));
      output.push_back(static_cast<uint8_t>(0x80 |
        ((codepoint >> 12) & 0x3f)));
      output.push_back(static_cast<uint8_t>(0x80 |
        ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<uint8_t>(0x80 | (codepoint & 0x3f)));
    }
  }

  uint32_t DIBPixelOffset(const uint8_t * data, size_t size)
  {
    if (size < sizeof(DWORD))
      return 0;

    const DWORD headerSize = *reinterpret_cast<const DWORD *>(data);
    if (headerSize < sizeof(BITMAPCOREHEADER) || headerSize > size)
      return 0;

    uint64_t offset = headerSize;
    if (headerSize == sizeof(BITMAPCOREHEADER))
    {
      const BITMAPCOREHEADER * header =
        reinterpret_cast<const BITMAPCOREHEADER *>(data);
      if (header->bcBitCount <= 8)
        offset += (1ULL << header->bcBitCount) * sizeof(RGBTRIPLE);
    }
    else if (headerSize >= sizeof(BITMAPINFOHEADER))
    {
      const BITMAPINFOHEADER * header =
        reinterpret_cast<const BITMAPINFOHEADER *>(data);
      if (headerSize == sizeof(BITMAPINFOHEADER) &&
          (header->biCompression == BI_BITFIELDS ||
           header->biCompression == DIB_ALPHA_BITFIELDS))
        offset += header->biCompression == DIB_ALPHA_BITFIELDS ? 16U : 12U;

      const uint32_t colors = header->biClrUsed ? header->biClrUsed :
        (header->biBitCount <= 8 ? 1U << header->biBitCount : 0U);
      offset += static_cast<uint64_t>(colors) * sizeof(RGBQUAD);
    }
    else
      return 0;

    return offset <= size &&
      offset <= (std::numeric_limits<uint32_t>::max)() ?
      static_cast<uint32_t>(offset) : 0;
  }
}

class CClipboardSpool
{
private:
  std::vector<uint8_t> m_memory;
  HANDLE m_file = INVALID_HANDLE_VALUE;
  uint64_t m_size = 0;

  bool Spill()
  {
    WCHAR path[MAX_PATH];
    const DWORD length = GetTempPathW(ARRAYSIZE(path), path);
    if (!length)
      return false;
    if (length >= ARRAYSIZE(path))
    {
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return false;
    }

    for (unsigned int attempt = 0;
         attempt != 32 && m_file == INVALID_HANDLE_VALUE; ++attempt)
    {
      WCHAR name[MAX_PATH];
      const uint64_t nonce = GetTickCount64() ^
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^ attempt;
      if (FAILED(StringCchPrintfW(name, ARRAYSIZE(name),
          L"%sLookingGlassClipboard-%08x-%016llx.tmp", path,
          GetCurrentProcessId(), nonce)))
      {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
      }
      m_file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    if (m_file == INVALID_HANDLE_VALUE)
      return false;

    if (!m_memory.empty() && !WriteAll(m_file,
        m_memory.data(), m_memory.size()))
    {
      const DWORD error = GetLastError();
      CloseHandle(m_file);
      m_file = INVALID_HANDLE_VALUE;
      SetLastError(error ? error : ERROR_WRITE_FAULT);
      return false;
    }
    std::vector<uint8_t>().swap(m_memory);
    return true;
  }

public:
  ~CClipboardSpool()
  {
    if (m_file != INVALID_HANDLE_VALUE)
      CloseHandle(m_file);
  }

  bool Append(const void * data, size_t size)
  {
    if (!size)
      return true;
    if (size > MAX_SPOOL_BYTES || m_size > MAX_SPOOL_BYTES - size)
    {
      SetLastError(ERROR_FILE_TOO_LARGE);
      return false;
    }
    const uint64_t nextSize = m_size + size;

    if (m_file == INVALID_HANDLE_VALUE && nextSize <= MEMORY_SPOOL_LIMIT)
    {
      try
      {
        const uint8_t * bytes = static_cast<const uint8_t *>(data);
        m_memory.insert(m_memory.end(), bytes, bytes + size);
      }
      catch (const std::bad_alloc&)
      {
        SetLastError(ERROR_OUTOFMEMORY);
        return false;
      }
      m_size = nextSize;
      return true;
    }

    if (m_file == INVALID_HANDLE_VALUE && !Spill())
      return false;

    LARGE_INTEGER position = {};
    position.QuadPart = m_size;
    if (!SetFilePointerEx(m_file, position, nullptr, FILE_BEGIN) ||
        !WriteAll(m_file, data, size))
      return false;
    m_size = nextSize;
    return true;
  }

  bool Read(uint64_t offset, void * data, size_t size)
  {
    if (offset > m_size || size > m_size - offset)
    {
      SetLastError(ERROR_INVALID_DATA);
      return false;
    }
    if (!size)
      return true;

    if (m_file == INVALID_HANDLE_VALUE)
    {
      memcpy(data, m_memory.data() + static_cast<size_t>(offset), size);
      return true;
    }

    LARGE_INTEGER position = {};
    position.QuadPart = offset;
    return SetFilePointerEx(m_file, position, nullptr, FILE_BEGIN) &&
      ReadAll(m_file, data, size);
  }

  uint64_t Size() const { return m_size; }
};

namespace
{
  template<typename Callback>
  bool ForEachUTF8(CClipboardSpool& spool, Callback callback)
  {
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    unsigned int remaining = 0;
    std::vector<uint8_t> buffer;
    try
    {
      buffer.resize(KVMFR_CLIPBOARD_DATA_BYTES);
    }
    catch (const std::bad_alloc&)
    {
      SetLastError(ERROR_OUTOFMEMORY);
      return false;
    }

    for (uint64_t offset = 0; offset < spool.Size();)
    {
      const size_t length = static_cast<size_t>((std::min<uint64_t>)(
        buffer.size(), spool.Size() - offset));
      if (!spool.Read(offset, buffer.data(), length))
        return false;
      offset += length;

      for (size_t index = 0; index < length; ++index)
      {
        uint8_t value = buffer[index];
        if (!remaining)
        {
          if (value < 0x80)
          {
            if (!callback(value))
              return false;
          }
          else if (value >= 0xc2 && value <= 0xdf)
          {
            codepoint = value & 0x1f;
            minimum = 0x80;
            remaining = 1;
          }
          else if (value >= 0xe0 && value <= 0xef)
          {
            codepoint = value & 0x0f;
            minimum = 0x800;
            remaining = 2;
          }
          else if (value >= 0xf0 && value <= 0xf4)
          {
            codepoint = value & 0x07;
            minimum = 0x10000;
            remaining = 3;
          }
          else if (!callback(0xfffd))
            return false;
          continue;
        }

        if ((value & 0xc0) != 0x80)
        {
          if (!callback(0xfffd))
            return false;
          codepoint = 0;
          minimum = 0;
          remaining = 0;
          --index;
          continue;
        }

        codepoint = (codepoint << 6) | (value & 0x3f);
        if (--remaining)
          continue;

        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
          codepoint = 0xfffd;
        if (!callback(codepoint))
          return false;
        codepoint = 0;
        minimum = 0;
      }
    }

    return !remaining || callback(0xfffd);
  }

  HGLOBAL UnicodeFromUTF8(CClipboardSpool& spool)
  {
    uint64_t units = 0;
    uint32_t previous = 0;
    SetLastError(ERROR_SUCCESS);
    if (!ForEachUTF8(spool, [&units, &previous](uint32_t codepoint) {
      if (codepoint == '\n' && previous != '\r')
        ++units;
      units += codepoint > 0xffff ? 2U : 1U;
      previous = codepoint;
      return units < (std::numeric_limits<SIZE_T>::max)() /
        sizeof(wchar_t);
    }))
    {
      if (!GetLastError())
        SetLastError(ERROR_FILE_TOO_LARGE);
      return nullptr;
    }

    ++units;
    if (units > (std::numeric_limits<SIZE_T>::max)() / sizeof(wchar_t))
    {
      SetLastError(ERROR_FILE_TOO_LARGE);
      return nullptr;
    }
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,
      static_cast<SIZE_T>(units * sizeof(wchar_t)));
    if (!memory)
      return nullptr;

    wchar_t * output = static_cast<wchar_t *>(GlobalLock(memory));
    if (!output)
    {
      GlobalFree(memory);
      return nullptr;
    }

    uint64_t index = 0;
    previous = 0;
    const bool valid = ForEachUTF8(spool,
      [&output, &index, &previous](uint32_t codepoint) {
        const uint32_t original = codepoint;
        if (codepoint == '\n' && previous != '\r')
          output[index++] = L'\r';
        if (codepoint > 0xffff)
        {
          codepoint -= 0x10000;
          output[index++] = static_cast<wchar_t>(0xd800 +
            (codepoint >> 10));
          output[index++] = static_cast<wchar_t>(0xdc00 +
            (codepoint & 0x3ff));
        }
        else
          output[index++] = static_cast<wchar_t>(codepoint);
        previous = original;
        return true;
      });
    if (valid)
      output[index] = L'\0';
    GlobalUnlock(memory);
    if (!valid)
    {
      const DWORD error = GetLastError();
      GlobalFree(memory);
      SetLastError(error ? error : ERROR_INVALID_DATA);
      return nullptr;
    }
    return memory;
  }

  HGLOBAL CopySpoolToGlobal(CClipboardSpool& spool, uint64_t offset)
  {
    if (offset > spool.Size())
    {
      SetLastError(ERROR_INVALID_DATA);
      return nullptr;
    }
    const uint64_t length = spool.Size() - offset;
    if (length > (std::numeric_limits<SIZE_T>::max)())
    {
      SetLastError(ERROR_FILE_TOO_LARGE);
      return nullptr;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,
      static_cast<SIZE_T>(length));
    if (!memory)
      return nullptr;
    void * output = GlobalLock(memory);
    if (!output || !spool.Read(offset, output, static_cast<size_t>(length)))
    {
      const DWORD error = GetLastError();
      if (output)
        GlobalUnlock(memory);
      GlobalFree(memory);
      SetLastError(error ? error : ERROR_READ_FAULT);
      return nullptr;
    }
    GlobalUnlock(memory);
    return memory;
  }
}

CClipboardManager::IncomingTransfer::~IncomingTransfer()
{
  if (event)
    CloseHandle(event);
}

CClipboardManager::CClipboardManager(HWND hwnd,
  CClipboardChannel& channel) : m_hwnd(hwnd), m_channel(channel)
{
}

CClipboardManager::~CClipboardManager()
{
  Shutdown();
}

bool CClipboardManager::Initialize()
{
  m_formatPNG = RegisterClipboardFormatW(L"PNG");
  m_formatJPEG = RegisterClipboardFormatW(L"JFIF");
  m_formatOrigin = RegisterClipboardFormatW(L"LookingGlassClipboardOrigin");
  if (!m_formatPNG || !m_formatJPEG || !m_formatOrigin)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to register clipboard formats");
    return false;
  }

  Atomic::Store(m_shutdown, false);

  m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_stop || !m_wake)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create clipboard events");
    Shutdown();
    return false;
  }

  m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create clipboard worker");
    Shutdown();
    return false;
  }

  if (!AddClipboardFormatListener(m_hwnd))
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to register the clipboard listener");
    Shutdown();
    return false;
  }
  m_listener = true;
  m_channel.SetHandler(this);
  return true;
}

void CClipboardManager::Shutdown()
{
  if (Atomic::Swap(m_shutdown, true))
    return;

  m_channel.ClearHandler(this);
  if (m_listener && m_hwnd)
    RemoveClipboardFormatListener(m_hwnd);
  m_listener = false;
  if (m_hwnd)
    KillTimer(m_hwnd, REMOTE_RETRY_TIMER);

  CancelIncoming(ERROR_OPERATION_ABORTED);
  if (m_stop)
    SetEvent(m_stop);
  if (m_thread)
    WaitForSingleObject(m_thread, INFINITE);
  if (m_thread)
    CloseHandle(m_thread);
  if (m_wake)
    CloseHandle(m_wake);
  if (m_stop)
    CloseHandle(m_stop);
  {
    std::lock_guard<std::mutex> lock(m_workLock);
    for (size_t i = 0; i < m_recordWorkCount; ++i)
      m_recordWork[i].reset();
    for (size_t i = 0; i < m_sendWorkCount; ++i)
      m_sendWork[i].reset();
    m_recordWorkCount = 0;
    m_sendWorkCount = 0;
  }
  m_thread = nullptr;
  m_wake = nullptr;
  m_stop = nullptr;
}

DWORD WINAPI CClipboardManager::ThreadProc(void * context)
{
  static_cast<CClipboardManager *>(context)->Thread();
  return 0;
}

void CClipboardManager::Thread()
{
  HANDLE events[] = { m_stop, m_wake };
  for (;;)
  {
    const DWORD result = WaitForMultipleObjects(
      ARRAYSIZE(events), events, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0)
      return;
    if (result != WAIT_OBJECT_0 + 1)
      return;

    for (;;)
    {
      Work work;
      bool haveWork = false;
      {
        std::lock_guard<std::mutex> lock(m_workLock);
        if (m_pendingControlCount)
        {
          work.type      = m_pendingControl[0].type;
          work.available = m_pendingControl[0].available;
          work.epoch     = m_pendingControl[0].epoch;
          work.reason    = m_pendingControl[0].reason;
          for (size_t i = 1; i < m_pendingControlCount; ++i)
            m_pendingControl[i - 1] = m_pendingControl[i];
          --m_pendingControlCount;
          haveWork = true;
        }
        if (!haveWork && m_recordWorkCount)
        {
          work = std::move(*m_recordWork[0]);
          for (size_t i = 1; i < m_recordWorkCount; ++i)
            m_recordWork[i - 1] = std::move(m_recordWork[i]);
          m_recordWork[m_recordWorkCount - 1].reset();
          --m_recordWorkCount;
          haveWork = true;
        }
        if (!haveWork)
          for (PendingCancel& pending : m_pendingCancel)
            if (pending.valid)
            {
              work.type     = WorkType::SEND;
              work.record   = pending.record;
              work.deadline = pending.deadline;
              pending.valid = false;
              haveWork = true;
              break;
            }
        if (!haveWork && m_sendWorkCount)
        {
          work = std::move(*m_sendWork[0]);
          for (size_t i = 1; i < m_sendWorkCount; ++i)
            m_sendWork[i - 1] = std::move(m_sendWork[i]);
          m_sendWork[m_sendWorkCount - 1].reset();
          --m_sendWorkCount;
          haveWork = true;
        }
      }
      if (!haveWork)
        break;
      ProcessWork(std::move(work));
      if (WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0)
        return;
    }
  }
}

bool CClipboardManager::QueueWork(Work&& work)
{
  if (Atomic::Load(m_shutdown))
    return false;

  const bool publication = work.type == WorkType::SEND &&
    (work.record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
     work.record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR);
  {
    std::lock_guard<std::mutex> lock(m_workLock);
    // A BUSY publication is retried through this same path. Validate it while
    // holding the queue lock so an older retry cannot erase a newer clipboard
    // publication which became live before it entered the queue.
    if (publication && work.record.clipboardGeneration != Atomic::Load(
        m_liveLocalGeneration, std::memory_order_acquire))
      return false;

    std::array<std::optional<Work>, MAX_WORK>& queue =
      work.type == WorkType::RECORD ?
      m_recordWork : m_sendWork;
    size_t& count = work.type == WorkType::RECORD ?
      m_recordWorkCount : m_sendWorkCount;
    auto erase = [&queue, &count](size_t index) {
      for (size_t i = index + 1; i < count; ++i)
        queue[i - 1] = std::move(queue[i]);
      queue[count - 1].reset();
      --count;
    };

    if (publication)
    {
      for (size_t i = 0; i < count;)
      {
        const Work& queued = *queue[i];
        if (queued.type == WorkType::SEND &&
            (queued.record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
             queued.record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR))
          erase(i);
        else
          ++i;
      }
    }

    if (count == MAX_WORK)
      return false;
    queue[count++].emplace(std::move(work));
  }
  SetEvent(m_wake);
  return true;
}

bool CClipboardManager::QueueCancel(
  const KVMFRClipboardMessage& record, uint32_t reason, uint64_t deadline)
{
  KVMFRClipboardMessage cancel = record;
  cancel.version  = KVMFR_CLIPBOARD_VERSION;
  cancel.type     = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
  cancel.token    = reason;
  cancel.length   = 0;
  cancel.flags    = 0;
  cancel.offset   = 0;
  cancel.size     = 0;
  cancel.sequence = 0;
  if (!deadline)
    deadline = GetTickCount64() + SEND_TIMEOUT_MS;

  if (Atomic::Load(m_shutdown) || !cancel.transfer)
    return false;
  {
    std::lock_guard<std::mutex> lock(m_workLock);
    for (size_t i = 0; i < m_sendWorkCount;)
    {
      const bool sameTransfer =
        m_sendWork[i]->record.transfer == cancel.transfer;
      if (sameTransfer &&
          (m_sendWork[i]->type == WorkType::SEND_DATA ||
           (m_sendWork[i]->type == WorkType::SEND &&
            (m_sendWork[i]->record.type ==
               KVMFR_CLIPBOARD_MESSAGE_REQUEST ||
             m_sendWork[i]->record.type ==
               KVMFR_CLIPBOARD_MESSAGE_CANCEL))))
      {
        for (size_t j = i + 1; j < m_sendWorkCount; ++j)
          m_sendWork[j - 1] = std::move(m_sendWork[j]);
        m_sendWork[m_sendWorkCount - 1].reset();
        --m_sendWorkCount;
      }
      else
        ++i;
    }

    PendingCancel * free = nullptr;
    for (PendingCancel& pending : m_pendingCancel)
    {
      if (pending.valid &&
          pending.record.transfer == cancel.transfer)
      {
        pending.record = cancel;
        pending.deadline = deadline;
        SetEvent(m_wake);
        return true;
      }
      if (!pending.valid && !free)
        free = &pending;
    }
    if (!free)
    {
      // CANCELs are idempotent and bounded by their transfer ID. Under a
      // pathological peer stall retain the newest cancellation instead of
      // allocating or failing a producer-side error path.
      free = &m_pendingCancel[
        m_pendingCancelCursor++ % MAX_PENDING_CANCEL];
    }
    free->record   = cancel;
    free->deadline = deadline;
    free->valid    = true;
  }
  SetEvent(m_wake);
  return true;
}

void CClipboardManager::QueueControl(WorkType type, bool available,
  uint64_t epoch, uint32_t reason)
{
  if (Atomic::Load(m_shutdown))
    return;
  {
    std::lock_guard<std::mutex> lock(m_workLock);
    if (m_pendingControlCount)
    {
      PendingControl& last = m_pendingControl[m_pendingControlCount - 1];
      if (last.type == type &&
          (type == WorkType::RESET || last.available == available))
      {
        last.available = available;
        last.epoch     = epoch;
        last.reason    = reason;
        SetEvent(m_wake);
        return;
      }
    }

    if (m_pendingControlCount == MAX_PENDING_CONTROL)
    {
      // Collapse a pathologically noisy lifecycle into one reset. RESET's
      // worker path republishes the channel's current state after teardown.
      m_pendingControlCount = 1;
      m_pendingControl[0].type      = WorkType::RESET;
      m_pendingControl[0].available = false;
      m_pendingControl[0].epoch     = epoch;
      m_pendingControl[0].reason    = reason ? reason :
        ERROR_OPERATION_ABORTED;
    }
    else
    {
      PendingControl& pending = m_pendingControl[m_pendingControlCount++];
      pending.type      = type;
      pending.available = available;
      pending.epoch     = epoch;
      pending.reason    = reason;
    }
  }
  SetEvent(m_wake);
}

bool CClipboardManager::QueueUI(UIWork&& work)
{
  if (Atomic::Load(m_shutdown))
    return false;

  std::array<KVMFRClipboardMessage, MAX_UI_WORK> dropped = {};
  size_t droppedCount = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(m_uiLock);
    auto erase = [this, &dropped, &droppedCount](size_t index) {
      if (m_uiWork[index].type == UIType::REQUEST)
        dropped[droppedCount++] = m_uiWork[index].record;
      for (size_t i = index + 1; i < m_uiWorkCount; ++i)
        m_uiWork[i - 1] = m_uiWork[i];
      --m_uiWorkCount;
      m_uiWork[m_uiWorkCount] = UIWork {};
    };

    if (work.type == UIType::STATE && !work.available)
      while (m_uiWorkCount)
        erase(m_uiWorkCount - 1);
    else if (work.type == UIType::STATE)
    {
      for (size_t i = 0; i < m_uiWorkCount;)
      {
        if (m_uiWork[i].type == UIType::STATE &&
            m_uiWork[i].available == work.available)
          erase(i);
        else
          ++i;
      }
    }
    else if (work.type == UIType::OFFER || work.type == UIType::CLEAR)
    {
      for (size_t i = 0; i < m_uiWorkCount;)
      {
        if (m_uiWork[i].type == UIType::OFFER ||
            m_uiWork[i].type == UIType::CLEAR)
          erase(i);
        else
          ++i;
      }
    }

    if (m_uiWorkCount == MAX_UI_WORK)
    {
      if (work.type == UIType::REQUEST)
        return false;
      erase(0);
    }
    m_uiWork[m_uiWorkCount++] = work;
  }

  for (size_t i = 0; i < droppedCount; ++i)
    QueueCancel(dropped[i], ERROR_BUSY);
  if (PostMessageW(m_hwnd, WM_CLIPBOARD_WORK, 0, 0))
    return true;

  bool removed = false;
  {
    std::lock_guard<std::recursive_mutex> lock(m_uiLock);
    for (size_t i = m_uiWorkCount; i; --i)
    {
      UIWork& queued = m_uiWork[i - 1];
      bool same = queued.type == work.type;
      if (same && work.type == UIType::STATE)
        same = queued.available == work.available &&
          queued.epoch == work.epoch;
      else if (same && (work.type == UIType::OFFER ||
                        work.type == UIType::CLEAR))
      {
        same = queued.record.clipboardGeneration ==
          work.record.clipboardGeneration;
        if (same && work.type == UIType::OFFER)
          same = queued.record.token == work.record.token;
      }
      else if (same && work.type == UIType::REQUEST)
        same = queued.record.transfer == work.record.transfer;
      if (!same)
        continue;

      for (size_t j = i; j < m_uiWorkCount; ++j)
        m_uiWork[j - 1] = m_uiWork[j];
      --m_uiWorkCount;
      m_uiWork[m_uiWorkCount] = UIWork {};
      removed = true;
      break;
    }
  }

  // A previously posted drain may already have consumed the record.
  return !removed;
}

void CClipboardManager::ProcessWork(Work&& work)
{
  switch (work.type)
  {
    case WorkType::STATE:
    {
      if (!work.available)
      {
        Atomic::Store(m_outgoingTransfer, 0, std::memory_order_release);
        CancelIncoming(ERROR_DEVICE_NOT_CONNECTED);
      }
      UIWork ui;
      ui.type = UIType::STATE;
      ui.available = work.available;
      ui.epoch = work.epoch;
      QueueUI(std::move(ui));
      break;
    }

    case WorkType::RESET:
    {
      Atomic::Store(m_outgoingTransfer, 0, std::memory_order_release);
      CancelIncoming(work.reason ? work.reason : ERROR_OPERATION_ABORTED);
      UIWork ui;
      ui.type = UIType::STATE;
      ui.available = false;
      ui.epoch = work.epoch;
      QueueUI(std::move(ui));
      if (m_channel.Available())
      {
        UIWork resume;
        resume.type = UIType::STATE;
        resume.available = true;
        resume.epoch = m_channel.Epoch();
        QueueUI(std::move(resume));
      }
      break;
    }

    case WorkType::RECORD:
      ProcessRecord(work.record,
        work.data.empty() ? nullptr : work.data.data());
      break;

    case WorkType::SEND:
      ProcessSend(std::move(work));
      break;

    case WorkType::SEND_DATA:
      ProcessSendData(std::move(work));
      break;
  }
}

void CClipboardManager::ProcessRecord(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  switch (record.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
    {
      CancelIncoming(ERROR_OPERATION_ABORTED);
      InvalidateOutgoing(ERROR_OPERATION_ABORTED);
      UIWork ui;
      ui.type = UIType::OFFER;
      ui.record = record;
      if (!QueueUI(std::move(ui)))
        DEBUG_WARN("Failed to queue remote clipboard offer");
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
    {
      CancelIncoming(ERROR_OPERATION_ABORTED);
      InvalidateOutgoing(ERROR_OPERATION_ABORTED);
      UIWork ui;
      ui.type = UIType::CLEAR;
      ui.record = record;
      if (!QueueUI(std::move(ui)))
        DEBUG_WARN("Failed to queue remote clipboard clear");
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
    {
      UIWork ui;
      ui.type = UIType::REQUEST;
      ui.record = record;
      if (!QueueUI(std::move(ui)))
        QueueCancel(record, ERROR_BUSY);
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
      ProcessData(record, data);
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
    {
      ReleaseOutgoing(record.transfer);
      CancelIncoming(record.token ? record.token : ERROR_OPERATION_ABORTED,
        record.transfer);
      break;
    }
  }
}

void CClipboardManager::ProcessData(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  bool cancel = false;
  uint32_t cancelReason = ERROR_INVALID_DATA;
  {
    std::lock_guard<std::mutex> lock(m_transferLock);
    const std::shared_ptr<IncomingTransfer> transfer = m_incoming;
    if (!transfer || transfer->complete ||
        transfer->transfer != record.transfer ||
        transfer->generation != record.clipboardGeneration ||
        transfer->format != record.format)
      cancel = true;
    else
    {
      bool valid = record.offset == transfer->nextOffset &&
        record.sequence == transfer->nextSequence;
      if (!transfer->began)
      {
        valid = valid && record.offset == 0 &&
          (record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN);
        transfer->began = true;
        transfer->sizeHint = record.size;
        if (record.size != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
            record.size > MAX_SPOOL_BYTES)
        {
          transfer->error = ERROR_FILE_TOO_LARGE;
          valid = false;
        }
      }
      else if (record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)
        valid = false;

      if (valid && record.length &&
          !transfer->spool->Append(data, record.length))
      {
        const DWORD error = GetLastError();
        transfer->error = error ? error : ERROR_DISK_FULL;
        valid = false;
      }

      if (valid)
      {
        transfer->nextOffset += record.length;
        ++transfer->nextSequence;
        if (record.flags & KVMFR_CLIPBOARD_FLAG_END)
        {
          valid = record.size == transfer->nextOffset &&
            (transfer->sizeHint == KVMFR_CLIPBOARD_SIZE_UNKNOWN ||
             transfer->sizeHint == transfer->nextOffset);
          if (valid)
          {
            transfer->complete = true;
            transfer->error = ERROR_SUCCESS;
          }
        }
      }

      if (!valid)
      {
        transfer->complete = true;
        if (transfer->error == ERROR_SUCCESS)
          transfer->error = ERROR_INVALID_DATA;
        cancel = true;
        cancelReason = transfer->error;
      }
      if (transfer->complete)
        SetEvent(transfer->event);
    }
  }

  if (cancel)
    QueueCancel(record, cancelReason);
}

void CClipboardManager::ProcessSend(Work&& work)
{
  if (!work.deadline)
    work.deadline = GetTickCount64() + SEND_TIMEOUT_MS;

  const bool publication =
    work.record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
    work.record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR;
  ClipboardChannelResult result;
  if (publication)
  {
    // Serialize the final generation check with invalidation. Once a remote
    // offer has invalidated this generation, no queued retry may enter the
    // channel after that invalidation point.
    std::lock_guard<std::mutex> lock(m_outgoingLock);
    if (work.record.clipboardGeneration != Atomic::Load(
        m_liveLocalGeneration, std::memory_order_acquire))
      return;
    result = m_channel.Send(work.record, nullptr);
  }
  else if (work.record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
  {
    std::lock_guard<std::mutex> lock(m_transferLock);
    if (!m_incoming || m_incoming->complete ||
        m_incoming->transfer != work.record.transfer ||
        m_incoming->generation != work.record.clipboardGeneration ||
        m_incoming->format != work.record.format)
      return;
    if (GetTickCount64() >= work.deadline)
    {
      m_incoming->complete = true;
      m_incoming->error = ERROR_TIMEOUT;
      SetEvent(m_incoming->event);
      return;
    }
    result = m_channel.Send(work.record, nullptr);
  }
  else
    result = m_channel.Send(work.record, nullptr);
  if (result == ClipboardChannelResult::ACCEPTED)
    return;

  const KVMFRClipboardMessageType publicationType = work.record.type;
  const uint64_t publicationGeneration = work.record.clipboardGeneration;
  auto failLocalPublication = [this, publicationType,
      publicationGeneration]() {
    if (publicationType != KVMFR_CLIPBOARD_MESSAGE_OFFER &&
        publicationType != KVMFR_CLIPBOARD_MESSAGE_CLEAR)
      return;
    uint64_t generation = publicationGeneration;
    if (Atomic::CAS(m_liveLocalGeneration, generation, UINT64_C(0),
        std::memory_order_acq_rel))
      PostMessageW(m_hwnd, WM_CLIPBOARDUPDATE, 0, 0);
  };

  if (result == ClipboardChannelResult::BUSY &&
      GetTickCount64() < work.deadline &&
      WaitForSingleObject(m_stop, CHANNEL_RETRY_MS) != WAIT_OBJECT_0)
  {
    if (work.record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL)
      QueueCancel(work.record, work.record.token, work.deadline);
    else if (work.record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
    {
      const KVMFRClipboardMessage record = work.record;
      bool live = false;
      bool queued = false;
      {
        std::lock_guard<std::mutex> lock(m_transferLock);
        live = m_incoming && !m_incoming->complete &&
          m_incoming->transfer == record.transfer &&
          m_incoming->generation == record.clipboardGeneration &&
          m_incoming->format == record.format;
        if (live)
          queued = QueueWork(std::move(work));
      }
      if (live && !queued)
        CancelIncoming(ERROR_BUSY, record.transfer);
    }
    else if (!QueueWork(std::move(work)))
      failLocalPublication();
    return;
  }

  if (work.record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
  {
    const uint32_t reason = result == ClipboardChannelResult::BUSY ?
      ERROR_TIMEOUT : ERROR_DEVICE_NOT_CONNECTED;
    CancelIncoming(reason, work.record.transfer);
  }
  else
    failLocalPublication();
}

void CClipboardManager::ProcessSendData(Work&& work)
{
  if (!work.spool)
  {
    ReleaseOutgoing(work.record.transfer);
    QueueCancel(work.record, ERROR_INVALID_DATA);
    return;
  }

  if (Atomic::Load(m_outgoingTransfer, std::memory_order_acquire) !=
      work.record.transfer)
    return;
  if (!work.deadline)
    work.deadline = GetTickCount64() + SEND_TIMEOUT_MS;
  if (GetTickCount64() >= work.deadline)
  {
    ReleaseOutgoing(work.record.transfer);
    QueueCancel(work.record, ERROR_TIMEOUT);
    return;
  }

  if (work.record.clipboardGeneration !=
      Atomic::Load(m_liveLocalGeneration, std::memory_order_acquire))
  {
    ReleaseOutgoing(work.record.transfer);
    QueueCancel(work.record, ERROR_OPERATION_ABORTED);
    return;
  }

  const uint64_t total = work.spool->Size();
  if (work.record.offset > total)
  {
    ReleaseOutgoing(work.record.transfer);
    QueueCancel(work.record, ERROR_INVALID_DATA);
    return;
  }
  const size_t length = static_cast<size_t>((std::min<uint64_t>)(
    KVMFR_CLIPBOARD_DATA_BYTES, total - work.record.offset));
  std::vector<uint8_t> data;
  if (length)
  {
    try
    {
      data.resize(length);
    }
    catch (const std::bad_alloc&)
    {
      ReleaseOutgoing(work.record.transfer);
      QueueCancel(work.record, ERROR_OUTOFMEMORY);
      return;
    }
    if (!work.spool->Read(work.record.offset, data.data(), length))
    {
      const DWORD error = GetLastError();
      ReleaseOutgoing(work.record.transfer);
      QueueCancel(work.record, error ? error : ERROR_READ_FAULT);
      return;
    }
  }

  KVMFRClipboardMessage message = work.record;
  message.type = KVMFR_CLIPBOARD_MESSAGE_DATA;
  message.token = 0;
  message.length = static_cast<uint32_t>(length);
  message.flags = 0;
  if (!message.offset)
    message.flags |= KVMFR_CLIPBOARD_FLAG_BEGIN;
  if (message.offset + length == total)
    message.flags |= KVMFR_CLIPBOARD_FLAG_END;
  message.size = message.flags & KVMFR_CLIPBOARD_FLAG_END ? total :
    (message.flags & KVMFR_CLIPBOARD_FLAG_BEGIN ? total :
      KVMFR_CLIPBOARD_SIZE_UNKNOWN);

  ClipboardChannelResult result = ClipboardChannelResult::FAILED;
  bool stale = false;
  bool timedOut = false;
  {
    std::lock_guard<std::mutex> lock(m_outgoingLock);
    stale = Atomic::Load(m_outgoingTransfer, std::memory_order_acquire) !=
        message.transfer ||
      message.clipboardGeneration != Atomic::Load(
        m_liveLocalGeneration, std::memory_order_acquire);
    timedOut = GetTickCount64() >= work.deadline;
    if (!stale && !timedOut)
      result = m_channel.Send(message,
        data.empty() ? nullptr : data.data());
  }
  if (stale)
  {
    ReleaseOutgoing(message.transfer);
    QueueCancel(message, ERROR_OPERATION_ABORTED);
    return;
  }
  if (timedOut)
  {
    ReleaseOutgoing(message.transfer);
    QueueCancel(message, ERROR_TIMEOUT);
    return;
  }
  if (result == ClipboardChannelResult::BUSY)
  {
    if (GetTickCount64() < work.deadline &&
        WaitForSingleObject(m_stop, CHANNEL_RETRY_MS) != WAIT_OBJECT_0)
    {
      const KVMFRClipboardMessage record = work.record;
      if (!QueueWork(std::move(work)))
      {
        ReleaseOutgoing(record.transfer);
        QueueCancel(record, ERROR_BUSY);
      }
    }
    else
    {
      ReleaseOutgoing(work.record.transfer);
      QueueCancel(work.record, ERROR_TIMEOUT);
    }
    return;
  }
  if (result != ClipboardChannelResult::ACCEPTED)
  {
    ReleaseOutgoing(work.record.transfer);
    QueueCancel(work.record, ERROR_DEVICE_NOT_CONNECTED);
    return;
  }
  work.deadline = GetTickCount64() + SEND_TIMEOUT_MS;
  if (message.flags & KVMFR_CLIPBOARD_FLAG_END)
  {
    ReleaseOutgoing(work.record.transfer);
    return;
  }

  work.record.offset += length;
  ++work.record.sequence;
  const KVMFRClipboardMessage record = work.record;
  if (!QueueWork(std::move(work)))
  {
    ReleaseOutgoing(record.transfer);
    QueueCancel(record, ERROR_BUSY);
  }
}

void CClipboardManager::CancelIncoming(uint32_t reason, uint64_t transferID)
{
  std::lock_guard<std::mutex> lock(m_transferLock);
  const std::shared_ptr<IncomingTransfer> transfer = m_incoming;
  if (!transfer || (transferID && transferID != transfer->transfer))
    return;
  transfer->complete = true;
  transfer->error = reason;
  SetEvent(transfer->event);
}

void CClipboardManager::ReleaseOutgoing(uint64_t transfer)
{
  std::lock_guard<std::mutex> lock(m_outgoingLock);
  Atomic::CAS(m_outgoingTransfer, transfer, UINT64_C(0),
    std::memory_order_acq_rel);
}

void CClipboardManager::ClipboardState(bool available, uint64_t epoch)
{
  QueueControl(WorkType::STATE, available, epoch, 0);
}

ClipboardChannelResult CClipboardManager::ClipboardRecord(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  switch (record.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      if (!record.clipboardGeneration || !record.token ||
          (record.token & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL) ||
          record.format || record.transfer || record.length || record.flags ||
          record.offset || record.size || record.sequence)
        return ClipboardChannelResult::FAILED;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      if (!record.clipboardGeneration || record.format || record.transfer ||
          record.length || record.flags || record.offset || record.size ||
          record.sequence || record.token)
        return ClipboardChannelResult::FAILED;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      if (!record.clipboardGeneration || !record.transfer ||
          !kvmfrClipboardTransferFromClient(record.transfer) ||
          !kvmfrClipboardFormatValid(record.format) || record.length ||
          record.flags || record.offset || record.size || record.sequence ||
          record.token)
        return ClipboardChannelResult::FAILED;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
      if (!record.clipboardGeneration || !record.transfer ||
          !kvmfrClipboardTransferFromHelper(record.transfer) ||
          !kvmfrClipboardFormatValid(record.format) ||
          (record.length && !data))
        return ClipboardChannelResult::FAILED;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      if (!record.transfer || record.length || record.flags)
        return ClipboardChannelResult::FAILED;
      break;
    default:
      return ClipboardChannelResult::FAILED;
  }

  Work work;
  work.type = WorkType::RECORD;
  work.record = record;
  if (record.length)
  {
    try
    {
      work.data.assign(data, data + record.length);
    }
    catch (const std::bad_alloc&)
    {
      return ClipboardChannelResult::BUSY;
    }
  }
  if (QueueWork(std::move(work)))
    return ClipboardChannelResult::ACCEPTED;
  return Atomic::Load(m_shutdown) ? ClipboardChannelResult::FAILED :
    ClipboardChannelResult::BUSY;
}

void CClipboardManager::ClipboardReset(uint64_t epoch, uint32_t reason)
{
  QueueControl(WorkType::RESET, false, epoch, reason);
}

bool CClipboardManager::HandleMessage(UINT message, WPARAM wParam,
  LPARAM, LRESULT& result)
{
  switch (message)
  {
    case WM_CLIPBOARD_WORK:
      DrainUI();
      result = 0;
      return true;

    case WM_CLIPBOARDUPDATE:
      HandleClipboardUpdate();
      result = 0;
      return true;

    case WM_RENDERFORMAT:
      RenderFormat(static_cast<UINT>(wParam));
      result = 0;
      return true;

    case WM_RENDERALLFORMATS:
      RenderAllFormats();
      result = 0;
      return true;

    case WM_TIMER:
      if (wParam == REMOTE_RETRY_TIMER)
      {
        RetryRemoteOffer();
        result = 0;
        return true;
      }
      break;

    case WM_DESTROYCLIPBOARD:
      HandleDestroyClipboard();
      result = 0;
      return true;
  }
  return false;
}

void CClipboardManager::DrainUI()
{
  std::lock_guard<std::recursive_mutex> dispatchLock(m_uiLock);
  for (;;)
  {
    UIWork work;
    if (!m_uiWorkCount)
      return;
    work = m_uiWork[0];
    for (size_t i = 1; i < m_uiWorkCount; ++i)
      m_uiWork[i - 1] = m_uiWork[i];
    --m_uiWorkCount;
    m_uiWork[m_uiWorkCount] = UIWork {};

    switch (work.type)
    {
      case UIType::STATE:
        HandleState(work.available, work.epoch);
        break;
      case UIType::OFFER:
        HandleOffer(work.record);
        break;
      case UIType::CLEAR:
        HandleClear(work.record);
        break;
      case UIType::REQUEST:
        HandleRequest(work.record);
        break;
    }
  }
}

void CClipboardManager::HandleState(bool available, uint64_t epoch)
{
  m_available = available;
  m_epoch = available ? epoch : 0;
  if (!available)
  {
    ClearRemoteRetry();
    InvalidateLocalClipboard(ERROR_DEVICE_NOT_CONNECTED);
    ClearOwnedClipboard();
    return;
  }

  PublishLocalClipboard();
}

void CClipboardManager::HandleOffer(
  const KVMFRClipboardMessage& record)
{
  if (!m_available || !record.clipboardGeneration ||
      !record.token || (record.token & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL))
    return;

  // Clipboard generations identify content within one publisher process;
  // they restart when the client does. Serialized channel order determines
  // which publication is current.
  ClearRemoteRetry();
  m_pendingRemoteOffer = record;
  m_remoteRetryDeadline = GetTickCount64() + REMOTE_RETRY_TIMEOUT_MS;
  InvalidateLocalClipboard(ERROR_OPERATION_ABORTED);
  RetryRemoteOffer();
}

void CClipboardManager::HandleClear(
  const KVMFRClipboardMessage&)
{
  ClearRemoteRetry();
  InvalidateLocalClipboard(ERROR_OPERATION_ABORTED);
  ClearOwnedClipboard();
}

void CClipboardManager::HandleRequest(
  const KVMFRClipboardMessage& record)
{
  if (!m_available || !record.transfer ||
      !kvmfrClipboardFormatValid(record.format) ||
      record.clipboardGeneration != m_localGeneration ||
      record.clipboardGeneration != Atomic::Load(
        m_liveLocalGeneration, std::memory_order_acquire) ||
      GetClipboardSequenceNumber() != m_localSequence)
  {
    QueueCancel(record, ERROR_NOT_FOUND);
    return;
  }

  uint64_t noTransfer = 0;
  if (!Atomic::CAS(m_outgoingTransfer, noTransfer,
      record.transfer, std::memory_order_acq_rel))
  {
    QueueCancel(record, ERROR_BUSY);
    return;
  }

  std::shared_ptr<CClipboardSpool> spool =
    CaptureFormat(record.format, m_localSequence);
  if (!spool)
  {
    const DWORD error = GetLastError();
    ReleaseOutgoing(record.transfer);
    QueueCancel(record, error ? error : ERROR_NOT_FOUND);
    return;
  }

  Work data;
  data.type = WorkType::SEND_DATA;
  data.record = record;
  data.record.version = KVMFR_CLIPBOARD_VERSION;
  data.record.type = KVMFR_CLIPBOARD_MESSAGE_DATA;
  data.record.sequence = 0;
  data.record.offset = 0;
  data.record.size = 0;
  data.record.token = 0;
  data.record.length = 0;
  data.record.flags = 0;
  data.spool = std::move(spool);
  if (!QueueWork(std::move(data)))
  {
    ReleaseOutgoing(record.transfer);
    QueueCancel(record, ERROR_BUSY);
  }
}

bool CClipboardManager::OpenClipboardRetry() const
{
  for (unsigned int attempt = 0; attempt != 8; ++attempt)
  {
    if (OpenClipboard(m_hwnd))
      return true;
    Sleep(5U << (std::min)(attempt, 5U));
  }
  return false;
}

bool CClipboardManager::IsOurClipboard()
{
  if (GetClipboardOwner() == m_hwnd)
  {
    m_ownedSequence = GetClipboardSequenceNumber();
    return true;
  }

  if (!m_formatOrigin || !m_remoteGeneration ||
      !IsClipboardFormatAvailable(m_formatOrigin) || !OpenClipboardRetry())
    return false;

  bool ours = false;
  HANDLE data = GetClipboardData(m_formatOrigin);
  if (data && GlobalSize(data) >= sizeof(ClipboardOrigin))
  {
    const ClipboardOrigin * origin =
      static_cast<const ClipboardOrigin *>(GlobalLock(data));
    if (origin)
    {
      ours = origin->magic == ORIGIN_MAGIC && origin->epoch == m_epoch &&
        origin->generation == m_remoteGeneration;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return ours;
}

uint32_t CClipboardManager::EnumerateFormats() const
{
  uint32_t formats = 0;
  if (IsClipboardFormatAvailable(CF_UNICODETEXT))
    formats |= KVMFR_CLIPBOARD_FORMAT_MASK_TEXT;
  if (m_formatPNG && IsClipboardFormatAvailable(m_formatPNG))
    formats |= KVMFR_CLIPBOARD_FORMAT_MASK_PNG;
  if (IsClipboardFormatAvailable(CF_DIBV5) ||
      IsClipboardFormatAvailable(CF_DIB))
    formats |= KVMFR_CLIPBOARD_FORMAT_MASK_BMP;
  if (IsClipboardFormatAvailable(CF_TIFF))
    formats |= KVMFR_CLIPBOARD_FORMAT_MASK_TIFF;
  if (m_formatJPEG && IsClipboardFormatAvailable(m_formatJPEG))
    formats |= KVMFR_CLIPBOARD_FORMAT_MASK_JPEG;
  return formats;
}

void CClipboardManager::HandleClipboardUpdate()
{
  if (m_applyingRemote || IsOurClipboard())
    return;

  ClearRemoteRetry();
  InvalidateLocalClipboard(ERROR_OPERATION_ABORTED);
  m_remoteGeneration = 0;
  m_remoteFormats = 0;
  m_ownedSequence = 0;
  CancelIncoming(ERROR_OPERATION_ABORTED);
  PublishLocalClipboard();
}

void CClipboardManager::HandleDestroyClipboard()
{
  if (m_applyingRemote)
    return;
  ClearRemoteRetry();
  InvalidateLocalClipboard(ERROR_OPERATION_ABORTED);
  m_remoteGeneration = 0;
  m_remoteFormats = 0;
  m_ownedSequence = 0;
  CancelIncoming(ERROR_OPERATION_ABORTED);
}

void CClipboardManager::PublishLocalClipboard()
{
  if (!m_available)
  {
    Atomic::Store(m_liveLocalGeneration, UINT64_C(0),
      std::memory_order_release);
    m_localSequence = GetClipboardSequenceNumber();
    return;
  }

  const DWORD before = GetClipboardSequenceNumber();
  const uint32_t formats = EnumerateFormats();
  const DWORD after = GetClipboardSequenceNumber();
  if (before != after)
  {
    PostMessageW(m_hwnd, WM_CLIPBOARDUPDATE, 0, 0);
    return;
  }

  uint64_t generation = ++m_localGeneration;
  if (!generation)
    generation = ++m_localGeneration;
  m_localSequence = after;
  if (!formats)
  {
    PublishClear(generation);
    return;
  }

  Work work;
  work.type = WorkType::SEND;
  work.record.version = KVMFR_CLIPBOARD_VERSION;
  work.record.type = KVMFR_CLIPBOARD_MESSAGE_OFFER;
  work.record.clipboardGeneration = generation;
  work.record.token = formats;
  {
    std::lock_guard<std::mutex> lock(m_outgoingLock);
    Atomic::Store(m_liveLocalGeneration, generation,
      std::memory_order_release);
  }
  if (!QueueWork(std::move(work)))
  {
    uint64_t live = generation;
    Atomic::CAS(m_liveLocalGeneration, live, UINT64_C(0),
      std::memory_order_acq_rel);
    PostMessageW(m_hwnd, WM_CLIPBOARDUPDATE, 0, 0);
  }
}

void CClipboardManager::PublishClear(uint64_t generation)
{
  Work work;
  work.type = WorkType::SEND;
  work.record.version = KVMFR_CLIPBOARD_VERSION;
  work.record.type = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
  work.record.clipboardGeneration = generation;
  {
    std::lock_guard<std::mutex> lock(m_outgoingLock);
    Atomic::Store(m_liveLocalGeneration, generation,
      std::memory_order_release);
  }
  if (!QueueWork(std::move(work)))
  {
    uint64_t live = generation;
    Atomic::CAS(m_liveLocalGeneration, live, UINT64_C(0),
      std::memory_order_acq_rel);
    PostMessageW(m_hwnd, WM_CLIPBOARDUPDATE, 0, 0);
  }
}

void CClipboardManager::InvalidateOutgoing(uint32_t reason)
{
  uint64_t generation;
  uint64_t transfer;
  {
    std::lock_guard<std::mutex> lock(m_outgoingLock);
    generation = Atomic::Swap(m_liveLocalGeneration,
      UINT64_C(0), std::memory_order_acq_rel);
    transfer = Atomic::Swap(
      m_outgoingTransfer, UINT64_C(0), std::memory_order_acq_rel);
  }
  if (!transfer)
    return;

  KVMFRClipboardMessage cancel = {};
  cancel.clipboardGeneration = generation;
  cancel.transfer = transfer;
  QueueCancel(cancel, reason);
}

void CClipboardManager::InvalidateLocalClipboard(uint32_t reason)
{
  m_localSequence = 0;
  InvalidateOutgoing(reason);
}

void CClipboardManager::ClearRemoteRetry()
{
  if (m_hwnd)
    KillTimer(m_hwnd, REMOTE_RETRY_TIMER);
  m_pendingRemoteOffer = {};
  m_remoteRetryDeadline = 0;
}

void CClipboardManager::RetryRemoteOffer()
{
  KillTimer(m_hwnd, REMOTE_RETRY_TIMER);
  if (!m_pendingRemoteOffer.clipboardGeneration || !m_available)
    return;
  const KVMFRClipboardMessage offer = m_pendingRemoteOffer;
  if (ApplyRemoteOffer(offer.token, offer.clipboardGeneration))
  {
    ClearRemoteRetry();
    return;
  }

  if (GetTickCount64() < m_remoteRetryDeadline &&
      SetTimer(m_hwnd, REMOTE_RETRY_TIMER, REMOTE_RETRY_MS, nullptr))
    return;

  DEBUG_WARN("Failed to apply remote clipboard offer");
  ClearRemoteRetry();
  PublishLocalClipboard();
}

bool CClipboardManager::SetOriginMarker(uint64_t generation) const
{
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(ClipboardOrigin));
  if (!memory)
    return false;
  ClipboardOrigin * origin =
    static_cast<ClipboardOrigin *>(GlobalLock(memory));
  if (!origin)
  {
    const DWORD error = GetLastError();
    GlobalFree(memory);
    SetLastError(error ? error : ERROR_NOT_ENOUGH_MEMORY);
    return false;
  }
  *origin = { ORIGIN_MAGIC, 0, m_epoch, generation };
  GlobalUnlock(memory);
  if (!SetClipboardData(m_formatOrigin, memory))
  {
    const DWORD error = GetLastError();
    GlobalFree(memory);
    SetLastError(error ? error : ERROR_INVALID_DATA);
    return false;
  }
  return true;
}

bool CClipboardManager::ApplyRemoteOffer(uint32_t formats,
  uint64_t generation)
{
  if (!OpenClipboardRetry())
  {
    DEBUG_WARN_HR(GetLastError(), "Failed to open the clipboard");
    return false;
  }

  m_applyingRemote = true;
  const bool emptied = EmptyClipboard() != FALSE;
  DWORD error = emptied ? ERROR_SUCCESS : GetLastError();
  if (emptied)
  {
    auto setDelayed = [&error](UINT format) {
      if (!format)
      {
        if (!error)
          error = ERROR_INVALID_DATA;
        return false;
      }
      SetLastError(ERROR_SUCCESS);
      const HANDLE result = SetClipboardData(format, nullptr);
      if (result || IsClipboardFormatAvailable(format))
        return true;
      error = GetLastError();
      if (!error)
        error = ERROR_INVALID_DATA;
      return false;
    };

    bool complete = true;
    if (formats & KVMFR_CLIPBOARD_FORMAT_MASK_TEXT)
      complete = setDelayed(CF_UNICODETEXT) && complete;
    if (formats & KVMFR_CLIPBOARD_FORMAT_MASK_PNG)
      complete = setDelayed(m_formatPNG) && complete;
    if (formats & KVMFR_CLIPBOARD_FORMAT_MASK_BMP)
      complete = setDelayed(CF_DIB) && complete;
    if (formats & KVMFR_CLIPBOARD_FORMAT_MASK_TIFF)
      complete = setDelayed(CF_TIFF) && complete;
    if (formats & KVMFR_CLIPBOARD_FORMAT_MASK_JPEG)
      complete = setDelayed(m_formatJPEG) && complete;
    if (!SetOriginMarker(generation))
    {
      if (!error)
        error = GetLastError();
      if (!error)
        error = ERROR_INVALID_DATA;
      complete = false;
    }

    if (complete)
    {
      m_remoteGeneration = generation;
      m_remoteFormats = formats;
    }
    else
    {
      EmptyClipboard();
      m_remoteGeneration = 0;
      m_remoteFormats = 0;
    }
  }
  CloseClipboard();
  m_applyingRemote = false;

  if (!emptied || error)
  {
    if (!error)
      error = ERROR_INVALID_DATA;
    SetLastError(error);
    DEBUG_WARN_HR(error, "Failed to replace the clipboard");
    return false;
  }
  m_ownedSequence = GetClipboardSequenceNumber();
  return true;
}

void CClipboardManager::ClearOwnedClipboard()
{
  CancelIncoming(ERROR_OPERATION_ABORTED);
  if (m_hwnd && GetClipboardOwner() == m_hwnd && OpenClipboardRetry())
  {
    m_applyingRemote = true;
    EmptyClipboard();
    CloseClipboard();
    m_applyingRemote = false;
  }
  m_remoteGeneration = 0;
  m_remoteFormats = 0;
  m_ownedSequence = 0;
}

KVMFRClipboardFormat CClipboardManager::ToWireFormat(UINT format) const
{
  if (format == CF_UNICODETEXT)
    return KVMFR_CLIPBOARD_FORMAT_TEXT;
  if (format == m_formatPNG)
    return KVMFR_CLIPBOARD_FORMAT_PNG;
  if (format == CF_DIB || format == CF_DIBV5)
    return KVMFR_CLIPBOARD_FORMAT_BMP;
  if (format == CF_TIFF)
    return KVMFR_CLIPBOARD_FORMAT_TIFF;
  if (format == m_formatJPEG)
    return KVMFR_CLIPBOARD_FORMAT_JPEG;
  return KVMFR_CLIPBOARD_FORMAT_NONE;
}

UINT CClipboardManager::ToWindowsFormat(KVMFRClipboardFormat format) const
{
  switch (format)
  {
    case KVMFR_CLIPBOARD_FORMAT_TEXT:
      return CF_UNICODETEXT;
    case KVMFR_CLIPBOARD_FORMAT_PNG:
      return m_formatPNG;
    case KVMFR_CLIPBOARD_FORMAT_BMP:
      return IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
    case KVMFR_CLIPBOARD_FORMAT_TIFF:
      return CF_TIFF;
    case KVMFR_CLIPBOARD_FORMAT_JPEG:
      return m_formatJPEG;
    default:
      return 0;
  }
}

std::shared_ptr<CClipboardSpool> CClipboardManager::CaptureFormat(
  KVMFRClipboardFormat format, DWORD sequence)
{
  if (GetClipboardSequenceNumber() != sequence)
  {
    SetLastError(ERROR_RETRY);
    return nullptr;
  }
  if (!OpenClipboardRetry())
    return nullptr;

  if (GetClipboardSequenceNumber() != sequence)
  {
    CloseClipboard();
    SetLastError(ERROR_RETRY);
    return nullptr;
  }

  UINT windowsFormat = ToWindowsFormat(format);
  HANDLE handle = windowsFormat ? GetClipboardData(windowsFormat) : nullptr;
  if (!handle && format == KVMFR_CLIPBOARD_FORMAT_BMP &&
      windowsFormat == CF_DIBV5)
  {
    windowsFormat = CF_DIB;
    handle = GetClipboardData(windowsFormat);
  }
  if (!handle)
  {
    const DWORD error = GetLastError();
    CloseClipboard();
    SetLastError(error ? error : ERROR_NOT_FOUND);
    return nullptr;
  }

  const SIZE_T sourceSize = GlobalSize(handle);
  if (sourceSize > MAX_SPOOL_BYTES)
  {
    CloseClipboard();
    SetLastError(ERROR_FILE_TOO_LARGE);
    return nullptr;
  }
  const uint8_t * source = static_cast<const uint8_t *>(GlobalLock(handle));
  if (!source || !sourceSize)
  {
    const DWORD error = GetLastError();
    if (source)
      GlobalUnlock(handle);
    CloseClipboard();
    SetLastError(error ? error : ERROR_INVALID_DATA);
    return nullptr;
  }

  std::shared_ptr<CClipboardSpool> spool;
  try
  {
    spool = std::make_shared<CClipboardSpool>();
  }
  catch (const std::bad_alloc&)
  {
    GlobalUnlock(handle);
    CloseClipboard();
    SetLastError(ERROR_OUTOFMEMORY);
    return nullptr;
  }

  SetLastError(ERROR_SUCCESS);
  bool success = true;
  if (format == KVMFR_CLIPBOARD_FORMAT_TEXT)
  {
    if (sourceSize % sizeof(wchar_t))
    {
      SetLastError(ERROR_INVALID_DATA);
      success = false;
    }
    else
    {
      const wchar_t * text = reinterpret_cast<const wchar_t *>(source);
      const size_t capacity = sourceSize / sizeof(wchar_t);
      size_t length = 0;
      while (length < capacity && text[length])
        ++length;

      std::vector<uint8_t> output;
      try
      {
        output.reserve(TEXT_CONVERSION_CHUNK);
        for (size_t index = 0; success && index < length; ++index)
        {
          uint32_t codepoint = static_cast<uint16_t>(text[index]);
          if (codepoint == '\r' && index + 1 < length &&
              text[index + 1] == L'\n')
            continue;
          if (codepoint >= 0xd800 && codepoint <= 0xdbff)
          {
            if (index + 1 < length)
            {
              const uint32_t low = static_cast<uint16_t>(text[index + 1]);
              if (low >= 0xdc00 && low <= 0xdfff)
              {
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                  (low - 0xdc00);
                ++index;
              }
              else
                codepoint = 0xfffd;
            }
            else
              codepoint = 0xfffd;
          }
          else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
            codepoint = 0xfffd;

          AppendUTF8(output, codepoint);
          if (output.size() >= TEXT_CONVERSION_CHUNK)
          {
            success = spool->Append(output.data(), output.size());
            output.clear();
          }
        }
      }
      catch (const std::bad_alloc&)
      {
        SetLastError(ERROR_OUTOFMEMORY);
        success = false;
      }
      if (success && !output.empty())
        success = spool->Append(output.data(), output.size());
    }
  }
  else if (format == KVMFR_CLIPBOARD_FORMAT_BMP)
  {
    const uint32_t dibOffset = DIBPixelOffset(source, sourceSize);
    if (!dibOffset || sourceSize >
        (std::numeric_limits<uint32_t>::max)() - sizeof(BITMAPFILEHEADER))
    {
      SetLastError(sourceSize >
        (std::numeric_limits<uint32_t>::max)() - sizeof(BITMAPFILEHEADER) ?
        ERROR_FILE_TOO_LARGE : ERROR_INVALID_DATA);
      success = false;
    }
    else
    {
      BITMAPFILEHEADER header = {};
      header.bfType = 0x4d42;
      header.bfSize = static_cast<DWORD>(
        sizeof(BITMAPFILEHEADER) + sourceSize);
      header.bfOffBits = sizeof(BITMAPFILEHEADER) + dibOffset;
      success = spool->Append(&header, sizeof(header)) &&
        spool->Append(source, sourceSize);
    }
  }
  else
    success = spool->Append(source, sourceSize);

  DWORD error = success ? ERROR_SUCCESS : GetLastError();
  if (!success && !error)
    error = ERROR_NOT_ENOUGH_MEMORY;
  GlobalUnlock(handle);
  if (GetClipboardSequenceNumber() != sequence)
  {
    CloseClipboard();
    SetLastError(ERROR_RETRY);
    return nullptr;
  }
  CloseClipboard();
  if (!success)
  {
    SetLastError(error);
    return nullptr;
  }
  return spool;
}

bool CClipboardManager::MaterializeFormat(KVMFRClipboardFormat format,
  CClipboardSpool& spool)
{
  UINT windowsFormat = ToWindowsFormat(format);
  if (format == KVMFR_CLIPBOARD_FORMAT_BMP)
    windowsFormat = CF_DIB;
  if (!windowsFormat)
  {
    SetLastError(ERROR_INVALID_DATA);
    return false;
  }

  HGLOBAL memory = nullptr;
  if (format == KVMFR_CLIPBOARD_FORMAT_TEXT)
    memory = UnicodeFromUTF8(spool);
  else if (format == KVMFR_CLIPBOARD_FORMAT_BMP)
  {
    BITMAPFILEHEADER header = {};
    const bool haveHeader = spool.Size() >= sizeof(header) &&
      spool.Read(0, &header, sizeof(header));
    if (!haveHeader ||
        header.bfType != 0x4d42 ||
        header.bfOffBits < sizeof(header) ||
        header.bfOffBits > spool.Size())
    {
      if (haveHeader && (header.bfType != 0x4d42 ||
          header.bfOffBits < sizeof(header) ||
          header.bfOffBits > spool.Size()))
        SetLastError(ERROR_INVALID_DATA);
      return false;
    }
    memory = CopySpoolToGlobal(spool, sizeof(header));
  }
  else
    memory = CopySpoolToGlobal(spool, 0);

  if (!memory)
    return false;
  if (!SetClipboardData(windowsFormat, memory))
  {
    const DWORD error = GetLastError();
    GlobalFree(memory);
    SetLastError(error ? error : ERROR_INVALID_DATA);
    return false;
  }
  return true;
}

void CClipboardManager::RenderFormat(UINT windowsFormat, uint64_t deadline)
{
  const KVMFRClipboardFormat format = ToWireFormat(windowsFormat);
  if (!m_available || !m_remoteGeneration ||
      !kvmfrClipboardFormatValid(format) ||
      !(m_remoteFormats & kvmfrClipboardFormatFlag(format)))
    return;
  if (deadline && GetTickCount64() >= deadline)
    return;

  std::shared_ptr<IncomingTransfer> transfer;
  try
  {
    transfer = std::make_shared<IncomingTransfer>();
    transfer->spool = std::make_shared<CClipboardSpool>();
  }
  catch (const std::bad_alloc&)
  {
    return;
  }
  transfer->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!transfer->event)
    return;
  if (!deadline)
    deadline = GetTickCount64() + RENDER_TIMEOUT_MS;

  transfer->generation = m_remoteGeneration;
  transfer->format = format;
  transfer->transfer = Atomic::FetchAdd(m_nextTransfer, UINT64_C(1),
    std::memory_order_relaxed);
  if (!kvmfrClipboardTransferFromHelper(transfer->transfer))
  {
    Atomic::Store(m_nextTransfer,
      KVMFR_CLIPBOARD_TRANSFER_HELPER | UINT64_C(2),
      std::memory_order_relaxed);
    transfer->transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER | UINT64_C(1);
  }

  {
    std::lock_guard<std::mutex> lock(m_transferLock);
    if (m_incoming && !m_incoming->complete)
      return;
    m_incoming = transfer;
  }

  Work request;
  request.type = WorkType::SEND;
  request.record.version = KVMFR_CLIPBOARD_VERSION;
  request.record.type = KVMFR_CLIPBOARD_MESSAGE_REQUEST;
  request.record.clipboardGeneration = transfer->generation;
  request.record.transfer = transfer->transfer;
  request.record.format = transfer->format;
  request.deadline = deadline;
  if (!QueueWork(std::move(request)))
    CancelIncoming(ERROR_BUSY, transfer->transfer);

  const uint64_t now = GetTickCount64();
  const DWORD waitMs = now >= deadline ? 0 : static_cast<DWORD>(
    (std::min<uint64_t>)(deadline - now, MAXDWORD));
  const DWORD wait = WaitForSingleObject(transfer->event, waitMs);
  const DWORD waitError = wait == WAIT_FAILED ? GetLastError() :
    ERROR_SUCCESS;
  bool requestComplete = false;
  uint32_t transferError = ERROR_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(m_transferLock);
    requestComplete = transfer->complete;
    transferError = transfer->error;
  }
  if (!requestComplete || transferError != ERROR_SUCCESS)
  {
    const uint32_t reason = requestComplete ? transferError :
      (wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
        (wait == WAIT_FAILED ? waitError : ERROR_INVALID_DATA));
    CancelIncoming(reason ? reason : ERROR_OPERATION_ABORTED,
      transfer->transfer);
    KVMFRClipboardMessage cancel = {};
    cancel.clipboardGeneration = transfer->generation;
    cancel.transfer            = transfer->transfer;
    cancel.format              = transfer->format;
    QueueCancel(cancel, reason ? reason : ERROR_OPERATION_ABORTED);
  }

  bool render = false;
  {
    std::lock_guard<std::mutex> lock(m_transferLock);
    render = transfer->complete && transfer->error == ERROR_SUCCESS &&
      transfer->generation == m_remoteGeneration &&
      GetClipboardOwner() == m_hwnd;
    if (m_incoming == transfer)
      m_incoming.reset();
  }

  if (render && !MaterializeFormat(format, *transfer->spool))
    DEBUG_WARN_HR(GetLastError(), "Failed to render clipboard format %u",
      format);
}

void CClipboardManager::RenderAllFormats()
{
  if (!OpenClipboardRetry())
    return;
  if (GetClipboardOwner() != m_hwnd)
  {
    CloseClipboard();
    return;
  }

  const uint32_t formats = m_remoteFormats;
  const uint64_t deadline = GetTickCount64() + RENDER_TIMEOUT_MS;
  if (formats & KVMFR_CLIPBOARD_FORMAT_MASK_TEXT)
    RenderFormat(CF_UNICODETEXT, deadline);
  if (GetTickCount64() < deadline &&
      (formats & KVMFR_CLIPBOARD_FORMAT_MASK_PNG))
    RenderFormat(m_formatPNG, deadline);
  if (GetTickCount64() < deadline &&
      (formats & KVMFR_CLIPBOARD_FORMAT_MASK_BMP))
    RenderFormat(CF_DIB, deadline);
  if (GetTickCount64() < deadline &&
      (formats & KVMFR_CLIPBOARD_FORMAT_MASK_TIFF))
    RenderFormat(CF_TIFF, deadline);
  if (GetTickCount64() < deadline &&
      (formats & KVMFR_CLIPBOARD_FORMAT_MASK_JPEG))
    RenderFormat(m_formatJPEG, deadline);
  CloseClipboard();
}
