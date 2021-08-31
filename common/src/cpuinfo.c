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

void lgDebugCPU(void)
{
  char model[1024];
  int procs;
  int cores;

  if (!lgCPUInfo(model, sizeof model, &procs, &cores))
  {
    DEBUG_WARN("Failed to get CPU information");
    return;
  }

  DEBUG_INFO("CPU Model: %s", model);
  DEBUG_INFO("CPU: %d cores, %d threads", cores, procs);
}
