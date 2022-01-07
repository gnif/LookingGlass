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

#include "common/cpuinfo.h"
#include "common/debug.h"
#include "common/windebug.h"

#include <windows.h>

static bool getCPUModel(char * model, size_t modelSize)
{
  if (!model)
    return true;

  LRESULT lr;
  DWORD cb = modelSize;

  if ((lr = RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\SYSTEM\\CentralProcessor\\0",
    "ProcessorNameString", RRF_RT_REG_SZ, NULL, model, &cb)))
  {
    DEBUG_WINERROR("Failed to query registry", lr);
    return false;
  }

  // trim any whitespace
  --cb;
  while (cb > 0 && isspace(model[cb-1]))
    --cb;
  model[cb] = '\0';

  return true;
}

static bool getCoreCount(int * cores, int * procs, int * sockets)
{
  if (!cores && !procs)
    return true;

  DWORD cb = 0;
  GetLogicalProcessorInformationEx(RelationAll, NULL, &cb);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
  {
    DEBUG_WINERROR("Failed to call GetLogicalProcessorInformationEx", GetLastError());
    return false;
  }

  BYTE buffer[cb];
  if (!GetLogicalProcessorInformationEx(RelationAll,
      (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) buffer, &cb))
  {
    DEBUG_WINERROR("Failed to call GetLogicalProcessorInformationEx", GetLastError());
    return false;
  }

  if (cores)
    *cores = 0;

  if (procs)
    *procs = 0;

  if (sockets)
    *sockets = 0;

  DWORD offset = 0;
  while (offset < cb)
  {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX lpi =
      (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) (buffer + offset);
    switch (lpi->Relationship)
    {
      case RelationProcessorCore:
        if (cores)
          ++*cores;

        if (procs)
          for (int i = 0; i < lpi->Processor.GroupCount; ++i)
            *procs += __builtin_popcount(lpi->Processor.GroupMask[i].Mask);
        break;

      case RelationProcessorPackage:
        if (sockets)
          ++*sockets;
        break;

      default:
        break;
    }
    offset += lpi->Size;
  }

  return true;
}

bool lgCPUInfo(char * model, size_t modelSize, int * procs, int * cores,
    int * sockets)
{
  return getCPUModel(model, modelSize) && getCoreCount(cores, procs, sockets);
}
