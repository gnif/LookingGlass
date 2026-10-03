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

#define CINTERFACE
#define COBJMACROS
#define CONST_VTABLE

#include "platform/windows/clipboard_files_win32.h"

#include <LGProtocol/KVMFRClipboard.h>

#include <shlobj.h>
#include <winioctl.h>

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WINDOWS_EPOCH_TICKS UINT64_C(116444736000000000)

typedef struct WindowsClipboardFileIdentity
{
  uint64_t               size;
  uint64_t               createdNs;
  uint64_t               modifiedNs;
  DWORD                  volumeSerial;
  DWORD                  fileIndexHigh;
  DWORD                  fileIndexLow;
  DWORD                  attributes;
  KVMFRClipboardFileType type;
}
WindowsClipboardFileIdentity;

typedef struct WindowsClipboardLocalNode
{
  uint64_t                     id;
  wchar_t                    * name;
  wchar_t                    * path;
  WindowsClipboardFileIdentity identity;
  uint64_t                   * children;
  size_t                       childCount;
  size_t                       childCapacity;
  bool                         childrenLoaded;
}
WindowsClipboardLocalNode;

struct WindowsClipboardLocalFiles
{
  volatile LONG                 refs;
  CRITICAL_SECTION              lock;
  WindowsClipboardLocalNode   * nodes;
  size_t                        nodeCount;
  size_t                        nodeCapacity;
  uint64_t                    * roots;
  size_t                        rootCount;
  size_t                        rootCapacity;
  bool                          published;
};

typedef struct WindowsClipboardRemoteEntry
{
  uint64_t               node;
  uint64_t               parent;
  uint64_t               size;
  uint64_t               createdNs;
  uint64_t               modifiedNs;
  KVMFRClipboardFileType type;
  wchar_t              * name;
  uint8_t                listState;
}
WindowsClipboardRemoteEntry;

struct WindowsClipboardRemoteManifest
{
  volatile LONG                 refs;
  CRITICAL_SECTION              lock;
  WindowsClipboardRemoteEntry * entries;
  size_t                        entryCount;
  size_t                        entryCapacity;
  uint8_t                       rootState;
  bool                          frozen;
};

static void setError(LG_ClipboardFileError * error,
    LG_ClipboardFileError value)
{
  if (error)
    *error = value;
}

static LG_ClipboardFileError fileError(DWORD error)
{
  switch (error)
  {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_HANDLE:
      return LG_CLIPBOARD_FILE_ERROR_NOT_FOUND;

    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return LG_CLIPBOARD_FILE_ERROR_ACCESS;

    case ERROR_DIRECTORY:
      return LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;

    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;

    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return LG_CLIPBOARD_FILE_ERROR_NO_SPACE;

    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_BROKEN_PIPE:
      return LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;

    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED:
      return LG_CLIPBOARD_FILE_ERROR_CANCELLED;

    case ERROR_NOT_SUPPORTED:
      return LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;

    case ERROR_INVALID_DATA:
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
      return LG_CLIPBOARD_FILE_ERROR_INVALID;

    default:
      return LG_CLIPBOARD_FILE_ERROR_IO;
  }
}

static uint64_t fileTimeToUnixNs(FILETIME time)
{
  ULARGE_INTEGER value;

  value.LowPart  = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  if (value.QuadPart <= WINDOWS_EPOCH_TICKS)
    return 0;

  const uint64_t ticks = value.QuadPart - WINDOWS_EPOCH_TICKS;
  return ticks <= UINT64_MAX / UINT64_C(100) ?
    ticks * UINT64_C(100) : UINT64_MAX;
}

static FILETIME unixNsToFileTime(uint64_t ns)
{
  ULARGE_INTEGER value;
  FILETIME       result;
  const uint64_t ticks = ns / UINT64_C(100);

  value.QuadPart        = ticks <= UINT64_MAX - WINDOWS_EPOCH_TICKS ?
    ticks + WINDOWS_EPOCH_TICKS : UINT64_MAX;
  result.dwLowDateTime  = value.LowPart;
  result.dwHighDateTime = value.HighPart;
  return result;
}

static wchar_t * duplicateWide(const wchar_t * value)
{
  if (!value)
    return NULL;

  const size_t length = wcslen(value);
  if (length > (SIZE_MAX / sizeof(*value)) - 1U)
    return NULL;

  wchar_t * result = malloc((length + 1U) * sizeof(*result));
  if (!result)
    return NULL;

  memcpy(result, value, (length + 1U) * sizeof(*result));
  return result;
}

static wchar_t * joinPath(const wchar_t * parent, const wchar_t * child)
{
  const size_t parentLength = wcslen(parent);
  const size_t childLength  = wcslen(child);
  const bool   separator    = parentLength && parent[parentLength - 1U] != L'\\' &&
    parent[parentLength - 1U] != L'/';

  if (parentLength > SIZE_MAX - childLength - (separator ? 2U : 1U) ||
      parentLength + childLength + (separator ? 2U : 1U) >
        SIZE_MAX / sizeof(wchar_t))
    return NULL;

  const size_t length = parentLength + childLength + (separator ? 1U : 0U);
  wchar_t    * result = malloc((length + 1U) * sizeof(*result));
  if (!result)
    return NULL;

  memcpy(result, parent, parentLength * sizeof(*result));
  size_t offset = parentLength;
  if (separator)
    result[offset++] = L'\\';
  memcpy(result + offset, child, (childLength + 1U) * sizeof(*result));
  return result;
}

static bool hasDotComponent(const wchar_t * path, size_t offset)
{
  const size_t length          = wcslen(path);
  while (offset < length)
  {
    size_t end = offset;
    while (end < length && path[end] != L'\\')
      ++end;

    const size_t component = end - offset;
    if ((component == 1U && path[offset] == L'.') ||
        (component == 2U && path[offset] == L'.' &&
          path[offset + 1U] == L'.'))
      return true;

    offset = end + 1U;
  }

  return false;
}

static wchar_t * normalizeSourcePath(const wchar_t * input)
{
  if (!input || !*input)
  {
    SetLastError(ERROR_INVALID_NAME);
    return NULL;
  }

  wchar_t * path = duplicateWide(input);
  if (!path)
  {
    SetLastError(ERROR_OUTOFMEMORY);
    return NULL;
  }

  for (wchar_t * value = path; *value; ++value)
    if (*value == L'/')
      *value = L'\\';

  const size_t length          = wcslen(path);
  const bool   drive           = length >= 3U &&
    ((path[0] >= L'A' && path[0] <= L'Z') ||
     (path[0] >= L'a' && path[0] <= L'z')) &&
    path[1] == L':' && path[2] == L'\\';
  const bool   unc             = length >= 3U && path[0] == L'\\' &&
    path[1] == L'\\' && path[2] != L'?' && path[2] != L'.';
  const bool   extendedDrive   = length >= 7U &&
    wcsncmp(path, L"\\\\?\\", 4U) == 0 &&
    ((path[4] >= L'A' && path[4] <= L'Z') ||
     (path[4] >= L'a' && path[4] <= L'z')) &&
    path[5] == L':' && path[6] == L'\\';
  const bool   extendedUNC     = length >= 8U &&
    wcsncmp(path, L"\\\\?\\", 4U) == 0 &&
    (path[4] == L'U' || path[4] == L'u') &&
    (path[5] == L'N' || path[5] == L'n') &&
    (path[6] == L'C' || path[6] == L'c') && path[7] == L'\\';
  const size_t componentOffset = drive ? 3U : unc ? 2U :
    extendedDrive ? 7U : extendedUNC ? 8U : 0U;

  if (!componentOffset || hasDotComponent(path, componentOffset))
  {
    free(path);
    SetLastError(ERROR_INVALID_NAME);
    return NULL;
  }

  if (extendedDrive || extendedUNC)
  {
    if (extendedUNC)
    {
      path[4] = L'U';
      path[5] = L'N';
      path[6] = L'C';
    }
    return path;
  }

  const size_t prefixLength = unc ? 8U : 4U;
  const size_t skip         = unc ? 2U : 0U;
  if (length - skip > (SIZE_MAX / sizeof(wchar_t)) - prefixLength - 1U)
  {
    free(path);
    SetLastError(ERROR_OUTOFMEMORY);
    return NULL;
  }

  wchar_t * result = malloc((prefixLength + length - skip + 1U) *
    sizeof(*result));
  if (!result)
  {
    free(path);
    SetLastError(ERROR_OUTOFMEMORY);
    return NULL;
  }

  const wchar_t * prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
  memcpy(result, prefix, prefixLength * sizeof(*result));
  memcpy(result + prefixLength, path + skip,
    (length - skip + 1U) * sizeof(*result));
  free(path);
  return result;
}

static wchar_t * baseName(const wchar_t * path)
{
  size_t end = wcslen(path);
  while (end > 1U && (path[end - 1U] == L'\\' || path[end - 1U] == L'/'))
  {
    if (end == 3U && path[1] == L':')
      break;
    --end;
  }

  size_t start = end;
  while (start && path[start - 1U] != L'\\' && path[start - 1U] != L'/')
    --start;

  size_t length = end - start;
  if (length == 2U && path[start + 1U] == L':')
    length = 1U;
  if (length > (SIZE_MAX / sizeof(wchar_t)) - 1U)
    return NULL;

  wchar_t * result = malloc((length + 1U) * sizeof(*result));
  if (!result)
    return NULL;

  memcpy(result, path + start, length * sizeof(*result));
  result[length] = L'\0';
  return result;
}

static bool validComponent(const wchar_t * name)
{
  if (!name)
    return false;

  const size_t length = wcslen(name);
  if (!length || length > 255U || !wcscmp(name, L".") ||
      !wcscmp(name, L"..") || name[length - 1U] == L'.' ||
      name[length - 1U] == L' ')
    return false;

  for (const wchar_t * value = name; *value; ++value)
    if (*value < 32 || *value == L'\\' || *value == L'/' ||
        *value == L'<' || *value == L'>' || *value == L':' ||
        *value == L'"' || *value == L'|' || *value == L'?' ||
        *value == L'*')
      return false;

  const size_t baseLength = wcscspn(name, L".");
  wchar_t      base[5]    = { 0 };
  size_t       count      = 0;
  while (count < baseLength && count < ARRAYSIZE(base) - 1U)
  {
    wchar_t value = name[count];
    if (value >= L'a' && value <= L'z')
      value = (wchar_t)(value - (L'a' - L'A'));
    base[count++] = value;
  }

  if (baseLength == 3U && (!wcscmp(base, L"CON") ||
      !wcscmp(base, L"PRN") ||
      !wcscmp(base, L"AUX") || !wcscmp(base, L"NUL")))
    return false;

  if (baseLength == 4U && (!wcsncmp(base, L"COM", 3U) ||
      !wcsncmp(base, L"LPT", 3U)))
  {
    const wchar_t suffix = base[3];
    if ((suffix >= L'1' && suffix <= L'9') || suffix == L'\u00b9' ||
        suffix == L'\u00b2' || suffix == L'\u00b3')
      return false;
  }

  return true;
}

static bool sameComponent(const wchar_t * first, const wchar_t * second)
{
  const int result = CompareStringOrdinal(first, -1, second, -1, TRUE);
  return result == CSTR_EQUAL || (!result && !_wcsicmp(first, second));
}

static bool captureIdentity(const wchar_t * path,
    WindowsClipboardFileIdentity * identity, LG_ClipboardFileError * error)
{
  const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
    FILE_FLAG_BACKUP_SEMANTICS;
  HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
    OPEN_EXISTING, flags, NULL);
  if (file == INVALID_HANDLE_VALUE)
  {
    setError(error, fileError(GetLastError()));
    return false;
  }

  BY_HANDLE_FILE_INFORMATION information;
  FILE_ATTRIBUTE_TAG_INFO    tag;
  memset(&information, 0, sizeof(information));
  memset(&tag, 0, sizeof(tag));
  const bool  gotInformation = GetFileInformationByHandle(file,
    &information) != FALSE;
  const bool  gotTag         = gotInformation &&
    GetFileInformationByHandleEx(file,
    FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE;
  const DWORD winError       = gotTag ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);

  if (!gotTag)
  {
    setError(error, fileError(winError));
    return false;
  }

  const DWORD attributes = information.dwFileAttributes | tag.FileAttributes;
  if ((attributes & FILE_ATTRIBUTE_DEVICE) ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);
    return false;
  }

  const bool directory =
    (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  identity->size           = directory ? 0 :
    ((uint64_t)information.nFileSizeHigh << 32U) |
      information.nFileSizeLow;
  identity->createdNs      = fileTimeToUnixNs(information.ftCreationTime);
  identity->modifiedNs     = fileTimeToUnixNs(information.ftLastWriteTime);
  identity->volumeSerial   = information.dwVolumeSerialNumber;
  identity->fileIndexHigh  = information.nFileIndexHigh;
  identity->fileIndexLow   = information.nFileIndexLow;
  identity->attributes     = information.dwFileAttributes;
  identity->type           = directory ?
    KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY :
    KVMFR_CLIPBOARD_FILE_TYPE_REGULAR;
  return true;
}

static bool sameIdentity(const WindowsClipboardFileIdentity * expected,
    const BY_HANDLE_FILE_INFORMATION * information)
{
  const bool     directory =
    (information->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const uint64_t size      = directory ? 0 :
    ((uint64_t)information->nFileSizeHigh << 32U) |
      information->nFileSizeLow;

  return !(information->dwFileAttributes &
      (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT)) &&
    directory ==
      (expected->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY) &&
    size == expected->size &&
    fileTimeToUnixNs(information->ftCreationTime) == expected->createdNs &&
    fileTimeToUnixNs(information->ftLastWriteTime) == expected->modifiedNs &&
    information->dwVolumeSerialNumber == expected->volumeSerial &&
    information->nFileIndexHigh == expected->fileIndexHigh &&
    information->nFileIndexLow == expected->fileIndexLow;
}

static bool checkIdentity(const wchar_t * path,
    const WindowsClipboardFileIdentity * expected,
    LG_ClipboardFileError * error)
{
  WindowsClipboardFileIdentity current;
  if (!captureIdentity(path, &current, error))
    return false;

  if (current.size          != expected->size ||
      current.createdNs     != expected->createdNs ||
      current.modifiedNs    != expected->modifiedNs ||
      current.volumeSerial  != expected->volumeSerial ||
      current.fileIndexHigh != expected->fileIndexHigh ||
      current.fileIndexLow  != expected->fileIndexLow ||
      current.attributes    != expected->attributes ||
      current.type          != expected->type)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_STALE);
    return false;
  }

  return true;
}

static void freeLocalNode(WindowsClipboardLocalNode * node)
{
  free(node->name);
  free(node->path);
  free(node->children);
  memset(node, 0, sizeof(*node));
}

static bool reserveLocalNodes(WindowsClipboardLocalFiles * files,
    size_t count)
{
  if (count <= files->nodeCapacity)
    return true;
  if (count > SIZE_MAX / sizeof(*files->nodes))
    return false;

  size_t capacity = files->nodeCapacity ? files->nodeCapacity : 16U;
  while (capacity < count)
  {
    if (capacity > SIZE_MAX / 2U)
    {
      capacity = count;
      break;
    }
    capacity *= 2U;
  }
  if (capacity > SIZE_MAX / sizeof(*files->nodes))
    capacity = count;

  void * nodes = realloc(files->nodes, capacity * sizeof(*files->nodes));
  if (!nodes)
    return false;

  files->nodes        = nodes;
  files->nodeCapacity = capacity;
  return true;
}

static bool reserveIDs(uint64_t ** values, size_t * capacity, size_t count)
{
  if (count <= *capacity)
    return true;
  if (count > SIZE_MAX / sizeof(**values))
    return false;

  size_t next = *capacity ? *capacity : 16U;
  while (next < count)
  {
    if (next > SIZE_MAX / 2U)
    {
      next = count;
      break;
    }
    next *= 2U;
  }
  if (next > SIZE_MAX / sizeof(**values))
    next = count;

  void * result = realloc(*values, next * sizeof(**values));
  if (!result)
    return false;

  *values   = result;
  *capacity = next;
  return true;
}

static bool addLocalRoot(WindowsClipboardLocalFiles * files,
    wchar_t * path, LG_ClipboardFileError * error)
{
  WindowsClipboardLocalNode node;
  memset(&node, 0, sizeof(node));
  node.path = path;
  node.name = baseName(path);
  if (!node.name)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return false;
  }
  if (!validComponent(node.name))
  {
    free(node.name);
    setError(error, LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);
    return false;
  }
  if (!captureIdentity(node.path, &node.identity, error))
  {
    free(node.name);
    return false;
  }

  for (size_t index = 0; index < files->rootCount; ++index)
  {
    const uint64_t root = files->roots[index];
    if (sameComponent(files->nodes[root - 1U].name, node.name))
    {
      free(node.name);
      setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
      return false;
    }
  }

  if (!reserveLocalNodes(files, files->nodeCount + 1U) ||
      !reserveIDs(&files->roots, &files->rootCapacity,
        files->rootCount + 1U))
  {
    free(node.name);
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return false;
  }

  node.id                               = files->nodeCount + 1U;
  files->nodes[files->nodeCount++]      = node;
  files->roots[files->rootCount++]      = node.id;
  return true;
}

static bool refreshLocalRoots(WindowsClipboardLocalFiles * files,
    LG_ClipboardFileError * error)
{
  WindowsClipboardFileIdentity * identities = calloc(files->rootCount,
    sizeof(*identities));
  if (!identities && files->rootCount)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return false;
  }

  for (size_t index = 0; index < files->rootCount; ++index)
  {
    const uint64_t id = files->roots[index];
    if (!captureIdentity(files->nodes[id - 1U].path, &identities[index],
        error))
    {
      free(identities);
      return false;
    }
  }

  for (size_t index = 0; index < files->rootCount; ++index)
  {
    const uint64_t id = files->roots[index];
    files->nodes[id - 1U].identity = identities[index];
  }

  free(identities);
  return true;
}

static void freeLocalNodeArray(WindowsClipboardLocalNode * nodes,
    size_t count)
{
  if (!nodes)
    return;

  for (size_t index = 0; index < count; ++index)
    freeLocalNode(&nodes[index]);
  free(nodes);
}

static bool loadLocalChildren(WindowsClipboardLocalFiles * files,
    size_t parentIndex, LG_ClipboardFileError * error)
{
  if (parentIndex >= files->nodeCount)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NOT_FOUND);
    return false;
  }

  WindowsClipboardLocalNode * parent = &files->nodes[parentIndex];
  if (parent->identity.type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY);
    return false;
  }
  if (parent->childrenLoaded)
    return true;
  if (!checkIdentity(parent->path, &parent->identity, error))
    return false;

  wchar_t * parentPath = duplicateWide(parent->path);
  wchar_t * pattern    = parentPath ? joinPath(parentPath, L"*") : NULL;
  if (!parentPath || !pattern)
  {
    free(parentPath);
    free(pattern);
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return false;
  }

  WIN32_FIND_DATAW found;
  memset(&found, 0, sizeof(found));
  HANDLE search = FindFirstFileExW(pattern, FindExInfoBasic, &found,
    FindExSearchNameMatch, NULL, 0);
  free(pattern);
  if (search == INVALID_HANDLE_VALUE)
  {
    const DWORD winError = GetLastError();
    if (winError == ERROR_FILE_NOT_FOUND &&
        checkIdentity(parentPath, &parent->identity, error))
    {
      parent->childrenLoaded = true;
      free(parentPath);
      return true;
    }

    free(parentPath);
    if (winError != ERROR_FILE_NOT_FOUND)
      setError(error, fileError(winError));
    return false;
  }

  WindowsClipboardLocalNode * children = NULL;
  size_t                      count    = 0;
  size_t                      capacity = 0;
  bool                        success  = true;
  DWORD                       winError = ERROR_SUCCESS;

  do
  {
    if (!wcscmp(found.cFileName, L".") ||
        !wcscmp(found.cFileName, L".."))
      continue;
    if (!validComponent(found.cFileName))
    {
      setError(error, LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);
      success = false;
      break;
    }

    for (size_t index = 0; index < count; ++index)
      if (sameComponent(children[index].name, found.cFileName))
      {
        setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
        success = false;
        break;
      }
    if (!success)
      break;

    if (count == capacity)
    {
      size_t next = capacity ? capacity * 2U : 16U;
      if (next < capacity || next > SIZE_MAX / sizeof(*children))
      {
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        success = false;
        break;
      }
      void * result = realloc(children, next * sizeof(*children));
      if (!result)
      {
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        success = false;
        break;
      }
      children = result;
      memset(children + capacity, 0,
        (next - capacity) * sizeof(*children));
      capacity = next;
    }

    WindowsClipboardLocalNode * child = &children[count];
    child->name = duplicateWide(found.cFileName);
    child->path = child->name ? joinPath(parentPath, child->name) : NULL;
    if (!child->name || !child->path)
    {
      setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
      success = false;
      break;
    }
    if (!captureIdentity(child->path, &child->identity, error))
    {
      success = false;
      break;
    }
    ++count;
  }
  while (FindNextFileW(search, &found));

  if (success)
  {
    winError = GetLastError();
    if (winError != ERROR_NO_MORE_FILES)
    {
      setError(error, fileError(winError));
      success = false;
    }
  }
  FindClose(search);

  if (success && !checkIdentity(parentPath,
      &files->nodes[parentIndex].identity, error))
    success = false;
  free(parentPath);

  if (success && (!reserveLocalNodes(files, files->nodeCount + count) ||
      !reserveIDs(&files->nodes[parentIndex].children,
        &files->nodes[parentIndex].childCapacity, count)))
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    success = false;
  }

  if (!success)
  {
    freeLocalNodeArray(children, count + (count < capacity ? 1U : 0U));
    return false;
  }

  parent = &files->nodes[parentIndex];
  for (size_t index = 0; index < count; ++index)
  {
    WindowsClipboardLocalNode * child = &children[index];
    child->id                                 = files->nodeCount + 1U;
    parent->children[parent->childCount++]    = child->id;
    files->nodes[files->nodeCount++]          = *child;
    memset(child, 0, sizeof(*child));
  }
  parent->childrenLoaded = true;
  freeLocalNodeArray(children, count);
  return true;
}

static bool wideToUTF8(const wchar_t * text, uint8_t ** data,
    size_t * size)
{
  *data = NULL;
  *size = 0;
  if (!text || !*text)
  {
    SetLastError(ERROR_INVALID_DATA);
    return false;
  }

  const size_t length = wcslen(text);
  if (length > INT_MAX)
  {
    SetLastError(ERROR_INVALID_DATA);
    return false;
  }

  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
    text, (int)length, NULL, 0, NULL, NULL);
  if (count <= 0)
    return false;

  uint8_t * output = malloc((size_t)count);
  if (!output)
  {
    SetLastError(ERROR_OUTOFMEMORY);
    return false;
  }

  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int)length,
      (char *)output, count, NULL, NULL) != count)
  {
    free(output);
    return false;
  }

  *data = output;
  *size = (size_t)count;
  return true;
}

WindowsClipboardLocalFiles * windowsClipboardLocalFilesCapture(HDROP drop,
    LG_ClipboardFileError * error)
{
  setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
  if (!drop)
    return NULL;

  WindowsClipboardLocalFiles * files = calloc(1, sizeof(*files));
  if (!files)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    return NULL;
  }
  files->refs = 1;
  if (!InitializeCriticalSectionEx(&files->lock, 0, 0))
  {
    const DWORD winError = GetLastError();
    free(files);
    setError(error, fileError(winError ? winError : ERROR_OUTOFMEMORY));
    return NULL;
  }

  const UINT count = DragQueryFileW(drop, UINT_MAX, NULL, 0);
  if (!count)
  {
    windowsClipboardLocalFilesRelease(files);
    return NULL;
  }

  for (UINT index = 0; index < count; ++index)
  {
    const UINT length = DragQueryFileW(drop, index, NULL, 0);
    if (!length)
    {
      windowsClipboardLocalFilesRelease(files);
      return NULL;
    }

    wchar_t * input = malloc(((size_t)length + 1U) * sizeof(*input));
    if (!input)
    {
      setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
      windowsClipboardLocalFilesRelease(files);
      return NULL;
    }
    if (DragQueryFileW(drop, index, input, length + 1U) != length)
    {
      free(input);
      windowsClipboardLocalFilesRelease(files);
      return NULL;
    }

    wchar_t * path = normalizeSourcePath(input);
    free(input);
    if (!path)
    {
      setError(error, fileError(GetLastError()));
      windowsClipboardLocalFilesRelease(files);
      return NULL;
    }
    if (!addLocalRoot(files, path, error))
    {
      free(path);
      windowsClipboardLocalFilesRelease(files);
      return NULL;
    }
  }

  setError(error, LG_CLIPBOARD_FILE_ERROR_NONE);
  return files;
}

void windowsClipboardLocalFilesRetain(WindowsClipboardLocalFiles * files)
{
  if (files)
    InterlockedIncrement(&files->refs);
}

void windowsClipboardLocalFilesRelease(WindowsClipboardLocalFiles * files)
{
  if (!files || InterlockedDecrement(&files->refs))
    return;

  for (size_t index = 0; index < files->nodeCount; ++index)
    freeLocalNode(&files->nodes[index]);
  free(files->nodes);
  free(files->roots);
  DeleteCriticalSection(&files->lock);
  free(files);
}

bool windowsClipboardLocalFilesList(WindowsClipboardLocalFiles * files,
    uint64_t node, void ** data, size_t * size,
    LG_ClipboardFileError * error)
{
  if (!data || !size)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
    return false;
  }
  *data = NULL;
  *size = 0;
  setError(error, LG_CLIPBOARD_FILE_ERROR_NONE);
  if (!files)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
    return false;
  }

  EnterCriticalSection(&files->lock);
  const uint64_t * children   = NULL;
  size_t           childCount = 0;
  bool             success    = true;
  if (node == KVMFR_CLIPBOARD_FILE_ROOT_NODE)
  {
    if (!files->published && !refreshLocalRoots(files, error))
      success = false;
    if (success)
    {
      files->published = true;
      children         = files->roots;
      childCount       = files->rootCount;
    }
  }
  else if (!files->published || node > files->nodeCount)
  {
    setError(error, !files->published ? LG_CLIPBOARD_FILE_ERROR_INVALID :
      LG_CLIPBOARD_FILE_ERROR_NOT_FOUND);
    success = false;
  }
  else
  {
    const size_t index = (size_t)(node - 1U);
    if (!loadLocalChildren(files, index, error))
      success = false;
    else
    {
      children   = files->nodes[index].children;
      childCount = files->nodes[index].childCount;
    }
  }

  uint8_t * output   = NULL;
  size_t    used     = 0;
  size_t    capacity = 0;
  for (size_t index = 0; success && index < childCount; ++index)
  {
    const uint64_t id = children[index];
    if (!id || id > files->nodeCount)
    {
      setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
      success = false;
      break;
    }

    const WindowsClipboardLocalNode * child    = &files->nodes[id - 1U];
    uint8_t                         * name     = NULL;
    size_t                            nameSize = 0;
    if (!wideToUTF8(child->name, &name, &nameSize))
    {
      setError(error, fileError(GetLastError()));
      success = false;
      break;
    }

    const size_t recordSize = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(nameSize);
    if (recordSize < sizeof(KVMFRClipboardFileEntry) ||
        used > SIZE_MAX - recordSize)
    {
      free(name);
      setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
      success = false;
      break;
    }
    if (used + recordSize > capacity)
    {
      size_t next = capacity ? capacity : 4096U;
      while (next < used + recordSize)
      {
        if (next > SIZE_MAX / 2U)
        {
          next = used + recordSize;
          break;
        }
        next *= 2U;
      }
      uint8_t * result = realloc(output, next);
      if (!result)
      {
        free(name);
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        success = false;
        break;
      }
      output   = result;
      capacity = next;
    }

    KVMFRClipboardFileEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.node       = child->id;
    entry.size       = child->identity.size;
    entry.createdNs  = child->identity.createdNs;
    entry.modifiedNs = child->identity.modifiedNs;
    entry.type       = child->identity.type;
    entry.nameLength = (uint32_t)nameSize;
    memset(output + used, 0, recordSize);
    memcpy(output + used, &entry, sizeof(entry));
    memcpy(output + used + sizeof(entry), name, nameSize);
    used += recordSize;
    free(name);
  }
  LeaveCriticalSection(&files->lock);

  if (!success)
  {
    free(output);
    return false;
  }

  *data = output;
  *size = used;
  return true;
}

bool windowsClipboardLocalFilesRead(WindowsClipboardLocalFiles * files,
    uint64_t node, uint64_t offset, uint32_t length, void ** data,
    size_t * size, LG_ClipboardFileError * error)
{
  if (!data || !size)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
    return false;
  }
  *data = NULL;
  *size = 0;
  setError(error, LG_CLIPBOARD_FILE_ERROR_NONE);
  if (!files || length > KVMFR_CLIPBOARD_FILE_READ_BYTES ||
      offset > INT64_MAX || offset > UINT64_MAX - length)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
    return false;
  }

  wchar_t                    * path = NULL;
  WindowsClipboardFileIdentity identity;
  memset(&identity, 0, sizeof(identity));
  EnterCriticalSection(&files->lock);
  if (!files->published || !node || node > files->nodeCount)
    setError(error, LG_CLIPBOARD_FILE_ERROR_NOT_FOUND);
  else
  {
    const WindowsClipboardLocalNode * source = &files->nodes[node - 1U];
    if (source->identity.type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
      setError(error, LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY);
    else
    {
      path     = duplicateWide(source->path);
      identity = source->identity;
      if (!path)
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    }
  }
  LeaveCriticalSection(&files->lock);
  if (!path)
    return false;

  HANDLE file = CreateFileW(path, GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS |
      FILE_FLAG_OPEN_REPARSE_POINT,
    NULL);
  free(path);
  if (file == INVALID_HANDLE_VALUE)
  {
    setError(error, fileError(GetLastError()));
    return false;
  }

  FILE_STANDARD_INFO          standard;
  FILE_ATTRIBUTE_TAG_INFO     tag;
  BY_HANDLE_FILE_INFORMATION  before;
  memset(&standard, 0, sizeof(standard));
  memset(&tag, 0, sizeof(tag));
  memset(&before, 0, sizeof(before));
  bool  success  = GetFileInformationByHandleEx(file, FileStandardInfo,
      &standard, sizeof(standard)) &&
    GetFileInformationByHandleEx(file, FileAttributeTagInfo,
      &tag, sizeof(tag)) &&
    GetFileInformationByHandle(file, &before);
  DWORD winError = success ? ERROR_SUCCESS : GetLastError();
  if (success && (standard.Directory ||
      (tag.FileAttributes &
        (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT))))
  {
    setError(error, standard.Directory ?
      LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY :
      LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED);
    success = false;
  }
  if (success && (!sameIdentity(&identity, &before) ||
      standard.EndOfFile.QuadPart < 0 ||
      (uint64_t)standard.EndOfFile.QuadPart != identity.size))
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_STALE);
    success = false;
  }
  if (success && (offset > identity.size ||
      length > identity.size - offset))
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
    success = false;
  }

  const uint32_t wanted = success ? length : 0;
  uint8_t      * output = wanted ? malloc(wanted) : NULL;
  if (success && wanted && !output)
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    success = false;
  }

  LARGE_INTEGER position;
  DWORD         read = 0;
  position.QuadPart = (LONGLONG)offset;
  if (success && (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) ||
      (wanted && !ReadFile(file, output, wanted, &read, NULL))))
  {
    winError = GetLastError();
    success  = false;
  }

  BY_HANDLE_FILE_INFORMATION after;
  memset(&after, 0, sizeof(after));
  if (success && !GetFileInformationByHandle(file, &after))
  {
    winError = GetLastError();
    success  = false;
  }
  if (success && !sameIdentity(&identity, &after))
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_STALE);
    success = false;
  }
  CloseHandle(file);

  if (!success || read != wanted)
  {
    free(output);
    if (error && *error == LG_CLIPBOARD_FILE_ERROR_NONE)
      *error = fileError(winError ? winError : ERROR_READ_FAULT);
    return false;
  }

  *data = output;
  *size = wanted;
  return true;
}

static wchar_t * utf8ToWide(const uint8_t * data, size_t size)
{
  if (!data || !size || size > INT_MAX)
  {
    SetLastError(ERROR_INVALID_DATA);
    return NULL;
  }

  for (size_t index = 0; index < size; ++index)
    if (!data[index])
    {
      SetLastError(ERROR_INVALID_DATA);
      return NULL;
    }

  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
    (const char *)data, (int)size, NULL, 0);
  if (count <= 0)
    return NULL;
  wchar_t * output = malloc(((size_t)count + 1U) * sizeof(*output));
  if (!output)
  {
    SetLastError(ERROR_OUTOFMEMORY);
    return NULL;
  }

  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      (const char *)data, (int)size, output, count) != count)
  {
    free(output);
    return NULL;
  }
  output[count] = L'\0';
  return output;
}

static void freeRemoteEntry(WindowsClipboardRemoteEntry * entry)
{
  free(entry->name);
  memset(entry, 0, sizeof(*entry));
}

static void remoteManifestRetain(WindowsClipboardRemoteManifest * manifest)
{
  InterlockedIncrement(&manifest->refs);
}

static void remoteManifestRelease(WindowsClipboardRemoteManifest * manifest)
{
  if (InterlockedDecrement(&manifest->refs))
    return;

  for (size_t index = 0; index < manifest->entryCount; ++index)
    freeRemoteEntry(&manifest->entries[index]);
  free(manifest->entries);
  DeleteCriticalSection(&manifest->lock);
  free(manifest);
}

static bool reserveRemoteEntries(WindowsClipboardRemoteManifest * manifest,
    size_t count)
{
  if (count <= manifest->entryCapacity)
    return true;
  if (count > SIZE_MAX / sizeof(*manifest->entries))
    return false;

  size_t capacity = manifest->entryCapacity ? manifest->entryCapacity : 16U;
  while (capacity < count)
  {
    if (capacity > SIZE_MAX / 2U)
    {
      capacity = count;
      break;
    }
    capacity *= 2U;
  }
  if (capacity > SIZE_MAX / sizeof(*manifest->entries))
    capacity = count;

  void * entries = realloc(manifest->entries,
    capacity * sizeof(*manifest->entries));
  if (!entries)
    return false;

  manifest->entries       = entries;
  manifest->entryCapacity = capacity;
  return true;
}

static ptrdiff_t findRemoteEntry(
    const WindowsClipboardRemoteManifest * manifest, uint64_t node)
{
  for (size_t index = 0; index < manifest->entryCount; ++index)
    if (manifest->entries[index].node == node)
      return (ptrdiff_t)index;
  return -1;
}

WindowsClipboardRemoteManifest * windowsClipboardRemoteManifestCreate(void)
{
  WindowsClipboardRemoteManifest * manifest = calloc(1, sizeof(*manifest));
  if (!manifest)
    return NULL;

  manifest->refs = 1;
  if (!InitializeCriticalSectionEx(&manifest->lock, 0, 0))
  {
    free(manifest);
    return NULL;
  }
  return manifest;
}

void windowsClipboardRemoteManifestFree(
    WindowsClipboardRemoteManifest * manifest)
{
  if (manifest)
    remoteManifestRelease(manifest);
}

bool windowsClipboardRemoteManifestAppendList(
    WindowsClipboardRemoteManifest * manifest, uint64_t parent,
    const void * data, size_t size, LG_ClipboardFileError * error)
{
  setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);
  if (!manifest || (!data && size))
    return false;

  EnterCriticalSection(&manifest->lock);
  if (manifest->frozen)
  {
    LeaveCriticalSection(&manifest->lock);
    return false;
  }

  ptrdiff_t parentIndex = -1;
  uint8_t * parentState = &manifest->rootState;
  if (parent)
  {
    parentIndex = findRemoteEntry(manifest, parent);
    if (parentIndex < 0 ||
        manifest->entries[parentIndex].type !=
          KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY)
    {
      LeaveCriticalSection(&manifest->lock);
      return false;
    }
    parentState = &manifest->entries[parentIndex].listState;
  }
  if (*parentState != 1U)
  {
    LeaveCriticalSection(&manifest->lock);
    return false;
  }

  WindowsClipboardRemoteEntry * pending  = NULL;
  size_t                        count    = 0;
  size_t                        capacity = 0;
  size_t                        offset   = 0;
  bool                          success  = true;
  while (offset < size)
  {
    if (size - offset < sizeof(KVMFRClipboardFileEntry))
    {
      success = false;
      break;
    }

    KVMFRClipboardFileEntry wire;
    memcpy(&wire, (const uint8_t *)data + offset, sizeof(wire));
    const size_t bytes = KVMFR_CLIPBOARD_FILE_ENTRY_BYTES(wire.nameLength);
    if (!wire.node || !wire.nameLength ||
        (wire.type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR &&
         wire.type != KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY) ||
        bytes < sizeof(wire) || bytes > size - offset ||
        wire.nameLength > bytes - sizeof(wire))
    {
      success = false;
      break;
    }

    wchar_t * name = utf8ToWide((const uint8_t *)data + offset +
      sizeof(wire), wire.nameLength);
    if (!name || !validComponent(name))
    {
      if (!name && GetLastError() == ERROR_OUTOFMEMORY)
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
      free(name);
      success = false;
      break;
    }

    for (size_t pad = sizeof(wire) + wire.nameLength; pad < bytes; ++pad)
      if (((const uint8_t *)data)[offset + pad])
      {
        success = false;
        break;
      }
    if (!success)
    {
      free(name);
      break;
    }

    if (findRemoteEntry(manifest, wire.node) >= 0)
      success = false;
    for (size_t index = 0; success && index < count; ++index)
      if (pending[index].node == wire.node ||
          sameComponent(pending[index].name, name))
        success = false;
    for (size_t index = 0; success && index < manifest->entryCount; ++index)
      if (manifest->entries[index].parent == parent &&
          sameComponent(manifest->entries[index].name, name))
        success = false;
    if (!success)
    {
      free(name);
      break;
    }

    if (count == capacity)
    {
      size_t next = capacity ? capacity * 2U : 16U;
      if (next < capacity || next > SIZE_MAX / sizeof(*pending))
      {
        free(name);
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        success = false;
        break;
      }
      void * result = realloc(pending, next * sizeof(*pending));
      if (!result)
      {
        free(name);
        setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        success = false;
        break;
      }
      pending = result;
      memset(pending + capacity, 0,
        (next - capacity) * sizeof(*pending));
      capacity = next;
    }

    WindowsClipboardRemoteEntry * entry = &pending[count++];
    entry->node       = wire.node;
    entry->parent     = parent;
    entry->size       = wire.type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR ?
      wire.size : 0;
    entry->createdNs  = wire.createdNs;
    entry->modifiedNs = wire.modifiedNs;
    entry->type       = wire.type;
    entry->name       = name;
    offset += bytes;
  }

  if (success && offset != size)
    success = false;
  if (success && (manifest->entryCount > SIZE_MAX - count ||
      !reserveRemoteEntries(manifest, manifest->entryCount + count)))
  {
    setError(error, LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
    success = false;
  }

  if (success)
  {
    for (size_t index = 0; index < count; ++index)
    {
      manifest->entries[manifest->entryCount++] = pending[index];
      memset(&pending[index], 0, sizeof(pending[index]));
    }
    if (parentIndex < 0)
      manifest->rootState = 2U;
    else
      manifest->entries[parentIndex].listState = 2U;
    setError(error, LG_CLIPBOARD_FILE_ERROR_NONE);
  }
  else if (!error || *error == LG_CLIPBOARD_FILE_ERROR_INVALID)
    setError(error, LG_CLIPBOARD_FILE_ERROR_INVALID);

  for (size_t index = 0; index < count; ++index)
    freeRemoteEntry(&pending[index]);
  free(pending);
  LeaveCriticalSection(&manifest->lock);
  return success;
}

bool windowsClipboardRemoteManifestNextDirectory(
    WindowsClipboardRemoteManifest * manifest, uint64_t * node)
{
  if (!manifest || !node)
    return false;

  EnterCriticalSection(&manifest->lock);
  bool found = false;
  if (!manifest->frozen && manifest->rootState == 0U)
  {
    manifest->rootState = 1U;
    *node               = KVMFR_CLIPBOARD_FILE_ROOT_NODE;
    found               = true;
  }
  else if (!manifest->frozen)
    for (size_t index = 0; index < manifest->entryCount; ++index)
    {
      WindowsClipboardRemoteEntry * entry = &manifest->entries[index];
      if (entry->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY &&
          entry->listState == 0U)
      {
        entry->listState = 1U;
        *node            = entry->node;
        found            = true;
        break;
      }
    }
  LeaveCriticalSection(&manifest->lock);
  return found;
}

bool windowsClipboardRemoteManifestHasEntries(
    const WindowsClipboardRemoteManifest * manifest)
{
  if (!manifest)
    return false;

  WindowsClipboardRemoteManifest * mutableManifest =
    (WindowsClipboardRemoteManifest *)manifest;
  EnterCriticalSection(&mutableManifest->lock);
  const bool result = mutableManifest->entryCount != 0;
  LeaveCriticalSection(&mutableManifest->lock);
  return result;
}

typedef struct WindowsClipboardFileDataObject
{
  IDataObject                            iface;
  volatile LONG                          refs;
  WindowsClipboardRemoteManifest       * manifest;
  uint64_t                               dataset;
  uint64_t                               acquisition;
  WindowsClipboardRemoteProviderOps      ops;
  void                                 * opaque;
  FILEDESCRIPTORW                      * descriptors;
  size_t                                 descriptorCount;
  UINT                                   fileDescriptorFormat;
  UINT                                   fileContentsFormat;
  UINT                                   preferredDropEffectFormat;
}
WindowsClipboardFileDataObject;

typedef struct WindowsClipboardFileStream
{
  IStream                          iface;
  volatile LONG                    refs;
  CRITICAL_SECTION                 lock;
  WindowsClipboardFileDataObject * owner;
  size_t                           entryIndex;
  uint64_t                         offset;
}
WindowsClipboardFileStream;

typedef struct WindowsClipboardFormatEnumerator
{
  IEnumFORMATETC iface;
  volatile LONG  refs;
  FORMATETC      formats[3];
  ULONG          count;
  ULONG          index;
}
WindowsClipboardFormatEnumerator;

static WindowsClipboardFileDataObject * dataObjectFromInterface(
    IDataObject * iface)
{
  return CONTAINING_RECORD(iface, WindowsClipboardFileDataObject, iface);
}

static WindowsClipboardFileStream * streamFromInterface(IStream * iface)
{
  return CONTAINING_RECORD(iface, WindowsClipboardFileStream, iface);
}

static WindowsClipboardFormatEnumerator * formatEnumeratorFromInterface(
    IEnumFORMATETC * iface)
{
  return CONTAINING_RECORD(iface, WindowsClipboardFormatEnumerator, iface);
}

static HRESULT STDMETHODCALLTYPE dataObjectQueryInterface(IDataObject * self,
    REFIID iid, void ** object);
static ULONG STDMETHODCALLTYPE dataObjectAddRef(IDataObject * self);
static ULONG STDMETHODCALLTYPE dataObjectRelease(IDataObject * self);
static HRESULT STDMETHODCALLTYPE dataObjectGetData(IDataObject * self,
    FORMATETC * format, STGMEDIUM * medium);
static HRESULT STDMETHODCALLTYPE dataObjectGetDataHere(IDataObject * self,
    FORMATETC * format, STGMEDIUM * medium);
static HRESULT STDMETHODCALLTYPE dataObjectQueryGetData(IDataObject * self,
    FORMATETC * format);
static HRESULT STDMETHODCALLTYPE dataObjectGetCanonicalFormatEtc(
    IDataObject * self, FORMATETC * input, FORMATETC * output);
static HRESULT STDMETHODCALLTYPE dataObjectSetData(IDataObject * self,
    FORMATETC * format, STGMEDIUM * medium, BOOL release);
static HRESULT STDMETHODCALLTYPE dataObjectEnumFormatEtc(IDataObject * self,
    DWORD direction, IEnumFORMATETC ** enumerator);
static HRESULT STDMETHODCALLTYPE dataObjectDAdvise(IDataObject * self,
    FORMATETC * format, DWORD flags, IAdviseSink * sink,
    DWORD * connection);
static HRESULT STDMETHODCALLTYPE dataObjectDUnadvise(IDataObject * self,
    DWORD connection);
static HRESULT STDMETHODCALLTYPE dataObjectEnumDAdvise(IDataObject * self,
    IEnumSTATDATA ** enumerator);

static const IDataObjectVtbl dataObjectVTable =
{
  .QueryInterface         = dataObjectQueryInterface,
  .AddRef                 = dataObjectAddRef,
  .Release                = dataObjectRelease,
  .GetData                = dataObjectGetData,
  .GetDataHere            = dataObjectGetDataHere,
  .QueryGetData           = dataObjectQueryGetData,
  .GetCanonicalFormatEtc  = dataObjectGetCanonicalFormatEtc,
  .SetData                = dataObjectSetData,
  .EnumFormatEtc          = dataObjectEnumFormatEtc,
  .DAdvise                = dataObjectDAdvise,
  .DUnadvise              = dataObjectDUnadvise,
  .EnumDAdvise            = dataObjectEnumDAdvise,
};

static HRESULT STDMETHODCALLTYPE streamQueryInterface(IStream * self,
    REFIID iid, void ** object);
static ULONG STDMETHODCALLTYPE streamAddRef(IStream * self);
static ULONG STDMETHODCALLTYPE streamRelease(IStream * self);
static HRESULT STDMETHODCALLTYPE streamRead(IStream * self, void * data,
    ULONG length, ULONG * read);
static HRESULT STDMETHODCALLTYPE streamWrite(IStream * self,
    const void * data, ULONG length, ULONG * written);
static HRESULT STDMETHODCALLTYPE streamSeek(IStream * self,
    LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER * position);
static HRESULT STDMETHODCALLTYPE streamSetSize(IStream * self,
    ULARGE_INTEGER size);
static HRESULT STDMETHODCALLTYPE streamCopyTo(IStream * self,
    IStream * target, ULARGE_INTEGER count, ULARGE_INTEGER * read,
    ULARGE_INTEGER * written);
static HRESULT STDMETHODCALLTYPE streamCommit(IStream * self, DWORD flags);
static HRESULT STDMETHODCALLTYPE streamRevert(IStream * self);
static HRESULT STDMETHODCALLTYPE streamLockRegion(IStream * self,
    ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type);
static HRESULT STDMETHODCALLTYPE streamUnlockRegion(IStream * self,
    ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type);
static HRESULT STDMETHODCALLTYPE streamStat(IStream * self, STATSTG * stat,
    DWORD flags);
static HRESULT STDMETHODCALLTYPE streamClone(IStream * self,
    IStream ** clone);

static const IStreamVtbl streamVTable =
{
  .QueryInterface = streamQueryInterface,
  .AddRef         = streamAddRef,
  .Release        = streamRelease,
  .Read           = streamRead,
  .Write          = streamWrite,
  .Seek           = streamSeek,
  .SetSize        = streamSetSize,
  .CopyTo         = streamCopyTo,
  .Commit         = streamCommit,
  .Revert         = streamRevert,
  .LockRegion     = streamLockRegion,
  .UnlockRegion   = streamUnlockRegion,
  .Stat           = streamStat,
  .Clone          = streamClone,
};

static HRESULT STDMETHODCALLTYPE formatEnumeratorQueryInterface(
    IEnumFORMATETC * self, REFIID iid, void ** object);
static ULONG STDMETHODCALLTYPE formatEnumeratorAddRef(IEnumFORMATETC * self);
static ULONG STDMETHODCALLTYPE formatEnumeratorRelease(IEnumFORMATETC * self);
static HRESULT STDMETHODCALLTYPE formatEnumeratorNext(IEnumFORMATETC * self,
    ULONG count, FORMATETC * formats, ULONG * fetched);
static HRESULT STDMETHODCALLTYPE formatEnumeratorSkip(IEnumFORMATETC * self,
    ULONG count);
static HRESULT STDMETHODCALLTYPE formatEnumeratorReset(IEnumFORMATETC * self);
static HRESULT STDMETHODCALLTYPE formatEnumeratorClone(IEnumFORMATETC * self,
    IEnumFORMATETC ** clone);

static const IEnumFORMATETCVtbl formatEnumeratorVTable =
{
  .QueryInterface = formatEnumeratorQueryInterface,
  .AddRef         = formatEnumeratorAddRef,
  .Release        = formatEnumeratorRelease,
  .Next           = formatEnumeratorNext,
  .Skip           = formatEnumeratorSkip,
  .Reset          = formatEnumeratorReset,
  .Clone          = formatEnumeratorClone,
};

static HRESULT STDMETHODCALLTYPE dataObjectQueryInterface(IDataObject * self,
    REFIID iid, void ** object)
{
  if (!object)
    return E_POINTER;
  *object = NULL;
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject))
  {
    *object = self;
    IDataObject_AddRef(self);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dataObjectAddRef(IDataObject * self)
{
  WindowsClipboardFileDataObject * object = dataObjectFromInterface(self);
  return (ULONG)InterlockedIncrement(&object->refs);
}

static ULONG STDMETHODCALLTYPE dataObjectRelease(IDataObject * self)
{
  WindowsClipboardFileDataObject * object = dataObjectFromInterface(self);
  const LONG                       refs   =
    InterlockedDecrement(&object->refs);
  if (refs)
    return (ULONG)refs;

  object->ops.release(object->opaque, object->dataset, object->acquisition);
  remoteManifestRelease(object->manifest);
  free(object->descriptors);
  free(object);
  return 0;
}

static bool dataObjectFormatMatches(const FORMATETC * format,
    UINT clipboardFormat, DWORD medium)
{
  return format && format->cfFormat == clipboardFormat &&
    format->dwAspect == DVASPECT_CONTENT && format->lindex >= -1 &&
    (format->tymed & medium) != 0;
}

static HRESULT createFileStream(WindowsClipboardFileDataObject * owner,
    size_t entryIndex, uint64_t offset, IStream ** result)
{
  *result = NULL;
  WindowsClipboardFileStream * stream = calloc(1, sizeof(*stream));
  if (!stream)
    return E_OUTOFMEMORY;
  if (!InitializeCriticalSectionEx(&stream->lock, 0, 0))
  {
    const DWORD winError = GetLastError();
    free(stream);
    return HRESULT_FROM_WIN32(winError ? winError : ERROR_OUTOFMEMORY);
  }

  stream->iface.lpVtbl = &streamVTable;
  stream->refs         = 1;
  stream->owner        = owner;
  stream->entryIndex   = entryIndex;
  stream->offset       = offset;
  IDataObject_AddRef(&owner->iface);
  *result = &stream->iface;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dataObjectGetData(IDataObject * self,
    FORMATETC * format, STGMEDIUM * medium)
{
  if (!format || !medium)
    return E_POINTER;
  memset(medium, 0, sizeof(*medium));

  WindowsClipboardFileDataObject * object = dataObjectFromInterface(self);
  if (dataObjectFormatMatches(format, object->fileDescriptorFormat,
      TYMED_HGLOBAL) && format->lindex == -1)
  {
    if (object->descriptorCount > UINT_MAX ||
        object->descriptorCount >
          (SIZE_MAX - offsetof(FILEGROUPDESCRIPTORW, fgd)) /
            sizeof(*object->descriptors))
      return E_OUTOFMEMORY;

    const SIZE_T bytes  = offsetof(FILEGROUPDESCRIPTORW, fgd) +
      object->descriptorCount * sizeof(*object->descriptors);
    HGLOBAL      memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory)
      return E_OUTOFMEMORY;

    FILEGROUPDESCRIPTORW * group = GlobalLock(memory);
    if (!group)
    {
      GlobalFree(memory);
      return E_OUTOFMEMORY;
    }
    group->cItems = (UINT)object->descriptorCount;
    memcpy(group->fgd, object->descriptors,
      object->descriptorCount * sizeof(*object->descriptors));
    GlobalUnlock(memory);
    medium->tymed   = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    return S_OK;
  }

  if (dataObjectFormatMatches(format, object->fileContentsFormat,
      TYMED_ISTREAM))
  {
    if (format->lindex < 0 ||
        (size_t)format->lindex >= object->manifest->entryCount)
      return DV_E_LINDEX;
    const WindowsClipboardRemoteEntry * entry =
      &object->manifest->entries[(size_t)format->lindex];
    if (entry->type != KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
      return DV_E_LINDEX;

    IStream       * stream = NULL;
    const HRESULT   result = createFileStream(object,
      (size_t)format->lindex, 0, &stream);
    if (FAILED(result))
      return result;
    medium->tymed = TYMED_ISTREAM;
    medium->pstm  = stream;
    return S_OK;
  }

  if (dataObjectFormatMatches(format, object->preferredDropEffectFormat,
      TYMED_HGLOBAL) && format->lindex == -1)
  {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!memory)
      return E_OUTOFMEMORY;
    DWORD * effect = GlobalLock(memory);
    if (!effect)
    {
      GlobalFree(memory);
      return E_OUTOFMEMORY;
    }
    *effect = DROPEFFECT_COPY;
    GlobalUnlock(memory);
    medium->tymed   = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    return S_OK;
  }

  return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE dataObjectGetDataHere(IDataObject * self,
    FORMATETC * format, STGMEDIUM * medium)
{
  (void)self;
  (void)format;
  (void)medium;
  return DATA_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE dataObjectQueryGetData(IDataObject * self,
    FORMATETC * format)
{
  if (!format)
    return E_POINTER;
  WindowsClipboardFileDataObject * object = dataObjectFromInterface(self);
  if (dataObjectFormatMatches(format, object->fileDescriptorFormat,
      TYMED_HGLOBAL) && format->lindex == -1)
    return S_OK;
  if (dataObjectFormatMatches(format, object->preferredDropEffectFormat,
      TYMED_HGLOBAL) && format->lindex == -1)
    return S_OK;
  if (dataObjectFormatMatches(format, object->fileContentsFormat,
      TYMED_ISTREAM))
  {
    if (format->lindex == -1)
      for (size_t index = 0; index < object->manifest->entryCount; ++index)
        if (object->manifest->entries[index].type ==
            KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
          return S_OK;
    if (format->lindex >= 0 &&
        (size_t)format->lindex < object->manifest->entryCount &&
        object->manifest->entries[(size_t)format->lindex].type ==
          KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
      return S_OK;
  }
  return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE dataObjectGetCanonicalFormatEtc(
    IDataObject * self, FORMATETC * input, FORMATETC * output)
{
  (void)self;
  if (!input || !output)
    return E_POINTER;
  *output     = *input;
  output->ptd = NULL;
  return DATA_S_SAMEFORMATETC;
}

static HRESULT STDMETHODCALLTYPE dataObjectSetData(IDataObject * self,
    FORMATETC * format, STGMEDIUM * medium, BOOL release)
{
  (void)self;
  (void)format;
  (void)medium;
  (void)release;
  return E_NOTIMPL;
}

static HRESULT createFormatEnumerator(
    const WindowsClipboardFileDataObject * object, ULONG index,
    IEnumFORMATETC ** result)
{
  *result = NULL;
  WindowsClipboardFormatEnumerator * enumerator =
    calloc(1, sizeof(*enumerator));
  if (!enumerator)
    return E_OUTOFMEMORY;

  enumerator->iface.lpVtbl           = &formatEnumeratorVTable;
  enumerator->refs                   = 1;
  enumerator->formats[0].cfFormat    =
    (CLIPFORMAT)object->fileDescriptorFormat;
  enumerator->formats[0].dwAspect    = DVASPECT_CONTENT;
  enumerator->formats[0].lindex      = -1;
  enumerator->formats[0].tymed       = TYMED_HGLOBAL;

  bool hasRegular = false;
  for (size_t entry = 0; entry < object->manifest->entryCount; ++entry)
    if (object->manifest->entries[entry].type ==
        KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
    {
      hasRegular = true;
      break;
    }

  ULONG format = 1;
  if (hasRegular)
  {
    enumerator->formats[format].cfFormat =
      (CLIPFORMAT)object->fileContentsFormat;
    enumerator->formats[format].dwAspect = DVASPECT_CONTENT;
    enumerator->formats[format].lindex   = -1;
    enumerator->formats[format].tymed    = TYMED_ISTREAM;
    ++format;
  }
  enumerator->formats[format].cfFormat =
    (CLIPFORMAT)object->preferredDropEffectFormat;
  enumerator->formats[format].dwAspect = DVASPECT_CONTENT;
  enumerator->formats[format].lindex   = -1;
  enumerator->formats[format].tymed    = TYMED_HGLOBAL;

  enumerator->count = format + 1U;
  enumerator->index = index > enumerator->count ?
    enumerator->count : index;
  *result           = &enumerator->iface;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dataObjectEnumFormatEtc(IDataObject * self,
    DWORD direction, IEnumFORMATETC ** enumerator)
{
  if (!enumerator)
    return E_POINTER;
  *enumerator = NULL;
  if (direction != DATADIR_GET)
    return E_NOTIMPL;
  return createFormatEnumerator(dataObjectFromInterface(self), 0,
    enumerator);
}

static HRESULT STDMETHODCALLTYPE dataObjectDAdvise(IDataObject * self,
    FORMATETC * format, DWORD flags, IAdviseSink * sink,
    DWORD * connection)
{
  (void)self;
  (void)format;
  (void)flags;
  (void)sink;
  (void)connection;
  return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE streamQueryInterface(IStream * self,
    REFIID iid, void ** object)
{
  if (!object)
    return E_POINTER;
  *object = NULL;
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_ISequentialStream) ||
      IsEqualIID(iid, &IID_IStream))
  {
    *object = self;
    IStream_AddRef(self);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE streamAddRef(IStream * self)
{
  WindowsClipboardFileStream * stream = streamFromInterface(self);
  return (ULONG)InterlockedIncrement(&stream->refs);
}

static ULONG STDMETHODCALLTYPE streamRelease(IStream * self)
{
  WindowsClipboardFileStream * stream = streamFromInterface(self);
  const LONG                   refs   = InterlockedDecrement(&stream->refs);
  if (refs)
    return (ULONG)refs;

  IDataObject_Release(&stream->owner->iface);
  DeleteCriticalSection(&stream->lock);
  free(stream);
  return 0;
}

static HRESULT STDMETHODCALLTYPE streamRead(IStream * self, void * data,
    ULONG length, ULONG * read)
{
  if (read)
    *read = 0;
  if (!data && length)
    return STG_E_INVALIDPOINTER;
  if (!length)
    return S_OK;

  WindowsClipboardFileStream     * stream = streamFromInterface(self);
  WindowsClipboardFileDataObject * owner  = stream->owner;
  EnterCriticalSection(&stream->lock);
  const WindowsClipboardRemoteEntry * entry  =
    &owner->manifest->entries[stream->entryIndex];
  if (stream->offset >= entry->size)
  {
    LeaveCriticalSection(&stream->lock);
    return S_FALSE;
  }

  const ULONG wanted    = (ULONG)((entry->size - stream->offset < length) ?
    entry->size - stream->offset : length);
  ULONG       completed = 0;
  HRESULT     result    = S_OK;
  while (completed < wanted)
  {
    const ULONG chunk  =
      wanted - completed > KVMFR_CLIPBOARD_FILE_READ_BYTES ?
      KVMFR_CLIPBOARD_FILE_READ_BYTES : wanted - completed;
    ULONG       actual = 0;
    result = owner->ops.read(owner->opaque, owner->dataset,
      owner->acquisition, entry->node, stream->offset,
      (uint8_t *)data + completed, chunk, &actual);
    if (FAILED(result))
      break;
    if (actual != chunk)
    {
      result = STG_E_READFAULT;
      break;
    }

    stream->offset += actual;
    completed      += actual;
  }
  LeaveCriticalSection(&stream->lock);

  if (read)
    *read = completed;
  if (FAILED(result))
    return result;
  return completed == length ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE streamWrite(IStream * self,
    const void * data, ULONG length, ULONG * written)
{
  (void)self;
  (void)data;
  (void)length;
  if (written)
    *written = 0;
  return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE streamSeek(IStream * self,
    LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER * position)
{
  WindowsClipboardFileStream * stream = streamFromInterface(self);
  EnterCriticalSection(&stream->lock);
  const WindowsClipboardRemoteEntry * entry  =
    &stream->owner->manifest->entries[stream->entryIndex];

  uint64_t base;
  switch (origin)
  {
    case STREAM_SEEK_SET:
      base = 0;
      break;

    case STREAM_SEEK_CUR:
      base = stream->offset;
      break;

    case STREAM_SEEK_END:
      base = entry->size;
      break;

    default:
      LeaveCriticalSection(&stream->lock);
      return STG_E_INVALIDFUNCTION;
  }

  uint64_t next;
  if (move.QuadPart < 0)
  {
    const uint64_t amount = (uint64_t)(-(move.QuadPart + 1)) + 1U;
    if (amount > base)
    {
      LeaveCriticalSection(&stream->lock);
      return STG_E_INVALIDFUNCTION;
    }
    next = base - amount;
  }
  else
  {
    const uint64_t amount = (uint64_t)move.QuadPart;
    if (amount > UINT64_MAX - base)
    {
      LeaveCriticalSection(&stream->lock);
      return STG_E_INVALIDFUNCTION;
    }
    next = base + amount;
  }

  stream->offset = next;
  if (position)
    position->QuadPart = next;
  LeaveCriticalSection(&stream->lock);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE streamSetSize(IStream * self,
    ULARGE_INTEGER size)
{
  (void)self;
  (void)size;
  return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE streamCopyTo(IStream * self,
    IStream * target, ULARGE_INTEGER count, ULARGE_INTEGER * read,
    ULARGE_INTEGER * written)
{
  if (!target)
    return STG_E_INVALIDPOINTER;
  if (read)
    read->QuadPart = 0;
  if (written)
    written->QuadPart = 0;

  uint8_t * buffer = malloc(KVMFR_CLIPBOARD_FILE_READ_BYTES);
  if (!buffer)
    return E_OUTOFMEMORY;

  uint64_t remaining = count.QuadPart;
  HRESULT  result    = S_OK;
  while (remaining)
  {
    const ULONG wanted =
      (ULONG)(remaining > KVMFR_CLIPBOARD_FILE_READ_BYTES ?
      KVMFR_CLIPBOARD_FILE_READ_BYTES : remaining);
    ULONG       got    = 0;
    result = IStream_Read(self, buffer, wanted, &got);
    if (FAILED(result) || !got)
    {
      if (SUCCEEDED(result))
        result = S_FALSE;
      break;
    }

    ULONG sent = 0;
    result = IStream_Write(target, buffer, got, &sent);
    if (read)
      read->QuadPart += got;
    if (written)
      written->QuadPart += sent;
    if (FAILED(result) || sent != got)
    {
      if (SUCCEEDED(result))
        result = STG_E_WRITEFAULT;
      break;
    }
    remaining -= got;
  }

  free(buffer);
  return result;
}

static HRESULT STDMETHODCALLTYPE streamCommit(IStream * self, DWORD flags)
{
  (void)self;
  (void)flags;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE streamRevert(IStream * self)
{
  (void)self;
  return STG_E_REVERTED;
}

static HRESULT STDMETHODCALLTYPE streamLockRegion(IStream * self,
    ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type)
{
  (void)self;
  (void)offset;
  (void)count;
  (void)type;
  return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE streamUnlockRegion(IStream * self,
    ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type)
{
  (void)self;
  (void)offset;
  (void)count;
  (void)type;
  return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE streamStat(IStream * self, STATSTG * stat,
    DWORD flags)
{
  if (!stat)
    return STG_E_INVALIDPOINTER;
  memset(stat, 0, sizeof(*stat));

  WindowsClipboardFileStream        * stream = streamFromInterface(self);
  const WindowsClipboardRemoteEntry * entry  =
    &stream->owner->manifest->entries[stream->entryIndex];
  stat->type            = STGTY_STREAM;
  stat->cbSize.QuadPart = entry->size;
  stat->grfMode         = STGM_READ;
  stat->ctime           = unixNsToFileTime(entry->createdNs);
  stat->mtime           = unixNsToFileTime(entry->modifiedNs);
  if (!(flags & STATFLAG_NONAME))
  {
    const size_t length = wcslen(entry->name);
    if (length > (SIZE_MAX / sizeof(wchar_t)) - 1U)
      return E_OUTOFMEMORY;
    stat->pwcsName = CoTaskMemAlloc((length + 1U) * sizeof(wchar_t));
    if (!stat->pwcsName)
      return E_OUTOFMEMORY;
    memcpy(stat->pwcsName, entry->name,
      (length + 1U) * sizeof(wchar_t));
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE streamClone(IStream * self,
    IStream ** clone)
{
  if (!clone)
    return E_POINTER;
  *clone = NULL;

  WindowsClipboardFileStream * stream = streamFromInterface(self);
  EnterCriticalSection(&stream->lock);
  const HRESULT result = createFileStream(stream->owner,
    stream->entryIndex, stream->offset, clone);
  LeaveCriticalSection(&stream->lock);
  return result;
}

static HRESULT STDMETHODCALLTYPE formatEnumeratorQueryInterface(
    IEnumFORMATETC * self, REFIID iid, void ** object)
{
  if (!object)
    return E_POINTER;
  *object = NULL;
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_IEnumFORMATETC))
  {
    *object = self;
    IEnumFORMATETC_AddRef(self);
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE formatEnumeratorAddRef(IEnumFORMATETC * self)
{
  WindowsClipboardFormatEnumerator * enumerator =
    formatEnumeratorFromInterface(self);
  return (ULONG)InterlockedIncrement(&enumerator->refs);
}

static ULONG STDMETHODCALLTYPE formatEnumeratorRelease(IEnumFORMATETC * self)
{
  WindowsClipboardFormatEnumerator * enumerator =
    formatEnumeratorFromInterface(self);
  const LONG                         refs       =
    InterlockedDecrement(&enumerator->refs);
  if (!refs)
    free(enumerator);
  return refs ? (ULONG)refs : 0;
}

static HRESULT STDMETHODCALLTYPE formatEnumeratorNext(IEnumFORMATETC * self,
    ULONG count, FORMATETC * formats, ULONG * fetched)
{
  if (fetched)
    *fetched = 0;
  if (!formats || (count != 1U && !fetched))
    return E_INVALIDARG;

  WindowsClipboardFormatEnumerator * enumerator =
    formatEnumeratorFromInterface(self);
  ULONG                              copied     = 0;
  while (copied < count && enumerator->index < enumerator->count)
    formats[copied++] = enumerator->formats[enumerator->index++];
  if (fetched)
    *fetched = copied;
  return copied == count ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE formatEnumeratorSkip(IEnumFORMATETC * self,
    ULONG count)
{
  WindowsClipboardFormatEnumerator * enumerator =
    formatEnumeratorFromInterface(self);
  const ULONG                        available  =
    enumerator->count - enumerator->index;
  const ULONG                        skipped    =
    count < available ? count : available;
  enumerator->index += skipped;
  return skipped == count ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE formatEnumeratorReset(IEnumFORMATETC * self)
{
  WindowsClipboardFormatEnumerator * enumerator =
    formatEnumeratorFromInterface(self);
  enumerator->index = 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE formatEnumeratorClone(IEnumFORMATETC * self,
    IEnumFORMATETC ** clone)
{
  if (!clone)
    return E_POINTER;
  *clone = NULL;

  WindowsClipboardFormatEnumerator * source =
    formatEnumeratorFromInterface(self);
  WindowsClipboardFormatEnumerator * result = calloc(1, sizeof(*result));
  if (!result)
    return E_OUTOFMEMORY;

  result->iface.lpVtbl = &formatEnumeratorVTable;
  result->refs         = 1;
  memcpy(result->formats, source->formats, sizeof(result->formats));
  result->count = source->count;
  result->index = source->index;
  *clone        = &result->iface;
  return S_OK;
}

HRESULT windowsClipboardRemoteCreateDataObject(
    WindowsClipboardRemoteManifest * manifest, uint64_t dataset,
    uint64_t acquisition, const WindowsClipboardRemoteProviderOps * ops,
    void * opaque, IDataObject ** object)
{
  if (!object)
    return E_POINTER;
  *object = NULL;
  if (!manifest || !dataset || !acquisition || !ops || !ops->read ||
      !ops->release)
    return E_INVALIDARG;

  EnterCriticalSection(&manifest->lock);
  if (manifest->rootState != 2U ||
      !manifest->entryCount || manifest->entryCount > LONG_MAX)
  {
    LeaveCriticalSection(&manifest->lock);
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }

  FILEDESCRIPTORW  * descriptors = calloc(manifest->entryCount,
    sizeof(*descriptors));
  wchar_t         ** paths       = calloc(manifest->entryCount,
    sizeof(*paths));
  if (!descriptors || !paths)
  {
    free(descriptors);
    free(paths);
    LeaveCriticalSection(&manifest->lock);
    return E_OUTOFMEMORY;
  }

  HRESULT result = S_OK;
  for (size_t index = 0; index < manifest->entryCount; ++index)
  {
    const WindowsClipboardRemoteEntry * entry = &manifest->entries[index];
    if (!entry->node || !validComponent(entry->name) ||
        (entry->type == KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY &&
          entry->listState != 2U))
    {
      result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      break;
    }

    if (!entry->parent)
      paths[index] = duplicateWide(entry->name);
    else
    {
      const ptrdiff_t parent = findRemoteEntry(manifest, entry->parent);
      if (parent < 0 || (size_t)parent >= index ||
          manifest->entries[parent].type !=
            KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY || !paths[parent])
      {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        break;
      }
      paths[index] = joinPath(paths[parent], entry->name);
    }
    if (!paths[index])
    {
      result = E_OUTOFMEMORY;
      break;
    }

    const size_t pathLength = wcslen(paths[index]);
    if (pathLength >= ARRAYSIZE(descriptors[index].cFileName))
    {
      result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      break;
    }

    FILEDESCRIPTORW * descriptor = &descriptors[index];
    descriptor->dwFlags          = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI;
    descriptor->dwFileAttributes = entry->type ==
      KVMFR_CLIPBOARD_FILE_TYPE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY :
      FILE_ATTRIBUTE_NORMAL;
    if (entry->createdNs)
    {
      descriptor->dwFlags        |= FD_CREATETIME;
      descriptor->ftCreationTime = unixNsToFileTime(entry->createdNs);
    }
    if (entry->modifiedNs)
    {
      descriptor->dwFlags         |= FD_WRITESTIME;
      descriptor->ftLastWriteTime = unixNsToFileTime(entry->modifiedNs);
    }
    if (entry->type == KVMFR_CLIPBOARD_FILE_TYPE_REGULAR)
    {
      descriptor->dwFlags       |= FD_FILESIZE;
      descriptor->nFileSizeHigh = (DWORD)(entry->size >> 32U);
      descriptor->nFileSizeLow  = (DWORD)entry->size;
    }
    memcpy(descriptor->cFileName, paths[index],
      (pathLength + 1U) * sizeof(wchar_t));
  }

  for (size_t index = 0; index < manifest->entryCount; ++index)
    free(paths[index]);
  free(paths);
  if (FAILED(result))
  {
    free(descriptors);
    LeaveCriticalSection(&manifest->lock);
    return result;
  }

  const UINT fileDescriptorFormat      =
    RegisterClipboardFormatW(L"FileGroupDescriptorW");
  const UINT fileContentsFormat        =
    RegisterClipboardFormatW(L"FileContents");
  const UINT preferredDropEffectFormat =
    RegisterClipboardFormatW(L"Preferred DropEffect");
  if (!fileDescriptorFormat || !fileContentsFormat ||
      !preferredDropEffectFormat)
  {
    const DWORD winError = GetLastError();
    free(descriptors);
    LeaveCriticalSection(&manifest->lock);
    return HRESULT_FROM_WIN32(winError ? winError : ERROR_INVALID_DATA);
  }

  WindowsClipboardFileDataObject * dataObject =
    calloc(1, sizeof(*dataObject));
  if (!dataObject)
  {
    free(descriptors);
    LeaveCriticalSection(&manifest->lock);
    return E_OUTOFMEMORY;
  }

  dataObject->iface.lpVtbl              = &dataObjectVTable;
  dataObject->refs                      = 1;
  dataObject->manifest                  = manifest;
  dataObject->dataset                   = dataset;
  dataObject->acquisition               = acquisition;
  dataObject->ops                       = *ops;
  dataObject->opaque                    = opaque;
  dataObject->descriptors               = descriptors;
  dataObject->descriptorCount           = manifest->entryCount;
  dataObject->fileDescriptorFormat      = fileDescriptorFormat;
  dataObject->fileContentsFormat        = fileContentsFormat;
  dataObject->preferredDropEffectFormat = preferredDropEffectFormat;

  manifest->frozen = true;
  remoteManifestRetain(manifest);
  LeaveCriticalSection(&manifest->lock);

  *object = &dataObject->iface;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE dataObjectDUnadvise(IDataObject * self,
    DWORD connection)
{
  (void)self;
  (void)connection;
  return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE dataObjectEnumDAdvise(IDataObject * self,
    IEnumSTATDATA ** enumerator)
{
  (void)self;
  if (enumerator)
    *enumerator = NULL;
  return OLE_E_ADVISENOTSUPPORTED;
}
