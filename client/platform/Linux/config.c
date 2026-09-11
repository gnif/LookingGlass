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

#include "config_platform.h"

#include "common/debug.h"
#include "common/paths.h"
#include "common/stringutils.h"

#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

static bool visitJoinedPath(const char * directory, const char * filename,
    ConfigFileVisitor visitor, void * opaque)
{
  char * path;
  if (alloc_sprintf(&path, "%s/%s", directory, filename) < 0)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  const bool result = visitor(path, opaque);
  free(path);
  return result;
}

bool config_platformVisitDefaultFiles(
    ConfigFileVisitor visitor, void * opaque)
{
  if (!visitor("/etc/looking-glass-client.ini", opaque))
    return false;

  const struct passwd * password = getpwuid(getuid());
  if (!password)
    DEBUG_WARN("getpwuid failed, unable to load user configuration");
  else if (!visitJoinedPath(password->pw_dir, ".looking-glass-client.ini",
        visitor, opaque))
    return false;

  return visitJoinedPath(lgConfigDir(), "client.ini", visitor, opaque);
}
