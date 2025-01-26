/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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
  APP_STATE_TRANSITION_TO_IDLE,
  APP_STATE_REINIT_LGMP,
  APP_STATE_SHUTDOWN
};

struct app
{
  int exitcode;

  PLGMPHost lgmp;
  void *ivshmemBase;

  PLGMPHostQueue pointerQueue;
  PLGMPMemory    pointerMemory[LGMP_Q_POINTER_LEN];
  PLGMPMemory    pointerShapeMemory[POINTER_SHAPE_BUFFERS];
  LG_Lock        pointerLock;
  CapturePointer pointerInfo;
  PLGMPMemory    pointerShape;
  bool           pointerShapeValid;
  unsigned int   pointerIndex;
  unsigned int   pointerShapeIndex;

  unsigned       alignSize;
  size_t         maxFrameSize;
  PLGMPHostQueue frameQueue;
  PLGMPMemory    frameMemory[LGMP_Q_FRAME_LEN];
  KVMFRFrame   * frame      [LGMP_Q_FRAME_LEN];
  FrameBuffer  * frameBuffer[LGMP_Q_FRAME_LEN];

  unsigned int   captureIndex;
  unsigned int   readIndex;
  bool           frameValid;
  uint32_t       frameSerial;

  CaptureInterface * iface;
  bool captureStarted;

  enum AppState state, lastState;
  LGTimer  * lgmpTimer;
  LGThread * frameThread;
  bool threadsStarted;
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

static StringList getValuesCaptureBackend(struct Option * opt)
{
  StringList sl = stringlist_new(false);
  if (!sl)
    return NULL;

  for (int i = 0; CaptureInterfaces[i]; ++i)
    stringlist_push(sl, (char *)CaptureInterfaces[i]->shortName);

  return sl;
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
    .getValues      = getValuesCaptureBackend
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

inline static void setAppState(enum AppState state)
{
  if (app.state == APP_STATE_SHUTDOWN)
    return;
  app.lastState = app.state;
  app.state     = state;
}

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
      setAppState(APP_STATE_REINIT_LGMP);
      return false;
    }

    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));
    setAppState(APP_STATE_SHUTDOWN);
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

static bool sendFrame(CaptureResult result, bool * restart)
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

  // only wait if the result from the capture was OK
  if (result == CAPTURE_RESULT_OK)
    result = app.iface->waitFrame(app.captureIndex, &frame, app.maxFrameSize);

  switch(result)
  {
    case CAPTURE_RESULT_OK:
      // reading the new subs count zeros it
      lgmpHostQueueNewSubs(app.frameQueue);
      break;

    case CAPTURE_RESULT_REINIT:
    {
      *restart = true;
      DEBUG_INFO("Frame thread reinit");
      return false;
    }

    case CAPTURE_RESULT_ERROR:
    {
      *restart = false;
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
           app.frameMemory[app.readIndex])) != LGMP_OK)
      DEBUG_ERROR("%s", lgmpStatusString(status));
    return true;
  }

  KVMFRFrame * fi = app.frame[app.captureIndex];
  KVMFRFrameFlags flags =
    (frame.hdr   ? FRAME_FLAG_HDR    : 0) |
    (frame.hdrPQ ? FRAME_FLAG_HDR_PQ : 0);

  switch(frame.format)
  {
    case CAPTURE_FMT_BGRA:
      fi->type = FRAME_TYPE_BGRA;
      break;

    case CAPTURE_FMT_RGBA:
      fi->type = FRAME_TYPE_RGBA;
      break;

    case CAPTURE_FMT_RGBA10:
      fi->type = FRAME_TYPE_RGBA10;
      break;

    case CAPTURE_FMT_RGBA16F:
      fi->type  = FRAME_TYPE_RGBA16F;
      flags    |= FRAME_FLAG_HDR;
      break;

    case CAPTURE_FMT_BGR_32:
      fi->type = FRAME_TYPE_BGR_32;
      break;

    case CAPTURE_FMT_RGB_24:
      fi->type = FRAME_TYPE_RGB_24;
      break;

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

  if (os_blockScreensaver())
    flags |= FRAME_FLAG_BLOCK_SCREENSAVER;

  if (os_getAndClearPendingActivationRequest())
    flags |= FRAME_FLAG_REQUEST_ACTIVATION;

  if (frame.truncated)
    flags |= FRAME_FLAG_TRUNCATED;

  fi->formatVer         = frame.formatVer;
  fi->frameSerial       = app.frameSerial++;
  fi->screenWidth       = frame.screenWidth;
  fi->screenHeight      = frame.screenHeight;
  fi->dataWidth         = frame.dataWidth;
  fi->dataHeight        = frame.dataHeight;
  fi->frameWidth        = frame.frameWidth;
  fi->frameHeight       = frame.frameHeight;
  fi->colorMetadata     = frame.colorMetadata;
  fi->stride            = frame.stride;
  fi->pitch             = frame.pitch;
  // fi->offset is initialized at startup
  fi->flags             = flags;
  fi->damageRectsCount  = frame.damageRectsCount;
  memcpy(fi->damageRects, frame.damageRects,
    frame.damageRectsCount * sizeof(FrameDamageRect));

  app.frameValid = true;

  framebuffer_prepare(app.frameBuffer[app.captureIndex]);

  /* we post and then get the frame, this is intentional! */
  if ((status = lgmpHostQueuePost(app.frameQueue, 0,
    app.frameMemory[app.captureIndex])) != LGMP_OK)
  {
    DEBUG_ERROR("%s", lgmpStatusString(status));
    return true;
  }

  app.iface->getFrame(
    app.captureIndex,
    app.frameBuffer[app.captureIndex],
    app.maxFrameSize);

  app.readIndex = app.captureIndex;
  if (++app.captureIndex == LGMP_Q_FRAME_LEN)
    app.captureIndex = 0;
  return true;
}

static int frameThread(void * opaque)
{
  DEBUG_INFO("Frame thread started");

  while(app.state == APP_STATE_RUNNING)
  {
    bool restart = false;
    if (!sendFrame(CAPTURE_RESULT_OK, &restart))
    {
      if (restart)
        setAppState(APP_STATE_TRANSITION_TO_IDLE);
      break;
    }
  }
  DEBUG_INFO("Frame thread stopped");

  return 0;
}

bool startThreads(void)
{
  if (app.threadsStarted)
    return true;

  if (app.iface->asyncCapture)
    if (!lgCreateThread("FrameThread", frameThread, NULL, &app.frameThread))
    {
      DEBUG_ERROR("Failed to create the frame thread");
      return false;
    }

  app.threadsStarted = true;
  return true;
}

bool stopThreads(void)
{
  if (!app.threadsStarted)
    return true;

  app.iface->stop();

  if (app.iface->asyncCapture && app.frameThread)
  {
    if (!lgJoinThread(app.frameThread, NULL))
    {
      DEBUG_WARN("Failed to join the frame thread");
      app.frameThread = NULL;
      return false;
    }
    app.frameThread = NULL;
  }

  app.threadsStarted = false;
  return true;
}

static bool captureStart(void)
{
  if (app.captureStarted)
    return true;

  if (!app.iface->init(app.ivshmemBase, &app.alignSize))
  {
    DEBUG_ERROR("Failed to initialize the capture device");
    return false;
  }

  if (app.iface->start && !app.iface->start())
  {
    DEBUG_ERROR("Failed to start the capture device");
    return false;
  }

  DEBUG_INFO("==== [ Capture Start ] ====");
  app.captureStarted = true;
  return true;
}

static bool captureStop(void)
{
  if (!app.captureStarted)
    return true;

  DEBUG_INFO("==== [ Capture Stop ] ====");

  if (!app.iface->deinit())
  {
    DEBUG_ERROR("Failed to deinitialize the capture device");
    return false;
  }

  app.frameValid = false;
  app.captureStarted = false;
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

void capturePostPointerBuffer(const CapturePointer * pointer)
{
  LG_LOCK(app.pointerLock);

  int x = app.pointerInfo.x;
  int y = app.pointerInfo.y;

  memcpy(&app.pointerInfo, pointer, sizeof(CapturePointer));

  /* if there was not a position update, restore the x & y */
  if (!pointer->positionUpdate)
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
  dst->data = malloc(1024);
  if (!dst->data)
  {
    DEBUG_ERROR("Out of memory");
    return false;
  }
  dst->size = 1024;

  {
    KVMFR kvmfr =
    {
      .magic    = KVMFR_MAGIC,
      .version  = KVMFR_VERSION,
      .features = os_hasSetCursorPos() ? KVMFR_FEATURE_SETCURSORPOS : 0
    };
    strncpy(kvmfr.hostver, BUILD_VERSION, sizeof(kvmfr.hostver) - 1);
    if (!appendData(dst, &kvmfr, sizeof(kvmfr)))
      return false;
  }

  {
    int cpus, cores, sockets;
    char model[1024];
    if (!cpuInfo_get(model, sizeof(model), &cpus, &cores, &sockets))
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
    goto fail_init;

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
  app.maxFrameSize = (app.maxFrameSize - (app.alignSize - 1)) & ~(app.alignSize - 1);
  app.maxFrameSize /= LGMP_Q_FRAME_LEN;
  DEBUG_INFO("Max Frame Size   : %u MiB", (unsigned int)(app.maxFrameSize / 1048576LL));

  for(int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if ((status = lgmpHostMemAllocAligned(app.lgmp, app.maxFrameSize,
            app.alignSize, &app.frameMemory[i])) != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Frame): %s", lgmpStatusString(status));
      goto fail_lgmp;
    }

    app.frame[i] = lgmpHostMemPtr(app.frameMemory[i]);

    /* put the framebuffer on the border of the next page, this is to allow for
       aligned DMA transfers by the receiver */
    const unsigned alignOffset = app.alignSize - sizeof(FrameBuffer);
    app.frame[i]->offset = alignOffset;
    app.frameBuffer[i] = (FrameBuffer *)(((uint8_t*)app.frame[i]) + alignOffset);
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

  // make sure rng is actually seeded for LGMP
  srand((unsigned)time(NULL));

  app.lastState = APP_STATE_RUNNING;
  app.state     = APP_STATE_RUNNING;
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
    DEBUG_INFO("Configuration file not found or invalid, continuing anyway...");

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
  cpuInfo_log();

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
  app.ivshmemBase = shmDev.mem;

  int exitcode  = 0;
  DEBUG_INFO("IVSHMEM Size     : %u MiB", shmDev.size / 1048576);
  DEBUG_INFO("IVSHMEM Address  : 0x%" PRIXPTR, (uintptr_t)shmDev.mem);
  DEBUG_INFO("Max Pointer Size : %u KiB", (unsigned int)MAX_POINTER_SIZE / 1024);
  DEBUG_INFO("KVMFR Version    : %u", KVMFR_VERSION);

  app.alignSize         = sysinfo_getPageSize();
  app.frameValid        = false;
  app.pointerShapeValid = false;

  int throttleFps = option_get_int("app", "throttleFPS");
  int throttleUs = throttleFps ? 1000000 / throttleFps : 0;
  uint64_t previousFrameTime = 0;

  {
    const char * ifaceName = option_get_string("app", "capture");
    CaptureInterface * iface = NULL;
    for(int i = 0; CaptureInterfaces[i]; ++i)
    {
      if (*ifaceName)
      {
        if (strcasecmp(ifaceName, CaptureInterfaces[i]->shortName) != 0)
          continue;
      }
      else
      {
        /* do not try to init deprecated interfaces unless they are explicity
        selected in the host configuration */
        if (CaptureInterfaces[i]->deprecated)
          continue;
      }

      iface = CaptureInterfaces[i];
      DEBUG_INFO("Trying           : %s", iface->getName());

      if (!iface->create(
        captureGetPointerBuffer,
        capturePostPointerBuffer,
        ARRAY_LENGTH(app.frameBuffer)))
      {
        iface = NULL;
        continue;
      }

      app.iface = iface;
      if (captureStart())
        break;

      iface->free();
      iface = NULL;
    }

    if (!iface)
    {
      app.iface = NULL;

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
  }

  if (!lgmpSetup(&shmDev))
  {
    exitcode = LG_HOST_EXIT_FATAL;
    goto fail_ivshmem;
  }

  LG_LOCK_INIT(app.pointerLock);

  do
  {
    switch(app.state)
    {
      case APP_STATE_REINIT_LGMP:
        DEBUG_INFO("Performing LGMP reinitialization");
        lgmpShutdown();
        setAppState(app.lastState);
        if (!lgmpSetup(&shmDev))
          goto fail_lgmp;
        break;

      case APP_STATE_IDLE:
        // if there are no clients subscribed, just remain idle
        if (!lgmpHostQueueHasSubs(app.pointerQueue) &&
            !lgmpHostQueueHasSubs(app.frameQueue))
        {
          usleep(100000);
          continue;
        }

        // clients subscribed, start the capture
        if (!captureStart() || !startThreads())
        {
          exitcode = LG_HOST_EXIT_FAILED;
          goto fail;
        }
        setAppState(APP_STATE_RUNNING);
        break;

      case APP_STATE_TRANSITION_TO_IDLE:
        if (!stopThreads() || !captureStop())
        {
          exitcode = LG_HOST_EXIT_FAILED;
          goto fail;
        }
        setAppState(APP_STATE_IDLE);
        break;

      case APP_STATE_RUNNING:
      {
        // if there are no clients subscribed, go idle
        if (!lgmpHostQueueHasSubs(app.pointerQueue) &&
            !lgmpHostQueueHasSubs(app.frameQueue))
        {
          setAppState(APP_STATE_TRANSITION_TO_IDLE);
          break;
        }

        // if there is a brand new client, send them the pointer
        if (unlikely(lgmpHostQueueNewSubs(app.pointerQueue) > 0))
        {
          LG_LOCK(app.pointerLock);
          sendPointer(true);
          LG_UNLOCK(app.pointerLock);
        }

        const uint64_t delta = microtime() - previousFrameTime;
        if (delta < throttleUs)
        {
          const uint64_t us = throttleUs - delta;
          // only delay if the time is reasonable
          if (us > 1000)
            nsleep(us * 1000);
        }

        const uint64_t captureStartTime = microtime();

        const CaptureResult result = app.iface->capture(
          app.captureIndex, app.frameBuffer[app.captureIndex]);

        if (likely(result == CAPTURE_RESULT_OK))
          previousFrameTime = captureStartTime;
        else if (likely(result == CAPTURE_RESULT_TIMEOUT))
        {
          if (!app.iface->asyncCapture)
            if (unlikely(app.frameValid &&
                  lgmpHostQueueNewSubs(app.frameQueue) > 0))
            {
              LGMP_STATUS status;
              if ((status = lgmpHostQueuePost(app.frameQueue, 0,
                      app.frameMemory[app.readIndex])) != LGMP_OK)
                DEBUG_ERROR("%s", lgmpStatusString(status));
            }
        }
        else
        {
          switch(result)
          {
            case CAPTURE_RESULT_REINIT:
              setAppState(APP_STATE_TRANSITION_TO_IDLE);
              continue;

            case CAPTURE_RESULT_ERROR:
              DEBUG_ERROR("Capture interface reported a fatal error");
              exitcode = LG_HOST_EXIT_FAILED;
              goto fail;

            default:
              DEBUG_ASSERT("Invalid capture result");
          }
        }

        if (!app.iface->asyncCapture)
        {
          bool restart = false;
          if (!sendFrame(result, &restart) && restart)
            setAppState(APP_STATE_TRANSITION_TO_IDLE);
        }
        break;
      }

      case APP_STATE_SHUTDOWN:
        break;
    }
  }
  while(app.state != APP_STATE_SHUTDOWN);

  exitcode = app.exitcode;

fail:
  stopThreads();
  captureStop();
  app.iface->free();

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

void app_quit(int exitcode)
{
  if (app.state == APP_STATE_SHUTDOWN)
  {
    DEBUG_INFO("Received second shutdown request, force quitting");
    exit(LG_HOST_EXIT_USER);
  }

  app.exitcode = exitcode;
  app_shutdown();
}
