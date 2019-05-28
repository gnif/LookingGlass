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

#include "interface/platform.h"
#include "interface/capture.h"
#include "dynamic/capture.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/KVMFR.h"
#include "common/crash.h"

#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)
#define MAX_FRAMES 2

struct app
{
  unsigned int clientInstance;

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
  bool             reinit;
  osThreadHandle * pointerThread;
  osThreadHandle * frameThread;
};

static struct app app;

static int pointerThread(void * opaque)
{
  DEBUG_INFO("Pointer thread started");

  volatile KVMFRCursor * ci = &(app.shmHeader->cursor);

  uint8_t        flags;
  bool           pointerValid   = false;
  bool           shapeValid     = false;
  unsigned int   clientInstance = 0;
  CapturePointer pointer        = { 0 };

  while(app.running)
  {
    bool resend = false;

    pointer.shapeUpdate = false;

    switch(app.iface->getPointer(&pointer))
    {
      case CAPTURE_RESULT_OK:
      {
        pointerValid = true;
        break;
      }

      case CAPTURE_RESULT_REINIT:
      {
        app.reinit = true;
        DEBUG_INFO("Pointer thread reinit");
        return 0;
      }

      case CAPTURE_RESULT_ERROR:
      {
        DEBUG_ERROR("Failed to get the pointer");
        return 0;
      }

      case CAPTURE_RESULT_TIMEOUT:
      {
        // if the pointer is valid and the client has restarted, send it
        if (pointerValid && clientInstance != app.clientInstance)
        {
          resend = true;
          break;
        }

        continue;
      }
    }

    clientInstance = app.clientInstance;

    // wait for the client to finish with the previous update
    while((ci->flags & ~KVMFR_CURSOR_FLAG_UPDATE) != 0 && app.running)
      usleep(1000);

    flags  = KVMFR_CURSOR_FLAG_UPDATE;
    ci->x  = pointer.x;
    ci->y  = pointer.y;
    flags |= KVMFR_CURSOR_FLAG_POS;
    if (pointer.visible)
      flags |= KVMFR_CURSOR_FLAG_VISIBLE;

    // if we have shape data
    if (pointer.shapeUpdate || (shapeValid && resend))
    {
      switch(pointer.format)
      {
        case CAPTURE_FMT_COLOR : ci->type = CURSOR_TYPE_COLOR       ; break;
        case CAPTURE_FMT_MONO  : ci->type = CURSOR_TYPE_MONOCHROME  ; break;
        case CAPTURE_FMT_MASKED: ci->type = CURSOR_TYPE_MASKED_COLOR; break;
        default:
          DEBUG_ERROR("Invalid pointer format: %d", pointer.format);
          continue;
      }

      ci->width   = pointer.width;
      ci->height  = pointer.height;
      ci->pitch   = pointer.pitch;
      ci->dataPos = app.pointerOffset;
      ++ci->version;
      shapeValid = true;
      flags |= KVMFR_CURSOR_FLAG_SHAPE;
    }

    // update the flags for the client
    ci->flags = flags;
  }

  DEBUG_INFO("Pointer thread stopped");
  return 0;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  volatile KVMFRFrame * fi = &(app.shmHeader->frame);

  bool         frameValid     = false;
  int          frameIndex     = 0;
  unsigned int clientInstance = 0;
  CaptureFrame frame          = { 0 };

  while(app.running)
  {
    frame.data = app.frame[frameIndex];

    switch(app.iface->getFrame(&frame))
    {
      case CAPTURE_RESULT_OK:
        break;

      case CAPTURE_RESULT_REINIT:
      {
        app.reinit = true;
        DEBUG_INFO("Frame thread reinit");
        return 0;
      }

      case CAPTURE_RESULT_ERROR:
      {
        DEBUG_ERROR("Failed to get the frame");
        return 0;
      }

      case CAPTURE_RESULT_TIMEOUT:
      {
        if (frameValid && clientInstance != app.clientInstance)
        {
          // resend the last frame
          if (--frameIndex < 0)
            frameIndex = MAX_FRAMES - 1;
          break;
        }

        continue;
      }
    }

    clientInstance = app.clientInstance;

    // wait for the client to finish with the previous frame
    while(fi->flags & KVMFR_FRAME_FLAG_UPDATE && app.running)
      usleep(1000);

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
    frameValid  = true;

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
  app.iface->stop();

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

static bool captureRestart()
{
  DEBUG_INFO("==== [ Capture Restart ] ====");
  if (!stopThreads())
    return false;

  if (!app.iface->deinit() || !app.iface->init(app.pointerData, app.pointerDataSize))
  {
    DEBUG_ERROR("Failed to reinitialize the capture device");
    return false;
  }

  if (!captureStart())
    return false;

  return true;
}

// this is called from the platform specific startup routine
int app_main(int argc, char * argv[])
{
  if (!installCrashHandler(os_getExecutable()))
    DEBUG_WARN("Failed to install the crash handler");

  // register capture interface options
  for(int i = 0; CaptureInterfaces[i]; ++i)
    if (CaptureInterfaces[i]->initOptions)
      CaptureInterfaces[i]->initOptions();

  // try load values from a config file
  option_load("looking-glass-host.ini");

  // parse the command line arguments
  if (!option_parse(argc, argv))
  {
    option_free();
    DEBUG_ERROR("Failure to parse the command line");
    return -1;
  }

  if (!option_validate())
  {
    option_free();
    return -1;
  }

  // perform platform specific initialization
  if (!app_init())
    return -1;

  unsigned int shmemSize = os_shmemSize();
  uint8_t    * shmemMap  = NULL;
  int          exitcode  = 0;

  DEBUG_INFO("Looking Glass Host (" BUILD_VERSION ")");
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

    if (iface->init(app.pointerData, app.pointerDataSize))
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

  app.iface = iface;

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

  volatile char * flags = (volatile char *)&(app.shmHeader->flags);

  while(app.running)
  {
    if (INTERLOCKED_AND8(flags, ~(KVMFR_HEADER_FLAG_RESTART)) & KVMFR_HEADER_FLAG_RESTART)
    {
      DEBUG_INFO("Client restarted");
      ++app.clientInstance;
    }

    if (app.reinit && !captureRestart())
    {
      exitcode = -1;
      goto exit;
    }
    app.reinit = false;

    switch(iface->capture())
    {
      case CAPTURE_RESULT_OK:
        break;

      case CAPTURE_RESULT_TIMEOUT:
        continue;

      case CAPTURE_RESULT_REINIT:
        if (!captureRestart())
        {
          exitcode = -1;
          goto exit;
        }
        app.reinit = false;
        continue;

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