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

#include "common/filesystem.h"

#include "common/win32.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static wchar_t * widePath(const char * path)
{
  return lgUtf8ToWide(path);
}

static LGFileType attributesType(DWORD attributes)
{
  if (attributes & FILE_ATTRIBUTE_DIRECTORY)
    return LG_FILE_TYPE_DIRECTORY;
  if (!(attributes & FILE_ATTRIBUTE_DEVICE))
    return LG_FILE_TYPE_REGULAR;
  return LG_FILE_TYPE_OTHER;
}

static LGFileType pathError(DWORD error)
{
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
    LG_FILE_TYPE_MISSING : LG_FILE_TYPE_ERROR;
}

static LGFileType pathType(const char * path, bool follow)
{
  wchar_t * wide = widePath(path);
  if (!wide)
    return LG_FILE_TYPE_ERROR;

  if (follow)
  {
    HANDLE      file      = CreateFileW(wide, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    const DWORD openError = file == INVALID_HANDLE_VALUE ?
      GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (file == INVALID_HANDLE_VALUE)
      return pathError(openError);

    BY_HANDLE_FILE_INFORMATION info;
    const bool                 result = GetFileInformationByHandle(file, &info);
    const DWORD                error  = result ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!result)
      return pathError(error);
    return attributesType(info.dwFileAttributes);
  }

  const DWORD attributes = GetFileAttributesW(wide);
  const DWORD error      = attributes == INVALID_FILE_ATTRIBUTES ?
    GetLastError() : ERROR_SUCCESS;
  free(wide);
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return pathError(error);
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
    return LG_FILE_TYPE_OTHER;
  return attributesType(attributes);
}

LGFileType lgPathType(const char * path)
{
  return pathType(path, true);
}

LGFileType lgPathTypeNoFollow(const char * path)
{
  return pathType(path, false);
}

bool lgCreateDirectory(const char * path)
{
  wchar_t * wide = widePath(path);
  if (!wide)
    return false;

  bool result = CreateDirectoryW(wide, NULL);
  if (!result && GetLastError() == ERROR_ALREADY_EXISTS)
  {
    const DWORD attributes = GetFileAttributesW(wide);
    result = attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY);
  }
  free(wide);
  return result;
}

bool lgDeleteFile(const char * path)
{
  wchar_t * wide = widePath(path);
  if (!wide)
    return false;

  const bool result = DeleteFileW(wide);
  free(wide);
  return result;
}

bool lgDeleteDirectory(const char * path)
{
  wchar_t * wide = widePath(path);
  if (!wide)
    return false;

  const bool result = RemoveDirectoryW(wide);
  free(wide);
  return result;
}

FILE * lgOpenFile(const char * path, const char * mode)
{
  wchar_t * widePathValue = lgUtf8ToWide(path);
  wchar_t * wideMode      = lgUtf8ToWide(mode);
  if (!widePathValue || !wideMode)
  {
    free(widePathValue);
    free(wideMode);
    return NULL;
  }

  FILE * result = _wfopen(widePathValue, wideMode);
  free(widePathValue);
  free(wideMode);
  return result;
}

StringList lgListDirectory(const char * path)
{
  wchar_t * wide = widePath(path);
  if (!wide)
    return NULL;

  const size_t length = wcslen(wide);
  if (length > SIZE_MAX / sizeof(*wide) - 3)
  {
    free(wide);
    return NULL;
  }

  wchar_t * pattern = realloc(wide, (length + 3) * sizeof(*pattern));
  if (!pattern)
  {
    free(wide);
    return NULL;
  }
  size_t offset = length;
  if (length && pattern[length - 1] != L'\\' &&
      pattern[length - 1] != L'/')
    pattern[offset++] = L'\\';
  pattern[offset++] = L'*';
  pattern[offset]   = L'\0';

  StringList result = stringlist_new(true);
  if (!result)
  {
    free(pattern);
    return NULL;
  }

  WIN32_FIND_DATAW entry;
  HANDLE           find = FindFirstFileW(pattern, &entry);
  free(pattern);
  if (find == INVALID_HANDLE_VALUE)
  {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND)
      return result;
    stringlist_free(&result);
    return NULL;
  }

  do
  {
    if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
      continue;

    char * name = lgWideToUtf8(entry.cFileName);
    if (!name)
      goto fail;

    const unsigned int count = stringlist_count(result);
    stringlist_push(result, name);
    if (stringlist_count(result) != count + 1)
    {
      free(name);
      goto fail;
    }
  }
  while (FindNextFileW(find, &entry));

  const DWORD error = GetLastError();
  FindClose(find);
  if (error != ERROR_NO_MORE_FILES)
    goto fail_closed;
  return result;

fail:
  FindClose(find);

fail_closed:
  stringlist_free(&result);
  return NULL;
}
