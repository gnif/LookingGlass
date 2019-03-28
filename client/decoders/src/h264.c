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

#include "lg-decoder.h"

#include "debug.h"
#include "memcpySSE.h"
#include "parsers/nal.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <SDL2/SDL_syswm.h>
#include <va/va_glx.h>

#define SURFACE_NUM 3

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

  NAL                 nal;
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

  if (!nal_initialize(&this->nal))
  {
    DEBUG_INFO("Failed to initialize NAL parser");
    free(this);
    return false;
  }

  lgd_h264_deinitialize(this);
  return true;
}

static void lgd_h264_destroy(void * opaque)
{
  struct Inst * this = (struct Inst *)opaque;
  nal_deinitialize(this->nal);
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
      VAProfileH264High,
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
    VAProfileH264High,
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
    VAProfileH264High,
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

static bool setup_pic_buffer(struct Inst * this, const NAL_SLICE * slice)
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

  const NAL_SPS * sps;
  if (!nal_get_sps(this->nal, &sps))
  {
    DEBUG_ERROR("nal_get_sps");
    return false;
  }

  const NAL_PPS * pps;
  if (!nal_get_pps(this->nal, &pps))
  {
    DEBUG_ERROR("nal_get_pps");
    return false;
  }

  memset(p, 0, sizeof(VAPictureParameterBufferH264));
  p->picture_width_in_mbs_minus1  = sps->pic_width_in_mbs_minus1;
  p->picture_height_in_mbs_minus1 = sps->pic_height_in_map_units_minus1;
  p->bit_depth_luma_minus8        = sps->bit_depth_luma_minus8;
  p->bit_depth_chroma_minus8      = sps->bit_depth_chroma_minus8;
  p->num_ref_frames               = sps->num_ref_frames;

  p->seq_fields.value                                  = 0;
  p->seq_fields.bits.chroma_format_idc                 = sps->chroma_format_idc;
  p->seq_fields.bits.residual_colour_transform_flag    = sps->gaps_in_frame_num_value_allowed_flag;
  p->seq_fields.bits.frame_mbs_only_flag               = sps->frame_mbs_only_flag;
  p->seq_fields.bits.mb_adaptive_frame_field_flag      = sps->mb_adaptive_frame_field_flag;
  p->seq_fields.bits.direct_8x8_inference_flag         = sps->direct_8x8_inference_flag;
  p->seq_fields.bits.MinLumaBiPredSize8x8              = sps->level_idc >= 31;
  p->seq_fields.bits.log2_max_frame_num_minus4         = sps->log2_max_frame_num_minus4;
  p->seq_fields.bits.pic_order_cnt_type                = sps->pic_order_cnt_type;
  p->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
  p->seq_fields.bits.delta_pic_order_always_zero_flag  = sps->delta_pic_order_always_zero_flag;

#if 0
  // these are deprecated, FMO is not supported
  p->num_slice_groups_minus1        = pps->num_slice_groups_minus1;
  p->slice_group_map_type           = pps->slice_group_map_type;
  p->slice_group_change_rate_minus1 = pps->slice_group_change_rate_minus1;
#endif

  p->pic_init_qp_minus26            = pps->pic_init_qp_minus26;
  p->pic_init_qs_minus26            = pps->pic_init_qs_minus26;
  p->chroma_qp_index_offset         = pps->chroma_qp_index_offset;
  p->second_chroma_qp_index_offset  = pps->second_chroma_qp_index_offset;

  p->pic_fields.value = 0;
  p->pic_fields.bits.entropy_coding_mode_flag               = pps->entropy_coding_mode_flag;
  p->pic_fields.bits.weighted_pred_flag                     = pps->weighted_pred_flag;
  p->pic_fields.bits.weighted_bipred_idc                    = pps->weighted_bipred_idc;
  p->pic_fields.bits.transform_8x8_mode_flag                = pps->transform_8x8_mode_flag;
  p->pic_fields.bits.field_pic_flag                         = slice->field_pic_flag;
  p->pic_fields.bits.constrained_intra_pred_flag            = pps->constrained_intra_pred_flag;
  p->pic_fields.bits.pic_order_present_flag                 = pps->pic_order_present_flag;
  p->pic_fields.bits.deblocking_filter_control_present_flag = pps->deblocking_filter_control_present_flag;
  p->pic_fields.bits.redundant_pic_cnt_present_flag         = pps->redundant_pic_cnt_present_flag;
  p->pic_fields.bits.reference_pic_flag                     = slice->nal_ref_idc != 0;

  p->frame_num = slice->frame_num;

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

static void fill_pred_weight_table(
  NAL_PW_TABLE_L * list,
  uint32_t active,
  uint32_t luma_log2_weight_denom,
  uint8_t  luma_weight_flag,
  short    luma_weight[32],
  short    luma_offset[32],
  uint32_t chroma_log2_weight_denom,
  uint8_t  chroma_weight_flag,
  short    chroma_weight[32][2],
  short    chroma_offset[32][2]
)
{
  assert(active < 32);

  for(uint32_t i = 0; i <= active; ++i)
  {
    NAL_PW_TABLE_L * l = &list[i];
    if (luma_weight_flag)
    {
      luma_weight[i] = l->luma_weight;
      luma_offset[i] = l->luma_offset;
    }
    else
    {
      luma_weight[i] = 1 << luma_log2_weight_denom;
      luma_weight[i] = 0;
    }

    if (chroma_weight_flag)
    {
      chroma_weight[i][0] = l->chroma_weight[0];
      chroma_offset[i][0] = l->chroma_offset[0];
      chroma_weight[i][1] = l->chroma_weight[1];
      chroma_offset[i][1] = l->chroma_offset[1];
    }
    else
    {
      chroma_weight[i][0] = 1 << chroma_log2_weight_denom;
      chroma_weight[i][0] = 0;
      chroma_weight[i][1] = 1 << chroma_log2_weight_denom;
      chroma_weight[i][1] = 0;
    }
  }
}

static bool setup_sli_buffer(struct Inst * this, size_t srcSize, const NAL_SLICE * slice, const size_t seek)
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

  memset(s, 0, sizeof(VASliceParameterBufferH264));

  s->slice_data_size               = srcSize;
  s->slice_data_bit_offset         = seek << 3;
  s->slice_data_flag               = VA_SLICE_DATA_FLAG_ALL;

  s->first_mb_in_slice             = slice->first_mb_in_slice;
  s->slice_type                    = slice->slice_type;
  s->direct_spatial_mv_pred_flag   = slice->direct_spatial_mv_pred_flag;
  s->num_ref_idx_l0_active_minus1  = slice->num_ref_idx_l0_active_minus1;
  s->num_ref_idx_l1_active_minus1  = slice->num_ref_idx_l1_active_minus1;
  s->cabac_init_idc                = slice->cabac_init_idc;
  s->slice_qp_delta                = slice->slice_qp_delta;
  s->disable_deblocking_filter_idc = slice->disable_deblocking_filter_idc;
  s->slice_alpha_c0_offset_div2    = slice->slice_alpha_c0_offset_div2;
  s->slice_beta_offset_div2        = slice->slice_beta_offset_div2;
  s->luma_log2_weight_denom        = slice->pred_weight_table.luma_log2_weight_denom;
  s->chroma_log2_weight_denom      = slice->pred_weight_table.chroma_log2_weight_denom;
  s->luma_weight_l0_flag           = slice->pred_weight_table.luma_weight_flag  [0];
  s->chroma_weight_l0_flag         = slice->pred_weight_table.chroma_weight_flag[0];
  s->luma_weight_l1_flag           = slice->pred_weight_table.luma_weight_flag  [1];
  s->chroma_weight_l1_flag         = slice->pred_weight_table.chroma_weight_flag[1];

  //RefPicList0/1

  fill_pred_weight_table(
    slice->pred_weight_table.l0,
    s->num_ref_idx_l0_active_minus1,
    s->luma_log2_weight_denom,
    s->luma_weight_l0_flag,
    s->luma_weight_l0,
    s->luma_offset_l0,
    s->chroma_log2_weight_denom,
    s->chroma_weight_l0_flag,
    s->chroma_weight_l0,
    s->chroma_weight_l0
  );

  fill_pred_weight_table(
    slice->pred_weight_table.l1,
    s->num_ref_idx_l1_active_minus1,
    s->luma_log2_weight_denom,
    s->luma_weight_l1_flag,
    s->luma_weight_l1,
    s->luma_offset_l1,
    s->chroma_log2_weight_denom,
    s->chroma_weight_l1_flag,
    s->chroma_weight_l1,
    s->chroma_weight_l1
  );

#if 0
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
#endif

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

static bool lgd_h264_decode(void * opaque, const uint8_t * src, size_t srcSize)
{
  VAStatus status;
  struct Inst * this = (struct Inst *)opaque;

  size_t seek;
  if (!nal_parse(this->nal, src, srcSize, &seek))
  {
    DEBUG_WARN("nal_parse, perhaps mid stream");
    return true;
  }

  const NAL_SLICE * slice;
  if (!nal_get_slice(this->nal, &slice))
  {
    DEBUG_WARN("nal_get_slice failed");
    return true;
  }

  assert(seek < srcSize);
  this->sliceType = slice->slice_type;

  // don't start until we have an I-FRAME
  if (this->frameNum == 0 && this->sliceType != NAL_SLICE_TYPE_I)
    return true;

  {
    if (!setup_pic_buffer(this, slice)) return false;
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
    if (!setup_sli_buffer(this, srcSize, slice, seek)) return false;
    if (!setup_dat_buffer(this, src, srcSize        )) return false;
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