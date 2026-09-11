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

#include "common/paths.h"

#include "common/debug.h"
#include "common/windebug.h"
#include "common/win32.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

static char * configDir;
static char * dataDir;

static char * makePath(
    REFKNOWNFOLDERID folderId, const wchar_t * appName)
{
  PWSTR         base;
  const HRESULT status = SHGetKnownFolderPath(folderId, 0, NULL, &base);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to locate application data directory", status);
    return NULL;
  }

  const size_t baseLength = wcslen(base);
  const size_t appLength  = wcslen(appName);
  if (baseLength > SIZE_MAX - appLength - 2)
  {
    CoTaskMemFree(base);
    return NULL;
  }

  wchar_t * path = malloc(
      (baseLength + appLength + 2) * sizeof(*path));
  if (!path)
  {
    CoTaskMemFree(base);
    return NULL;
  }

  memcpy(path, base, baseLength * sizeof(*path));
  path[baseLength] = L'\\';
  memcpy(path + baseLength + 1, appName,
      (appLength + 1) * sizeof(*path));
  CoTaskMemFree(base);

  if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
  {
    DEBUG_WINERROR("Failed to create application data directory",
        GetLastError());
    free(path);
    return NULL;
  }

  char * result = lgWideToUtf8(path);
  free(path);
  return result;
}

void lgPathsInit(const char * appName)
{
  wchar_t * wideName = lgUtf8ToWide(appName);
  if (!wideName)
  {
    DEBUG_ERROR("Invalid UTF-8 application name");
    return;
  }

  char * newConfigDir = makePath(&FOLDERID_RoamingAppData, wideName);
  char * newDataDir   = makePath(&FOLDERID_LocalAppData, wideName);
  free(wideName);
  if (!newConfigDir || !newDataDir)
  {
    free(newConfigDir);
    free(newDataDir);
    return;
  }

  free(configDir);
  free(dataDir);
  configDir = newConfigDir;
  dataDir   = newDataDir;
}

const char * lgConfigDir(void)
{
  return configDir ? configDir : ".";
}

const char * lgDataDir(void)
{
  return dataDir ? dataDir : ".";
}
