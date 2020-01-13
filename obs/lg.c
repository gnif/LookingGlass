#include <obs/obs-module.h>

#include <common/ivshmem.h>
#include <common/KVMFR.h>
#include <common/framebuffer.h>
#include <lgmp/client.h>

#include <stdio.h>

typedef struct
{
  obs_source_t    * context;
  bool              valid;
  char            * shmFile;
  uint32_t          width, height;
  FrameType         type;
  struct IVSHMEM    shmDev;
  PLGMPClient       lgmp;
  PLGMPClientQueue  frameQueue;
  gs_texture_t    * texture;
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
  lgUpdate(this, settings);
  return this;
}

static void deinit(LGPlugin * this)
{
  lgmpClientFree(&this->lgmp);

  if (this->shmFile)
  {
    bfree(this->shmFile);
    this->shmFile = NULL;
  }

  if (this->shmDev.mem)
    ivshmemClose(&this->shmDev);

  this->valid = false;
}

static void lgDestroy(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  deinit(this);
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

static void lgUpdate(void * data, obs_data_t * settings)
{
  LGPlugin * this = (LGPlugin *)data;

  deinit(this);
  this->shmFile = bstrdup(obs_data_get_string(settings, "shmFile"));
  if (!ivshmemOpenDev(&this->shmDev, this->shmFile))
    return;

  if (lgmpClientInit(this->shmDev.mem, this->shmDev.size, &this->lgmp) != LGMP_OK)
    return;

  if (lgmpClientSubscribe(this->lgmp, LGMP_Q_FRAME, &this->frameQueue) != LGMP_OK)
    return;

  this->valid = true;
}

static void lgVideoTick(void * data, float seconds)
{
  LGPlugin * this = (LGPlugin *)data;

  if (!this->valid)
    return;

  LGMP_STATUS status;
  LGMPMessage msg;

  if ((status = lgmpClientAdvanceToLast(this->frameQueue)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_EMPTY)
      return;

    printf("lgmpClientAdvanceToLast: %s\n", lgmpStatusString(status));
    this->valid = false;
    return;
  }

  if ((status = lgmpClientProcess(this->frameQueue, &msg)) != LGMP_OK)
  {
    if (status == LGMP_ERR_QUEUE_EMPTY)
      return;

    printf("lgmpClientProcess: %s\n", lgmpStatusString(status));
    this->valid = false;
    return;
  }

  obs_enter_graphics();

  KVMFRFrame * frame = (KVMFRFrame *)msg.mem;
  if (this->width  != frame->width  ||
      this->height != frame->height ||
      this->type   != frame->type)
  {
    if (this->texture)
      gs_texture_destroy(this->texture);
    this->texture = NULL;

    this->width   = frame->width;
    this->height  = frame->height;
    this->type    = frame->type;
  }

  if (!this->texture)
  {
    enum gs_color_format format;
    switch(this->type)
    {
      case FRAME_TYPE_BGRA  : format = GS_BGRA       ; break;
      case FRAME_TYPE_RGBA  : format = GS_RGBA       ; break;
      case FRAME_TYPE_RGBA10: format = GS_R10G10B10A2; break;
      default:
        printf("invalid type %d\n", this->type);
        this->valid = false;
        obs_leave_graphics();
        return;
    }

    this->texture = gs_texture_create(
        this->width, this->height, format, 1, NULL, GS_DYNAMIC);

    if (!this->texture)
    {
      printf("create texture failed\n");
      this->valid = false;
      obs_leave_graphics();
      return;
    }
  }

  FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);

  uint8_t *texData;
  uint32_t linesize;
  gs_texture_map(this->texture, &texData, &linesize);
  if (linesize == frame->pitch)
    framebuffer_read(fb, texData, frame->height * frame->pitch);
  gs_texture_unmap(this->texture);

//  gs_texture_set_image(this->texture, frameData, frame->pitch, false);
  lgmpClientMessageDone(this->frameQueue);

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
  if (!this->valid)
    return 0;

  return this->width;
}

static uint32_t lgGetHeight(void * data)
{
  LGPlugin * this = (LGPlugin *)data;
  if (!this->valid)
    return 0;

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