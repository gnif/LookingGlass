/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include <errno.h>
#include <linux/limits.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char configDir[PATH_MAX];
static char dataDir[PATH_MAX];

static void ensureDir(char * path, mode_t mode)
{
  struct stat st;
  if (stat(path, &st) >= 0)
  {
    if (S_ISDIR(st.st_mode))
      return;

    DEBUG_ERROR("Expected to be a directory: %s", path);
    exit(2);
  }

  for (char * p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
    *p = '\0';
    if (mkdir(path, mode) < 0 && errno != EEXIST)
    {
      *p = '/';
      DEBUG_ERROR("Failed to create directory: %s", path);
      return;
    }
    *p = '/';
  }

  if (mkdir(path, mode) < 0 && errno != EEXIST)
    DEBUG_ERROR("Failed to create directory: %s", path);
}

void lgPathsInit(const char * appName)
{
  const char * home = getenv("HOME");
  if (!home)
    home = getpwuid(getuid())->pw_dir;

  const char * dir;
  if ((dir = getenv("XDG_CONFIG_HOME")) != NULL)
    snprintf(configDir, sizeof(configDir), "%s/%s", dir, appName);
  else
    snprintf(configDir, sizeof(configDir), "%s/.config/%s", home, appName);

  if ((dir = getenv("XDG_DATA_HOME")) != NULL)
    snprintf(dataDir, sizeof(configDir), "%s/%s", dir, appName);
  else
    snprintf(dataDir, sizeof(configDir), "%s/.local/share/%s", home, appName);

  ensureDir(configDir, S_IRWXU);
  ensureDir(dataDir,   S_IRWXU);
}

const char * lgConfigDir(void)
{
  return configDir;
}

const char * lgDataDir(void)
{
  return dataDir;
}
