/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "app.h"

#include <stdio.h>
#include <inttypes.h>
#include "debug.h"
#include "capture/interfaces.h"
#include "KVMFR.h"

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)
#define MAX_FRAMES 2

struct app
{
  KVMFRHeader * shmHeader;
  uint8_t     * cursorData;
  unsigned int  cursorDataSize;
  unsigned int  cursorOffset;

  uint8_t     * frames;
  unsigned int  frameSize;
  uint8_t     * frame[MAX_FRAMES];
  unsigned int  frameOffset[MAX_FRAMES];
};

static struct app app;

int app_main()
{
  unsigned int shmemSize = os_shmemSize();
  uint8_t    * shmemMap  = NULL;
  int          exitcode  = 0;

  DEBUG_INFO("IVSHMEM Size     : %u MiB", shmemSize / 1048576);
  if (!os_shmemMmap((void **)&shmemMap) || !shmemMap)
  {
    DEBUG_ERROR("Failed to map the shared memory");
    return -1;
  }
  DEBUG_INFO("IVSHMEM Address  : 0x%" PRIXPTR, (uintptr_t)shmemMap);

  app.shmHeader        = (KVMFRHeader *)shmemMap;
  app.cursorData       = (uint8_t *)ALIGN_UP(shmemMap + sizeof(KVMFRHeader));
  app.cursorDataSize   = 1048576; // 1MB fixed for cursor size, should be more then enough
  app.cursorOffset     = app.cursorData - shmemMap;
  app.frames           = (uint8_t *)ALIGN_UP(app.cursorData + app.cursorDataSize);
  app.frameSize        = ALIGN_DN((shmemSize - (app.frames - shmemMap)) / MAX_FRAMES);

  DEBUG_INFO("Max Cursor Size  : %u MiB"     , app.cursorDataSize / 1048576);
  DEBUG_INFO("Max Frame Size   : %u MiB"     , app.frameSize      / 1048576);
  DEBUG_INFO("Cursor           : 0x%" PRIXPTR " (0x%08x)", (uintptr_t)app.cursorData, app.cursorOffset);

  for (int i = 0; i < MAX_FRAMES; ++i)
  {
    app.frame      [i] = app.frames + i * app.frameSize;
    app.frameOffset[i] = app.frame[i] - shmemMap;
    DEBUG_INFO("Frame %d          : 0x%" PRIXPTR " (0x%08x)", i, (uintptr_t)app.frame[i], app.frameOffset[i]);
  }

  struct CaptureInterface * iface = NULL;
  for(int i = 0; CaptureInterfaces[i]; ++i)
  {
    iface = CaptureInterfaces[i];
    DEBUG_INFO("Trying           : %s", iface->getName());
    if (!iface->create())
      continue;

    if (iface->init())
      break;

    iface->free();
    iface = NULL;
  }

  if (!iface)
  {
    DEBUG_ERROR("Failed to find a supported capture interface");
    exitcode = -1;
    goto fail;
  }

  DEBUG_INFO("Using            : %s", iface->getName());

  const unsigned int maxFrameSize = iface->getMaxFrameSize();
  if (maxFrameSize > app.frameSize)
  {
    DEBUG_ERROR("Maximum frame size of %d bytes excceds maximum space available", maxFrameSize);
    exitcode = -1;
    goto exit;
  }
  DEBUG_INFO("Capture Size     : %u MiB (%u)", maxFrameSize / 1048576, maxFrameSize);

  iface->capture();
  iface->capture();
  iface->capture();

exit:
  iface->deinit();
  iface->free();
fail:
  os_shmemUnmap();
  return exitcode;
}