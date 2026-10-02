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

#include "common/debug.h"
#include "common/stringutils.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static LGFileType pathType(const char * path, bool follow)
{
  struct stat value;
  if ((follow ? stat(path, &value) : lstat(path, &value)) < 0)
    return errno == ENOENT || errno == ENOTDIR ?
      LG_FILE_TYPE_MISSING : LG_FILE_TYPE_ERROR;

  if (S_ISREG(value.st_mode))
    return LG_FILE_TYPE_REGULAR;
  if (S_ISDIR(value.st_mode))
    return LG_FILE_TYPE_DIRECTORY;
  return LG_FILE_TYPE_OTHER;
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
  if (mkdir(path, S_IRWXU) == 0)
    return true;

  return errno == EEXIST && lgPathType(path) == LG_FILE_TYPE_DIRECTORY;
}

bool lgDeleteFile(const char * path)
{
  return unlink(path) == 0;
}

bool lgDeleteDirectory(const char * path)
{
  return rmdir(path) == 0;
}

FILE * lgOpenFile(const char * path, const char * mode)
{
  return fopen(path, mode);
}

bool lgFileErrorString(int error, char * buffer, size_t size)
{
  if (!buffer || !size)
    return false;

  const char * message = strerror(error);
  if (!message)
  {
    buffer[0] = '\0';
    return false;
  }

  str_copy(buffer, size, message, strlen(message));
  return true;
}

StringList lgListDirectory(const char * path)
{
  DIR * directory = opendir(path);
  if (!directory)
    return NULL;

  StringList result = stringlist_new(true);
  if (!result)
  {
    closedir(directory);
    return NULL;
  }

  int error = 0;
  for (;;)
  {
    errno = 0;
    struct dirent * entry = readdir(directory);
    if (!entry)
    {
      error = errno;
      break;
    }

    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;

    char * name = lg_strdup(entry->d_name);
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

  closedir(directory);
  if (error)
    goto fail_closed;
  return result;

fail:
  closedir(directory);

fail_closed:
  stringlist_free(&result);
  return NULL;
}
