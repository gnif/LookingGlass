/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "common/cpuinfo.h"
#include "common/debug.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool lgCPUInfo(char * model, size_t modelSize, int * procs, int * cores)
{
  FILE * cpuinfo = fopen("/proc/cpuinfo", "r");
  if (!cpuinfo)
  {
    DEBUG_ERROR("Failed to open /proc/cpuinfo: %s", strerror(errno));
    return false;
  }

  if (procs)
    *procs = 0;

  if (cores)
    *cores = 0;

  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), cpuinfo))
  {
    if (procs && strncmp(buffer, "processor", 9) == 0)
      ++*procs;
    else if (model && strncmp(buffer, "model name", 10) == 0)
    {
      const char * name = strstr(buffer, ": ");
      if (name)
        name += 2;
      snprintf(model, modelSize, "%s", name ? name : "Unknown");
      model = NULL;
    }
    else if (cores && strncmp(buffer, "cpu cores", 9) == 0)
    {
      const char * num = strstr(buffer, ": ");
      if (num)
      {
        *cores = atoi(num + 2);
        cores = NULL;
      }
    }

    // If a line is too long, skip it.
    while (buffer[strlen(buffer) - 1] != '\n')
      if (!fgets(buffer, sizeof(buffer), cpuinfo))
        goto done;
  }

done:
  fclose(cpuinfo);
  return true;
}
