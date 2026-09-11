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

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define ivshmemGetDMABuf testIVSHMEMGetDMABuf
#include "../transports/LGMP/platform/linux.c"
#undef ivshmemGetDMABuf

static struct IVSHMEM * expectedDevice;
static uint64_t         expectedOffset;
static uint64_t         expectedSize;
static int              sourceFD;
static unsigned int     exportCalls;
static bool             exportFails;

int testIVSHMEMGetDMABuf(struct IVSHMEM * device, uint64_t offset,
    uint64_t size)
{
  ++exportCalls;
  CHECK(device == expectedDevice);
  CHECK(offset == expectedOffset);
  CHECK(size == expectedSize);
  return exportFails ? -1 : dup(sourceFD);
}

int main(void)
{
  struct IVSHMEM device = { 0 };

  expectedDevice        = &device;
  expectedOffset        = 4096;
  expectedSize          = 8192;
  sourceFD              = open("/dev/null", O_RDONLY);
  CHECK(sourceFD >= 0);

  LG_ExternalFrame * frame = lgmpPlatformCreateExternalFrame(
      &device, expectedOffset, expectedSize);
  CHECK(frame);
  CHECK(exportCalls == 1);
  CHECK(lgExternalFrameType(frame) == LG_EXTERNAL_FRAME_DMABUF);

  int frameFD = -1;
  CHECK(lgExternalFrameQuery(frame, LG_EXTERNAL_FRAME_DMABUF,
        &frameFD, sizeof(frameFD)));
  CHECK(frameFD >= 0);
  CHECK(frameFD != sourceFD);
  CHECK(fcntl(frameFD, F_GETFD) != -1);
  CHECK(!lgExternalFrameQuery(frame, LG_EXTERNAL_FRAME_INVALID,
        &frameFD, sizeof(frameFD)));
  CHECK(!lgExternalFrameQuery(frame, LG_EXTERNAL_FRAME_DMABUF,
        NULL, sizeof(frameFD)));
  CHECK(!lgExternalFrameQuery(frame, LG_EXTERNAL_FRAME_DMABUF,
        &frameFD, sizeof(frameFD) - 1));

  CHECK(lgExternalFrameRetain(frame) == frame);
  lgExternalFrameRelease(frame);
  CHECK(fcntl(frameFD, F_GETFD) != -1);

  lgExternalFrameRelease(frame);
  errno = 0;
  CHECK(fcntl(frameFD, F_GETFD) == -1);
  CHECK(errno == EBADF);
  CHECK(fcntl(sourceFD, F_GETFD) != -1);

  exportFails = true;
  CHECK(!lgmpPlatformCreateExternalFrame(
        &device, expectedOffset, expectedSize));
  CHECK(exportCalls == 2);

  CHECK(close(sourceFD) == 0);
  return EXIT_SUCCESS;
}
