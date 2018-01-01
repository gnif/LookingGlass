/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include "lg-decoder.h"

#include "debug.h"
#include "memcpySSE.h"

#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL_syswm.h>
#include <va/va_glx.h>

#define SURFACE_NUM 3

#define NALU_AUD     9
#define SLICE_TYPE_P 0
#define SLICE_TYPE_B 1
#define SLICE_TYPE_I 2

struct Inst
{
  LG_RendererFormat   format;
  SDL_Window        * window;
  VADisplay           vaDisplay;
  int                 vaMajorVer, vaMinorVer;
  VASurfaceID         vaSurfaceID[SURFACE_NUM];
  VAConfigID          vaConfigID;
  VAContextID         vaContextID;
  int                 lastSID;
  int                 currentSID;
  VAPictureH264       curPic;
  VAPictureH264       oldPic;
  int                 frameNum;
  int                 fieldCount;
  VABufferID          picBufferID[SURFACE_NUM];
  VABufferID          matBufferID[SURFACE_NUM];
  VABufferID          sliBufferID[SURFACE_NUM];
  VABufferID          datBufferID[SURFACE_NUM];
  bool                t2First;
  int                 sliceType;
};

static const unsigned char MatrixBufferH264[] = {
  //ScalingList4x4[6][16]
  0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
  0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
  0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
  0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
  0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
  0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,
  //ScalingList8x8[2][64]
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static bool         lgd_h264_create          (void ** opaque);
static void         lgd_h264_destroy         (void  * opaque);
static bool         lgd_h264_initialize      (void  * opaque, const LG_RendererFormat format, SDL_Window * window);
static void         lgd_h264_deinitialize    (void  * opaque);
static LG_OutFormat lgd_h264_get_out_format  (void  * opaque);
static unsigned int lgd_h264_get_frame_pitch (void  * opaque);
static unsigned int lgd_h264_get_frame_stride(void  * opaque);
static bool         lgd_h264_decode          (void  * opaque, const uint8_t * src, size_t srcSize);
static bool         lgd_h264_get_buffer      (void  * opaque, uint8_t * dst, size_t dstSize);

static bool         lgd_h264_init_gl_texture  (void * opaque, GLenum target, GLuint texture, void ** ref);
static void         lgd_h264_free_gl_texture  (void * opaque, void * ref);
static bool         lgd_h264_update_gl_texture(void * opaque, void * ref);

#define check_surface(x, y, z) _check_surface(__LINE__, x, y, z)
static bool _check_surface(const unsigned int line, struct Inst * this, unsigned int sid, VASurfaceStatus *out)
{
  VASurfaceStatus surfStatus;
  VAStatus status = vaQuerySurfaceStatus(
    this->vaDisplay,
    this->vaSurfaceID[sid],
    &surfStatus
  );

  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaQuerySurfaceStatus: %s", vaErrorStr(status));
    return false;
  }

#if 0
  DEBUG_INFO("L%d: surface %u status: %d", line, sid, surfStatus);
#endif
  if (out)
    *out = surfStatus;
  return true;
}

static bool lgd_h264_create(void ** opaque)
{
  // create our local storage
  *opaque = malloc(sizeof(struct Inst));
  if (!*opaque)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
    return false;
  }
  memset(*opaque, 0, sizeof(struct Inst));
  struct Inst * this = (struct Inst *)*opaque;

  this->vaSurfaceID[0] = VA_INVALID_ID;
  this->vaConfigID     = VA_INVALID_ID;
  this->vaContextID    = VA_INVALID_ID;
  for(int i = 0; i < SURFACE_NUM; ++i)
    this->picBufferID[i] =
    this->matBufferID[i] =
	  this->sliBufferID[i] =
    this->datBufferID[i] = VA_INVALID_ID;

  lgd_h264_deinitialize(this);
  return true;
}

static void lgd_h264_destroy(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  lgd_h264_deinitialize(this);
  free(this);
}

static bool lgd_h264_initialize(void * opaque, const LG_RendererFormat format, SDL_Window * window)
{
  struct Inst * this = (struct Inst *)opaque;
  lgd_h264_deinitialize(this);

  memcpy(&this->format, &format, sizeof(LG_RendererFormat));
  this->window = window;

  SDL_SysWMinfo wminfo;
  SDL_VERSION(&wminfo.version);
  if (!SDL_GetWindowWMInfo(window, &wminfo))
  {
    DEBUG_ERROR("Failed to get SDL window WM Info");
    return false;
  }

  switch(wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      this->vaDisplay = vaGetDisplayGLX(wminfo.info.x11.display);
      break;

    default:
      DEBUG_ERROR("Unsupported window subsystem");
      return false;
  }

  VAStatus status;
  status = vaInitialize(this->vaDisplay, &this->vaMajorVer, &this->vaMinorVer);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaInitialize Failed");
    return false;
  }

  DEBUG_INFO("Vendor: %s", vaQueryVendorString(this->vaDisplay));

  VAEntrypoint entryPoints[5];
  int          entryPointCount;

  status = vaQueryConfigEntrypoints(
      this->vaDisplay,
      VAProfileH264Baseline,
      entryPoints,
      &entryPointCount
  );
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaQueryConfigEntrypoints Failed");
    return false;
  }

  int ep;
  for(ep = 0; ep < entryPointCount; ++ep)
    if (entryPoints[ep] == VAEntrypointVLD)
      break;

  if (ep == entryPointCount)
  {
    DEBUG_ERROR("Failed to find VAEntrypointVLD index");
    return false;
  }

  VAConfigAttrib attrib;
  attrib.type = VAConfigAttribRTFormat;
  vaGetConfigAttributes(
    this->vaDisplay,
    VAProfileH264ConstrainedBaseline,
    VAEntrypointVLD,
    &attrib,
    1);

  if (!(attrib.value & VA_RT_FORMAT_YUV420))
  {
    DEBUG_ERROR("Failed to find desired YUV420 RT format");
    return false;
  }

  status = vaCreateConfig(
    this->vaDisplay,
    VAProfileH264ConstrainedBaseline,
    VAEntrypointVLD,
    &attrib,
    1,
    &this->vaConfigID);

  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaCreateConfig");
    return false;
  }

  status = vaCreateSurfaces(
    this->vaDisplay,
    VA_RT_FORMAT_YUV420,
    this->format.width,
    this->format.height,
    this->vaSurfaceID,
    SURFACE_NUM,
    NULL,
    0
  );
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaCreateSurfaces");
    return false;
  }

  for(int i = 0; i < SURFACE_NUM; ++i)
    if (!check_surface(this, i, NULL))
      return false;

  status = vaCreateContext(
    this->vaDisplay,
    this->vaConfigID,
    this->format.width,
    this->format.height,
    VA_PROGRESSIVE,
    this->vaSurfaceID,
    SURFACE_NUM,
    &this->vaContextID
  );
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaCreateContext");
    return false;
  }

  this->currentSID = 0;
  this->sliceType  = 2;
  this->t2First    = true;

  status = vaBeginPicture(this->vaDisplay, this->vaContextID, this->vaSurfaceID[0]);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaBeginPicture");
    return false;
  }

  return true;
}

static void lgd_h264_deinitialize(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;

  for(int i = 0; i < SURFACE_NUM; ++i)
  {
    if (this->picBufferID[i] != VA_INVALID_ID)
      vaDestroyBuffer(this->vaDisplay, this->picBufferID[i]);

    if (this->matBufferID[i] != VA_INVALID_ID)
      vaDestroyBuffer(this->vaDisplay, this->matBufferID[i]);

    if (this->sliBufferID[i] != VA_INVALID_ID)
      vaDestroyBuffer(this->vaDisplay, this->sliBufferID[i]);

    if (this->datBufferID[i] != VA_INVALID_ID)
      vaDestroyBuffer(this->vaDisplay, this->datBufferID[i]);

    this->picBufferID[i] =
    this->matBufferID[i] =
	  this->sliBufferID[i] =
    this->datBufferID[i] = VA_INVALID_ID;
  }

  if (this->vaSurfaceID[0] != VA_INVALID_ID)
    vaDestroySurfaces(this->vaDisplay, this->vaSurfaceID, SURFACE_NUM);
  this->vaSurfaceID[0] = VA_INVALID_ID;

  if (this->vaContextID != VA_INVALID_ID)
    vaDestroyContext(this->vaDisplay, this->vaContextID);
  this->vaContextID = VA_INVALID_ID;

  if (this->vaConfigID != VA_INVALID_ID)
    vaDestroyConfig(this->vaDisplay, this->vaConfigID);
  this->vaConfigID = VA_INVALID_ID;

  if (this->vaDisplay)
    vaTerminate(this->vaDisplay);
  this->vaDisplay = NULL;
}

static LG_OutFormat lgd_h264_get_out_format(void * opaque)
{
  return LG_OUTPUT_YUV420;
}

static unsigned int lgd_h264_get_frame_pitch(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.width * 4;
}

static unsigned int lgd_h264_get_frame_stride(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  return this->format.width;
}

static bool get_buffer(struct Inst * this, const VABufferType type, const unsigned int size, VABufferID * buf_id)
{
  if (*buf_id != VA_INVALID_ID)
    return true;

  VAStatus status = vaCreateBuffer(this->vaDisplay, this->vaContextID, type, size, 1, NULL, buf_id);
  if (status != VA_STATUS_SUCCESS)
  {
		DEBUG_ERROR("Failed to create buffer: %s", vaErrorStr(status));
    return false;
  }

  if (!check_surface(this, this->currentSID, NULL))
    return false;

  return true;
}

static void set_slice_parameter_buffer(VASliceParameterBufferH264 *p)
{
  memset(p, 0, sizeof(VASliceParameterBufferH264));
  p->slice_data_size            = 0;
  p->slice_data_bit_offset      = 64;
  p->slice_alpha_c0_offset_div2 = 2;
  p->slice_beta_offset_div2     = 2;
  p->chroma_weight_l0_flag      = 1;
  p->chroma_weight_l0[0][0]     = 1;
  p->chroma_offset_l0[0][0]     = 0;
  p->chroma_weight_l0[0][1]     = 1;
  p->chroma_offset_l0[0][1]     = 0;
  p->luma_weight_l1_flag        = 1;
  p->chroma_weight_l1_flag      = 1;
  p->luma_weight_l0[0]          = 0x01;

  for (int i = 0; i < 32; i++)
    p->RefPicList0[i].flags =
    p->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;

  p->RefPicList1[0].picture_id = 0xffffffff;
}

static void set_slice_parameter_buffer_t2(VASliceParameterBufferH264 *p, const bool first)
{
  memset(p, 0, sizeof(VASliceParameterBufferH264));
  p->slice_data_size            = 0;
  p->slice_data_bit_offset      = 64;
  p->slice_alpha_c0_offset_div2 = 2;
  p->slice_beta_offset_div2     = 2;
  p->slice_type                 = 2;

  if (first)
  {
    p->luma_weight_l0_flag   = 1;
    p->chroma_weight_l0_flag = 1;
    p->luma_weight_l1_flag   = 1;
    p->chroma_weight_l1_flag = 1;
  }
  else
  {
    p->chroma_weight_l0_flag  = 1;
    p->chroma_weight_l0[0][0] = 1;
    p->chroma_offset_l0[0][0] = 0;
    p->chroma_weight_l0[0][1] = 1;
    p->chroma_offset_l0[0][1] = 0;
    p->luma_weight_l1_flag    = 1;
    p->chroma_weight_l1_flag  = 1;
    p->luma_weight_l0[0]      = 0x01;
  }

  for (int i = 0; i < 32; i++)
    p->RefPicList0[i].flags =
    p->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;

  p->RefPicList1[0].picture_id =
  p->RefPicList0[0].picture_id = 0xffffffff;
}

static bool setup_pic_buffer(struct Inst * this)
{
  VAStatus status;

  VABufferID * picBufferID = &this->picBufferID[this->currentSID];
  if (!get_buffer(this, VAPictureParameterBufferType, sizeof(VAPictureParameterBufferH264), picBufferID))
  {
    DEBUG_ERROR("get picBuffer failed");
    return false;
  }

  VAPictureParameterBufferH264 *p;
  status = vaMapBuffer(this->vaDisplay, *picBufferID, (void **)&p);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaMapBuffer: %s", vaErrorStr(status));
    return false;
  }

  memset(p, 0, sizeof(VAPictureParameterBufferH264));
  p->picture_width_in_mbs_minus1 = (this->format.width  + 15) / 16;
  p->picture_width_in_mbs_minus1 = (this->format.height + 15) / 16;
  p->num_ref_frames              = 1;
  p->seq_fields.value            = 145;
  p->pic_fields.value            = 0x501;
  p->frame_num                   = this->frameNum % 16;
  for(int i = 0; i < 16; ++i)
  {
    p->ReferenceFrames[i].flags      = VA_PICTURE_H264_INVALID;
    p->ReferenceFrames[i].picture_id = 0xFFFFFFFF;
  }

  this->curPic.picture_id          = this->vaSurfaceID[this->currentSID];
  this->curPic.frame_idx           = p->frame_num;
  this->curPic.flags               = 0;
  this->curPic.BottomFieldOrderCnt = this->fieldCount;
  this->curPic.TopFieldOrderCnt    = this->fieldCount;
  memcpy(&p->CurrPic, &this->curPic, sizeof(VAPictureH264));

  if (this->sliceType != 2)
  {
    memcpy(&p->ReferenceFrames[0], &this->oldPic, sizeof(VAPictureH264));
    p->ReferenceFrames[0].flags = 0;
  }

  status = vaUnmapBuffer(this->vaDisplay, *picBufferID);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaUnmapBuffer: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static bool setup_mat_buffer(struct Inst * this)
{
  VAStatus status;

  VABufferID * matBufferID = &this->matBufferID[this->currentSID];
  if (!get_buffer(this, VAIQMatrixBufferType, sizeof(VAIQMatrixBufferH264), matBufferID))
  {
    DEBUG_ERROR("get matBuffer failed");
    return false;
  }

  VAIQMatrixBufferH264 * m;
  status = vaMapBuffer(this->vaDisplay, *matBufferID, (void **)&m);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaMapBuffer: %s", vaErrorStr(status));
    return false;
  }

  memcpy(m, MatrixBufferH264, sizeof(MatrixBufferH264));

  status = vaUnmapBuffer(this->vaDisplay, *matBufferID);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaUnmapBuffer: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static bool setup_sli_buffer(struct Inst * this, size_t srcSize)
{
  VAStatus status;

  VABufferID * sliBufferID = &this->sliBufferID[this->currentSID];
  if (!get_buffer(this, VASliceParameterBufferType, sizeof(VASliceParameterBufferH264), sliBufferID))
  {
    DEBUG_ERROR("get sliBuffer failed");
    return false;
  }

  VASliceParameterBufferH264 * s;
  status = vaMapBuffer(this->vaDisplay, *sliBufferID, (void **)&s);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaMapBuffer: %s", vaErrorStr(status));
    return false;
  }

  if (this->sliceType == 2)
  {
    set_slice_parameter_buffer_t2(s, this->t2First);
    this->t2First = false;
  }
  else
  {
    set_slice_parameter_buffer(s);
    memcpy(&s->RefPicList0[0], &this->oldPic, sizeof(VAPictureH264));
    s->RefPicList0[0].flags = 0;
  }
  s->slice_data_bit_offset = 0;
  s->slice_data_size       = srcSize;

  status = vaUnmapBuffer(this->vaDisplay, *sliBufferID);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaUnmapBuffer: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static bool setup_dat_buffer(struct Inst * this, const uint8_t * src, size_t srcSize)
{
  VAStatus status;

  VABufferID * datBufferID = &this->datBufferID[this->currentSID];

  if (!get_buffer(this, VASliceDataBufferType, srcSize, datBufferID))
  {
    DEBUG_ERROR("get datBuffer failed");
    return false;
  }

  uint8_t * d;
  status = vaMapBuffer(this->vaDisplay, *datBufferID, (void **)&d);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaMapBuffer: %s", vaErrorStr(status));
    return false;
  }

  memcpySSE(d, src, srcSize);

  status = vaUnmapBuffer(this->vaDisplay, *datBufferID);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaUnmapBuffer: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static bool parse_nalu(struct Inst * this, const uint8_t * src, size_t size)
{
  static const uint8_t startCode[4] = {0x00, 0x00, 0x00, 0x01};

  if (memcmp(src, startCode, sizeof(startCode)) != 0)
  {
    DEBUG_ERROR("Missing start code");
    return false;
  }
  src += 4;

  if (*src & 0x80)
  {
    DEBUG_ERROR("forbidden_zero_bit is set");
    return false;
  }

//  uint8_t nal_ref_idc       = (*src & 0x60) >> 5;
  uint8_t nal_ref_unit_type = (*src & 0x1F);
  ++src;

  if (nal_ref_unit_type == NALU_AUD)
  {
    static const int pic_type_to_slice_type[3] =
    {
      SLICE_TYPE_I,
      SLICE_TYPE_P,
      SLICE_TYPE_B
    };

    const uint8_t primary_pic_type = (*src & 0xE0) >> 5;
    this->sliceType = pic_type_to_slice_type[primary_pic_type];
    return true;
  }

  return false;
}

static bool lgd_h264_decode(void * opaque, const uint8_t * src, size_t srcSize)
{
  VAStatus status;
  struct Inst * this = (struct Inst *)opaque;

  if (!parse_nalu(this, src, srcSize))
  {
    DEBUG_ERROR("Failed to parse required information");
    return false;
  }

  // don't start until we have an I-FRAME
  if (this->frameNum == 0 && this->sliceType != SLICE_TYPE_I)
    return true;

  {
    if (!setup_pic_buffer(this)) return false;
    if (!setup_mat_buffer(this)) return false;

    VABufferID bufferIDs[] =
    {
      this->picBufferID[this->currentSID],
      this->matBufferID[this->currentSID]
    };

    status = vaRenderPicture(this->vaDisplay, this->vaContextID, bufferIDs, 2);
    if (status != VA_STATUS_SUCCESS)
    {
      DEBUG_ERROR("vaRenderPicture: %s", vaErrorStr(status));
      return false;
    }

    // intel broke the ABI here, see:
    // https://github.com/01org/libva/commit/3eb038aa13bdd785808286c0a4995bd7a1ef07e9
    // the buffers are released by vaRenderPicture in old versions
    if (this->vaMajorVer == 0 && this->vaMinorVer < 40)
    {
      this->picBufferID[this->currentSID] =
      this->matBufferID[this->currentSID] = VA_INVALID_ID;
    }
  }

  {
    if (!setup_sli_buffer(this, srcSize     )) return false;
    if (!setup_dat_buffer(this, src, srcSize)) return false;
    VABufferID bufferIDs[] =
    {
      this->sliBufferID[this->currentSID],
      this->datBufferID[this->currentSID]
    };

    status = vaRenderPicture(this->vaDisplay, this->vaContextID, bufferIDs, 2);
    if (status != VA_STATUS_SUCCESS)
    {
      DEBUG_ERROR("vaRenderPicture: %s", vaErrorStr(status));
      return false;
    }

    // intel broke the ABI here, see:
    // https://github.com/01org/libva/commit/3eb038aa13bdd785808286c0a4995bd7a1ef07e9
    // the buffers are released by vaRenderPicture in old versions
    if (this->vaMajorVer == 0 && this->vaMinorVer < 40)
    {
      this->sliBufferID[this->currentSID] =
      this->datBufferID[this->currentSID] = VA_INVALID_ID;
    }
  }

  status = vaEndPicture(this->vaDisplay, this->vaContextID);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaEndPicture: %s", vaErrorStr(status));
    return false;
  }

  // advance to the next surface and save the old picture info
  this->lastSID = this->currentSID;
  if (++this->currentSID == SURFACE_NUM)
    this->currentSID = 0;
  this->frameNum   += 1;
  this->fieldCount += 2;
  memcpy(&this->oldPic, &this->curPic, sizeof(VAPictureH264));

  // prepare the next surface
  status = vaBeginPicture(this->vaDisplay, this->vaContextID, this->vaSurfaceID[this->currentSID]);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaBeginPicture: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static bool lgd_h264_get_buffer(void * opaque, uint8_t * dst, size_t dstSize)
{
  struct Inst * this = (struct Inst *)opaque;
  VAStatus status;

  // don't return anything until we have some data
  if (this->frameNum == 0)
    return true;

  // ensure the surface is ready
  status = vaSyncSurface(this->vaDisplay, this->vaSurfaceID[this->lastSID]);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaSyncSurface: %s", vaErrorStr(status));
    return false;
  }

#if 0
  // this doesn't work on my system, seems the vdpau va driver is bugged
  VASurfaceStatus surfStatus;
  if (!check_surface(this, this->lastSID, &surfStatus))
    return false;

  if (surfStatus != VASurfaceReady)
  {
    DEBUG_ERROR("vaSyncSurface didn't block, the surface is not ready!");
    return false;
  }
#endif

  // get the decoded data
  VAImage decoded =
  {
    .image_id = VA_INVALID_ID,
    .buf      = VA_INVALID_ID
  };

  status = vaDeriveImage(this->vaDisplay, this->vaSurfaceID[this->lastSID], &decoded);
  if (status == VA_STATUS_ERROR_OPERATION_FAILED)
  {
    VAImageFormat format =
    {
      .fourcc         = VA_FOURCC_NV12,
      .byte_order     = VA_LSB_FIRST,
      .bits_per_pixel = 12
    };

    status = vaCreateImage(
      this->vaDisplay,
      &format,
      this->format.width,
      this->format.height,
      &decoded
    );

    if (status != VA_STATUS_SUCCESS)
    {
      DEBUG_ERROR("vaCreateImage: %s", vaErrorStr(status));
      return false;
    }

    status = vaPutImage(
      this->vaDisplay,
      this->vaSurfaceID[this->lastSID],
      decoded.image_id,
      0                 , 0                  ,
      this->format.width, this->format.height,
      0                 , 0                  ,
      this->format.width, this->format.height
    );

    if (status != VA_STATUS_SUCCESS)
    {
      vaDestroyImage(this->vaDisplay, decoded.image_id);
      DEBUG_ERROR("vaPutImage: %s", vaErrorStr(status));
      return false;
    }
  }
  else
  {
    if (status != VA_STATUS_SUCCESS)
    {
      DEBUG_ERROR("vaDeriveImage: %s", vaErrorStr(status));
      return false;
    }
  }

  uint8_t * d;
  status = vaMapBuffer(this->vaDisplay, decoded.buf, (void **)&d);
  if (status != VA_STATUS_SUCCESS)
  {
    vaDestroyImage(this->vaDisplay, decoded.image_id);
    DEBUG_ERROR("vaMapBuffer: %s", vaErrorStr(status));
    return false;
  }

  memcpySSE(dst, d, decoded.data_size);

  status = vaUnmapBuffer(this->vaDisplay, decoded.buf);
  if (status != VA_STATUS_SUCCESS)
  {
    vaDestroyImage(this->vaDisplay, decoded.image_id);
    DEBUG_ERROR("vaUnmapBuffer: %s", vaErrorStr(status));
    return false;
  }

  status = vaDestroyImage(this->vaDisplay, decoded.image_id);
  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaDestroyImage: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static bool lgd_h264_init_gl_texture(void * opaque, GLenum target, GLuint texture, void ** ref)
{
  struct Inst * this = (struct Inst *)opaque;
  VAStatus status;

  status = vaCreateSurfaceGLX(this->vaDisplay, target, texture, ref);
  if (status != VA_STATUS_SUCCESS)
  {
    *ref = NULL;
    DEBUG_ERROR("vaCreateSurfaceGLX: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

static void lgd_h264_free_gl_texture(void * opaque, void * ref)
{
  struct Inst * this = (struct Inst *)opaque;
  VAStatus status;

  status = vaDestroySurfaceGLX(this->vaDisplay, ref);
  if (status != VA_STATUS_SUCCESS)
    DEBUG_ERROR("vaDestroySurfaceGLX: %s", vaErrorStr(status));
}

static bool lgd_h264_update_gl_texture(void * opaque, void * ref)
{
  struct Inst * this = (struct Inst *)opaque;
  VAStatus status;

  // don't return anything until we have some data
  if (this->frameNum == 0)
    return true;

  status = vaCopySurfaceGLX(
    this->vaDisplay,
    ref,
    this->vaSurfaceID[this->lastSID],
    0
  );

  if (status != VA_STATUS_SUCCESS)
  {
    DEBUG_ERROR("vaCopySurfaceGLX: %s", vaErrorStr(status));
    return false;
  }

  return true;
}

const LG_Decoder LGD_H264 =
{
  .name              = "H.264",
  .create            = lgd_h264_create,
  .destroy           = lgd_h264_destroy,
  .initialize        = lgd_h264_initialize,
  .deinitialize      = lgd_h264_deinitialize,
  .get_out_format    = lgd_h264_get_out_format,
  .get_frame_pitch   = lgd_h264_get_frame_pitch,
  .get_frame_stride  = lgd_h264_get_frame_stride,
  .decode            = lgd_h264_decode,
  .get_buffer        = lgd_h264_get_buffer,

  .has_gl            = true,
  .init_gl_texture   = lgd_h264_init_gl_texture,
  .free_gl_texture   = lgd_h264_free_gl_texture,
  .update_gl_texture = lgd_h264_update_gl_texture
};