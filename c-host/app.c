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
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "locking.h"
#include "capture/interfaces.h"
#include "KVMFR.h"

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)
#define MAX_FRAMES 2

struct app
{
  KVMFRHeader * shmHeader;
  uint8_t     * pointerData;
  unsigned int  pointerDataSize;
  unsigned int  pointerOffset;

  CaptureInterface * iface;

  uint8_t     * frames;
  unsigned int  frameSize;
  uint8_t     * frame[MAX_FRAMES];
  unsigned int  frameOffset[MAX_FRAMES];

  bool             running;
  osEventHandle  * updateEvent;
  osThreadHandle * pointerThread;
  osEventHandle  * pointerEvent;
  osThreadHandle * frameThread;
  osEventHandle  * frameEvent;
};

static struct app app;

static int pointerThread(void * opaque)
{
  DEBUG_INFO("Cursor thread started");

  while(app.running)
  {
    if (!os_waitEvent(app.pointerEvent) || !app.running)
      break;

#if 0
    CapturePointer pointer;
    pointer->data = app.pointerData;
    if (!app.iface->getPointer(&pointer))
      DEBUG_ERROR("Failed to get the pointer");
#endif
    os_signalEvent(app.updateEvent);
  }

  DEBUG_INFO("Cursor thread stopped");
  return 0;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  int      frameIndex = 0;
  volatile KVMFRFrame * fi = &(app.shmHeader->frame);

  while(app.running)
  {
    if (!os_waitEvent(app.frameEvent) || !app.running)
      break;

    CaptureFrame frame;
    frame.data = app.frame[frameIndex];
    if (!app.iface->getFrame(&frame))
      DEBUG_ERROR("Failed to get the frame");
    os_signalEvent(app.updateEvent);

    // wait for the client to finish with the previous frame
    while(fi->flags & KVMFR_FRAME_FLAG_UPDATE)
    {
      // this generally never happens
      usleep(1000);
    }

    switch(frame.format)
    {
      case CAPTURE_FMT_BGRA  : fi->type = FRAME_TYPE_BGRA  ; break;
      case CAPTURE_FMT_RGBA  : fi->type = FRAME_TYPE_RGBA  ; break;
      case CAPTURE_FMT_RGBA10: fi->type = FRAME_TYPE_RGBA10; break;
      case CAPTURE_FMT_YUV420: fi->type = FRAME_TYPE_YUV420; break;
      default:
        DEBUG_ERROR("Unsupported frame format %d, skipping frame", frame.format);
        continue;
    }

    fi->width   = frame.width;
    fi->height  = frame.height;
    fi->stride  = frame.stride;
    fi->pitch   = frame.pitch;
    fi->dataPos = app.frameOffset[frameIndex];

    INTERLOCKED_OR8(&fi->flags, KVMFR_FRAME_FLAG_UPDATE);

    if (++frameIndex == MAX_FRAMES)
      frameIndex = 0;
  }
  DEBUG_INFO("Frame thread stopped");
  return 0;
}

bool startThreads()
{
  app.running = true;
  if (!os_createThread("CursorThread", pointerThread, NULL, &app.pointerThread))
  {
    DEBUG_ERROR("Failed to create the pointer thread");
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
  os_signalEvent(app.frameEvent  );
  os_signalEvent(app.pointerEvent);

  if (app.frameThread && !os_joinThread(app.frameThread, NULL))
  {
    DEBUG_WARN("Failed to join the frame thread");
    ok = false;
  }
  app.frameThread = NULL;

  if (app.pointerThread && !os_joinThread(app.pointerThread, NULL))
  {
    DEBUG_WARN("Failed to join the pointer thread");
    ok = false;
  }
  app.pointerThread = NULL;

  return ok;
}

static bool captureStart()
{
  DEBUG_INFO("Using            : %s", app.iface->getName());

  const unsigned int maxFrameSize = app.iface->getMaxFrameSize();
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
  app.pointerData      = (uint8_t *)ALIGN_UP(shmemMap + sizeof(KVMFRHeader));
  app.pointerDataSize  = 1048576; // 1MB fixed for pointer size, should be more then enough
  app.pointerOffset    = app.pointerData - shmemMap;
  app.frames           = (uint8_t *)ALIGN_UP(app.pointerData + app.pointerDataSize);
  app.frameSize        = ALIGN_DN((shmemSize - (app.frames - shmemMap)) / MAX_FRAMES);

  DEBUG_INFO("Max Cursor Size  : %u MiB"     , app.pointerDataSize / 1048576);
  DEBUG_INFO("Max Frame Size   : %u MiB"     , app.frameSize      / 1048576);
  DEBUG_INFO("Cursor           : 0x%" PRIXPTR " (0x%08x)", (uintptr_t)app.pointerData, app.pointerOffset);

  for (int i = 0; i < MAX_FRAMES; ++i)
  {
    app.frame      [i] = app.frames + i * app.frameSize;
    app.frameOffset[i] = app.frame[i] - shmemMap;
    DEBUG_INFO("Frame %d          : 0x%" PRIXPTR " (0x%08x)", i, (uintptr_t)app.frame[i], app.frameOffset[i]);
  }

  CaptureInterface * iface = NULL;
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

  app.iface      = iface;
  app.frameEvent = os_createEvent();
  if (!app.frameEvent)
  {
    DEBUG_ERROR("Failed to create the frame event");
    exitcode = -1;
    goto exit;
  }

  app.updateEvent = os_createEvent();
  if (!app.updateEvent)
  {
    DEBUG_ERROR("Failed to create the update event");
    exitcode = -1;
    goto exit;
  }

  app.pointerEvent = os_createEvent();
  if (!app.pointerEvent)
  {
    DEBUG_ERROR("Failed to create the pointer event");
    exitcode = -1;
    goto exit;
  }

  // initialize the shared memory headers
  memcpy(app.shmHeader->magic, KVMFR_HEADER_MAGIC, sizeof(KVMFR_HEADER_MAGIC));
  app.shmHeader->version = KVMFR_HEADER_VERSION;

  // zero and notify the client we are starting
  memset(&(app.shmHeader->frame ), 0, sizeof(KVMFRFrame ));
  memset(&(app.shmHeader->cursor), 0, sizeof(KVMFRCursor));
  app.shmHeader->flags &= ~KVMFR_HEADER_FLAG_RESTART;

  if (!captureStart())
  {
    exitcode = -1;
    goto exit;
  }

  // start signalled
  os_signalEvent(app.updateEvent);

  volatile char * flags = (volatile char *)&(app.shmHeader->flags);

  while(app.running)
  {
    INTERLOCKED_AND8(flags, ~(KVMFR_HEADER_FLAG_RESTART));

    // wait for one of the threads to flag an update
    if (!os_waitEvent(app.updateEvent) || !app.running)
      break;

    bool frameUpdate   = false;
    bool pointerUpdate = false;

retry_capture:
    switch(iface->capture(&frameUpdate, &pointerUpdate))
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

        if (!captureStart())
        {
          exitcode = -1;
          goto finish;
        }

        // start signalled
        os_signalEvent(app.updateEvent);
        continue;

      case CAPTURE_RESULT_ERROR:
        DEBUG_ERROR("Capture interface reported a fatal error");
        exitcode = -1;
        goto finish;
    }

    if (!frameUpdate && !pointerUpdate)
      goto retry_capture;

    if (frameUpdate && !os_signalEvent(app.frameEvent))
    {
      DEBUG_ERROR("Failed to signal the frame thread");
      exitcode = -1;
      goto finish;
    }

    if (pointerUpdate && !os_signalEvent(app.pointerEvent))
    {
      DEBUG_ERROR("Failed to signal the pointer thread");
      exitcode = -1;
      goto finish;
    }
  }

finish:
  stopThreads();
exit:

  if (app.pointerEvent)
    os_freeEvent(app.pointerEvent);

  if (app.frameEvent)
    os_freeEvent(app.frameEvent);

  if (app.updateEvent)
    os_freeEvent(app.updateEvent);

  iface->deinit();
  iface->free();
fail:
  os_shmemUnmap();
  return exitcode;
}

void app_quit()
{
  app.running = false;
  os_signalEvent(app.updateEvent);
}