/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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
#include "common/util.h"

void cpuInfo_log(void)
{
  char model[1024];
  int procs;
  int cores;
  int sockets;

  if (!cpuInfo_get(model, sizeof model, &procs, &cores, &sockets))
  {
    DEBUG_WARN("Failed to get CPU information");
    return;
  }

  DEBUG_INFO("CPU Model: %s", model);
  DEBUG_INFO("CPU: %d sockets, %d cores, %d threads", sockets, cores, procs);
}

const CPUInfoFeatures * cpuInfo_getFeatures(void)
{
  static bool initialized = false;
  static CPUInfoFeatures features;

  if (likely(initialized))
    return &features;

  int cpuid[4] = {0};

  // leaf1
  asm volatile
  (
    "cpuid;"
    : "=a" (cpuid[0]),
      "=b" (cpuid[1]),
      "=c" (cpuid[2]),
      "=d" (cpuid[3])
    : "a" (1)
  );

  features.sse     = cpuid[3] & (1 << 25);
  features.sse2    = cpuid[3] & (1 << 26);
  features.sse3    = cpuid[2] & (1 <<  0);
  features.ssse3   = cpuid[2] & (1 <<  9);
  features.fma     = cpuid[2] & (1 << 12);
  features.sse4_1  = cpuid[2] & (1 << 19);
  features.sse4_2  = cpuid[2] & (1 << 20);
  features.popcnt  = cpuid[2] & (1 << 23);
  features.aes     = cpuid[2] & (1 << 25);
  features.xsave   = cpuid[2] & (1 << 26);
  features.osxsave = cpuid[2] & (1 << 27);
  features.avx     = cpuid[2] & (1 << 28);

  // leaf7
  asm volatile
  (
    "cpuid;"
    : "=a" (cpuid[0]),
      "=b" (cpuid[1]),
      "=c" (cpuid[2]),
      "=d" (cpuid[3])
    : "a" (7), "c" (0)
  );

  features.avx2 = cpuid[1] & (1 << 5);
  features.bmi1 = cpuid[2] & (1 << 3);
  features.bmi2 = cpuid[2] & (1 << 8);

  if (features.osxsave && features.avx)
  {
    int xgetbv = 0;
    asm volatile
    (
      "xgetbv;"
      : "=a" (xgetbv)
      : "c" (0)
      : "edx"
    );

    if (!(xgetbv & 0x6))
    {
      features.avx  = false;
      features.avx2 = false;
    }
  }

  return &features;
};
