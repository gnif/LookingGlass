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

#include "nal.h"

#include "debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

#define DEBUG_NAL

struct NAL
{
  uint8_t      primary_pic_type;
  bool         primary_pic_type_valid;

  bool         sps_valid;
  NAL_SPS      sps;
  int32_t    * sps_offset_for_ref_frame;
  uint32_t     sps_num_ref_frames_in_pic_order_cnt_cycle;

  bool         vui_valid;
  NAL_VUI      vui;
  NAL_CPB    * vui_nal_hrd_parameters_cpb;
  uint32_t     vui_nal_hrd_parameters_cpb_size;
  NAL_CPB    * vui_vcl_hrd_parameters_cpb;
  uint32_t     vui_vcl_hrd_parameters_cpb_size;

  bool         pps_valid;
  NAL_PPS      pps;
};

bool nal_initialize(NAL * ptr)
{
  *ptr = (NAL)malloc(sizeof(struct NAL));
  memset(*ptr, 0, sizeof(struct NAL));
  return true;
}

void nal_deinitialize(NAL this)
{
  if (this->sps_offset_for_ref_frame)
    free(this->sps_offset_for_ref_frame);

  if (this->vui_nal_hrd_parameters_cpb)
    free(this->vui_nal_hrd_parameters_cpb);

  if (this->vui_vcl_hrd_parameters_cpb)
    free(this->vui_vcl_hrd_parameters_cpb);

  free(this);
}

static bool parse_nal_hrd(
    NAL_HRD * const hrd,
    NAL_CPB  ** cpb,
    uint32_t *  cpb_size,
    const uint8_t * src,
    size_t size,
    size_t * const offset)
{
  hrd->cpb_cnt_minus1 = decode_u_golomb(src, offset);
  hrd->bit_rate_scale = get_bits(src, offset, 4);
  hrd->cpb_size_scale = get_bits(src, offset, 4);

  if (*cpb_size < hrd->cpb_size_scale)
  {
    *cpb      = realloc(*cpb, hrd->cpb_size_scale * sizeof(NAL_CPB));
    *cpb_size = hrd->cpb_size_scale;
  }

  hrd->cpb = *cpb;
  for(uint32_t i = 0; i < hrd->cpb_size_scale; ++i)
  {
    hrd->cpb[i].bit_rate_value_minus1 = decode_u_golomb(src, offset);
    hrd->cpb[i].cpb_size_value_minus1 = decode_u_golomb(src, offset);
    hrd->cpb[i].cbr_flag              = get_bit(src, offset);
  }

  hrd->initial_cpb_removal_delay_length_minus1 = get_bits(src, offset, 5);
  hrd->cpb_removal_delay_length_minus1         = get_bits(src, offset, 5);
  hrd->dpb_output_delay_length_minus1          = get_bits(src, offset, 5);
  hrd->time_offset_length                      = get_bits(src, offset, 5);

  return true;
}

static bool parse_nal_vui(NAL this, const uint8_t * src, size_t size, size_t * const offset)
{
  NAL_VUI * vui = &this->vui;
  memset(vui, 0, sizeof(NAL_VUI));

  vui->aspect_ratio_info_present_flag = get_bit(src, offset);
  if (vui->aspect_ratio_info_present_flag)
  {
    vui->aspect_ratio_idc = get_bits(src, offset, 8);
    if (vui->aspect_ratio_idc == IDC_VUI_ASPECT_RATIO_EXTENDED_SAR)
    {
      vui->sar_width  = get_bits(src, offset, 16);
      vui->sar_height = get_bits(src, offset, 16);
    }
  }

  vui->overscan_info_present_flag = get_bit(src, offset);
  if (vui->overscan_info_present_flag)
    vui->overscan_appropriate_flag = get_bit(src, offset);

  vui->video_signal_type_present_flag = get_bit(src, offset);
  if (vui->video_signal_type_present_flag)
  {
    vui->video_format                    = get_bits(src, offset, 3);
    vui->video_full_range_flag           = get_bit(src, offset);
    vui->colour_description_present_flag = get_bit(src, offset);
    if (vui->colour_description_present_flag)
    {
      vui->colour_primaries         = get_bits(src, offset, 8);
      vui->transfer_characteristics = get_bits(src, offset, 8);
      vui->matrix_coefficients      = get_bits(src, offset, 8);
    }
  }

  vui->chroma_loc_info_present_flag = get_bit(src, offset);
  if (vui->chroma_loc_info_present_flag)
  {
    vui->chroma_sample_loc_type_top_field    = decode_u_golomb(src, offset);
    vui->chroma_sample_loc_type_bottom_field = decode_u_golomb(src, offset);
  }

  vui->timing_info_present_flag = get_bit(src, offset);
  if (vui->timing_info_present_flag)
  {
    vui->num_units_in_tick     = get_bits(src, offset, 32);
    vui->time_scale            = get_bits(src, offset, 32);
    vui->fixed_frame_rate_flag = get_bit(src, offset);
  }

  vui->nal_hrd_parameters_present_flag = get_bit(src, offset);
  if (vui->nal_hrd_parameters_present_flag)
    if (!parse_nal_hrd(
        &vui->nal_hrd_parameters,
        &this->vui_nal_hrd_parameters_cpb,
        &this->vui_nal_hrd_parameters_cpb_size,
        src,
        size,
        offset))
      return false;

  vui->vcl_hrd_parameters_present_flag = get_bit(src, offset);
  if (vui->vcl_hrd_parameters_present_flag)
    if (!parse_nal_hrd(
        &vui->vcl_hrd_parameters,
        &this->vui_vcl_hrd_parameters_cpb,
        &this->vui_vcl_hrd_parameters_cpb_size,
        src,
        size,
        offset))
      return false;

  if (vui->nal_hrd_parameters_present_flag || vui->vcl_hrd_parameters_present_flag)
    vui->low_delay_hrd_flag = get_bit(src, offset);

  vui->pic_struct_present_flag    = get_bit(src, offset);
  vui->bitstream_restriction_flag = get_bit(src, offset);
  if (vui->bitstream_restriction_flag)
  {
    vui->motion_vectors_over_pic_boundaries_flag = get_bit(src, offset);
    vui->max_bytes_per_pic_denom                 = decode_u_golomb(src, offset);
    vui->max_bits_per_mb_denom                   = decode_u_golomb(src, offset);
    vui->log2_max_mv_length_horizontal           = decode_u_golomb(src, offset);
    vui->log2_max_mv_length_vertical             = decode_u_golomb(src, offset);
    vui->num_reorder_frames                      = decode_u_golomb(src, offset);
    vui->max_dec_frame_buffering                 = decode_u_golomb(src, offset);
  }

  return true;
}

static bool parse_nal_trailing_bits(NAL this, const uint8_t * src, size_t size, size_t * const offset)
{
  if (!get_bit(src, offset))
  {
    DEBUG_ERROR("Missing stop bit");
    return false;
  }

  // byte align
  *offset = (*offset + 0x7) & ~0x7;
  return true;
}

static bool parse_nal_sps(NAL this, const uint8_t * src, size_t size, size_t * const offset)
{
  this->sps_valid = false;
  memset(&this->sps, 0, sizeof(this->sps));

  this->sps.profile_idc = get_bits(src, offset, 8);
  if ((this->sps.profile_idc != IDC_PROFILE_BASELINE) &&
      (this->sps.profile_idc != IDC_PROFILE_MAIN    ) &&
      (this->sps.profile_idc != IDC_PROFILE_EXTENDED) &&
      (this->sps.profile_idc != IDC_PROFILE_HP      ) &&
      (this->sps.profile_idc != IDC_PROFILE_Hi10P   ) &&
      (this->sps.profile_idc != IDC_PROFILE_Hi422   ) &&
      (this->sps.profile_idc != IDC_PROFILE_Hi444   ) &&
      (this->sps.profile_idc != IDC_PROFILE_CAVLC444))
  {
    DEBUG_ERROR("Invalid profile IDC (%d) encountered", this->sps.profile_idc);
    return false;
  }

  this->sps.constraint_set_flags[0] = get_bit(src, offset);
  this->sps.constraint_set_flags[1] = get_bit(src, offset);
  this->sps.constraint_set_flags[2] = get_bit(src, offset);
  *offset += 5;

  this->sps.level_idc            = get_bits(src, offset, 8);
  this->sps.seq_parameter_set_id = decode_u_golomb(src, offset);

  if ((this->sps.profile_idc == IDC_PROFILE_HP      ) ||
      (this->sps.profile_idc == IDC_PROFILE_Hi10P   ) ||
      (this->sps.profile_idc == IDC_PROFILE_Hi422   ) ||
      (this->sps.profile_idc == IDC_PROFILE_Hi444   ) ||
      (this->sps.profile_idc == IDC_PROFILE_CAVLC444))
  {
    this->sps.chroma_format_idc = decode_u_golomb(src, offset);
    if (this->sps.chroma_format_idc == IDC_CHROMA_FORMAT_YUV444)
      this->sps.seperate_colour_plane_flag = get_bit(src, offset);

    this->sps.bit_depth_luma_minus8           = decode_u_golomb(src, offset);
    this->sps.bit_depth_chroma_minus8         = decode_u_golomb(src, offset);
    this->sps.lossless_qpprime_y_zero_flag    = get_bit(src, offset);
    this->sps.seq_scaling_matrix_present_flag = get_bit(src, offset);

    if (this->sps.seq_scaling_matrix_present_flag)
    {
      const int cnt = this->sps.chroma_format_idc == IDC_CHROMA_FORMAT_YUV444 ? 12 : 8;
      for(int i = 0; i < cnt; ++i)
        this->sps.seq_scaling_list_present_flag[i] = get_bit(src, offset);
    }
  }
  else
    this->sps.chroma_format_idc = IDC_CHROMA_FORMAT_YUV420;

  this->sps.log2_max_frame_num_minus4 = decode_u_golomb(src, offset);
  this->sps.pic_order_cnt_type        = decode_u_golomb(src, offset);

  if (this->sps.pic_order_cnt_type == 0)
    this->sps.log2_max_pic_order_cnt_lsb_minus4 = decode_u_golomb(src, offset);
  else
  {
    if (this->sps.pic_order_cnt_type == 1)
    {
      this->sps.delta_pic_order_always_zero_flag = get_bit(src, offset);
      this->sps.offset_for_non_ref_pic           = decode_s_golomb(src, offset);
      this->sps.offset_for_top_to_bottom_field   = decode_s_golomb(src, offset);

      this->sps.num_ref_frames_in_pic_order_cnt_cycle = decode_u_golomb(src, offset);
      if (this->sps.num_ref_frames_in_pic_order_cnt_cycle > this->sps_num_ref_frames_in_pic_order_cnt_cycle)
      {
        this->sps_offset_for_ref_frame = realloc(
          this->sps_offset_for_ref_frame,
          this->sps.num_ref_frames_in_pic_order_cnt_cycle * sizeof(int32_t)
        );
        this->sps_num_ref_frames_in_pic_order_cnt_cycle = this->sps.num_ref_frames_in_pic_order_cnt_cycle;
      }

      this->sps.offset_for_ref_frame = this->sps_offset_for_ref_frame;
      for(uint32_t i = 0; i < this->sps.num_ref_frames_in_pic_order_cnt_cycle; ++i)
        this->sps.offset_for_ref_frame[i] = decode_s_golomb(src, offset);
    }
  }

  this->sps.num_ref_frames                       = decode_u_golomb(src, offset);
  this->sps.gaps_in_frame_num_value_allowed_flag = get_bit(src, offset);
  this->sps.pic_width_in_mbs_minus1              = decode_u_golomb(src, offset);
  this->sps.pic_height_in_map_units_minus1       = decode_u_golomb(src, offset);
  this->sps.frame_mbs_only_flag                  = get_bit(src, offset);

  if (!this->sps.frame_mbs_only_flag)
    this->sps.mb_adaptive_frame_field_flag = get_bit(src, offset);

  this->sps.direct_8x8_inference_flag = get_bit(src, offset);
  this->sps.frame_cropping_flag       = get_bit(src, offset);

  if (this->sps.frame_cropping_flag)
  {
    this->sps.frame_crop_left_offset   = decode_u_golomb(src, offset);
    this->sps.frame_crop_right_offset  = decode_u_golomb(src, offset);
    this->sps.frame_crop_top_offset    = decode_u_golomb(src, offset);
    this->sps.frame_crop_bottom_offset = decode_u_golomb(src, offset);
  }

  this->sps.vui_parameters_present_flag = get_bit(src, offset);

#ifdef DEBUG_NAL
  DEBUG_INFO("SPS\n"
    "profile_idc                          : %u\n"
    "constraint_set_flags                 : %u %u %u\n"
    "level_idc                            : %u\n"
    "sec_parameter_set_id                 : %u\n"
    "chroma_format_idc                    : %u\n"
    "seperate_colour_plane_flag           : %u\n"
    "bit_depth_luma_minus8                : %u\n"
    "bit_depth_chroma_minus8              : %u\n"
    "lossless_qpprime_y_zero_flag         : %u\n"
    "seq_scaling_matrix_present_flag      : %u\n"
    "log2_max_frame_num_minus4            : %u\n"
    "pic_order_cnt_type                   : %u\n"
    "log2_max_pic_order_cnt_lsb_minus4    : %u\n"
    "delta_pic_order_always_zero_flag     : %u\n"
    "offset_for_non_ref_pic               : %d\n"
    "offset_for_top_to_bottom_field       : %d\n"
    "num_ref_frames_in_pic_order_cnt_cycle: %u\n"
    "num_ref_frames                       : %u\n"
    "gaps_in_frame_num_value_allowed_flag : %u\n"
    "pic_width_in_mbs_minus1              : %3u (%u)\n"
    "pic_height_in_map_units_minus1       : %3u (%u)\n"
    "frame_mbs_only_flag                  : %u\n"
    "mb_adaptive_frame_field_flag         : %u\n"
    "direct_8x8_inference_flag            : %u\n"
    "frame_cropping_flag                  : %u\n"
    "frame_crop_left_offset               : %u\n"
    "frame_crop_right_offset              : %u\n"
    "frame_crop_top_offset                : %u\n"
    "frame_crop_bottom_offset             : %u\n"
    "vui_parameters_present_flag          : %u",
    this->sps.profile_idc,
    this->sps.constraint_set_flags[0],
    this->sps.constraint_set_flags[1],
    this->sps.constraint_set_flags[2],
    this->sps.level_idc,
    this->sps.seq_parameter_set_id,
    this->sps.chroma_format_idc,
    this->sps.seperate_colour_plane_flag,
    this->sps.bit_depth_luma_minus8,
    this->sps.bit_depth_chroma_minus8,
    this->sps.lossless_qpprime_y_zero_flag,
    this->sps.seq_scaling_matrix_present_flag,
    this->sps.log2_max_frame_num_minus4,
    this->sps.pic_order_cnt_type,
    this->sps.log2_max_pic_order_cnt_lsb_minus4,
    this->sps.delta_pic_order_always_zero_flag,
    this->sps.offset_for_non_ref_pic,
    this->sps.offset_for_top_to_bottom_field,
    this->sps.num_ref_frames_in_pic_order_cnt_cycle,
    this->sps.num_ref_frames,
    this->sps.gaps_in_frame_num_value_allowed_flag,
    this->sps.pic_width_in_mbs_minus1       , (this->sps.pic_width_in_mbs_minus1        + 1) * 16,
    this->sps.pic_height_in_map_units_minus1, (this->sps.pic_height_in_map_units_minus1 + 1) * 16,
    this->sps.frame_mbs_only_flag,
    this->sps.mb_adaptive_frame_field_flag,
    this->sps.direct_8x8_inference_flag,
    this->sps.frame_cropping_flag,
    this->sps.frame_crop_left_offset,
    this->sps.frame_crop_right_offset,
    this->sps.frame_crop_top_offset,
    this->sps.frame_crop_bottom_offset,
    this->sps.vui_parameters_present_flag
  );
#endif

  if (this->sps.vui_parameters_present_flag)
  {
    if (!parse_nal_vui(this, src, size, offset))
      return false;
    this->vui_valid = true;
  }

  if (!parse_nal_trailing_bits(this, src, size, offset))
    return false;

  this->sps_valid = true;
  return true;
}

static bool parse_nal_pps(NAL this, const uint8_t * src, size_t size, size_t * const offset)
{
  return false;
}

bool nal_parse(NAL this, const uint8_t * src, size_t size)
{
  static FILE * fd = NULL;
  if (!fd)
    fd = fopen("/tmp/stream.h264", "w");
  fwrite(src, size, 1, fd);

  const size_t bits = size << 4;
  size_t offset = 0;
  while(offset < bits)
  {
    // look for the start header
    if (get_bits(src, &offset, 32) != 1)
    {
      offset -= 24;
      continue;
    }

    // ensure the forbidden zero bit is not set
    if (get_bit(src, &offset) != 0)
    {
      DEBUG_ERROR("forbidden_zero_bit is set");
      return false;
    }

    uint8_t nal_ref_idc       = get_bits(src, &offset, 2);
    uint8_t nal_ref_unit_type = get_bits(src, &offset, 5);
    DEBUG_INFO("ref idc: %d, ref unit type: %d", nal_ref_idc, nal_ref_unit_type);

    switch(nal_ref_unit_type)
    {
      case NAL_TYPE_AUD:
      {
        this->primary_pic_type       = get_bits(src, &offset, 3);
        this->primary_pic_type_valid = true;
        if (!parse_nal_trailing_bits(this, src, size, &offset))
          return false;
        break;
      }

      case NAL_TYPE_SPS:
        if (!parse_nal_sps(this, src, size, &offset))
          return false;
        break;

      case NAL_TYPE_PPS:
        if (!parse_nal_pps(this, src, size, &offset))
          return false;
        break;

      default:
        DEBUG_ERROR("Unknown NAL ref unit type: %d", nal_ref_unit_type);
        return false;
    }

    // byte align
    offset = (offset + 0x7) & ~0x7;
  }

  return true;
}

bool nal_get_sps(NAL this, const NAL_SPS ** sps)
{
  if (!this->sps_valid)
    return false;
  *sps = &this->sps;
  return true;
}

bool nal_get_primary_picture_type(NAL this, uint8_t * pic_type)
{
  if (!this->primary_pic_type_valid)
    return false;
  *pic_type = this->primary_pic_type;
  return true;
}

bool nal_get_pps(NAL this, const NAL_PPS ** pps)
{
  if (!this->pps_valid)
    return false;

  *pps = &this->pps;
  return true;
}