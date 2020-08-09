#define _GNU_SOURCE //needed for pthread_setname_np

#include <obs/obs-module.h>
#include <obs/util/threading.h>

#include <common/ivshmem.h>
#include <common/KVMFR.h>
#include <common/framebuffer.h>
#include <lgmp/client.h>

#include <stdio.h>
#include <unistd.h>

typedef enum
{
  STATE_STOPPED,
  STATE_OPEN,
  STATE_STARTING,
  STATE_RUNNING,
  STATE_STOPPING
}
LGState;

typedef struct
{
  obs_source_t    * context;
  LGState           state;
  char            * shmFile;
  uint32_t          width, height;
  FrameType         type;
  struct IVSHMEM    shmDev;
  PLGMPClient       lgmp;
  PLGMPClientQueue  frameQueue;
  gs_texture_t    * texture;
  uint8_t         * texData;
  uint32_t          linesize;

  pthread_t         frameThread;
  os_sem_t        * frameSem;
}
LGPlugin;

static void lgUpdate(void * data, obs_data_t * settings);

static const char * lgGetName(void * unused)
{
  return obs_module_text("Looking Glass Client");
}

static void * lgCreate(obs_data_t * settings, obs_source_t * context)
{
  LGPlugin * this = bzalloc(sizeof(LGPlugin));
  this->context = context;
  os_sem_init(&this->frameSem, 0);

  lgUpdate(this, settings);
  return this;
}

static void deinit(LGPlugin * this)
{
  switch(this->state)
  {
    case STATE_STARTING:
      /* wait for startup to finish */
      while(this->state == STATE_STARTING)
        usleep(1);
      /* fallthrough */

    case STATE_RUNNING:
    case STATE_STOPPING:
      this->state = STATE_STOPPING;
      pthread_join(this->frameThread, NULL);
      this->state = STATE_STOPPED;
      /* fallthrough */

    case STATE_OPEN:
      lgmpClientFree(&this->lgmp);
      ivshmemClose(&this->shmDev);
      break;

    case STATE_STOPPED:
      break;
  }

  if (this->shmFile)
  {
    bfree(this->shmFile);
    this->shmFile = NULL;
  }

  if (this->texture)
  {
    obs_enter_graphics();
    gs_texture_destroy(this->texture);
    gs_texture_unmap(this->texture);
    obs_leave_graphics();
    this->texture = NULL;
  }

  this->state = STATE_STOPPED;
}

static void lgDestroy(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  deinit(this);
  os_sem_destroy(this->frameSem);
  bfree(this);
}

static void lgGetDefaults(obs_data_t * defaults)
{
  obs_data_set_default_string(defaults, "shmFile", "/dev/shm/looking-glass");
}

static obs_properties_t * lgGetProperties(void * data)
{
  obs_properties_t * props = obs_properties_create();

  obs_properties_add_text(props, "shmFile", obs_module_text("SHM File"), OBS_TEXT_DEFAULT);

  return props;
}

static void * frameThread(void * data)
{
  LGPlugin * this = (LGPlugin *)data;

  if (lgmpClientSubscribe(this->lgmp, LGMP_Q_FRAME, &this->frameQueue) != LGMP_OK)
  {
    this->state = STATE_STOPPING;
    return NULL;
  }

  this->state = STATE_RUNNING;
  os_sem_post(this->frameSem);

  while(this->state == STATE_RUNNING)
  {
    LGMP_STATUS status;

    os_sem_wait(this->frameSem);
    if ((status = lgmpClientAdvanceToLast(this->frameQueue)) != LGMP_OK)
    {
      if (status != LGMP_ERR_QUEUE_EMPTY)
      {
        os_sem_post(this->frameSem);
        printf("lgmpClientAdvanceToLast: %s\n", lgmpStatusString(status));
        break;
      }
    }
    os_sem_post(this->frameSem);
    usleep(1000);
  }

  lgmpClientUnsubscribe(&this->frameQueue);
  this->state = STATE_STOPPING;
  return NULL;
}

static void lgUpdate(void * data, obs_data_t * settings)
{
  LGPlugin * this = (LGPlugin *)data;

  deinit(this);
  this->shmFile = bstrdup(obs_data_get_string(settings, "shmFile"));
  if (!ivshmemOpenDev(&this->shmDev, this->shmFile))
    return;

  this->state = STATE_OPEN;

  uint32_t udataSize;
  KVMFR * udata;

  if (lgmpClientInit(this->shmDev.mem, this->shmDev.size, &this->lgmp)
      != LGMP_OK)
    return;

  usleep(200000);

  if (lgmpClientSessionInit(this->lgmp, &udataSize, (uint8_t **)&udata)
      != LGMP_OK)
    return;

  if (udataSize != sizeof(KVMFR) ||
      memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) != 0 ||
      udata->version != KVMFR_VERSION)
  {
    printf("The host application is not compatible with this client\n");
    printf("Expected KVMFR version %d\n", KVMFR_VERSION);
    printf("This is not a Looking Glass error, do not report this\n");
    return;
  }

  this->state = STATE_STARTING;
  pthread_create(&this->frameThread, NULL, frameThread, this);
  pthread_setname_np(this->frameThread, "LGFrameThread");
}

static void lgVideoTick(void * data, float seconds)
{
  LGPlugin * this = (LGPlugin *)data;

  if (this->state != STATE_RUNNING)
    return;

  LGMP_STATUS status;
  LGMPMessage msg;

  os_sem_wait(this->frameSem);
  if (this->state != STATE_RUNNING)
  {
    os_sem_post(this->frameSem);
    return;
  }

  if ((status = lgmpClientAdvanceToLast(this->frameQueue)) != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_EMPTY)
    {
      os_sem_post(this->frameSem);
      printf("lgmpClientAdvanceToLast: %s\n", lgmpStatusString(status));
      return;
    }
  }

  if ((status = lgmpClientProcess(this->frameQueue, &msg)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_EMPTY)
    {
      os_sem_post(this->frameSem);
      return;
    }

    printf("lgmpClientProcess: %s\n", lgmpStatusString(status));
    this->state = STATE_STOPPING;
    os_sem_post(this->frameSem);
    return;
  }

  bool updateTexture = false;
  KVMFRFrame * frame = (KVMFRFrame *)msg.mem;
  if (this->width  != frame->width  ||
      this->height != frame->height ||
      this->type   != frame->type)
  {
    updateTexture = true;
    this->width   = frame->width;
    this->height  = frame->height;
    this->type    = frame->type;
  }

  if (!this->texture || updateTexture)
  {
    obs_enter_graphics();
    if (this->texture)
    {
      gs_texture_unmap(this->texture);
      gs_texture_destroy(this->texture);
    }
    this->texture = NULL;

    enum gs_color_format format;
    switch(this->type)
    {
      case FRAME_TYPE_BGRA  : format = GS_BGRA       ; break;
      case FRAME_TYPE_RGBA  : format = GS_RGBA       ; break;
      case FRAME_TYPE_RGBA10: format = GS_R10G10B10A2; break;
      default:
        printf("invalid type %d\n", this->type);
        os_sem_post(this->frameSem);
        obs_leave_graphics();
        return;
    }

    this->texture = gs_texture_create(
        this->width, this->height, format, 1, NULL, GS_DYNAMIC);

    if (!this->texture)
    {
      printf("create texture failed\n");
      os_sem_post(this->frameSem);
      obs_leave_graphics();
      return;
    }

    gs_texture_map(this->texture, &this->texData, &this->linesize);
    obs_leave_graphics();
  }

  FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);
  framebuffer_read(
      fb,
      this->texData,    // dst
      this->linesize,   // dstpitch
      frame->height,    // height
      frame->width,     // width
      4,                // bpp
      frame->pitch      // linepitch
  );

  lgmpClientMessageDone(this->frameQueue);
  os_sem_post(this->frameSem);

  obs_enter_graphics();
  gs_texture_unmap(this->texture);
  gs_texture_map(this->texture, &this->texData, &this->linesize);
  obs_leave_graphics();
}

static void lgVideoRender(void * data, gs_effect_t * effect)
{
  LGPlugin * this = (LGPlugin *)data;

  if (!this->texture)
    return;

  effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
  gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
  gs_effect_set_texture(image, this->texture);
  while (gs_effect_loop(effect, "Draw")) {
    gs_draw_sprite(this->texture, 0, 0, 0);
  }
}

static uint32_t lgGetWidth(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->width;
}

static uint32_t lgGetHeight(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  return this->height;
}

struct obs_source_info lg_source =
{
  .id             = "looking-glass-obs",
  .type           = OBS_SOURCE_TYPE_INPUT,
  .output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
                    OBS_SOURCE_DO_NOT_DUPLICATE,
  .get_name       = lgGetName,
  .create         = lgCreate,
  .destroy        = lgDestroy,
  .update         = lgUpdate,
  .get_defaults   = lgGetDefaults,
  .get_properties = lgGetProperties,
  .video_tick     = lgVideoTick,
  .video_render   = lgVideoRender,
  .get_width      = lgGetWidth,
  .get_height     = lgGetHeight,
//  .icon_type      = OBS_ICON_TYPE_DESKTOP_CAPTURE
};
