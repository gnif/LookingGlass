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

#include "common/debug.h"
#include "common/proctitle.h"

#include <string.h>
#include <unistd.h>

static bool initialized;
static char * buffer;
static size_t totalSize = 0;

static char ** duplicateArray(char ** array, int count)
{
  size_t size = (count + 1) * sizeof(char *);
  char ** new = malloc(size);
  if (!new)
    return false;

  memset(new, 0, size);
  for (int i = 0; i < count; ++i)
  {
    new[i] = strdup(array[i]);
    if (!new[i])
      goto fail;
  }

  new[count] = NULL;
  return new;

fail:
  for (int i = 0; i < count; ++i)
    if (new[i])
      free(new[i]);

  return NULL;
}

static int extendContiguous(char ** end, char ** array)
{
  int i;
  for (i = 0; array[i]; ++i)
    if (array[i] == *end)
      *end += strlen(array[i]) + 1;

  return i;
}

void lgInitProcessTitle(int argc, char ***pargv)
{
  if (initialized)
    return;

  char **argv = *pargv;
  // it is technically possible to have no arguments, but then no titles for you
  if (!argv[0])
  {
    initialized = true;
    return;
  }

  char * start = argv[0];
  char * end = start;
  extendContiguous(&end, argv);

  char ** newArgv = duplicateArray(argv, argc);
  if (!newArgv)
  {
    initialized = true;
    return;
  }
  *pargv = argv;

  char * envEnd = end;
  int vars = extendContiguous(&envEnd, environ);

  if (envEnd != end)
  {
    char ** newEnv = duplicateArray(environ, vars);
    if (newEnv)
    {
      environ = newEnv;
      end = envEnd;
    }
  }

  initialized = true;
  buffer = start;
  totalSize = end - start;
}

bool lgCanSetProcessTitle(void)
{
  return initialized && totalSize > 0;
}

void lgSetProcessTitle(const char * title)
{
  if (!initialized)
  {
    DEBUG_ERROR("Attempt to use lgSetProcessTitle without lgInitProcessTitle");
    return;
  }

  if (!totalSize)
    return;

  size_t effective = strlen(title) + 1;
  if (effective > totalSize)
    effective = totalSize;

  memcpy(buffer, title, effective - 1);
  buffer[effective - 1] = '\0';
}
