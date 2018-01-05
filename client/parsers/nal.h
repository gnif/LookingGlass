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

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define NAL_TYPE_SPS  7
#define NAL_TYPE_PPS  8
#define NAL_TYPE_AUD  9

#define IDC_PROFILE_BASELINE 66
#define IDC_PROFILE_MAIN     77
#define IDC_PROFILE_EXTENDED 88
#define IDC_PROFILE_HP       100
#define IDC_PROFILE_Hi10P    110
#define IDC_PROFILE_Hi422    122
#define IDC_PROFILE_Hi444    244
#define IDC_PROFILE_CAVLC444 44

#define IDC_CHROMA_FORMAT_YUV400 0
#define IDC_CHROMA_FORMAT_YUV420 1
#define IDC_CHROMA_FORMAT_YVU422 2
#define IDC_CHROMA_FORMAT_YUV444 3

#define IDC_VUI_ASPECT_RATIO_EXTENDED_SAR 0xFF

#define NAL_PICTURE_TYPE_I 0
#define NAL_PICTURE_TYPE_P 1
#define NAL_PICTURE_TYPE_B 2

typedef struct NAL_SPS
{
  uint8_t    profile_idc;
  uint8_t    constraint_set_flags[3];
  uint8_t    level_idc;
  uint32_t   seq_parameter_set_id;
  uint32_t   chroma_format_idc;
  uint8_t    seperate_colour_plane_flag;
  uint32_t   bit_depth_luma_minus8;
  uint32_t   bit_depth_chroma_minus8;
  uint8_t    lossless_qpprime_y_zero_flag;
  uint8_t    seq_scaling_matrix_present_flag;
  uint8_t    seq_scaling_list_present_flag[12];
  uint32_t   log2_max_frame_num_minus4;
  uint32_t   pic_order_cnt_type;
  uint32_t   log2_max_pic_order_cnt_lsb_minus4;
  uint8_t    delta_pic_order_always_zero_flag;
  int32_t    offset_for_non_ref_pic;
  int32_t    offset_for_top_to_bottom_field;
  uint32_t   num_ref_frames_in_pic_order_cnt_cycle;
  int32_t  * offset_for_ref_frame;
  uint32_t   num_ref_frames;
  uint8_t    gaps_in_frame_num_value_allowed_flag;
  uint32_t   pic_width_in_mbs_minus1;
  uint32_t   pic_height_in_map_units_minus1;
  uint8_t    frame_mbs_only_flag;
  uint8_t    mb_adaptive_frame_field_flag;
  uint8_t    direct_8x8_inference_flag;
  uint8_t    frame_cropping_flag;
  uint32_t   frame_crop_left_offset;
  uint32_t   frame_crop_right_offset;
  uint32_t   frame_crop_top_offset;
  uint32_t   frame_crop_bottom_offset;
  uint8_t    vui_parameters_present_flag;
}
NAL_SPS;

typedef struct NAL_CPB
{
  uint32_t bit_rate_value_minus1;
  uint32_t cpb_size_value_minus1;
  uint8_t  cbr_flag;
}
NAL_CPB;

typedef struct NAL_HRD
{
  uint32_t     cpb_cnt_minus1;
  uint8_t      bit_rate_scale;
  uint8_t      cpb_size_scale;
  uint8_t      cpb_size_count;
  NAL_CPB    * cpb;
  uint8_t      initial_cpb_removal_delay_length_minus1;
  uint8_t      cpb_removal_delay_length_minus1;
  uint8_t      dpb_output_delay_length_minus1;
  uint8_t      time_offset_length;
}
NAL_HRD;

typedef struct NAL_VUI
{
  uint8_t    aspect_ratio_info_present_flag;
  uint8_t    aspect_ratio_idc;
  uint16_t   sar_width;
  uint16_t   sar_height;
  uint8_t    overscan_info_present_flag;
  uint8_t    overscan_appropriate_flag;
  uint8_t    video_signal_type_present_flag;
  uint8_t    video_format;
  uint8_t    video_full_range_flag;
  uint8_t    colour_description_present_flag;
  uint8_t    colour_primaries;
  uint8_t    transfer_characteristics;
  uint8_t    matrix_coefficients;
  uint8_t    chroma_loc_info_present_flag;
  uint32_t   chroma_sample_loc_type_top_field;
  uint32_t   chroma_sample_loc_type_bottom_field;
  uint8_t    timing_info_present_flag;
  uint32_t   num_units_in_tick;
  uint32_t   time_scale;
  uint8_t    fixed_frame_rate_flag;
  uint8_t    nal_hrd_parameters_present_flag;
  NAL_HRD    nal_hrd_parameters;
  uint8_t    vcl_hrd_parameters_present_flag;
  NAL_HRD    vcl_hrd_parameters;
  uint8_t    low_delay_hrd_flag;
  uint8_t    pic_struct_present_flag;
  uint8_t    bitstream_restriction_flag;
  uint8_t    motion_vectors_over_pic_boundaries_flag;
  uint32_t   max_bytes_per_pic_denom;
  uint32_t   max_bits_per_mb_denom;
  uint32_t   log2_max_mv_length_horizontal;
  uint32_t   log2_max_mv_length_vertical;
  uint32_t   num_reorder_frames;
  uint32_t   max_dec_frame_buffering;
}
NAL_VUI;

typedef struct NAL_SLICE_GROUP_T0
{
  uint32_t   run_length_minus1;
}
NAL_SLICE_GROUP_T0;

typedef struct NAL_SLICE_GROUP_T2
{
  uint32_t   top_left;
  uint32_t   bottom_right;
}
NAL_SLICE_GROUP_T2;

typedef union NAL_SLICE_GROUP
{
  NAL_SLICE_GROUP_T0 t0;
  NAL_SLICE_GROUP_T2 t2;
}
NAL_SLICE_GROUP;

typedef struct NAL_PPS
{
  uint32_t          pic_parameter_set_id;
  uint32_t          seq_parameter_set_id;
  uint8_t           entropy_coding_mode_flag;
  uint8_t           pic_order_present_flag;
  uint32_t          num_slice_groups_minus1;
  NAL_SLICE_GROUP * slice_groups;
  uint32_t          slice_group_map_type;
  uint8_t           slice_group_change_direction_flag;
  uint32_t          slice_group_change_rate_minus1;
  uint32_t          pic_size_in_map_units_minus1;
  uint32_t        * slice_group_id;
  uint32_t          num_ref_idx_l0_active_minus1;
  uint32_t          num_ref_idx_l1_active_minus1;
  uint8_t           weighted_pred_flag;
  uint8_t           weighted_bipred_idc;
  int32_t           pic_init_qp_minus26;
  int32_t           pic_init_qs_minus26;
  int32_t           chroma_qp_index_offset;
  uint8_t           deblocking_filter_control_present_flag;
  uint8_t           constrained_intra_pred_flag;
  uint8_t           redundant_pic_cnt_present_flag;
}
NAL_PPS;

typedef struct NAL * NAL;

bool nal_initialize  (NAL * ptr);
void nal_deinitialize(NAL this );
bool nal_parse       (NAL this, const uint8_t * src, size_t size);

bool nal_get_primary_picture_type(NAL this, uint8_t * pic_type);
bool nal_get_sps(NAL this, const NAL_SPS ** sps);
bool nal_get_pps(NAL this, const NAL_PPS ** pps);