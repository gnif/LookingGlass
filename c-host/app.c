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
#include <unistd.h>
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

  bool             running;
  osThreadHandle * cursorThread;
  osThreadHandle * frameThread;
};

static struct app app;

static int cursorThread(void * opaque)
{
  DEBUG_INFO("Cursor thread started");

  while(app.running)
    usleep(10000);

  DEBUG_INFO("Cursor thread stopped");
  return 0;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  while(app.running)
    usleep(10000);

  DEBUG_INFO("Frame thread stopped");
  return 0;
}

bool startThreads()
{
  app.running = true;
  if (!os_createThread("CursorThread", cursorThread, NULL, &app.cursorThread))
  {
    DEBUG_ERROR("Failed to create the cursor thread");
    return false;
  }

  if (!os_createThread("FrameThread", frameThread, NULL, &app.frameThread))
  {
    DEBUG_ERROR("Failed to create the frame thread");
    return false;
  }

  return true;
}

bool stopThreads()
{
  bool ok = true;

  app.running = false;
  if (app.frameThread && !os_joinThread(app.frameThread, NULL))
  {
    DEBUG_WARN("Failed to join the frame thread");
    ok = false;
  }
  app.frameThread = NULL;

  if (app.cursorThread && !os_joinThread(app.cursorThread, NULL))
  {
    DEBUG_WARN("Failed to join the cursor thread");
    ok = false;
  }
  app.cursorThread = NULL;

  return ok;
}

static bool captureStart(struct CaptureInterface * iface)
{
  DEBUG_INFO("Using            : %s", iface->getName());

  const unsigned int maxFrameSize = iface->getMaxFrameSize();
  if (maxFrameSize > app.frameSize)
  {
    DEBUG_ERROR("Maximum frame size of %d bytes excceds maximum space available", maxFrameSize);
    return false;
  }
  DEBUG_INFO("Capture Size     : %u MiB (%u)", maxFrameSize / 1048576, maxFrameSize);

  DEBUG_INFO("==== [ Capture  Start ] ====");
  return startThreads();
}

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
    {
      iface = NULL;
      continue;
    }

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

  if (!captureStart(iface))
  {
    exitcode = -1;
    goto exit;
  }

  while(app.running)
  {
    bool hasFrameUpdate   = false;
    bool hasPointerUpdate = false;
    switch(iface->capture(&hasFrameUpdate, &hasPointerUpdate))
    {
      case CAPTURE_RESULT_OK:
        break;

      case CAPTURE_RESULT_TIMEOUT:
        continue;

      case CAPTURE_RESULT_REINIT:
        DEBUG_INFO("==== [ Capture Reinit ] ====");
        if (!stopThreads())
        {
          exitcode = -1;
          goto finish;
        }

        if (!iface->deinit() || !iface->init())
        {
          DEBUG_ERROR("Failed to reinitialize the capture device");
          exitcode = -1;
          goto finish;
        }

        if (!captureStart(iface))
        {
          exitcode = -1;
          goto finish;
        }
        break;

      case CAPTURE_RESULT_ERROR:
        DEBUG_ERROR("Capture interface reported a fatal error");
        exitcode = -1;
        goto finish;
    }
  }

finish:
  stopThreads();
exit:
  iface->deinit();
  iface->free();
fail:
  os_shmemUnmap();
  return exitcode;
}

void app_quit()
{
  app.running = false;
}