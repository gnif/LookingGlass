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

#include "interface/platform.h"
#include "interface/capture.h"
#include "dynamic/capture.h"
#include "common/version.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/KVMFR.h"
#include "common/crash.h"
#include "common/thread.h"
#include "common/ivshmem.h"
#include "common/sysinfo.h"
#include "common/time.h"
#include "common/stringutils.h"
#include "common/cpuinfo.h"
#include "common/util.h"
#include "common/array.h"

#include <lgmp/host.h>

#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CONFIG_FILE "looking-glass-host.ini"
#define POINTER_SHAPE_BUFFERS 3

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

#define MAX_POINTER_SIZE (sizeof(KVMFRCursor) + (512 * 512 * 4))

enum AppState
{
  APP_STATE_RUNNING,
  APP_STATE_IDLE,
  APP_STATE_RESTART,
  APP_STATE_REINIT,
  APP_STATE_SHUTDOWN
};

struct app
{
  int exitcode;

  PLGMPHost lgmp;

  PLGMPHostQueue pointerQueue;
  PLGMPMemory    pointerMemory[LGMP_Q_POINTER_LEN];
  PLGMPMemory    pointerShapeMemory[POINTER_SHAPE_BUFFERS];
  LG_Lock        pointerLock;
  CapturePointer pointerInfo;
  PLGMPMemory    pointerShape;
  bool           pointerShapeValid;
  unsigned int   pointerIndex;
  unsigned int   pointerShapeIndex;

  long           pageSize;
  size_t         maxFrameSize;
  PLGMPHostQueue frameQueue;
  PLGMPMemory    frameMemory[LGMP_Q_FRAME_LEN];
  unsigned int   frameIndex;
  bool           frameValid;
  uint32_t       frameSerial;

  CaptureInterface * iface;

  enum AppState state;
  LGTimer  * lgmpTimer;
  LGThread * frameThread;
};

static struct app app;

static bool validateCaptureBackend(struct Option * opt, const char ** error)
{
  if (!*opt->value.x_string)
    return true;

  for (int i = 0; CaptureInterfaces[i]; ++i)
    if (!strcasecmp(opt->value.x_string, CaptureInterfaces[i]->shortName))
      return true;

  return false;
}

static struct Option options[] =
{
  {
    .module         = "app",
    .name           = "capture",
    .description    = "Select the capture backend",
    .type           = OPTION_TYPE_STRING,
    .value.x_string = "",
    .validator      = validateCaptureBackend,
  },
  {
    .module         = "app",
    .name           = "throttleFPS",
    .description    = "Throttle Capture Frame Rate",
    .type           = OPTION_TYPE_INT,
    .value.x_int    = 0,
  },
  {0}
};

static bool lgmpTimer(void * opaque)
{
  LGMP_STATUS status;
  if ((status = lgmpHostProcess(app.lgmp)) != LGMP_OK)
  {
    // something has messed up the LGMP headers, etc, we need to reinit
    if (status == LGMP_ERR_CORRUPTED)
    {
      DEBUG_ERROR("LGMP reported the shared memory has been corrrupted, "
          "attempting to recover");
      app.state = APP_STATE_REINIT;
      return false;
    }

    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    app.state = APP_STATE_SHUTDOWN;
    return false;
  }

  uint8_t data[LGMP_MSGS_SIZE];
  size_t size;
  while((status = lgmpHostReadData(app.pointerQueue, &data, &size)) == LGMP_OK)
  {
    KVMFRMessage *msg = (KVMFRMessage *)data;
    switch(msg->type)
    {
      case KVMFR_MESSAGE_SETCURSORPOS:
      {
        KVMFRSetCursorPos *sp = (KVMFRSetCursorPos *)msg;
        os_setCursorPos(sp->x, sp->y);
        break;
      }
    }

    lgmpHostAckData(app.pointerQueue);
  }

  return true;
}

static bool sendFrame(void)
{
  CaptureFrame frame = { 0 };
  bool repeatFrame = false;

  //wait until there is room in the queue
  while(app.state == APP_STATE_RUNNING &&
      lgmpHostQueuePending(app.frameQueue) == LGMP_Q_FRAME_LEN)
  {
    usleep(1);
    continue;
  }

  if (app.state != APP_STATE_RUNNING)
    return false;

  switch(app.iface->waitFrame(&frame, app.maxFrameSize))
  {
    case CAPTURE_RESULT_OK:
      // reading the new subs count zeros it
      lgmpHostQueueNewSubs(app.frameQueue);
      break;

    case CAPTURE_RESULT_REINIT:
    {
      app.state = APP_STATE_RESTART;
      DEBUG_INFO("Frame thread reinit");
      return false;
    }

    case CAPTURE_RESULT_ERROR:
    {
      DEBUG_ERROR("Failed to get the frame");
      return false;
    }

    case CAPTURE_RESULT_TIMEOUT:
    {
      if (app.frameValid && lgmpHostQueueNewSubs(app.frameQueue) > 0)
      {
        // resend the last frame
        repeatFrame = app.iface->asyncCapture;
        break;
      }

      return true;
    }
  }

  LGMP_STATUS status;

  // if we are repeating a frame just send the last frame again
  if (repeatFrame)
  {
    if ((status = lgmpHostQueuePost(app.frameQueue, 0,
           app.frameMemory[app.frameIndex])) != LGMP_OK)
      DEBUG_ERROR("%s", lgmpStatusString(status));
    return true;
  }

  // we increment the index first so that if we need to repeat a frame
  // the index still points to the latest valid frame
  if (++app.frameIndex == LGMP_Q_FRAME_LEN)
    app.frameIndex = 0;

  KVMFRFrame * fi = lgmpHostMemPtr(app.frameMemory[app.frameIndex]);
  switch(frame.format)
  {
    case CAPTURE_FMT_BGRA   : fi->type = FRAME_TYPE_BGRA   ; break;
    case CAPTURE_FMT_RGBA   : fi->type = FRAME_TYPE_RGBA   ; break;
    case CAPTURE_FMT_RGBA10 : fi->type = FRAME_TYPE_RGBA10 ; break;
    case CAPTURE_FMT_RGBA16F: fi->type = FRAME_TYPE_RGBA16F; break;
    default:
      DEBUG_ERROR("Unsupported frame format %d, skipping frame", frame.format);
      return true;
  }

  switch(frame.rotation)
  {
    case CAPTURE_ROT_0  : fi->rotation = FRAME_ROT_0  ; break;
    case CAPTURE_ROT_90 : fi->rotation = FRAME_ROT_90 ; break;
    case CAPTURE_ROT_180: fi->rotation = FRAME_ROT_180; break;
    case CAPTURE_ROT_270: fi->rotation = FRAME_ROT_270; break;
    default:
      DEBUG_WARN("Unsupported frame rotation %d", frame.rotation);
      fi->rotation = FRAME_ROT_0;
      break;
  }

  fi->formatVer         = frame.formatVer;
  fi->frameSerial       = app.frameSerial++;
  fi->screenWidth       = frame.screenWidth;
  fi->screenHeight      = frame.screenHeight;
  fi->frameWidth        = frame.frameWidth;
  fi->frameHeight       = frame.frameHeight;
  fi->stride            = frame.stride;
  fi->pitch             = frame.pitch;
  fi->offset            = app.pageSize - sizeof(FrameBuffer);
  fi->flags             =
    (os_blockScreensaver() ?
     FRAME_FLAG_BLOCK_SCREENSAVER : 0) |
    (os_getAndClearPendingActivationRequest() ?
      FRAME_FLAG_REQUEST_ACTIVATION : 0) |
    (frame.truncated ?
      FRAME_FLAG_TRUNCATED : 0);

  fi->damageRectsCount  = frame.damageRectsCount;
  memcpy(fi->damageRects, frame.damageRects,
    frame.damageRectsCount * sizeof(FrameDamageRect));

  app.frameValid = true;

  // put the framebuffer on the border of the next page
  // this is to allow for aligned DMA transfers by the receiver
  FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)fi) + fi->offset);
  framebuffer_prepare(fb);

  /* we post and then get the frame, this is intentional! */
  if ((status = lgmpHostQueuePost(app.frameQueue, 0,
    app.frameMemory[app.frameIndex])) != LGMP_OK)
  {
    DEBUG_ERROR("%s", lgmpStatusString(status));
    return true;
  }

  app.iface->getFrame(fb, frame.frameHeight, app.frameIndex);
  return true;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  while(app.state == APP_STATE_RUNNING)
  {
    if (!sendFrame())
      break;
  }
  DEBUG_INFO("Frame thread stopped");
  return 0;
}

bool startThreads(void)
{
  app.state = APP_STATE_RUNNING;
  if (!app.iface->asyncCapture)
    return true;

  if (!lgCreateThread("FrameThread", frameThread, NULL, &app.frameThread))
  {
    DEBUG_ERROR("Failed to create the frame thread");
    return false;
  }

  return true;
}

bool stopThreads(void)
{
  app.iface->stop();
  if (app.state != APP_STATE_SHUTDOWN &&
      app.state != APP_STATE_REINIT)
    app.state = APP_STATE_IDLE;

  if (!app.iface->asyncCapture)
    return true;

  if (app.frameThread)
  {
    if (!lgJoinThread(app.frameThread, NULL))
    {
      DEBUG_WARN("Failed to join the frame thread");
      app.frameThread = NULL;
      return false;
    }
    app.frameThread = NULL;
  }

  return true;
}

static bool captureStart(void)
{
  if (app.state == APP_STATE_IDLE)
  {
    if (!app.iface->init())
    {
      DEBUG_ERROR("Failed to initialize the capture device");
      return false;
    }

    if (app.iface->start && !app.iface->start())
    {
      DEBUG_ERROR("Failed to start the capture device");
      return false;
    }
  }

  DEBUG_INFO("==== [ Capture Start ] ====");
  return true;
}

static bool captureStop(void)
{
  DEBUG_INFO("==== [ Capture Stop ] ====");

  if (!app.iface->deinit())
  {
    DEBUG_ERROR("Failed to deinitialize the capture device");
    return false;
  }

  return true;
}

bool captureGetPointerBuffer(void ** data, uint32_t * size)
{
  PLGMPMemory mem = app.pointerShapeMemory[app.pointerShapeIndex];
  *data = (uint8_t*)lgmpHostMemPtr(mem) + sizeof(KVMFRCursor);
  *size = MAX_POINTER_SIZE - sizeof(KVMFRCursor);
  return true;
}

static void postPointer(uint32_t flags, PLGMPMemory mem)
{
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

static void sendPointer(bool newClient)
{
  // new clients need the last known shape and current position
  if (newClient)
  {
    PLGMPMemory mem;
    if (app.pointerShapeValid)
      mem = app.pointerShape;
    else
    {
      mem = app.pointerMemory[app.pointerIndex];
      if (++app.pointerIndex == LGMP_Q_POINTER_LEN)
        app.pointerIndex = 0;
    }

    // update the saved details with the current cursor position
    KVMFRCursor *cursor = lgmpHostMemPtr(mem);
    cursor->x = app.pointerInfo.x;
    cursor->y = app.pointerInfo.y;

    const uint32_t flags = CURSOR_FLAG_POSITION |
      (app.pointerShapeValid   ? CURSOR_FLAG_SHAPE   : 0) |
      (app.pointerInfo.visible ? CURSOR_FLAG_VISIBLE : 0);

    postPointer(flags, mem);
    return;
  }

  uint32_t flags = 0;
  PLGMPMemory mem;
  if (app.pointerInfo.shapeUpdate)
  {
    mem = app.pointerShapeMemory[app.pointerShapeIndex];
    if (++app.pointerShapeIndex == POINTER_SHAPE_BUFFERS)
      app.pointerShapeIndex = 0;
  }
  else
  {
    mem = app.pointerMemory[app.pointerIndex];
    if (++app.pointerIndex == LGMP_Q_POINTER_LEN)
      app.pointerIndex = 0;
  }
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
    flags |= CURSOR_FLAG_SHAPE;

    app.pointerShape = mem;
  }

  postPointer(flags, mem);
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

static void lgmpShutdown(void)
{
  if (app.lgmpTimer)
    lgTimerDestroy(app.lgmpTimer);

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    lgmpHostMemFree(&app.frameMemory[i]);
  for(int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
    lgmpHostMemFree(&app.pointerMemory[i]);
  for(int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
    lgmpHostMemFree(&app.pointerShapeMemory[i]);
  lgmpHostFree(&app.lgmp);

  app.pointerShapeValid = false;
}

typedef struct KVMFRUserData
{
  size_t    size;
  size_t    used;
  uint8_t * data;
}
KVMFRUserData;

static bool appendData(KVMFRUserData * dst, const void * src, const size_t size)
{
  if (size > dst->size - dst->used)
  {
    size_t newSize = dst->size + max(1024, size);
    dst->data = realloc(dst->data, newSize);
    if (!dst->data)
    {
      DEBUG_ERROR("Out of memory");
      return false;
    }

    memset(dst->data + dst->size, 0, newSize - dst->size);
    dst->size = newSize;
  }

  memcpy(dst->data + dst->used, src, size);
  dst->used += size;
  return true;
}

static bool newKVMFRData(KVMFRUserData * dst)
{
  {
    KVMFR kvmfr =
    {
      .magic    = KVMFR_MAGIC,
      .version  = KVMFR_VERSION,
      .features = os_hasSetCursorPos() ? KVMFR_FEATURE_SETCURSORPOS : 0
    };
    strncpy(kvmfr.hostver, BUILD_VERSION, sizeof(kvmfr.hostver) - 1);
    appendData(dst, &kvmfr, sizeof(kvmfr));
  }

  {
    int cpus, cores, sockets;
    char model[1024];
    if (!lgCPUInfo(model, sizeof(model), &cpus, &cores, &sockets))
      return false;

    KVMFRRecord_VMInfo vmInfo =
    {
      .cpus    = cpus,
      .cores   = cores,
      .sockets = sockets,
    };

    const uint8_t * uuid = os_getUUID();
    if (uuid)
      memcpy(vmInfo.uuid, uuid, 16);

    strncpy(vmInfo.capture, app.iface->getName(), sizeof(vmInfo.capture) - 1);

    const int modelLen = strlen(model) + 1;
    const KVMFRRecord record =
    {
      .type = KVMFR_RECORD_VMINFO,
      .size = sizeof(vmInfo) + modelLen
    };

    if (!appendData(dst, &record, sizeof(record)) ||
        !appendData(dst, &vmInfo, sizeof(vmInfo)) ||
        !appendData(dst, model  , modelLen      ))
      return false;
  }

  {
    KVMFRRecord_OSInfo osInfo =
    {
      .os = os_getKVMFRType()
    };

    const char * osName = os_getOSName();
    if (!osName)
      osName = "";
    const int osNameLen = strlen(osName) + 1;

    KVMFRRecord record =
    {
      .type = KVMFR_RECORD_OSINFO,
      .size = sizeof(osInfo) + osNameLen
    };

    if (!appendData(dst, &record, sizeof(record)) ||
        !appendData(dst, &osInfo, sizeof(osInfo)) ||
        !appendData(dst, osName , osNameLen))
      return false;
  }

  return true;
}

static bool lgmpSetup(struct IVSHMEM * shmDev)
{
  KVMFRUserData udata = { 0 };
  if (!newKVMFRData(&udata))
    return false;

  LGMP_STATUS status;
  if ((status = lgmpHostInit(shmDev->mem, shmDev->size, &app.lgmp,
          udata.used, udata.data)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostInit Failed: %s", lgmpStatusString(status));
    goto fail_init;
  }

  if ((status = lgmpHostQueueNew(app.lgmp, FRAME_QUEUE_CONFIG, &app.frameQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s", lgmpStatusString(status));
    goto fail_lgmp;
  }

  if ((status = lgmpHostQueueNew(app.lgmp, POINTER_QUEUE_CONFIG, &app.pointerQueue)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueNew Failed (Pointer): %s", lgmpStatusString(status));
    goto fail_lgmp;
  }

  for(int i = 0; i < LGMP_Q_POINTER_LEN; ++i)
  {
    if ((status = lgmpHostMemAlloc(app.lgmp, sizeof(KVMFRCursor), &app.pointerMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer): %s", lgmpStatusString(status));
      goto fail_lgmp;
    }
    memset(lgmpHostMemPtr(app.pointerMemory[i]), 0, sizeof(KVMFRCursor));
  }

  for(int i = 0; i < POINTER_SHAPE_BUFFERS; ++i)
  {
    if ((status = lgmpHostMemAlloc(app.lgmp, MAX_POINTER_SIZE, &app.pointerShapeMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Pointer Shapes): %s", lgmpStatusString(status));
      goto fail_lgmp;
    }
    memset(lgmpHostMemPtr(app.pointerShapeMemory[i]), 0, MAX_POINTER_SIZE);
  }

  app.maxFrameSize = lgmpHostMemAvail(app.lgmp);
  app.maxFrameSize = (app.maxFrameSize - (app.pageSize - 1)) & ~(app.pageSize - 1);
  app.maxFrameSize /= LGMP_Q_FRAME_LEN;
  DEBUG_INFO("Max Frame Size   : %u MiB", (unsigned int)(app.maxFrameSize / 1048576LL));

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(app.lgmp, app.maxFrameSize,
            app.pageSize, &app.frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Frame): %s", lgmpStatusString(status));
      goto fail_lgmp;
    }
  }

  if (!lgCreateTimer(10, lgmpTimer, NULL, &app.lgmpTimer))
  {
    DEBUG_ERROR("Failed to create the LGMP timer");
    goto fail_lgmp;
  }

  free(udata.data);
  return true;

fail_lgmp:
  lgmpShutdown();

fail_init:
  free(udata.data);
  return false;
}

// this is called from the platform specific startup routine
int app_main(int argc, char * argv[])
{
  if (!installCrashHandler(os_getExecutable()))
    DEBUG_WARN("Failed to install the crash handler");

  app.state = APP_STATE_RUNNING;
  ivshmemOptionsInit();

  // register capture interface options
  for(int i = 0; CaptureInterfaces[i]; ++i)
    if (CaptureInterfaces[i]->initOptions)
      CaptureInterfaces[i]->initOptions();

  option_register(options);

  // try load values from a config file
  const char * dataPath = os_getDataPath();
  if (!dataPath)
  {
    option_free();
    DEBUG_ERROR("Failed to get the application's data path");
    return LG_HOST_EXIT_FATAL;
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
    return LG_HOST_EXIT_FATAL;
  }

  if (!option_validate())
  {
    option_free();
    return LG_HOST_EXIT_FATAL;
  }

  // perform platform specific initialization
  if (!app_init())
    return LG_HOST_EXIT_FATAL;

  DEBUG_INFO("Looking Glass Host (%s)", BUILD_VERSION);
  lgDebugCPU();

  struct IVSHMEM shmDev = { 0 };
  if (!ivshmemInit(&shmDev))
  {
    DEBUG_ERROR("Failed to find the IVSHMEM device");
    return LG_HOST_EXIT_FATAL;
  }

  if (!ivshmemOpen(&shmDev))
  {
    DEBUG_ERROR("Failed to open the IVSHMEM device");
    return LG_HOST_EXIT_FATAL;
  }

  int exitcode  = 0;
  DEBUG_INFO("IVSHMEM Size     : %u MiB", shmDev.size / 1048576);
  DEBUG_INFO("IVSHMEM Address  : 0x%" PRIXPTR, (uintptr_t)shmDev.mem);
  DEBUG_INFO("Max Pointer Size : %u KiB", (unsigned int)MAX_POINTER_SIZE / 1024);
  DEBUG_INFO("KVMFR Version    : %u", KVMFR_VERSION);

  app.pageSize          = sysinfo_getPageSize();
  app.frameValid        = false;
  app.pointerShapeValid = false;

  int throttleFps = option_get_int("app", "throttleFPS");
  int throttleUs = throttleFps ? 1000000 / throttleFps : 0;
  uint64_t previousFrameTime = 0;

  const char * ifaceName = option_get_string("app", "capture");
  CaptureInterface * iface = NULL;
  for(int i = 0; CaptureInterfaces[i]; ++i)
  {
    iface = CaptureInterfaces[i];
    if (*ifaceName && strcasecmp(ifaceName, iface->shortName))
      continue;

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
    if (*ifaceName)
      DEBUG_ERROR("Specified capture interface not supported");
    else
      DEBUG_ERROR("Failed to find a supported capture interface");
    exitcode = LG_HOST_EXIT_FAILED;
    goto fail_lgmp;
  }

  DEBUG_INFO("Using            : %s", iface->getName());
  DEBUG_INFO("Capture Method   : %s", iface->asyncCapture ?
      "Asynchronous" : "Synchronous");

  app.iface = iface;

  if (!lgmpSetup(&shmDev))
  {
    exitcode = LG_HOST_EXIT_FATAL;
    goto fail_ivshmem;
  }

  LG_LOCK_INIT(app.pointerLock);

  if (app.iface->start && !app.iface->start())
  {
    DEBUG_ERROR("Failed to start the capture interface");
    exitcode = LG_HOST_EXIT_FATAL;
    goto fail_lgmp;
  }

  while(app.state != APP_STATE_SHUTDOWN)
  {
    if (app.state == APP_STATE_REINIT)
    {
      DEBUG_INFO("Performing LGMP reinitialization");
      lgmpShutdown();
      if (!lgmpSetup(&shmDev))
        goto fail_lgmp;
      app.state = APP_STATE_RUNNING;
    }

    if (app.state == APP_STATE_IDLE)
    {
      if(lgmpHostQueueHasSubs(app.pointerQueue) ||
          lgmpHostQueueHasSubs(app.frameQueue))
      {
        if (!captureStart())
        {
          exitcode = LG_HOST_EXIT_FAILED;
          goto fail_capture;
        }

        if (!startThreads())
        {
          exitcode = LG_HOST_EXIT_FAILED;
          goto fail_threads;
        }
      }
      else
      {
        usleep(100000);
        continue;
      }
    }

    while(app.state != APP_STATE_SHUTDOWN && (
          lgmpHostQueueHasSubs(app.pointerQueue) ||
          lgmpHostQueueHasSubs(app.frameQueue)))
    {
      if (app.state == APP_STATE_RESTART || app.state == APP_STATE_REINIT)
        break;

      if (lgmpHostQueueNewSubs(app.pointerQueue) > 0)
      {
        LG_LOCK(app.pointerLock);
        sendPointer(true);
        LG_UNLOCK(app.pointerLock);
      }

      const uint64_t now = microtime();
      const uint64_t delta = now - previousFrameTime;
      if (delta < throttleUs)
      {
        nsleep((throttleUs - delta) * 1000);
        previousFrameTime = microtime();
      }
      else
        previousFrameTime = now;

      switch(iface->capture())
      {
        case CAPTURE_RESULT_OK:
          break;

        case CAPTURE_RESULT_TIMEOUT:
          if (!iface->asyncCapture)
            if (app.frameValid && lgmpHostQueueNewSubs(app.frameQueue) > 0)
            {
              LGMP_STATUS status;
              if ((status = lgmpHostQueuePost(app.frameQueue, 0,
                      app.frameMemory[app.frameIndex])) != LGMP_OK)
                DEBUG_ERROR("%s", lgmpStatusString(status));
            }

          continue;

        case CAPTURE_RESULT_REINIT:
          app.state = APP_STATE_RESTART;
          continue;

        case CAPTURE_RESULT_ERROR:
          DEBUG_ERROR("Capture interface reported a fatal error");
          exitcode = LG_HOST_EXIT_FAILED;
          goto fail_capture;
      }

      if (!iface->asyncCapture)
        sendFrame();
    }

    if (app.state != APP_STATE_SHUTDOWN)
    {
      if (!stopThreads())
      {
        exitcode = LG_HOST_EXIT_FAILED;
        goto fail_threads;
      }

      if (!captureStop())
      {
        exitcode = LG_HOST_EXIT_FAILED;
        goto fail_capture;
      }

      continue;
    }

    break;
  }

  exitcode = app.exitcode;
  stopThreads();

fail_threads:
  captureStop();

fail_capture:
  iface->free();
  LG_LOCK_FREE(app.pointerLock);

fail_lgmp:
  lgmpShutdown();

fail_ivshmem:
  ivshmemClose(&shmDev);
  ivshmemFree(&shmDev);
  DEBUG_INFO("Host application exited");
  return exitcode;
}

void app_shutdown(void)
{
  app.state = APP_STATE_SHUTDOWN;
}

void app_quit(void)
{
  if (app.state == APP_STATE_SHUTDOWN)
  {
    DEBUG_INFO("Received second shutdown request, force quitting");
    exit(LG_HOST_EXIT_USER);
  }

  app.exitcode = LG_HOST_EXIT_USER;
  app_shutdown();
}
