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

#include "clipboard.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/windebug.h"
#include "core/clipboard.h"
#include "core/clipboard_files.h"
#include "platform.h"
#include "platform/windows/clipboard_files.h"

#define WINDOWS_CB_OPEN_ATTEMPTS     8
#define WINDOWS_CB_RETRY_INTERVAL_MS 50
#define WINDOWS_CB_RETRY_TIMEOUT_MS  5000
#define WINDOWS_CB_RETRY_TIMER_ID    ((UINT_PTR)0x4C47)

enum WindowsClipboardMessage
{
  WM_LG_CLIPBOARD_INIT     = WM_APP + 0x100,
  WM_LG_CLIPBOARD_DISPATCH = WM_APP + 0x101,
};

enum WindowsClipboardOperationType
{
  WINDOWS_CB_OPERATION_NOTICE,
  WINDOWS_CB_OPERATION_RELEASE,
  WINDOWS_CB_OPERATION_REQUEST,
  WINDOWS_CB_OPERATION_DATA,
};

enum WindowsClipboardCaptureResult
{
  WINDOWS_CB_CAPTURE_RETRY,
  WINDOWS_CB_CAPTURE_TYPES,
  WINDOWS_CB_CAPTURE_FILES,
};

struct WindowsClipboardOperation
{
  struct WindowsClipboardOperation * next;
  enum WindowsClipboardOperationType type;
  uint64_t                           generation;
  LG_ClipboardRequest                request;
  LG_ClipboardData                   dataType;
  uint8_t                          * data;
  size_t                             size;
};

struct WindowsClipboardTransfer
{
  uint64_t         generation;
  LG_ClipboardData type;
  uint8_t        * data;
  size_t           size;
  size_t           capacity;
  bool             started;
};

struct WindowsClipboardState
{
  struct WindowsClipboardOperation * queueHead;
  struct WindowsClipboardOperation * queueTail;
  HWND                               window;
  HWND                               localOwner;
  DWORD                              localSequence;
  uint32_t                           localTypes;
  UINT                               pngFormat;
  UINT                               jfifFormat;
  UINT                               jpegFormat;
  IDataObject                      * remoteFiles;
  LG_ClipboardRequest                canceledRequest;
  ULONGLONG                          retryDeadline;
  DWORD                              retrySequence;
  bool                               listener;
  bool                               accepting;
  bool                               oleInitialized;
  bool                               applyingRemote;
  bool                               pendingUpdate;
};

static SRWLOCK                      windowsCBQueueLock = SRWLOCK_INIT;
static struct WindowsClipboardState windowsCB;
static _Atomic(uint64_t)            windowsCBGeneration;

static void updateLocalClipboard(void);

static uint16_t readLE16(const uint8_t * data)
{
  return (uint16_t)data[0] |
    (uint16_t)((uint16_t)data[1] << 8);
}

static uint32_t readLE32(const uint8_t * data)
{
  return (uint32_t)data[0] |
    (uint32_t)data[1] << 8 |
    (uint32_t)data[2] << 16 |
    (uint32_t)data[3] << 24;
}

static void writeLE16(uint8_t * data, uint16_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
}

static void writeLE32(uint8_t * data, uint32_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

static void freeOperation(struct WindowsClipboardOperation * operation)
{
  if (!operation)
    return;

  free(operation->data);
  free(operation);
}

static void freeOperations(struct WindowsClipboardOperation * operation)
{
  while (operation)
  {
    struct WindowsClipboardOperation * next = operation->next;
    freeOperation(operation);
    operation = next;
  }
}

static struct WindowsClipboardOperation * newOperation(
    enum WindowsClipboardOperationType type)
{
  struct WindowsClipboardOperation * operation =
    calloc(1, sizeof(*operation));
  if (!operation)
  {
    DEBUG_ERROR("Out of memory while queuing a Windows clipboard operation");
    return NULL;
  }

  operation->type = type;
  return operation;
}

static bool queueOperation(struct WindowsClipboardOperation * operation)
{
  if (!operation)
    return false;

  AcquireSRWLockExclusive(&windowsCBQueueLock);
  if (!windowsCB.accepting || !windowsCB.window)
  {
    ReleaseSRWLockExclusive(&windowsCBQueueLock);
    freeOperation(operation);
    return false;
  }

  const bool wake = !windowsCB.queueTail;
  if (windowsCB.queueTail)
    windowsCB.queueTail->next = operation;
  else
    windowsCB.queueHead = operation;
  windowsCB.queueTail = operation;

  if (wake && !PostMessageW(
        windowsCB.window, WM_LG_CLIPBOARD_DISPATCH, 0, 0))
  {
    windowsCB.queueHead = NULL;
    windowsCB.queueTail = NULL;
    ReleaseSRWLockExclusive(&windowsCBQueueLock);
    DEBUG_WINERROR("Failed to queue a Windows clipboard operation",
        GetLastError());
    freeOperation(operation);
    return false;
  }

  ReleaseSRWLockExclusive(&windowsCBQueueLock);
  return true;
}

static struct WindowsClipboardOperation * takeOperation(void)
{
  AcquireSRWLockExclusive(&windowsCBQueueLock);
  struct WindowsClipboardOperation * operation = windowsCB.queueHead;
  if (operation)
  {
    windowsCB.queueHead = operation->next;
    operation->next     = NULL;
    if (!windowsCB.queueHead)
      windowsCB.queueTail = NULL;
  }
  ReleaseSRWLockExclusive(&windowsCBQueueLock);
  return operation;
}

static bool requestCanceled(LG_ClipboardRequest request)
{
  AcquireSRWLockShared(&windowsCBQueueLock);
  const bool canceled = windowsCB.canceledRequest == request;
  ReleaseSRWLockShared(&windowsCBQueueLock);
  return canceled;
}

static bool openClipboard(void)
{
  DWORD delay = 1;
  for (unsigned int attempt = 0;
      attempt < WINDOWS_CB_OPEN_ATTEMPTS; ++attempt)
  {
    if (OpenClipboard(windowsCB.window))
      return true;

    if (attempt + 1 < WINDOWS_CB_OPEN_ATTEMPTS)
    {
      Sleep(delay);
      delay *= 2;
    }
  }

  DEBUG_WINERROR("Failed to open the Windows clipboard", GetLastError());
  return false;
}

static bool checkedAdd(size_t a, size_t b, size_t * result)
{
  if (a > SIZE_MAX - b)
    return false;

  *result = a + b;
  return true;
}

static bool uint64ToSize(uint64_t value, size_t * result)
{
  const size_t converted = (size_t)value;
  if ((uint64_t)converted != value)
    return false;

  *result = converted;
  return true;
}

static bool validateDIB(const uint8_t * data, size_t size,
    size_t * pixelOffset)
{
  if (!data || size < sizeof(uint32_t))
    return false;

  const uint32_t headerSize = readLE32(data);
  size_t         offset;
  if (headerSize == sizeof(BITMAPCOREHEADER))
  {
    if (size < sizeof(BITMAPCOREHEADER) || !readLE16(data + 4) ||
        !readLE16(data + 6) || readLE16(data + 8) != 1)
      return false;

    const uint16_t bits = readLE16(data + 10);
    if (!bits || bits > 32)
      return false;

    const size_t colors = bits <= 8 ? (size_t)1 << bits : 0;
    if (colors > (SIZE_MAX - headerSize) / sizeof(RGBTRIPLE))
      return false;
    offset = headerSize + colors * sizeof(RGBTRIPLE);
  }
  else
  {
    if (headerSize < sizeof(BITMAPINFOHEADER) || headerSize > size ||
        !readLE32(data + 4) || !readLE32(data + 8) ||
        readLE16(data + 12) != 1)
      return false;

    const uint16_t bits        = readLE16(data + 14);
    const uint32_t compression = readLE32(data + 16);
    const uint32_t imageSize   = readLE32(data + 20);
    const uint32_t usedColors  = readLE32(data + 32);
    if (bits > 32)
      return false;

    size_t maskSize = 0;
    if (headerSize == sizeof(BITMAPINFOHEADER))
    {
      if (compression == BI_BITFIELDS)
        maskSize = 3 * sizeof(uint32_t);
#ifdef BI_ALPHABITFIELDS
      else if (compression == BI_ALPHABITFIELDS)
        maskSize = 4 * sizeof(uint32_t);
#endif
    }

    const size_t colors = usedColors ? usedColors :
      (bits && bits <= 8 ? (size_t)1 << bits : 0);
    if (!checkedAdd(headerSize, maskSize, &offset) ||
        colors > (SIZE_MAX - offset) / sizeof(RGBQUAD))
      return false;
    offset += colors * sizeof(RGBQUAD);
    if (imageSize && (offset > size || imageSize > size - offset))
      return false;
  }

  if (offset > size)
    return false;

  *pixelOffset = offset;
  return true;
}

static bool bmpToDIB(const void * source, size_t sourceSize,
    uint8_t ** output, size_t * outputSize)
{
  const uint8_t * data = source;
  if (!data || sourceSize < sizeof(BITMAPFILEHEADER) ||
      readLE16(data) != UINT16_C(0x4D42))
    return false;

  const uint32_t fileSizeValue = readLE32(data + 2);
  const uint32_t fileOffset    = readLE32(data + 10);
  const size_t   fileSize      = fileSizeValue ? fileSizeValue : sourceSize;
  if (fileSize > sourceSize || fileSize < sizeof(BITMAPFILEHEADER) ||
      fileOffset < sizeof(BITMAPFILEHEADER) || fileOffset > fileSize)
    return false;

  const uint8_t * dib     = data + sizeof(BITMAPFILEHEADER);
  const size_t    dibSize = fileSize - sizeof(BITMAPFILEHEADER);
  size_t          minimumOffset;
  if (!validateDIB(dib, dibSize, &minimumOffset) ||
      fileOffset - sizeof(BITMAPFILEHEADER) < minimumOffset)
    return false;

  uint8_t * copy = malloc(dibSize ? dibSize : 1);
  if (!copy)
    return false;

  if (dibSize)
    memcpy(copy, dib, dibSize);
  *output     = copy;
  *outputSize = dibSize;
  return true;
}

static bool dibToBMP(const void * source, size_t sourceSize,
    uint8_t ** output, size_t * outputSize)
{
  const uint8_t * dib = source;
  size_t          dibOffset;
  if (!validateDIB(dib, sourceSize, &dibOffset) ||
      sourceSize > UINT32_MAX - sizeof(BITMAPFILEHEADER))
    return false;

  const size_t size = sizeof(BITMAPFILEHEADER) + sourceSize;
  uint8_t    * bmp  = malloc(size);
  if (!bmp)
    return false;

  memset(bmp, 0, sizeof(BITMAPFILEHEADER));
  writeLE16(bmp, UINT16_C(0x4D42));
  writeLE32(bmp + 2, (uint32_t)size);
  writeLE32(bmp + 10,
      (uint32_t)(sizeof(BITMAPFILEHEADER) + dibOffset));
  memcpy(bmp + sizeof(BITMAPFILEHEADER), dib, sourceSize);
  *output     = bmp;
  *outputSize = size;
  return true;
}

static bool utf8ToClipboardText(const void * source, size_t sourceSize,
    uint8_t ** output, size_t * outputSize)
{
  if ((sourceSize && !source) || sourceSize > INT_MAX ||
      (sourceSize && memchr(source, 0, sourceSize)))
    return false;

  int sourceLength = 0;
  if (sourceSize)
  {
    sourceLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        source, (int)sourceSize, NULL, 0);
    if (sourceLength <= 0)
      return false;
  }

  wchar_t * wide = malloc(
      ((size_t)sourceLength + 1) * sizeof(*wide));
  if (!wide)
    return false;
  if (sourceLength && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        source, (int)sourceSize, wide, sourceLength) != sourceLength)
  {
    free(wide);
    return false;
  }

  size_t lineBreaks = 0;
  for (int i = 0; i < sourceLength; ++i)
    if (wide[i] == L'\n' && (!i || wide[i - 1] != L'\r'))
      ++lineBreaks;
  if ((size_t)sourceLength >
      (SIZE_MAX / sizeof(wchar_t)) - lineBreaks - 1)
  {
    free(wide);
    return false;
  }

  const size_t count = (size_t)sourceLength + lineBreaks + 1;
  wchar_t    * text  = malloc(count * sizeof(*text));
  if (!text)
  {
    free(wide);
    return false;
  }

  size_t at = 0;
  for (int i = 0; i < sourceLength; ++i)
  {
    if (wide[i] == L'\n' && (!i || wide[i - 1] != L'\r'))
      text[at++] = L'\r';
    text[at++] = wide[i];
  }
  text[at++] = L'\0';
  free(wide);

  *output     = (uint8_t *)text;
  *outputSize = at * sizeof(*text);
  return true;
}

static bool clipboardTextToUtf8(const void * source, size_t sourceSize,
    uint8_t ** output, size_t * outputSize)
{
  if (!source || sourceSize < sizeof(wchar_t) ||
      sourceSize % sizeof(wchar_t))
    return false;

  const wchar_t * text     = source;
  const size_t    capacity = sourceSize / sizeof(*text);
  size_t          length   = 0;
  while (length < capacity && text[length])
    ++length;
  if (length == capacity || length > INT_MAX)
    return false;

  wchar_t * normalized = malloc((length + 1) * sizeof(*normalized));
  if (!normalized)
    return false;

  size_t normalizedLength = 0;
  for (size_t i = 0; i < length; ++i)
  {
    if (text[i] == L'\r' && i + 1 < length && text[i + 1] == L'\n')
    {
      ++i;
      normalized[normalizedLength++] = L'\n';
    }
    else
      normalized[normalizedLength++] = text[i];
  }

  const int size = normalizedLength ?
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        normalized, (int)normalizedLength, NULL, 0, NULL, NULL) : 0;
  if (normalizedLength && size <= 0)
  {
    free(normalized);
    return false;
  }

  uint8_t * utf8 = size ? malloc((size_t)size) : NULL;
  if (size && (!utf8 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        normalized, (int)normalizedLength, (char *)utf8, size,
        NULL, NULL) != size))
  {
    free(normalized);
    free(utf8);
    return false;
  }
  free(normalized);

  *output     = utf8;
  *outputSize = (size_t)size;
  return true;
}

static HGLOBAL makeGlobalData(const void * data, size_t size)
{
  if (size && !data)
    return NULL;

  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
  if (!handle)
    return NULL;

  void * target = GlobalLock(handle);
  if (!target)
  {
    GlobalFree(handle);
    return NULL;
  }
  if (size)
    memcpy(target, data, size);
  GlobalUnlock(handle);
  return handle;
}

static bool copyGlobalData(HANDLE handle, uint8_t ** output,
    size_t * outputSize)
{
  const SIZE_T size = GlobalSize(handle);
  if (!size)
    return false;

  const void * source = GlobalLock(handle);
  if (!source)
    return false;

  uint8_t * data = malloc(size);
  if (data)
    memcpy(data, source, size);
  GlobalUnlock(handle);
  if (!data)
    return false;

  *output     = data;
  *outputSize = size;
  return true;
}

static bool localTypeAvailable(LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      return IsClipboardFormatAvailable(CF_UNICODETEXT);

    case LG_CLIPBOARD_DATA_PNG:
      return windowsCB.pngFormat &&
        IsClipboardFormatAvailable(windowsCB.pngFormat);

    case LG_CLIPBOARD_DATA_BMP:
      return IsClipboardFormatAvailable(CF_DIBV5) ||
        IsClipboardFormatAvailable(CF_DIB);

    case LG_CLIPBOARD_DATA_TIFF:
      return IsClipboardFormatAvailable(CF_TIFF);

    case LG_CLIPBOARD_DATA_JPEG:
      return (windowsCB.jfifFormat &&
          IsClipboardFormatAvailable(windowsCB.jfifFormat)) ||
        (windowsCB.jpegFormat &&
          IsClipboardFormatAvailable(windowsCB.jpegFormat));

    case LG_CLIPBOARD_DATA_FILES:
      return IsClipboardFormatAvailable(CF_HDROP);

    case LG_CLIPBOARD_DATA_NONE:
      return false;
  }

  return false;
}

static UINT localClipboardFormat(LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      return CF_UNICODETEXT;
    case LG_CLIPBOARD_DATA_PNG:
      return windowsCB.pngFormat;
    case LG_CLIPBOARD_DATA_BMP:
      if (IsClipboardFormatAvailable(CF_DIBV5))
        return CF_DIBV5;
      return CF_DIB;
    case LG_CLIPBOARD_DATA_TIFF:
      return CF_TIFF;
    case LG_CLIPBOARD_DATA_JPEG:
      if (windowsCB.jfifFormat &&
          IsClipboardFormatAvailable(windowsCB.jfifFormat))
        return windowsCB.jfifFormat;
      return windowsCB.jpegFormat;
    case LG_CLIPBOARD_DATA_FILES:
    case LG_CLIPBOARD_DATA_NONE:
      return 0;
  }

  return 0;
}

static bool readLocalClipboard(LG_ClipboardData type,
    uint8_t ** output, size_t * outputSize)
{
  const UINT format = localClipboardFormat(type);
  if (!format)
    return false;

  HANDLE handle = GetClipboardData(format);
  if (!handle && type == LG_CLIPBOARD_DATA_BMP && format == CF_DIBV5 &&
      IsClipboardFormatAvailable(CF_DIB))
    handle = GetClipboardData(CF_DIB);
  else if (!handle && type == LG_CLIPBOARD_DATA_JPEG)
  {
    const UINT alternate = format == windowsCB.jfifFormat ?
      windowsCB.jpegFormat : windowsCB.jfifFormat;
    if (alternate && IsClipboardFormatAvailable(alternate))
      handle = GetClipboardData(alternate);
  }
  if (!handle)
    return false;

  uint8_t * data = NULL;
  size_t    size = 0;
  if (type == LG_CLIPBOARD_DATA_TEXT)
  {
    const SIZE_T globalSize = GlobalSize(handle);
    const void * source     = globalSize ? GlobalLock(handle) : NULL;
    const bool   result     = source && clipboardTextToUtf8(
        source, globalSize, &data, &size);
    if (source)
      GlobalUnlock(handle);
    if (!result)
      return false;
  }
  else if (type == LG_CLIPBOARD_DATA_BMP)
  {
    const SIZE_T sourceSize = GlobalSize(handle);
    if (!sourceSize ||
        sourceSize > UINT32_MAX - sizeof(BITMAPFILEHEADER))
      return false;

    uint8_t * dib     = NULL;
    size_t    dibSize = 0;
    if (!copyGlobalData(handle, &dib, &dibSize))
      return false;
    const bool result = dibToBMP(dib, dibSize, &data, &size);
    free(dib);
    if (!result)
      return false;
  }
  else if (!copyGlobalData(handle, &data, &size))
    return false;

  *output     = data;
  *outputSize = size;
  return true;
}

static void releaseRemoteFilesReference(void)
{
  if (!windowsCB.remoteFiles)
    return;

  windowsCB.remoteFiles->lpVtbl->Release(windowsCB.remoteFiles);
  windowsCB.remoteFiles = NULL;
}

static bool remoteFilesAreCurrent(void)
{
  return windowsCB.oleInitialized && windowsCB.remoteFiles &&
    OleIsCurrentClipboard(windowsCB.remoteFiles) == S_OK;
}

static HRESULT setOleClipboard(IDataObject * object)
{
  HRESULT status = E_FAIL;
  DWORD   delay  = 1;
  // OleSetClipboard can synchronously dispatch a clipboard notification.
  windowsCB.applyingRemote = true;
  for (unsigned int attempt = 0;
      attempt < WINDOWS_CB_OPEN_ATTEMPTS; ++attempt)
  {
    status = OleSetClipboard(object);
    if (SUCCEEDED(status))
      break;
    if (attempt + 1U < WINDOWS_CB_OPEN_ATTEMPTS)
    {
      Sleep(delay);
      delay *= 2U;
    }
  }
  windowsCB.applyingRemote = false;
  if (windowsCB.pendingUpdate && windowsCB.accepting && windowsCB.window)
  {
    windowsCB.pendingUpdate = false;
    if (!PostMessageW(windowsCB.window, WM_CLIPBOARDUPDATE, 0, 0))
      DEBUG_WINERROR("Failed to replay a Windows clipboard update",
          GetLastError());
  }
  return status;
}

static bool clearRemoteFilesClipboard(void)
{
  if (!windowsCB.remoteFiles)
    return true;

  if (remoteFilesAreCurrent())
  {
    const HRESULT status = setOleClipboard(NULL);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to clear the Windows file clipboard", status);
      return false;
    }
  }

  releaseRemoteFilesReference();
  return true;
}

struct WindowsClipboardCapture
{
  LG_ClipboardData types[LG_CLIPBOARD_DATA_NONE];
  size_t           count;
  uint32_t         mask;
  uint8_t        * fileData;
  size_t           fileSize;
  HWND             owner;
  DWORD            sequence;
};

static enum WindowsClipboardCaptureResult captureLocalClipboard(
    HWND owner, DWORD sequence, struct WindowsClipboardCapture * capture)
{
  memset(capture, 0, sizeof(*capture));
  // Explorer can still own the clipboard when WM_CLIPBOARDUPDATE arrives.
  // Acquire it before probing formats so that generation is retried instead
  // of being recorded as an empty clipboard.
  if (!openClipboard())
    return WINDOWS_CB_CAPTURE_RETRY;

  if (owner != GetClipboardOwner() ||
      sequence != GetClipboardSequenceNumber())
  {
    CloseClipboard();
    return WINDOWS_CB_CAPTURE_RETRY;
  }

  const bool files  = localTypeAvailable(LG_CLIPBOARD_DATA_FILES);
  bool       copied = false;
  if (files)
  {
    HANDLE handle = GetClipboardData(CF_HDROP);
    copied = handle && copyGlobalData(handle,
        &capture->fileData, &capture->fileSize);
  }
  else
  {
    for (LG_ClipboardData type = LG_CLIPBOARD_DATA_TEXT;
        type < LG_CLIPBOARD_DATA_FILES; ++type)
    {
      if (!localTypeAvailable(type))
        continue;

      capture->types[capture->count++] = type;
      capture->mask                   |= UINT32_C(1) << type;
    }
  }

  capture->owner              = GetClipboardOwner();
  capture->sequence           = GetClipboardSequenceNumber();
  const bool sequenceChanged  = capture->sequence != sequence;
  const BOOL closed           = CloseClipboard();
  const DWORD closeError      = closed ? ERROR_SUCCESS : GetLastError();
  const HWND currentOwner     = GetClipboardOwner();
  const DWORD currentSequence = GetClipboardSequenceNumber();
  const bool stable           = closed && capture->owner == currentOwner &&
    capture->sequence == currentSequence && capture->owner == owner &&
    (!sequenceChanged || (files && owner));
  if (!stable || (files && !copied))
  {
    if (!closed)
      DEBUG_WINERROR("Failed to close the Windows clipboard", closeError);
    free(capture->fileData);
    capture->fileData = NULL;
    return WINDOWS_CB_CAPTURE_RETRY;
  }

  return files ? WINDOWS_CB_CAPTURE_FILES : WINDOWS_CB_CAPTURE_TYPES;
}

static void clearLocalRetry(void)
{
  if (windowsCB.window && windowsCB.retryDeadline)
    KillTimer(windowsCB.window, WINDOWS_CB_RETRY_TIMER_ID);
  windowsCB.retryDeadline = 0;
  windowsCB.retrySequence = 0;
}

static void expireLocalRetry(DWORD sequence)
{
  clearLocalRetry();
  if (!windowsCB.accepting || remoteFilesAreCurrent())
    return;
  if (sequence != GetClipboardSequenceNumber())
  {
    PostMessageW(windowsCB.window, WM_CLIPBOARDUPDATE, 0, 0);
    return;
  }

  windowsCB.localOwner    = GetClipboardOwner();
  windowsCB.localSequence = sequence;
  windowsCB.localTypes    = 0;
  clipboardFiles_clearLocal();
  clipboard_release();
}

static void deferLocalClipboard(DWORD sequence)
{
  const ULONGLONG now = GetTickCount64();
  if (!windowsCB.retryDeadline || windowsCB.retrySequence != sequence)
  {
    clearLocalRetry();
    windowsCB.retrySequence = sequence;
    windowsCB.retryDeadline = now + WINDOWS_CB_RETRY_TIMEOUT_MS;
  }

  if (now < windowsCB.retryDeadline &&
      SetTimer(windowsCB.window, WINDOWS_CB_RETRY_TIMER_ID,
        WINDOWS_CB_RETRY_INTERVAL_MS, NULL))
    return;

  if (now >= windowsCB.retryDeadline)
    DEBUG_WARN("Timed out waiting for the Windows clipboard");
  else
    DEBUG_WINERROR("Failed to schedule a Windows clipboard retry",
        GetLastError());
  expireLocalRetry(sequence);
}

static void retryLocalClipboard(void)
{
  if (windowsCB.window)
    KillTimer(windowsCB.window, WINDOWS_CB_RETRY_TIMER_ID);
  if (!windowsCB.retryDeadline)
    return;
  if (!windowsCB.accepting || remoteFilesAreCurrent())
  {
    clearLocalRetry();
    return;
  }

  const DWORD sequence = GetClipboardSequenceNumber();
  if (sequence != windowsCB.retrySequence)
  {
    clearLocalRetry();
    PostMessageW(windowsCB.window, WM_CLIPBOARDUPDATE, 0, 0);
    return;
  }
  if (GetTickCount64() >= windowsCB.retryDeadline)
  {
    DEBUG_WARN("Timed out waiting for the Windows clipboard");
    expireLocalRetry(sequence);
    return;
  }

  updateLocalClipboard();
}

static void updateLocalClipboard(void)
{
  if (windowsCB.applyingRemote)
  {
    windowsCB.pendingUpdate = true;
    return;
  }

  const HWND  owner    = GetClipboardOwner();
  const DWORD sequence = GetClipboardSequenceNumber();
  if (remoteFilesAreCurrent())
  {
    clearLocalRetry();
    return;
  }
  releaseRemoteFilesReference();
  if (owner == windowsCB.window)
    return;
  if (owner == windowsCB.localOwner &&
      sequence == windowsCB.localSequence)
    return;

  if (!windowsCB.retryDeadline || windowsCB.retrySequence != sequence)
    atomic_fetch_add_explicit(
        &windowsCBGeneration, 1, memory_order_acq_rel);

  struct WindowsClipboardCapture capture;
  const enum WindowsClipboardCaptureResult result =
    captureLocalClipboard(owner, sequence, &capture);
  if (result == WINDOWS_CB_CAPTURE_RETRY)
  {
    if (owner != GetClipboardOwner() ||
        sequence != GetClipboardSequenceNumber())
    {
      clearLocalRetry();
      PostMessageW(windowsCB.window, WM_CLIPBOARDUPDATE, 0, 0);
    }
    else
      deferLocalClipboard(sequence);
    return;
  }

  if (result == WINDOWS_CB_CAPTURE_FILES)
  {
    HGLOBAL duplicate = makeGlobalData(capture.fileData, capture.fileSize);
    free(capture.fileData);
    if (!duplicate)
    {
      deferLocalClipboard(capture.sequence);
      return;
    }

    const bool published = windowsClipboardFiles_setLocalDrop(
        (HDROP)duplicate);
    GlobalFree(duplicate);
    if (!published)
    {
      DEBUG_WARN("Failed to capture the Windows file clipboard");
      deferLocalClipboard(capture.sequence);
      return;
    }

    clearLocalRetry();
    windowsCB.localOwner    = capture.owner;
    windowsCB.localSequence = capture.sequence;
    windowsCB.localTypes    = UINT32_C(1) << LG_CLIPBOARD_DATA_FILES;
    return;
  }

  clearLocalRetry();
  windowsCB.localOwner    = capture.owner;
  windowsCB.localSequence = capture.sequence;
  windowsCB.localTypes    = capture.mask;
  clipboardFiles_clearLocal();
  if (capture.count)
    clipboard_notifyTypes(capture.types, capture.count);
  else
    clipboard_release();
}

static void handleLocalRequest(
    const struct WindowsClipboardOperation * operation)
{
  const LG_ClipboardData type     = operation->dataType;
  const uint32_t         typeMask = type < LG_CLIPBOARD_DATA_NONE ?
    UINT32_C(1) << type : 0;
  if (requestCanceled(operation->request))
    return;
  if (!typeMask || !(windowsCB.localTypes & typeMask) ||
      windowsCB.localSequence != GetClipboardSequenceNumber() ||
      windowsCB.localOwner != GetClipboardOwner() || !openClipboard())
  {
    clipboard_abort(operation->request);
    return;
  }

  const bool  current         = windowsCB.localSequence ==
      GetClipboardSequenceNumber() &&
    windowsCB.localOwner == GetClipboardOwner();
  uint8_t   * data            = NULL;
  size_t      size            = 0;
  const bool  read            = current && readLocalClipboard(type,
      &data, &size);
  const HWND  owner           = GetClipboardOwner();
  const DWORD sequence        = GetClipboardSequenceNumber();
  const bool  sequenceChanged = sequence != windowsCB.localSequence;
  CloseClipboard();
  const bool  unchanged       = owner == GetClipboardOwner() &&
    sequence == GetClipboardSequenceNumber();
  if (requestCanceled(operation->request))
  {
    free(data);
    return;
  }
  if (!read || (sequenceChanged && !owner) || !unchanged ||
      owner != windowsCB.localOwner)
  {
    free(data);
    clipboard_abort(operation->request);
    return;
  }

  windowsCB.localSequence = sequence;
  clipboard_data(operation->request, type, data, size);
  free(data);
}

static bool prepareRemoteData(LG_ClipboardData type,
    const void * source, size_t sourceSize, UINT formats[2],
    HGLOBAL handles[2], size_t * count)
{
  if (sourceSize && !source)
    return false;

  uint8_t    * converted     = NULL;
  size_t       convertedSize = 0;
  const void * data          = source;
  size_t       size          = sourceSize;

  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      if (!utf8ToClipboardText(
            source, sourceSize, &converted, &convertedSize))
        return false;
      data       = converted;
      size       = convertedSize;
      formats[0] = CF_UNICODETEXT;
      *count     = 1;
      break;

    case LG_CLIPBOARD_DATA_PNG:
      if (!windowsCB.pngFormat)
        return false;
      formats[0] = windowsCB.pngFormat;
      *count     = 1;
      break;

    case LG_CLIPBOARD_DATA_BMP:
      if (!bmpToDIB(source, sourceSize, &converted, &convertedSize))
        return false;
      data       = converted;
      size       = convertedSize;
      formats[0] = CF_DIB;
      *count     = 1;
      break;

    case LG_CLIPBOARD_DATA_TIFF:
      formats[0] = CF_TIFF;
      *count     = 1;
      break;

    case LG_CLIPBOARD_DATA_JPEG:
      if (!windowsCB.jfifFormat && !windowsCB.jpegFormat)
        return false;
      if (windowsCB.jfifFormat)
        formats[(*count)++] = windowsCB.jfifFormat;
      if (windowsCB.jpegFormat)
        formats[(*count)++] = windowsCB.jpegFormat;
      break;

    case LG_CLIPBOARD_DATA_FILES:
    case LG_CLIPBOARD_DATA_NONE:
      return false;
  }

  bool success = true;
  for (size_t i = 0; i < *count; ++i)
  {
    handles[i] = makeGlobalData(data, size);
    if (!handles[i])
    {
      success = false;
      break;
    }
  }
  free(converted);
  if (success)
    return true;

  for (size_t i = 0; i < *count; ++i)
    if (handles[i])
      GlobalFree(handles[i]);
  return false;
}

static void handleRemoteData(
    const struct WindowsClipboardOperation * operation)
{
  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire))
    return;

  UINT    formats[2] = { 0 };
  HGLOBAL handles[2] = { NULL };
  size_t  count      = 0;
  if (!prepareRemoteData(operation->dataType,
        operation->data, operation->size, formats, handles, &count))
  {
    DEBUG_WARN("Ignoring invalid guest clipboard data");
    return;
  }

  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire) ||
      !clearRemoteFilesClipboard() || !openClipboard())
    goto fail;
  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire) || !EmptyClipboard())
  {
    CloseClipboard();
    goto fail;
  }
  bool published = false;
  for (size_t i = 0; i < count; ++i)
  {
    if (operation->generation != atomic_load_explicit(
          &windowsCBGeneration, memory_order_acquire))
      break;

    if (SetClipboardData(formats[i], handles[i]))
    {
      handles[i] = NULL;
      published  = true;
    }
    else
      DEBUG_WINERROR("Failed to publish Windows clipboard data",
          GetLastError());
  }
  CloseClipboard();
  clipboardFiles_clearLocal();
  windowsCB.localOwner    = GetClipboardOwner();
  windowsCB.localSequence = GetClipboardSequenceNumber();
  windowsCB.localTypes    = 0;
  if (!published)
    DEBUG_WARN("Failed to publish any Windows clipboard format");

fail:
  for (size_t i = 0; i < count; ++i)
    if (handles[i])
      GlobalFree(handles[i]);
}

static void handleRemoteFiles(
    const struct WindowsClipboardOperation * operation)
{
  if (!windowsCB.oleInitialized)
  {
    DEBUG_WARN("OLE is unavailable; cannot publish guest clipboard files");
    return;
  }

  const uint64_t presentation =
    clipboardFiles_remotePresentationAcquire();
  if (!presentation)
  {
    DEBUG_WARN("Failed to acquire the guest clipboard file offer");
    return;
  }

  IDataObject * object = NULL;
  if (!windowsClipboardFiles_createRemoteDataObject(
        presentation, &object) || !object)
  {
    clipboardFiles_remotePresentationRelease(presentation);
    DEBUG_WARN("Failed to create the guest clipboard file object");
    return;
  }

  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire))
  {
    object->lpVtbl->Release(object);
    return;
  }

  const HRESULT status = setOleClipboard(object);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to publish guest clipboard files", status);
    object->lpVtbl->Release(object);
    return;
  }

  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire))
  {
    const HRESULT clearStatus = OleIsCurrentClipboard(object) == S_OK ?
      setOleClipboard(NULL) : S_OK;
    releaseRemoteFilesReference();
    if (FAILED(clearStatus) && OleIsCurrentClipboard(object) == S_OK)
    {
      DEBUG_WINERROR("Failed to withdraw stale guest clipboard files",
          clearStatus);
      windowsCB.remoteFiles = object;
    }
    else
      object->lpVtbl->Release(object);
    return;
  }

  releaseRemoteFilesReference();
  windowsCB.remoteFiles   = object;
  windowsCB.localOwner    = GetClipboardOwner();
  windowsCB.localSequence = GetClipboardSequenceNumber();
  windowsCB.localTypes    = 0;
  clipboardFiles_clearLocal();
  clipboardFiles_remotePresentationDelivered(presentation);
}

static void handleRemoteRelease(
    const struct WindowsClipboardOperation * operation)
{
  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire))
    return;

  if (windowsCB.remoteFiles)
  {
    const bool current = remoteFilesAreCurrent();
    if (!clearRemoteFilesClipboard())
      return;
    if (current)
    {
      windowsCB.localOwner    = GetClipboardOwner();
      windowsCB.localSequence = GetClipboardSequenceNumber();
      windowsCB.localTypes    = 0;
      clipboardFiles_clearLocal();
    }
    return;
  }

  if (GetClipboardOwner() != windowsCB.window || !openClipboard())
    return;

  bool released = false;
  if (operation->generation == atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire) &&
      GetClipboardOwner() == windowsCB.window)
    released = EmptyClipboard();
  CloseClipboard();
  if (released)
  {
    windowsCB.localOwner    = GetClipboardOwner();
    windowsCB.localSequence = GetClipboardSequenceNumber();
    windowsCB.localTypes    = 0;
    clipboardFiles_clearLocal();
  }
}

static bool growTransfer(struct WindowsClipboardTransfer * transfer,
    size_t wanted)
{
  if (wanted <= transfer->capacity)
    return true;

  size_t capacity = transfer->capacity ? transfer->capacity : 4096;
  while (capacity < wanted)
  {
    if (capacity > SIZE_MAX / 2)
    {
      capacity = wanted;
      break;
    }
    capacity *= 2;
  }

  uint8_t * data = realloc(transfer->data, capacity);
  if (!data)
    return false;
  transfer->data     = data;
  transfer->capacity = capacity;
  return true;
}

static void freeTransfer(struct WindowsClipboardTransfer * transfer)
{
  if (!transfer)
    return;

  free(transfer->data);
  free(transfer);
}

static LG_ClipboardResult remoteDataBegin(void * opaque,
    LG_ClipboardData type, uint64_t sizeHint)
{
  struct WindowsClipboardTransfer * transfer   = opaque;
  size_t                            size       = 0;
  if (transfer->started || type != transfer->type ||
      (sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN &&
        !uint64ToSize(sizeHint, &size)))
    return LG_CLIPBOARD_RESULT_FAILED;

  transfer->started = true;
  if (sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN && size &&
      !growTransfer(transfer, size))
    return LG_CLIPBOARD_RESULT_FAILED;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult remoteDataChunk(void * opaque, uint64_t offset,
    const void * data, size_t size)
{
  struct WindowsClipboardTransfer * transfer = opaque;
  if (!transfer->started || offset != transfer->size || !data || !size ||
      size > SIZE_MAX - transfer->size ||
      !growTransfer(transfer, transfer->size + size))
    return LG_CLIPBOARD_RESULT_FAILED;

  memcpy(transfer->data + transfer->size, data, size);
  transfer->size += size;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult remoteDataEnd(void * opaque, uint64_t finalSize)
{
  struct WindowsClipboardTransfer * transfer = opaque;
  if (!transfer->started || finalSize != transfer->size)
    return LG_CLIPBOARD_RESULT_FAILED;

  struct WindowsClipboardOperation * operation =
    newOperation(WINDOWS_CB_OPERATION_DATA);
  if (!operation)
    return LG_CLIPBOARD_RESULT_FAILED;

  operation->generation = transfer->generation;
  operation->dataType   = transfer->type;
  operation->data       = transfer->data;
  operation->size       = transfer->size;
  transfer->data        = NULL;
  if (!queueOperation(operation))
    return LG_CLIPBOARD_RESULT_FAILED;
  freeTransfer(transfer);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void remoteDataCancel(void * opaque,
    LG_ClipboardCancelReason reason)
{
  (void)reason;
  freeTransfer(opaque);
}

static const LG_ClipboardStreamOps remoteStream =
{
  .begin  = remoteDataBegin,
  .chunk  = remoteDataChunk,
  .end    = remoteDataEnd,
  .cancel = remoteDataCancel,
};

static void handleRemoteNotice(
    const struct WindowsClipboardOperation * operation)
{
  if (operation->generation != atomic_load_explicit(
        &windowsCBGeneration, memory_order_acquire))
    return;
  if (operation->dataType == LG_CLIPBOARD_DATA_FILES)
  {
    handleRemoteFiles(operation);
    return;
  }
  if (operation->dataType >= LG_CLIPBOARD_DATA_NONE)
    return;

  struct WindowsClipboardTransfer * transfer =
    calloc(1, sizeof(*transfer));
  if (!transfer)
  {
    DEBUG_ERROR("Out of memory while requesting guest clipboard data");
    return;
  }

  LG_ClipboardRequest request;
  transfer->generation = operation->generation;
  transfer->type       = operation->dataType;
  if (!clipboard_requestStream(
        transfer->type, &remoteStream, transfer, &request))
    freeTransfer(transfer);
}

static void dispatchOperations(void)
{
  struct WindowsClipboardOperation * operation;
  while ((operation = takeOperation()))
  {
    switch (operation->type)
    {
      case WINDOWS_CB_OPERATION_NOTICE:
        handleRemoteNotice(operation);
        break;

      case WINDOWS_CB_OPERATION_RELEASE:
        handleRemoteRelease(operation);
        break;

      case WINDOWS_CB_OPERATION_REQUEST:
        handleLocalRequest(operation);
        break;

      case WINDOWS_CB_OPERATION_DATA:
        handleRemoteData(operation);
        break;
    }

    freeOperation(operation);
  }
}

static bool eventInit(HWND window)
{
  if (!window)
    return false;

  const HRESULT oleStatus = OleInitialize(NULL);
  if (SUCCEEDED(oleStatus))
    windowsCB.oleInitialized = true;
  else
    DEBUG_WINERROR("Failed to initialize OLE for clipboard files",
        oleStatus);

  windowsCB.pngFormat  = RegisterClipboardFormatW(L"PNG");
  windowsCB.jfifFormat = RegisterClipboardFormatW(L"JFIF");
  windowsCB.jpegFormat = RegisterClipboardFormatW(L"JPEG");
  if (!windowsCB.pngFormat || !windowsCB.jfifFormat ||
      !windowsCB.jpegFormat)
  {
    DEBUG_WINERROR("Failed to register Windows clipboard formats",
        GetLastError());
    if (windowsCB.oleInitialized)
    {
      OleUninitialize();
      windowsCB.oleInitialized = false;
    }
    return false;
  }

  if (!AddClipboardFormatListener(window))
  {
    DEBUG_WINERROR("Failed to register the Windows clipboard listener",
        GetLastError());
    if (windowsCB.oleInitialized)
    {
      OleUninitialize();
      windowsCB.oleInitialized = false;
    }
    return false;
  }

  AcquireSRWLockExclusive(&windowsCBQueueLock);
  windowsCB.window    = window;
  windowsCB.listener  = true;
  windowsCB.accepting = true;
  ReleaseSRWLockExclusive(&windowsCBQueueLock);
  atomic_store_explicit(&windowsCBGeneration, 0, memory_order_release);
  updateLocalClipboard();
  return true;
}

bool windowsCBHandleMessage(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam, LRESULT * result)
{
  (void)lParam;

  switch (message)
  {
    case WM_LG_CLIPBOARD_INIT:
      *result = eventInit(window);
      return true;

    case WM_LG_CLIPBOARD_DISPATCH:
      dispatchOperations();
      *result = 0;
      return true;

    case WM_CLIPBOARDUPDATE:
      if (windowsCB.accepting)
        updateLocalClipboard();
      *result = 0;
      return true;

    case WM_TIMER:
      if ((UINT_PTR)wParam != WINDOWS_CB_RETRY_TIMER_ID)
        return false;
      retryLocalClipboard();
      *result = 0;
      return true;
  }

  return false;
}

void windowsCBEventShutdown(void)
{
  HWND                               window;
  bool                               listener;
  struct WindowsClipboardOperation * operations;

  clearLocalRetry();
  AcquireSRWLockExclusive(&windowsCBQueueLock);
  window              = windowsCB.window;
  listener            = windowsCB.listener;
  operations          = windowsCB.queueHead;
  windowsCB.accepting = false;
  windowsCB.listener  = false;
  windowsCB.window    = NULL;
  windowsCB.queueHead = NULL;
  windowsCB.queueTail = NULL;
  ReleaseSRWLockExclusive(&windowsCBQueueLock);

  atomic_fetch_add_explicit(
      &windowsCBGeneration, 1, memory_order_acq_rel);
  if (listener && window && !RemoveClipboardFormatListener(window))
    DEBUG_WINERROR("Failed to remove the Windows clipboard listener",
        GetLastError());
  if (!clearRemoteFilesClipboard())
    releaseRemoteFilesReference();
  if (windowsCB.oleInitialized)
  {
    OleUninitialize();
    windowsCB.oleInitialized = false;
  }
  freeOperations(operations);
}

bool windowsCBInit(void)
{
  const HWND window = windowsGetWindow();
  return window && SendMessageW(
      window, WM_LG_CLIPBOARD_INIT, 0, 0) != 0;
}

void windowsCBNotice(LG_ClipboardData type)
{
  const uint64_t                     generation = atomic_fetch_add_explicit(
      &windowsCBGeneration, 1, memory_order_acq_rel) + 1;
  struct WindowsClipboardOperation * operation  =
    newOperation(WINDOWS_CB_OPERATION_NOTICE);
  if (!operation)
    return;

  operation->generation = generation;
  operation->dataType   = type;
  queueOperation(operation);
}

void windowsCBRelease(void)
{
  const uint64_t                     generation = atomic_fetch_add_explicit(
      &windowsCBGeneration, 1, memory_order_acq_rel) + 1;
  struct WindowsClipboardOperation * operation  =
    newOperation(WINDOWS_CB_OPERATION_RELEASE);
  if (!operation)
    return;

  operation->generation = generation;
  queueOperation(operation);
}

void windowsCBRequest(LG_ClipboardRequest request, LG_ClipboardData type)
{
  struct WindowsClipboardOperation * operation =
    newOperation(WINDOWS_CB_OPERATION_REQUEST);
  if (!operation)
  {
    clipboard_abort(request);
    return;
  }

  operation->request  = request;
  operation->dataType = type;
  if (!queueOperation(operation))
    clipboard_abort(request);
}

void windowsCBRequestReady(LG_ClipboardRequest request)
{
  (void)request;
}

void windowsCBRequestCancel(LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  (void)reason;

  AcquireSRWLockExclusive(&windowsCBQueueLock);
  windowsCB.canceledRequest = request;
  ReleaseSRWLockExclusive(&windowsCBQueueLock);
}
