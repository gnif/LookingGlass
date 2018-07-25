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
  uint8_t           primary_pic_type;
  bool              primary_pic_type_valid;

  bool              sps_valid;
  NAL_SPS           sps;
  int32_t         * sps_offset_for_ref_frame;
  uint32_t          sps_num_ref_frames_in_pic_order_cnt_cycle;

  bool              vui_valid;
  NAL_VUI           vui;
  NAL_CPB         * vui_nal_hrd_parameters_cpb;
  uint32_t          vui_nal_hrd_parameters_cpb_size;
  NAL_CPB         * vui_vcl_hrd_parameters_cpb;
  uint32_t          vui_vcl_hrd_parameters_cpb_size;

  bool              pps_valid;
  NAL_PPS           pps;
  NAL_SLICE_GROUP * pps_slice_groups;
  uint32_t          pps_slice_groups_size;
  uint32_t        * pps_slice_group_id;
  uint32_t          pps_slice_group_id_size;

  bool              slice_valid;
  NAL_SLICE         slice;
  NAL_PW_TABLE_L  * slice_pred_weight_table_l0;
  uint32_t          slice_pred_weight_table_l0_size;
  NAL_PW_TABLE_L  * slice_pred_weight_table_l1;
  uint32_t          slice_pred_weight_table_l1_size;
};

bool nal_initialize(NAL * ptr)
{
  *ptr = (NAL)malloc(sizeof(struct NAL));
  memset(*ptr, 0, sizeof(struct NAL));
  return true;
}

void nal_deinitialize(NAL this)
{
  free(this->slice_pred_weight_table_l1);
  free(this->slice_pred_weight_table_l0);
  free(this->pps_slice_group_id);
  free(this->pps_slice_groups);
  free(this->sps_offset_for_ref_frame);
  free(this->vui_nal_hrd_parameters_cpb);
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
  NAL_PPS * pps = &this->pps;
  this->pps_valid = false;
  memset(pps, 0, sizeof(NAL_PPS));

  pps->pic_parameter_set_id     = decode_u_golomb(src, offset);
  pps->seq_parameter_set_id     = decode_u_golomb(src, offset);
  pps->entropy_coding_mode_flag = get_bit(src, offset);
  pps->pic_order_present_flag   = get_bit(src, offset);
  pps->num_slice_groups_minus1  = decode_u_golomb(src, offset);

  if (pps->num_slice_groups_minus1 > 0)
  {
    pps->slice_group_map_type = decode_u_golomb(src, offset);
    if (pps->slice_group_map_type == 0 || pps->slice_group_map_type == 2)
    {
      if (this->pps_slice_groups_size < pps->num_slice_groups_minus1 + 1)
      {
        this->pps_slice_groups_size = pps->num_slice_groups_minus1 + 1;
        this->pps_slice_groups      = (NAL_SLICE_GROUP *)realloc(
            this->pps_slice_groups, this->pps_slice_groups_size * sizeof(NAL_SLICE_GROUP));
      }
      pps->slice_groups = this->pps_slice_groups;
      memset(pps->slice_groups, 0, (pps->num_slice_groups_minus1 + 1) * sizeof(NAL_SLICE_GROUP));

      if (pps->slice_group_map_type == 0)
      {
        for(uint32_t group = 0; group <= pps->num_slice_groups_minus1; ++group)
          pps->slice_groups[group].t0.run_length_minus1 = decode_u_golomb(src, offset);
      }
      else
      {
        for(uint32_t group = 0; group < pps->num_slice_groups_minus1; ++group)
        {
          pps->slice_groups[group].t2.top_left     = decode_u_golomb(src, offset);
          pps->slice_groups[group].t2.bottom_right = decode_u_golomb(src, offset);
        }
      }
    }
    else
    {
      if (pps->slice_group_map_type == 3 ||
          pps->slice_group_map_type == 4 ||
          pps->slice_group_map_type == 5)
      {
        pps->slice_group_change_direction_flag = get_bit(src, offset);
        pps->slice_group_change_rate_minus1    = decode_u_golomb(src, offset);
      }
      else
      {
        if (pps->slice_group_map_type == 6)
        {
          pps->pic_size_in_map_units_minus1 = decode_u_golomb(src, offset);

          uint32_t slice_groups = pps->pic_size_in_map_units_minus1 + 1;
          uint32_t bits         = 0;
          if ((slice_groups & (slice_groups - 1)) != 0)
            ++slice_groups;

          while(slice_groups > 0)
          {
            slice_groups >>= 1;
            ++bits;
          }

          if (this->pps_slice_group_id_size < pps->pic_size_in_map_units_minus1 + 1)
          {
            this->pps_slice_group_id_size = pps->pic_size_in_map_units_minus1 + 1;
            this->pps_slice_group_id      = realloc(this->pps_slice_group_id,
                this->pps_slice_group_id_size * sizeof(uint32_t));
          }
          pps->slice_group_id = this->pps_slice_group_id;

          for(uint32_t group = 0; group <= pps->pic_size_in_map_units_minus1; ++group)
            pps->slice_group_id[group] = get_bits(src, offset, bits);
        }
        else
        {
          DEBUG_ERROR("Invalid slice_group_map_type: %d", pps->slice_group_map_type);
          return false;
        }
      }
    }
  }

  pps->num_ref_idx_l0_active_minus1           = decode_u_golomb(src, offset);
  pps->num_ref_idx_l1_active_minus1           = decode_u_golomb(src, offset);
  pps->weighted_pred_flag                     = get_bit(src, offset);
  pps->weighted_bipred_idc                    = get_bits(src, offset, 2);
  pps->pic_init_qp_minus26                    = decode_s_golomb(src, offset);
  pps->pic_init_qs_minus26                    = decode_s_golomb(src, offset);
  pps->chroma_qp_index_offset                 = decode_s_golomb(src, offset);
  pps->deblocking_filter_control_present_flag = get_bit(src, offset);
  pps->constrained_intra_pred_flag            = get_bit(src, offset);
  pps->redundant_pic_cnt_present_flag         = get_bit(src, offset);

  if (pps->num_ref_idx_l0_active_minus1 + 1 > this->slice_pred_weight_table_l0_size)
  {
    this->slice_pred_weight_table_l0_size = pps->num_ref_idx_l0_active_minus1 + 1;
    this->slice_pred_weight_table_l0 = realloc(this->slice_pred_weight_table_l0,
        this->slice_pred_weight_table_l0_size * sizeof(NAL_PW_TABLE_L));
  }

  if (pps->num_ref_idx_l1_active_minus1 + 1 > this->slice_pred_weight_table_l1_size)
  {
    this->slice_pred_weight_table_l1_size = pps->num_ref_idx_l1_active_minus1 + 1;
    this->slice_pred_weight_table_l1 = realloc(this->slice_pred_weight_table_l1,
        this->slice_pred_weight_table_l1_size * sizeof(NAL_PW_TABLE_L));
  }

  const bool extraData = get_bit(src, offset) == 0;
  --*offset;

  if (extraData)
  {
    pps->transform_8x8_mode_flag         = get_bit(src, offset);
    pps->pic_scaling_matrix_present_flag = get_bit(src, offset);
    if (pps->pic_scaling_matrix_present_flag)
    {
      //TODO
    }
    pps->second_chroma_qp_index_offset = decode_s_golomb(src, offset);
  }

#ifdef DEBUG_NAL
  DEBUG_INFO("PPS:\n"
    "pic_parameter_set_id                  : %u\n"
    "seq_parameter_set_id                  : %u\n"
    "entropy_coding_mode_flag              : %u\n"
    "pic_order_present_flag                : %u\n"
    "num_slice_groups_minus1               : %u\n"
    "slice_group_map_type                  : %u\n"
    "slice_group_change_direction_flag     : %u\n"
    "slice_group_change_rate_minus1        : %u\n"
    "pic_size_in_map_units_minus1          : %u\n"
    "num_ref_idx_l0_active_minus1          : %u\n"
    "num_ref_idx_l1_active_minus1          : %u\n"
    "weighted_pred_flag                    : %u\n"
    "weighted_bipred_idc                   : %u\n"
    "pic_init_qp_minus26                   : %d\n"
    "pic_init_qs_minus26                   : %d\n"
    "chroma_qp_index_offset                : %d\n"
    "deblocking_filter_control_present_flag: %u\n"
    "constrained_intra_pred_flag           : %u\n"
    "redundant_pic_cnt_present_flag        : %u\n"
    "transform_8x8_mode_flag               : %u\n"
    "pic_scaling_matrix_present_flag       : %u\n"
    "second_chroma_qp_index_offset         : %u",
    pps->pic_parameter_set_id,
    pps->seq_parameter_set_id,
    pps->entropy_coding_mode_flag,
    pps->pic_order_present_flag,
    pps->num_slice_groups_minus1,
    pps->slice_group_map_type,
    pps->slice_group_change_direction_flag,
    pps->slice_group_change_rate_minus1,
    pps->pic_size_in_map_units_minus1,
    pps->num_ref_idx_l0_active_minus1,
    pps->num_ref_idx_l1_active_minus1,
    pps->weighted_pred_flag,
    pps->weighted_bipred_idc,
    pps->pic_init_qp_minus26,
    pps->pic_init_qs_minus26,
    pps->chroma_qp_index_offset,
    pps->deblocking_filter_control_present_flag,
    pps->constrained_intra_pred_flag,
    pps->redundant_pic_cnt_present_flag,
    pps->transform_8x8_mode_flag,
    pps->pic_scaling_matrix_present_flag,
    pps->second_chroma_qp_index_offset
  );
#endif

  if (!parse_nal_trailing_bits(this, src, size, offset))
    return false;

  this->pps_valid = true;
  return true;
}

static bool parse_nal_ref_pic_list_reordering(NAL this, const uint8_t * src, size_t size, size_t * const offset)
{
  NAL_SLICE       * slice = &this->slice;
  NAL_RPL_REORDER * rpl   = &this->slice.ref_pic_list_reordering;

  if (slice->slice_type != NAL_SLICE_TYPE_I && slice->slice_type != NAL_SLICE_TYPE_SI)
  {
    rpl->ref_pic_list_reordering_flag_l0 = get_bit(src, offset);
    if(rpl->ref_pic_list_reordering_flag_l0)
    {
      int index = 0;
      NAL_RPL_REORDER_L * l;
      do
      {
        if (index > 2)
        {
          DEBUG_ERROR("too many reorder records");
          return false;
        }

        l = &rpl->l0[index++];
        l->valid                      = true;
        l->reordering_of_pic_nums_idc = decode_u_golomb(src, offset);
        if (l->reordering_of_pic_nums_idc == 0 || l->reordering_of_pic_nums_idc == 1)
          l->abs_diff_pic_num_minus1 = decode_u_golomb(src, offset);
        else
          if (l->reordering_of_pic_nums_idc == 2)
            l->long_term_pic_num = decode_u_golomb(src, offset);
      }
      while(l->reordering_of_pic_nums_idc != 3);
    }
  }

  if (slice->slice_type == NAL_SLICE_TYPE_B)
  {
    rpl->ref_pic_list_reordering_flag_l1 = get_bit(src, offset);
    if (rpl->ref_pic_list_reordering_flag_l1)
    {
      int index = 0;
      NAL_RPL_REORDER_L * l;
      do
      {
        if (index > 2)
        {
          DEBUG_ERROR("too many reorder records");
          return false;
        }

        l = &rpl->l1[index++];
        l->valid                      = true;
        l->reordering_of_pic_nums_idc = decode_u_golomb(src, offset);
        if (l->reordering_of_pic_nums_idc == 0 || l->reordering_of_pic_nums_idc == 1)
          l->abs_diff_pic_num_minus1 = decode_u_golomb(src, offset);
        else
          if (l->reordering_of_pic_nums_idc == 2)
            l->long_term_pic_num = decode_u_golomb(src, offset);
      }
      while(l->reordering_of_pic_nums_idc != 3);
    }
  }

  return true;
}

static bool parse_pred_weight_table(NAL this, const uint8_t * src, size_t size, size_t * const offset)
{
  NAL_SLICE    * slice = &this->slice;
  NAL_PW_TABLE * tbl   = &this->slice.pred_weight_table;

  tbl->luma_log2_weight_denom = decode_u_golomb(src, offset);
  if (this->sps.chroma_format_idc != 0)
    tbl->chroma_log2_weight_denom = decode_u_golomb(src, offset);

  for(uint32_t i = 0; i <= this->pps.num_ref_idx_l0_active_minus1; ++i)
  {
    NAL_PW_TABLE_L * l = &tbl->l0[i];

    tbl->luma_weight_flag[0] = get_bit(src, offset);
    if (tbl->luma_weight_flag[0])
    {
      l->luma_weight = decode_s_golomb(src, offset);
      l->luma_offset = decode_s_golomb(src, offset);
    }

    if (this->sps.chroma_format_idc != 0)
    {
      tbl->chroma_weight_flag[0] = get_bit(src, offset);
      if (tbl->chroma_weight_flag[0])
        for(int j = 0; j < 2; ++j)
        {
          l->chroma_weight[j] = decode_s_golomb(src, offset);
          l->chroma_offset[j] = decode_s_golomb(src, offset);
        }
    }
  }

  if (slice->slice_type == NAL_SLICE_TYPE_B)
  {
    for(uint32_t i = 0; i <= this->pps.num_ref_idx_l1_active_minus1; ++i)
    {
      NAL_PW_TABLE_L * l = &tbl->l1[i];

      tbl->luma_weight_flag[1] = get_bit(src, offset);
      if (tbl->luma_weight_flag[1])
      {
        l->luma_weight = decode_s_golomb(src, offset);
        l->luma_offset = decode_s_golomb(src, offset);
      }

      if (this->sps.chroma_format_idc != 0)
      {
        tbl->chroma_weight_flag[1] = get_bit(src, offset);
        if (tbl->chroma_weight_flag[1])
          for(int j = 0; j < 2; ++j)
          {
            l->chroma_weight[j] = decode_s_golomb(src, offset);
            l->chroma_offset[j] = decode_s_golomb(src, offset);
          }
      }
    }
  }

  return true;
}

static bool parse_dec_ref_pic_marking(
  NAL this,
  const uint8_t ref_unit_type,
  const uint8_t * src,
  size_t size,
  size_t * const offset
)
{
  NAL_RP_MARKING * m = &this->slice.dec_ref_pic_marking;
  if (ref_unit_type == 5)
  {
    m->no_output_of_prior_pics_flag = get_bit(src, offset);
    m->long_term_reference_flag     = get_bit(src, offset);
  }
  else
  {
    m->adaptive_ref_pic_marking_mode_flag = get_bit(src, offset);
    if (m->adaptive_ref_pic_marking_mode_flag)
    {
      uint32_t op;
      do
      {
        op = decode_u_golomb(src, offset);
        if (op == 1 || op == 3)
          m->difference_of_pic_nums_minus1 = decode_u_golomb(src, offset);

        if (op == 2)
          m->long_term_pic_num = decode_u_golomb(src, offset);

        if (op == 3 || op == 6)
          m->long_term_frame_idx = decode_u_golomb(src, offset);

        if (op == 4)
            m->max_long_term_frame_idx_plus1 = decode_u_golomb(src, offset);

      } while (op != 0);
    }
  }

  return true;
}

static bool parse_nal_coded_slice(
  NAL this,
  const uint8_t ref_idc,
  const uint8_t ref_unit_type,
  const uint8_t * src,
  size_t size,
  size_t * const offset
)
{
  if (!this->sps_valid || !this->pps_valid)
    return false;

  NAL_SLICE * slice = &this->slice;
  memset(slice, 0, sizeof(NAL_SLICE));

  slice->nal_ref_idc          = ref_idc;
  slice->first_mb_in_slice    = decode_u_golomb(src, offset);
  slice->slice_type           = decode_u_golomb(src, offset);
  slice->pic_parameter_set_id = decode_u_golomb(src, offset);
  slice->frame_num            = get_bits(src, offset, this->sps.log2_max_frame_num_minus4 + 4);
  slice->pred_weight_table.l0 = this->slice_pred_weight_table_l0;
  slice->pred_weight_table.l1 = this->slice_pred_weight_table_l1;

  if (!this->sps.frame_mbs_only_flag)
  {
    slice->field_pic_flag = get_bit(src, offset);
    if (slice->field_pic_flag)
      slice->bottom_field_flag = get_bit(src, offset);
  }

  if (ref_unit_type == 5)
    slice->idr_pic_id = decode_u_golomb(src, offset);

  if (this->sps.pic_order_cnt_type == 0)
  {
    slice->pic_order_cnt_lsb = get_bits(src, offset, this->sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    if (this->pps.pic_order_present_flag && !slice->field_pic_flag)
      slice->delta_pic_order_cnt_bottom = decode_s_golomb(src, offset);
  }
  else
    if (this->sps.pic_order_cnt_type == 1 && !this->sps.delta_pic_order_always_zero_flag)
    {
      slice->delta_pic_order_cnt[0] = decode_s_golomb(src, offset);
      if (this->pps.pic_order_present_flag && !slice->field_pic_flag)
        slice->delta_pic_order_cnt[1] = decode_s_golomb(src, offset);
    }

  if (this->pps.redundant_pic_cnt_present_flag)
    slice->redundant_pic_cnt = decode_u_golomb(src, offset);

  if (slice->slice_type == NAL_SLICE_TYPE_B)
    slice->direct_spatial_mv_pred_flag = get_bit(src, offset);

  if (slice->slice_type == NAL_SLICE_TYPE_P  ||
      slice->slice_type == NAL_SLICE_TYPE_SP ||
      slice->slice_type == NAL_SLICE_TYPE_B)
  {
    slice->num_ref_idx_active_override_flag = get_bit(src, offset);
    if (slice->num_ref_idx_active_override_flag)
    {
      slice->num_ref_idx_l0_active_minus1 = decode_u_golomb(src, offset);
      if (slice->slice_type == NAL_SLICE_TYPE_B)
        slice->num_ref_idx_l1_active_minus1 = decode_u_golomb(src, offset);
    }
  }

  if (!parse_nal_ref_pic_list_reordering(this, src, size, offset))
    return false;

  if ((this->pps.weighted_pred_flag && (slice->slice_type == NAL_SLICE_TYPE_P || slice->slice_type == NAL_SLICE_TYPE_SP)) ||
      (this->pps.weighted_bipred_idc == 1 && slice->slice_type == NAL_SLICE_TYPE_B))
  {
    if (!parse_pred_weight_table(this, src, size, offset))
      return false;
  }

  if (ref_idc != 0)
    if (!parse_dec_ref_pic_marking(this, ref_unit_type, src, size, offset))
      return false;

  if (this->pps.entropy_coding_mode_flag && slice->slice_type != NAL_SLICE_TYPE_I && slice->slice_type != NAL_SLICE_TYPE_SI)
    slice->cabac_init_idc = decode_u_golomb(src, offset);

  slice->slice_qp_delta = decode_s_golomb(src, offset);

  if (slice->slice_type == NAL_SLICE_TYPE_SP || slice->slice_type == NAL_SLICE_TYPE_SI)
  {
    if (slice->slice_type == NAL_SLICE_TYPE_SP)
      slice->sp_for_switch_flag = get_bit(src, offset);
    slice->slice_qs_delta = decode_s_golomb(src, offset);
  }

  if (this->pps.deblocking_filter_control_present_flag)
  {
    slice->disable_deblocking_filter_idc = decode_u_golomb(src, offset);
    if (slice->disable_deblocking_filter_idc != 1)
    {
      slice->slice_alpha_c0_offset_div2 = decode_s_golomb(src, offset);
      slice->slice_beta_offset_div2     = decode_s_golomb(src, offset);
    }
  }

  if (this->pps.num_slice_groups_minus1 > 0 && this->pps.slice_group_map_type >= 3 && this->pps.slice_group_map_type <= 5)
    slice->slice_group_change_cycle = decode_u_golomb(src, offset);

#ifdef DEBUG_NAL
  DEBUG_INFO("SLICE:\n"
    "first_mb_in_slice               : %u\n"
    "slice_type                      : %u\n"
    "pic_parameter_set_id            : %u\n"
    "frame_num                       : %u\n"
    "field_pic_flag                  : %u\n"
    "bottom_field_flag               : %u\n"
    "idr_pic_id                      : %u\n"
    "pic_order_cnt_lsb               : %u\n"
    "delta_pic_order_cnt_bottom      : %d\n"
    "delta_pic_order_cnt[0]          : %d\n"
    "delta_pic_order_cnt[1]          : %d\n"
    "redundant_pic_cnt               : %u\n"
    "direct_spatial_mv_pred_flag     : %u\n"
    "num_ref_idx_active_override_flag: %u\n"
    "num_ref_idx_l0_active_minus1    : %u\n"
    "num_ref_idx_l1_active_minus1    : %u",
    slice->first_mb_in_slice,
    slice->slice_type,
    slice->pic_parameter_set_id,
    slice->frame_num,
    slice->field_pic_flag,
    slice->bottom_field_flag,
    slice->idr_pic_id,
    slice->pic_order_cnt_lsb,
    slice->delta_pic_order_cnt_bottom,
    slice->delta_pic_order_cnt[0],
    slice->delta_pic_order_cnt[1],
    slice->redundant_pic_cnt,
    slice->direct_spatial_mv_pred_flag,
    slice->num_ref_idx_active_override_flag,
    slice->num_ref_idx_l0_active_minus1,
    slice->num_ref_idx_l1_active_minus1
  );
#endif

  if (!parse_nal_trailing_bits(this, src, size, offset))
    return false;

  this->slice_valid = true;
  return true;
}

bool nal_parse(NAL this, const uint8_t * src, size_t size, size_t * seek)
{
#ifdef DEBUG_NAL
  static FILE * fd = NULL;
  if (!fd)
    fd = fopen("/tmp/stream.h264", "w");
  fwrite(src, size, 1, fd);
  fflush(fd);
#endif

  *seek = 0;
  for(size_t i = 0; i < size - 4; ++i)
  {
    if (src[i++] != 0 || src[i++] != 0)
      break;

    if (src[i] == 0)
      ++i;

    if (src[i++] != 1)
      break;

    size_t offset = i << 3;
#ifdef DEBUG_NAL
    DEBUG_INFO("nal @ %lu (%lu)", *seek, offset);
#endif

    // ensure the forbidden zero bit is not set
    if (get_bit(src, &offset) != 0)
    {
      DEBUG_ERROR("forbidden_zero_bit is set");
      return false;
    }

    uint8_t ref_idc       = get_bits(src, &offset, 2);
    uint8_t ref_unit_type = get_bits(src, &offset, 5);
    DEBUG_INFO("ref idc: %d, ref unit type: %d", ref_idc, ref_unit_type);

    switch(ref_unit_type)
    {
      case NAL_TYPE_CODED_SLICE_IDR:
      case NAL_TYPE_CODED_SLICE_NON_IDR:
      case NAL_TYPE_CODED_SLICE_AUX:
        if (!parse_nal_coded_slice(this, ref_idc, ref_unit_type, src, size, &offset))
          return false;
        break;

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
        DEBUG_ERROR("Unknown NAL ref unit type: %d", ref_unit_type);
        return false;
    }

    i = offset >> 3;
    *seek = i;
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

bool nal_get_slice(NAL this, const NAL_SLICE ** slice)
{
  if (!this->slice_valid)
    return false;

  *slice = &this->slice;
  return true;
}