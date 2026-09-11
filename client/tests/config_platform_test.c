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
#include "test.h"

#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PATHS 3U

struct PathList
{
  char       * paths[MAX_PATHS];
  unsigned int count;
};

const char * lgConfigDir(void)
{
  return "/test/config";
}

static bool collectPath(const char * path, void * opaque)
{
  struct PathList * paths = opaque;
  CHECK(paths->count < MAX_PATHS);
  paths->paths[paths->count] = strdup(path);
  CHECK(paths->paths[paths->count]);
  ++paths->count;
  return true;
}

static bool stopAtFirstPath(const char * path, void * opaque)
{
  (void)path;
  unsigned int * count = opaque;
  ++*count;
  return false;
}

static void testCandidates(void)
{
  struct PathList paths = { 0 };
  CHECK(config_platformVisitDefaultFiles(collectPath, &paths));
  CHECK(paths.count == MAX_PATHS);
  CHECK(strcmp(paths.paths[0], "/etc/looking-glass-client.ini") == 0);

  const struct passwd * password = getpwuid(getuid());
  CHECK(password);

  const size_t homeLength = strlen(password->pw_dir);
  const char   suffix[]   = "/.looking-glass-client.ini";
  char       * expected   = malloc(homeLength + sizeof(suffix));
  CHECK(expected);
  memcpy(expected, password->pw_dir, homeLength);
  memcpy(expected + homeLength, suffix, sizeof(suffix));
  CHECK(strcmp(paths.paths[1], expected) == 0);
  CHECK(strcmp(paths.paths[2], "/test/config/client.ini") == 0);

  free(expected);
  for (unsigned int i = 0; i < paths.count; ++i)
    free(paths.paths[i]);

  unsigned int count = 0;
  CHECK(!config_platformVisitDefaultFiles(stopAtFirstPath, &count));
  CHECK(count == 1);
}

int main(void)
{
  testCandidates();
  return 0;
}
