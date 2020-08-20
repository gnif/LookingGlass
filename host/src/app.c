/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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
#include "common/thread.h"
#include "common/ivshmem.h"
#include "common/sysinfo.h"
#include "common/time.h"

#include <lgmp/host.h>

#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_FILE "looking-glass-host.ini"

#define ALIGN_DN(x) ((uintptr_t)(x) & ~0x7F)
#define ALIGN_UP(x) ALIGN_DN(x + 0x7F)

#define LGMP_Q_FRAME_LEN   2
#define LGMP_Q_POINTER_LEN 20

static const struct LGMPQueueConfig FRAME_QUEUE_CONFIG =
{
  .queueID     = LGMP_Q_FRAME,
  .numMessages = LGMP_Q_FRAME_LEN,
  .subTimeout  = 1000
};

static const struct LGMPQueueConfig POINTER_QUEUE_CONFIG =
{
  .queueID     = LGMP_Q_POINTER,
  .numMessages = LGMP_Q_POINTER_LEN,
  .subTimeout  = 1000
};

#define MAX_POINTER_SIZE (sizeof(KVMFRCursor) + (128 * 128 * 4))

enum AppState
{
  APP_STATE_RUNNING,
  APP_STATE_IDLE,
  APP_STATE_RESTART,
  APP_STATE_SHUTDOWN
};

struct app
{
  PLGMPHost     lgmp;

  PLGMPHostQueue pointerQueue;
  PLGMPMemory    pointerMemory[LGMP_Q_POINTER_LEN];
  LG_Lock        pointerLock;
  CapturePointer pointerInfo;
  PLGMPMemory    pointerShape;
  bool           pointerShapeValid;
  unsigned int   pointerIndex;

  size_t         maxFrameSize;
  PLGMPHostQueue frameQueue;
  PLGMPMemory    frameMemory[LGMP_Q_FRAME_LEN];
  unsigned int   frameIndex;

  CaptureInterface * iface;

  enum AppState state;
  LGTimer  * lgmpTimer;
  LGThread * frameThread;
};

static struct app app;

static bool lgmpTimer(void * opaque)
{
  LGMP_STATUS status;
  if ((status = lgmpHostProcess(app.lgmp)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    app.state = APP_STATE_SHUTDOWN;
    return false;
  }

  return true;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  bool         frameValid     = false;
  bool         repeatFrame    = false;
  CaptureFrame frame          = { 0 };
  const long   pageSize       = sysinfo_getPageSize();

  while(app.state == APP_STATE_RUNNING)
  {
    //wait until there is room in the queue
    if(lgmpHostQueuePending(app.frameQueue) == LGMP_Q_FRAME_LEN)
    {
      usleep(1);
      continue;
    }

    switch(app.iface->waitFrame(&frame))
    {
      case CAPTURE_RESULT_OK:
        repeatFrame = false;
        break;

      case CAPTURE_RESULT_REINIT:
      {
        app.state = APP_STATE_RESTART;
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
        if (frameValid && lgmpHostQueueNewSubs(app.frameQueue) > 0)
        {
          // resend the last frame
          repeatFrame = true;
          break;
        }

        continue;
      }
    }

    LGMP_STATUS status;

    // if we are repeating a frame just send the last frame again
    if (repeatFrame)
    {
      if ((status = lgmpHostQueuePost(app.frameQueue, 0, app.frameMemory[app.frameIndex])) != LGMP_OK)
        DEBUG_ERROR("%s", lgmpStatusString(status));
      continue;
    }

    // we increment the index first so that if we need to repeat a frame
    // the index still points to the latest valid frame
    if (++app.frameIndex == LGMP_Q_FRAME_LEN)
      app.frameIndex = 0;

    KVMFRFrame * fi = lgmpHostMemPtr(app.frameMemory[app.frameIndex]);
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
    fi->offset  = pageSize - FrameBufferStructSize;
    frameValid  = true;

    // put the framebuffer on the border of the next page
    // this is to allow for aligned DMA transfers by the receiver
    FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)fi) + fi->offset);
    framebuffer_prepare(fb);

    /* we post and then get the frame, this is intentional! */
    if ((status = lgmpHostQueuePost(app.frameQueue, 0, app.frameMemory[app.frameIndex])) != LGMP_OK)
    {
      DEBUG_ERROR("%s", lgmpStatusString(status));
      continue;
    }
    app.iface->getFrame(fb);
  }
  DEBUG_INFO("Frame thread stopped");
  return 0;
}

bool startThreads()
{
  app.state = APP_STATE_RUNNING;
  if (!lgCreateThread("FrameThread", frameThread, NULL, &app.frameThread))
  {
    DEBUG_ERROR("Failed to create the frame thread");
    return false;
  }

  return true;
}

bool stopThreads()
{
  bool ok = true;

  app.iface->stop();
  if (app.state != APP_STATE_SHUTDOWN)
    app.state = APP_STATE_IDLE;

  if (app.frameThread && !lgJoinThread(app.frameThread, NULL))
  {
    DEBUG_WARN("Failed to join the frame thread");
    ok = false;
  }
  app.frameThread = NULL;

  return ok;
}

static bool captureStart()
{
  if (app.state == APP_STATE_IDLE)
  {
    if (!app.iface->init())
    {
      DEBUG_ERROR("Initialize the capture device");
      return false;
    }
  }

  const unsigned int maxFrameSize = app.iface->getMaxFrameSize();
  if (maxFrameSize > app.maxFrameSize)
  {
    DEBUG_ERROR("Maximum frame size of %d bytes excceds maximum space available", maxFrameSize);
    return false;
  }
  DEBUG_INFO("Capture Size     : %u MiB (%u)", maxFrameSize / 1048576, maxFrameSize);

  DEBUG_INFO("==== [ Capture  Start ] ====");
  return startThreads();
}

static bool captureStop()
{
  DEBUG_INFO("==== [ Capture Stop ] ====");
  if (!stopThreads())
    return false;

  if (!app.iface->deinit())
  {
    DEBUG_ERROR("Failed to deinitialize the capture device");
    return false;
  }

  return true;
}

static bool captureRestart()
{
  return captureStop() && captureStart();
}

bool captureGetPointerBuffer(void ** data, uint32_t * size)
{
  // spin until there is room
  while(lgmpHostQueuePending(app.pointerQueue) == LGMP_Q_POINTER_LEN)
  {
    usleep(1);
    if (app.state == APP_STATE_RUNNING)
      return false;
  }

  PLGMPMemory mem = app.pointerMemory[app.pointerIndex];
  *data = ((uint8_t*)lgmpHostMemPtr(mem)) + sizeof(KVMFRCursor);
  *size = MAX_POINTER_SIZE - sizeof(KVMFRCursor);
  return true;
}

static void sendPointer(bool newClient)
{
  PLGMPMemory mem;
  if (app.pointerInfo.shapeUpdate || newClient)
  {
    if (!newClient)
    {
      // swap the latest shape buffer out of rotation
      PLGMPMemory tmp  = app.pointerShape;
      app.pointerShape = app.pointerMemory[app.pointerIndex];
      app.pointerMemory[app.pointerIndex] = tmp;
    }

    // use the last known shape buffer
    mem = app.pointerShape;
  }
  else
    mem = app.pointerMemory[app.pointerIndex];

  if (++app.pointerIndex == LGMP_Q_POINTER_LEN)
    app.pointerIndex = 0;

  uint32_t flags = 0;
  KVMFRCursor *cursor = lgmpHostMemPtr(mem);

  if (app.pointerInfo.positionUpdate || newClient)
  {
    flags |= CURSOR_FLAG_POSITION;
    cursor->x = app.pointerInfo.x;
    cursor->y = app.pointerInfo.y;
  }

  if (app.pointerInfo.visible)
    flags |= CURSOR_FLAG_VISIBLE;

  if (app.pointerInfo.shapeUpdate)
  {
    cursor->hx     = app.pointerInfo.hx;
    cursor->hy     = app.pointerInfo.hy;
    cursor->width  = app.pointerInfo.width;
    cursor->height = app.pointerInfo.height;
    cursor->pitch  = app.pointerInfo.pitch;
    switch(app.pointerInfo.format)
    {
      case CAPTURE_FMT_COLOR : cursor->type = CURSOR_TYPE_COLOR       ; break;
      case CAPTURE_FMT_MONO  : cursor->type = CURSOR_TYPE_MONOCHROME  ; break;
      case CAPTURE_FMT_MASKED: cursor->type = CURSOR_TYPE_MASKED_COLOR; break;

      default:
        DEBUG_ERROR("Invalid pointer type");
        return;
    }

    app.pointerShapeValid = true;
  }

  if ((app.pointerInfo.shapeUpdate || newClient) && app.pointerShapeValid)
    flags |= CURSOR_FLAG_SHAPE;

  LGMP_STATUS status;
  while ((status = lgmpHostQueuePost(app.pointerQueue, flags, mem)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_FULL)
    {
      usleep(1);
      continue;
    }

    DEBUG_ERROR("lgmpHostQueuePost Failed (Pointer): %s", lgmpStatusString(status));
    break;
  }
}

void capturePostPointerBuffer(CapturePointer pointer)
{
  LG_LOCK(app.pointerLock);

  int x = app.pointerInfo.x;
  int y = app.pointerInfo.y;

  memcpy(&app.pointerInfo, &pointer, sizeof(CapturePointer));

  /* if there was not a position update, restore the x & y */
  if (!pointer.positionUpdate)
  {
    app.pointerInfo.x = x;
    app.pointerInfo.y = y;
  }

  sendPointer(false);

  LG_UNLOCK(app.pointerLock);
}

// this is called from the platform specific startup routine
int app_main(int argc, char * argv[])
{
  if (!installCrashHandler(os_getExecutable()))
    DEBUG_WARN("Failed to install the crash handler");

  ivshmemOptionsInit();

  // register capture interface options
  for(int i = 0; CaptureInterfaces[i]; ++i)
    if (CaptureInterfaces[i]->initOptions)
      CaptureInterfaces[i]->initOptions();

  // try load values from a config file
  const char * dataPath = os_getDataPath();
  if (!dataPath)
  {
    option_free();
    DEBUG_ERROR("Failed to get the application's data path");
    return -1;
  }

  const size_t len = strlen(dataPath) + sizeof(CONFIG_FILE) + 1;
  char configFile[len];
  snprintf(configFile, sizeof(configFile), "%s%s", dataPath, CONFIG_FILE);
  DEBUG_INFO("Looking for configuration file at: %s", configFile);
  if (option_load(configFile))
    DEBUG_INFO("Configuration file loaded");
  else
    DEBUG_INFO("Configuration file not found or invalid");

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

  DEBUG_INFO("Looking Glass Host (" BUILD_VERSION ")");

  struct IVSHMEM shmDev;
  if (!ivshmemOpen(&shmDev))
  {
    DEBUG_ERROR("Failed to open the IVSHMEM device");
    return -1;
  }

  int exitcode  = 0;
  DEBUG_INFO("IVSHMEM Size     : %u MiB", shmDev.size / 1048576);
  DEBUG_INFO("IVSHMEM Address  : 0x%" PRIXPTR, (uintptr_t)shmDev.mem);
  DEBUG_INFO("Max Pointer Size : %u KiB", (unsigned int)MAX_POINTER_SIZE / 1024);
  DEBUG_INFO("KVMFR Version    : %u", KVMFR_VERSION);

  const KVMFR udata = {
    .magic   = KVMFR_MAGIC,
    .version = KVMFR_VERSION,
    .hostver = BUILD_VERSION
  };

  LGMP_STATUS status;
  if ((status = lgmpHostInit(shmDev.mem, shmDev.size, &app.lgmp,
          sizeof(udata), (uint8_t *)&udata)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    goto fail;
  }

  if ((status = lgmpHostQueueNew(app.lgmp, FRAME_QUEUE_CONFIG, &app.frameQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    goto fail;
  }

  if ((status = lgmpHostQueueNew(app.lgmp, POINTER_QUEUE_CONFIG, &app.pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew Failed (Pointer): %s", lgmpStatusString(status));
    goto fail;
  }

  for(int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(app.lgmp, MAX_POINTER_SIZE, &app.pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      goto fail;
    }
    memset(lgmpHostMemPtr(app.pointerMemory[i]), 0, MAX_POINTER_SIZE);
  }

  app.pointerShapeValid = false;
  if ((status = lgmpHostMemAlloc(app.lgmp, MAX_POINTER_SIZE, &app.pointerShape)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shape): %s", lgmpStatusString(status));
    goto fail;
  }

  const long sz = sysinfo_getPageSize();
  app.maxFrameSize = lgmpHostMemAvail(app.lgmp);
  app.maxFrameSize = (app.maxFrameSize - (sz - 1)) & ~(sz - 1);
  app.maxFrameSize /= LGMP_Q_FRAME_LEN;
  DEBUG_INFO("Max Frame Size   : %u MiB", (unsigned int)(app.maxFrameSize / 1048576LL));

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(app.lgmp, app.maxFrameSize, sz, &app.frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Frame): %s", lgmpStatusString(status));
      goto fail;
    }
  }

  CaptureInterface * iface = NULL;
  for(int i = 0; CaptureInterfaces[i]; ++i)
  {
    iface = CaptureInterfaces[i];
    DEBUG_INFO("Trying           : %s", iface->getName());

    if (!iface->create(captureGetPointerBuffer, capturePostPointerBuffer))
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

  DEBUG_INFO("Using            : %s", iface->getName());

  app.state = APP_STATE_RUNNING;
  app.iface = iface;

  LG_LOCK_INIT(app.pointerLock);

  if (!lgCreateTimer(100, lgmpTimer, NULL, &app.lgmpTimer))
  {
    DEBUG_ERROR("Failed to create the LGMP timer");
    goto fail;
  }

  while(app.state != APP_STATE_SHUTDOWN)
  {
    if(lgmpHostQueueHasSubs(app.pointerQueue) ||
        lgmpHostQueueHasSubs(app.frameQueue))
    {
      if (!captureStart())
      {
        exitcode = -1;
        goto exit;
      }
    }
    else
    {
      usleep(100000);
      continue;
    }

    while(app.state != APP_STATE_SHUTDOWN && (
          lgmpHostQueueHasSubs(app.pointerQueue) ||
          lgmpHostQueueHasSubs(app.frameQueue)))
    {
      if (app.state == APP_STATE_RESTART)
      {
        if (!captureRestart())
        {
          exitcode = -1;
          goto exit;
        }
        app.state = APP_STATE_RUNNING;
      }

      if (lgmpHostQueueNewSubs(app.pointerQueue) > 0)
      {
        LG_LOCK(app.pointerLock);
        sendPointer(true);
        LG_UNLOCK(app.pointerLock);
      }

      switch(iface->capture())
      {
        case CAPTURE_RESULT_OK:
          break;

        case CAPTURE_RESULT_TIMEOUT:
          continue;

        case CAPTURE_RESULT_REINIT:
          app.state = APP_STATE_RESTART;
          continue;

        case CAPTURE_RESULT_ERROR:
          DEBUG_ERROR("Capture interface reported a fatal error");
          exitcode = -1;
          goto finish;
      }
    }

    if (app.state != APP_STATE_SHUTDOWN)
      DEBUG_INFO("No subscribers, going to sleep...");
    captureStop();
  }

finish:
  stopThreads();

exit:
  lgTimerDestroy(app.lgmpTimer);
  LG_LOCK_FREE(app.pointerLock);

  iface->deinit();
  iface->free();
fail:

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&app.frameMemory[i]);
  for(int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&app.pointerMemory[i]);
  lgmpHostMemFree(&app.pointerShape);
  lgmpHostFree(&app.lgmp);

  ivshmemClose(&shmDev);
  return exitcode;
}

void app_quit()
{
  app.state = APP_STATE_SHUTDOWN;
}
