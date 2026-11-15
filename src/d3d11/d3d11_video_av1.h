#pragma once

/*
 * AV1 for the MediaCodec-backed D3D11 video decoder.
 *
 * DXVA (Chromium's D3D accelerator) hands over the tile data of a frame plus
 * DXVA_PicParams_AV1; MediaCodec wants OBUs.  This writes a sequence header,
 * a frame header and a tile group from the picture parameters.  Two things
 * DXVA does not carry are reconstructed here:
 *  - refresh_frame_flags: the caller keeps its own slot layout (one copy of
 *    every picture the app's RefFrameMapTextureIndex still names) and, when
 *    all slots are taken, learns from the next frame's map where the app put
 *    the picture (see D3D11VideoDecoder::FinalizeAv1Locked);
 *  - the global-motion reference (PrevGmParams): kept per slot in Av1State.
 */

#include <cstdint>
#include <vector>

namespace dxvk {

#pragma pack(push, 1)
  struct DxvaPicEntryAV1 {
    uint32_t width;
    uint32_t height;
    int32_t  wmmat[6];
    uint8_t  GlobalMotionFlags;   /* wminvalid:1 wmtype:2 */
    uint8_t  Index;
    uint16_t Reserved16Bits;
  };

  struct DxvaPicParamsAV1 {
    uint32_t width;
    uint32_t height;
    uint32_t max_width;
    uint32_t max_height;
    uint8_t  CurrPicTextureIndex;
    uint8_t  superres_denom;
    uint8_t  bitdepth;
    uint8_t  seq_profile;
    struct {
      uint8_t  cols;
      uint8_t  rows;
      uint16_t context_update_id;
      uint16_t widths[64];
      uint16_t heights[64];
    } tiles;
    uint32_t CodingParamToolFlags;
    uint8_t  FormatAndPictureInfoFlags;
    uint8_t  primary_ref_frame;
    uint8_t  order_hint;
    uint8_t  order_hint_bits;
    DxvaPicEntryAV1 frame_refs[7];
    uint8_t  RefFrameMapTextureIndex[8];
    struct {
      uint8_t  filter_level[2];
      uint8_t  filter_level_u;
      uint8_t  filter_level_v;
      uint8_t  sharpness_level;
      uint8_t  ControlFlags;        /* mode_ref_delta_enabled, _update, delta_lf_multi, delta_lf_present */
      int8_t   ref_deltas[8];
      int8_t   mode_deltas[2];
      uint8_t  delta_lf_res;
      uint8_t  frame_restoration_type[3];
      uint16_t log2_restoration_unit_size[3];
      uint16_t Reserved16Bits;
    } loop_filter;
    struct {
      uint8_t  ControlFlags;        /* delta_q_present:1 delta_q_res:2 */
      uint8_t  base_qindex;
      int8_t   y_dc_delta_q;
      int8_t   u_dc_delta_q;
      int8_t   v_dc_delta_q;
      int8_t   u_ac_delta_q;
      int8_t   v_ac_delta_q;
      uint8_t  qm_y;
      uint8_t  qm_u;
      uint8_t  qm_v;
      uint16_t Reserved16Bits;
    } quantization;
    struct {
      uint8_t  ControlFlags;        /* damping:2 bits:2 */
      uint8_t  y_strengths[8];      /* primary:6 secondary:2 */
      uint8_t  uv_strengths[8];
    } cdef;
    uint8_t  interp_filter;
    struct {
      uint8_t  ControlFlags;        /* enabled, update_map, update_data, temporal_update */
      uint8_t  Reserved24Bits[3];
      uint8_t  feature_mask[8];
      int16_t  feature_data[8][8];
    } segmentation;
    struct {
      uint16_t ControlFlags;
      uint16_t grain_seed;
      uint8_t  scaling_points_y[14][2];
      uint8_t  num_y_points;
      uint8_t  scaling_points_cb[10][2];
      uint8_t  num_cb_points;
      uint8_t  scaling_points_cr[10][2];
      uint8_t  num_cr_points;
      uint8_t  ar_coeffs_y[24];
      uint8_t  ar_coeffs_cb[25];
      uint8_t  ar_coeffs_cr[25];
      uint8_t  cb_mult;
      uint8_t  cb_luma_mult;
      uint8_t  cr_mult;
      uint8_t  cr_luma_mult;
      uint8_t  Reserved8Bits;
      int16_t  cb_offset;
      int16_t  cr_offset;
    } film_grain;
    uint32_t Reserved32Bits;
    uint32_t StatusReportFeedbackNumber;
  };

  struct DxvaTileAV1 {
    uint32_t DataOffset;
    uint32_t DataSize;
    uint16_t row;
    uint16_t column;
    uint16_t Reserved16Bits;
    uint8_t  anchor_frame;
    uint8_t  Reserved8Bits;
  };
#pragma pack(pop)

  /* Decoder-side reference state.  The decoder's slot layout is our own:
   * what matters is which picture (app texture) each slot holds. */
  struct Av1State {
    int      slotTex[8]        = { -1, -1, -1, -1, -1, -1, -1, -1 };
    uint8_t  slotOrderHint[8]  = { };
    int32_t  slotGm[8][8][6]   = { };   /* [slot][ref type LAST..ALTREF = 1..7][param] */
    bool     slotValid[8]      = { };
    std::vector<uint8_t> lastSeqHdr;
  };

  /* Appends the OBUs ([sequence header], frame header, tile group)
   * for one frame to out.  refresh: refresh_frame_flags to code (ignored for a
   * shown key frame).  Returns false for streams it cannot express. */
  bool av1WriteFrame(
          Av1State&               st,
    const DxvaPicParamsAV1&       pp,
    const DxvaTileAV1*            tiles,
          uint32_t                tileCount,
    const uint8_t*                data,
          size_t                  dataSize,
          uint8_t                 refresh,
    const int*                    refSlots,     /* decoder slot of each of the 7 references */
          std::vector<uint8_t>&   out,
          const char**            pError);

  inline bool av1IsKeyShow(const DxvaPicParamsAV1& pp) {
    return (pp.FormatAndPictureInfoFlags & 3) == 0 && (pp.FormatAndPictureInfoFlags & 4);
  }

  inline bool av1ShowFrame(const DxvaPicParamsAV1& pp) {
    return pp.FormatAndPictureInfoFlags & 4;
  }

}
