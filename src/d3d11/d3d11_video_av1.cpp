#include <algorithm>
#include <cstring>

#include "d3d11_video_av1.h"

/* Writes AV1 OBUs (spec section 5) from DXVA_PicParams_AV1.  Field meanings
 * follow Chromium's media/gpu/windows/d3d_av1_accelerator.cc, which fills the
 * structure from libgav1's parsed headers. */

namespace dxvk {

  namespace {

    /* coding flags */
    enum : uint32_t {
      C_SB128 = 1u << 0, C_INTRA_EDGE = 1u << 1, C_INTERINTRA = 1u << 2, C_MASKED = 1u << 3,
      C_WARPED = 1u << 4, C_DUAL_FILTER = 1u << 5, C_JNT_COMP = 1u << 6, C_SCREEN_CONTENT = 1u << 7,
      C_INTEGER_MV = 1u << 8, C_CDEF = 1u << 9, C_RESTORATION = 1u << 10, C_FILM_GRAIN = 1u << 11,
      C_INTRABC = 1u << 12, C_HP_MV = 1u << 13, C_SWITCHABLE_MOTION = 1u << 14, C_FILTER_INTRA = 1u << 15,
      C_DISABLE_FRAME_END_CDF = 1u << 16, C_DISABLE_CDF = 1u << 17, C_REFERENCE_MODE = 1u << 18,
      C_SKIP_MODE = 1u << 19, C_REDUCED_TX = 1u << 20, C_SUPERRES = 1u << 21,
      C_USE_REF_MVS = 1u << 24, C_ENABLE_REF_MVS = 1u << 25,
    };

    constexpr int KEY_FRAME = 0, INTER_FRAME = 1, INTRA_ONLY_FRAME = 2, SWITCH_FRAME = 3;
    constexpr int PRIMARY_REF_NONE = 7;
    constexpr int WARPEDMODEL_PREC_BITS = 16;

    class Bits {
    public:
      void f(uint32_t v, int n) {
        while (n--) {
          if (m_pos % 8 == 0)
            m_data.push_back(0);
          if ((v >> n) & 1)
            m_data.back() |= 0x80 >> (m_pos % 8);
          m_pos++;
        }
      }
      void su(int32_t v, int n) {     /* n bits two's complement */
        f(uint32_t(v) & ((1u << n) - 1u), n);
      }
      /* ns(n): non-symmetric unsigned */
      void ns(uint32_t n, uint32_t v) {
        if (n <= 1)
          return;
        int w = 0;
        for (uint32_t x = n; x; x >>= 1) w++;
        uint32_t m = (1u << w) - n;
        if (v < m) {
          f(v, w - 1);
        } else {
          uint32_t extra = v - m;
          f(m + (extra >> 1), w - 1);
          f(extra & 1, 1);
        }
      }
      void trailing() {
        f(1, 1);
        while (m_pos % 8)
          f(0, 1);
      }
      size_t pos() const { return m_pos; }
      const std::vector<uint8_t>& data() const { return m_data; }
    private:
      std::vector<uint8_t> m_data;
      size_t m_pos = 0;
    };

    void leb128(std::vector<uint8_t>& out, size_t v) {
      do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
      } while (v);
    }

    void obu(std::vector<uint8_t>& out, int type, const std::vector<uint8_t>& payload) {
      out.push_back(uint8_t((type << 3) | 0x2));   /* has_size_field */
      leb128(out, payload.size());
      out.insert(out.end(), payload.begin(), payload.end());
    }

    int tileLog2(int blkSize, int target) {
      int k = 0;
      for (; (blkSize << k) < target; k++) { }
      return k;
    }

    /* --- subexponential coding for global motion (inverse of spec 5.9.26-28) --- */

    int recenterNonneg(int r, int v) {
      if (v > (r << 1)) return v;
      else if (v >= r) return (v - r) << 1;
      else return ((r - v) << 1) - 1;
    }

    int recenterFiniteNonneg(int n, int r, int v) {
      if ((r << 1) <= n) return recenterNonneg(r, v);
      return recenterNonneg(n - 1 - r, n - 1 - v);
    }

    void writeSubexp(Bits& b, int numSyms, int v) {
      int i = 0, mk = 0, k = 3;
      for (;;) {
        int b2 = i ? k + i - 1 : k;
        int a = 1 << b2;
        if (numSyms <= mk + 3 * a) {
          b.ns(uint32_t(numSyms - mk), uint32_t(v - mk));
          return;
        }
        bool more = v >= mk + a;
        b.f(more, 1);
        if (more) {
          i++;
          mk += a;
        } else {
          b.f(uint32_t(v - mk), b2);
          return;
        }
      }
    }

    void writeGlobalParam(Bits& b, int type, int idx, int32_t value, int32_t prev, bool allowHp) {
      int absBits = 12, precBits = 15;      /* GM_ABS_ALPHA_BITS, GM_ALPHA_PREC_BITS */
      if (idx < 2) {
        if (type == 1 /* TRANSLATION */) {
          absBits = 9 - !allowHp;          /* GM_ABS_TRANS_ONLY_BITS */
          precBits = 3 - !allowHp;         /* GM_TRANS_ONLY_PREC_BITS */
        } else {
          absBits = 12;                     /* GM_ABS_TRANS_BITS */
          precBits = 6;                     /* GM_TRANS_PREC_BITS */
        }
      }
      int precDiff = WARPEDMODEL_PREC_BITS - precBits;
      int32_t round = (idx % 3) == 2 ? (1 << WARPEDMODEL_PREC_BITS) : 0;
      int32_t sub = (idx % 3) == 2 ? (1 << precBits) : 0;
      int32_t mx = 1 << absBits;
      int32_t r = (prev >> precDiff) - sub;
      int32_t x = (value - round) >> precDiff;    /* decoded value before << precDiff */
      int low = -mx, high = mx + 1;
      int n = high - low;
      int v = recenterFiniteNonneg(n, r - low, x - low);
      writeSubexp(b, n, v);
    }

    int32_t relDist(int a, int b, int bits) {
      if (!bits) return 0;
      int diff = a - b;
      int m = 1 << (bits - 1);
      return (diff & (m - 1)) - (diff & m);
    }

    const int kSegFeatureBits[8] = { 8, 6, 6, 6, 6, 3, 0, 0 };
    const bool kSegFeatureSigned[8] = { true, true, true, true, true, false, false, false };

    void writeDeltaQ(Bits& b, int8_t v) {
      b.f(v != 0, 1);
      if (v)
        b.su(v, 7);
    }

    std::vector<uint8_t> seqHeader(const DxvaPicParamsAV1& pp) {
      const uint32_t cf = pp.CodingParamToolFlags;
      Bits b;
      b.f(pp.seq_profile, 3);
      b.f(0, 1);                         /* still_picture */
      b.f(0, 1);                         /* reduced_still_picture_header */
      b.f(0, 1);                         /* timing_info_present_flag */
      b.f(0, 1);                         /* initial_display_delay_present_flag */
      b.f(0, 5);                         /* operating_points_cnt_minus_1 */
      b.f(0, 12);                        /* operating_point_idc[0] */
      uint64_t px = uint64_t(pp.max_width) * pp.max_height;
      uint32_t level = px <= 2228224u ? 9u : px <= 8912896u ? 13u : 17u;   /* 4.1 / 5.1 / 6.1 */
      b.f(level, 5);
      if (level > 7)
        b.f(0, 1);                       /* seq_tier */
      int wBits = 1, hBits = 1;
      while ((1u << wBits) < pp.max_width) wBits++;
      while ((1u << hBits) < pp.max_height) hBits++;
      b.f(uint32_t(wBits - 1), 4);
      b.f(uint32_t(hBits - 1), 4);
      b.f(pp.max_width - 1, wBits);
      b.f(pp.max_height - 1, hBits);
      b.f(0, 1);                         /* frame_id_numbers_present_flag */
      b.f(!!(cf & C_SB128), 1);
      b.f(!!(cf & C_FILTER_INTRA), 1);
      b.f(!!(cf & C_INTRA_EDGE), 1);
      b.f(!!(cf & C_INTERINTRA), 1);
      b.f(!!(cf & C_MASKED), 1);
      b.f(1, 1);                         /* enable_warped_motion: frame-level flag decides */
      b.f(!!(cf & C_DUAL_FILTER), 1);
      bool orderHint = pp.order_hint_bits > 0;
      b.f(orderHint, 1);
      if (orderHint) {
        b.f(!!(cf & C_JNT_COMP), 1);
        b.f(!!(cf & C_ENABLE_REF_MVS), 1);
      }
      b.f(1, 1);                         /* seq_choose_screen_content_tools -> SELECT */
      b.f(1, 1);                         /* seq_choose_integer_mv -> SELECT */
      if (orderHint)
        b.f(uint32_t(pp.order_hint_bits - 1), 3);
      b.f(1, 1);                         /* enable_superres (frame decides) */
      b.f(!!(cf & C_CDEF), 1);
      b.f(!!(cf & C_RESTORATION), 1);
      /* color_config */
      b.f(pp.bitdepth > 8, 1);           /* high_bitdepth */
      if (pp.seq_profile == 2 && pp.bitdepth > 8)
        b.f(pp.bitdepth == 12, 1);
      bool mono = pp.FormatAndPictureInfoFlags & 0x40;
      if (pp.seq_profile != 1)
        b.f(mono, 1);
      b.f(0, 1);                         /* color_description_present_flag */
      if (mono) {
        b.f(0, 1);                       /* color_range */
      } else {
        b.f(0, 1);                       /* color_range */
        bool ssx = pp.FormatAndPictureInfoFlags & 0x10, ssy = pp.FormatAndPictureInfoFlags & 0x20;
        if (pp.seq_profile == 2 && pp.bitdepth == 12) {
          b.f(ssx, 1);
          if (ssx) b.f(ssy, 1);
        }
        if (ssx && ssy)
          b.f(0, 2);                     /* chroma_sample_position */
        b.f(1, 1);                       /* separate_uv_delta_q */
      }
      b.f(!!(cf & C_FILM_GRAIN), 1);
      b.trailing();
      return b.data();
    }

  }


  bool av1WriteFrame(
          Av1State&               st,
    const DxvaPicParamsAV1&       pp,
    const DxvaTileAV1*            tiles,
          uint32_t                tileCount,
    const uint8_t*                data,
          size_t                  dataSize,
          uint8_t                 refresh,
    const int*                    refSlots,
          std::vector<uint8_t>&   out,
          const char**            pError) {
    auto fail = [pError](const char* e) { if (pError) *pError = e; return false; };
    const uint32_t cf = pp.CodingParamToolFlags;
    const int frameType = pp.FormatAndPictureInfoFlags & 3;
    const bool showFrame = pp.FormatAndPictureInfoFlags & 4;
    const bool showable = pp.FormatAndPictureInfoFlags & 8;
    const bool mono = pp.FormatAndPictureInfoFlags & 0x40;
    const bool ssx = pp.FormatAndPictureInfoFlags & 0x10, ssy = pp.FormatAndPictureInfoFlags & 0x20;
    const int numPlanes = mono ? 1 : 3;
    const bool frameIsIntra = frameType == KEY_FRAME || frameType == INTRA_ONLY_FRAME;
    const bool orderHintOn = pp.order_hint_bits > 0;
    const int ohBits = pp.order_hint_bits;

    if (cf & C_SUPERRES)
      return fail("superres");
    if (frameType == SWITCH_FRAME)
      return fail("switch frame");
    if (!tileCount)
      return fail("no tiles");

    /* sequence header on key frames / changes (the caller adds the temporal
     * delimiter: a hidden frame shares its temporal unit with the next frame) */
    std::vector<uint8_t> seq = seqHeader(pp);
    if (frameType == KEY_FRAME || seq != st.lastSeqHdr) {
      obu(out, 1, seq);
      st.lastSeqHdr = seq;
    }

    Bits b;
    b.f(0, 1);                           /* show_existing_frame */
    b.f(uint32_t(frameType), 2);
    b.f(showFrame, 1);
    if (!showFrame)
      b.f(showable, 1);
    bool errorResilient = frameType == KEY_FRAME && showFrame;
    if (!errorResilient)
      b.f(0, 1);                         /* error_resilient_mode */
    b.f(!!(cf & C_DISABLE_CDF), 1);
    bool screenContent = cf & C_SCREEN_CONTENT;
    b.f(screenContent, 1);               /* allow_screen_content_tools (SELECT) */
    bool forceIntegerMv = false;
    if (screenContent) {
      forceIntegerMv = cf & C_INTEGER_MV;
      b.f(forceIntegerMv, 1);
    }
    if (frameIsIntra)
      forceIntegerMv = true;
    bool sizeOverride = pp.width != pp.max_width || pp.height != pp.max_height;
    b.f(sizeOverride, 1);
    if (orderHintOn)
      b.f(pp.order_hint, ohBits);
    int primaryRef = pp.primary_ref_frame;
    if (frameIsIntra || errorResilient)
      primaryRef = PRIMARY_REF_NONE;
    else
      b.f(uint32_t(primaryRef), 3);

    uint8_t refreshFlags = refresh;
    if (frameType == KEY_FRAME && showFrame)
      refreshFlags = 0xff;
    else
      b.f(refreshFlags, 8);
    if (frameType == INTRA_ONLY_FRAME && refreshFlags == 0xff)
      return fail("intra-only frame refreshing all slots");

    int wBits = 1, hBits = 1;
    while ((1u << wBits) < pp.max_width) wBits++;
    while ((1u << hBits) < pp.max_height) hBits++;
    auto frameSize = [&]() {
      if (sizeOverride) {
        b.f(pp.width - 1, wBits);
        b.f(pp.height - 1, hBits);
      }
      b.f(0, 1);                         /* superres_params: use_superres (enable_superres = 1) */
    };

    int refSlot[7] = { };
    bool allowIntrabc = false;
    bool allowHp = false;
    if (frameIsIntra) {
      frameSize();
      b.f(0, 1);                         /* render_and_frame_size_different */
      if (screenContent) {
        allowIntrabc = cf & C_INTRABC;
        b.f(allowIntrabc, 1);
      }
    } else {
      if (orderHintOn)
        b.f(0, 1);                       /* frame_refs_short_signaling */
      for (int i = 0; i < 7; i++) {
        int slot = refSlots[i];
        if (slot < 0 || slot > 7 || !st.slotValid[slot])
          return fail("reference slot empty");
        refSlot[i] = slot;
        b.f(uint32_t(slot), 3);
      }
      if (sizeOverride) {
        for (int i = 0; i < 7; i++)
          b.f(0, 1);                     /* found_ref */
        frameSize();
        b.f(0, 1);                       /* render_and_frame_size_different */
      } else {
        frameSize();
        b.f(0, 1);
      }
      if (!forceIntegerMv) {
        allowHp = cf & C_HP_MV;
        b.f(allowHp, 1);
      }
      bool switchable = pp.interp_filter == 4;
      b.f(switchable, 1);
      if (!switchable)
        b.f(pp.interp_filter, 2);
      b.f(!!(cf & C_SWITCHABLE_MOTION), 1);
      if (cf & C_ENABLE_REF_MVS)
        b.f(!!(cf & C_USE_REF_MVS), 1);
    }

    if (!(cf & C_DISABLE_CDF))
      b.f(!!(cf & C_DISABLE_FRAME_END_CDF), 1);

    /* ---- tile_info (explicit sizes) ---- */
    const bool sb128 = cf & C_SB128;
    int miCols = 2 * ((int(pp.width) + 7) >> 3);
    int miRows = 2 * ((int(pp.height) + 7) >> 3);
    int sbCols = sb128 ? ((miCols + 31) >> 5) : ((miCols + 15) >> 4);
    int sbRows = sb128 ? ((miRows + 31) >> 5) : ((miRows + 15) >> 4);
    int sbSize = (sb128 ? 5 : 4) + 2;
    int maxTileWidthSb = 4096 >> sbSize;
    int maxTileAreaSb = (4096 * 2304) >> (2 * sbSize);
    int minLog2TileCols = tileLog2(maxTileWidthSb, sbCols);
    int minLog2Tiles = std::max(minLog2TileCols, tileLog2(maxTileAreaSb, sbRows * sbCols));
    b.f(0, 1);                           /* uniform_tile_spacing_flag */
    int startSb = 0, widest = 0, cols = 0;
    for (; startSb < sbCols && cols < pp.tiles.cols; cols++) {
      int maxWidth = std::min(sbCols - startSb, maxTileWidthSb);
      int w = pp.tiles.widths[cols];
      if (w < 1 || w > maxWidth)
        return fail("tile width");
      b.ns(uint32_t(maxWidth), uint32_t(w - 1));
      widest = std::max(widest, w);
      startSb += w;
    }
    if (startSb != sbCols)
      return fail("tile columns do not cover the frame");
    int colsLog2 = tileLog2(1, cols);
    int area = minLog2Tiles > 0 ? (sbRows * sbCols) >> (minLog2Tiles + 1) : sbRows * sbCols;
    int maxTileHeightSb = std::max(area / widest, 1);
    startSb = 0;
    int rows = 0;
    for (; startSb < sbRows && rows < pp.tiles.rows; rows++) {
      int maxHeight = std::min(sbRows - startSb, maxTileHeightSb);
      int h = pp.tiles.heights[rows];
      if (h < 1 || h > maxHeight)
        return fail("tile height");
      b.ns(uint32_t(maxHeight), uint32_t(h - 1));
      startSb += h;
    }
    if (startSb != sbRows)
      return fail("tile rows do not cover the frame");
    int rowsLog2 = tileLog2(1, rows);
    const int tileSizeBytes = 4;
    if (colsLog2 > 0 || rowsLog2 > 0) {
      b.f(pp.tiles.context_update_id, rowsLog2 + colsLog2);
      b.f(tileSizeBytes - 1, 2);
    }
    if (uint32_t(cols * rows) != tileCount)
      return fail("tile count does not match the tile layout");

    /* ---- quantization_params ---- */
    const auto& q = pp.quantization;
    b.f(q.base_qindex, 8);
    writeDeltaQ(b, q.y_dc_delta_q);
    if (numPlanes > 1) {
      bool diffUv = q.u_dc_delta_q != q.v_dc_delta_q || q.u_ac_delta_q != q.v_ac_delta_q;
      b.f(diffUv, 1);                    /* separate_uv_delta_q = 1 */
      writeDeltaQ(b, q.u_dc_delta_q);
      writeDeltaQ(b, q.u_ac_delta_q);
      if (diffUv) {
        writeDeltaQ(b, q.v_dc_delta_q);
        writeDeltaQ(b, q.v_ac_delta_q);
      }
    }
    bool usingQm = q.qm_y != 0xff;
    b.f(usingQm, 1);
    if (usingQm) {
      b.f(q.qm_y, 4);
      b.f(q.qm_u, 4);
      b.f(q.qm_v, 4);                    /* separate_uv_delta_q = 1 */
    }

    /* ---- segmentation_params ---- */
    const auto& sg = pp.segmentation;
    bool segEnabled = sg.ControlFlags & 1;
    b.f(segEnabled, 1);
    if (segEnabled) {
      bool updateData = true;
      if (primaryRef != PRIMARY_REF_NONE) {
        bool updateMap = sg.ControlFlags & 2;
        b.f(updateMap, 1);
        if (updateMap)
          b.f(!!(sg.ControlFlags & 8), 1);
        updateData = sg.ControlFlags & 4;
        b.f(updateData, 1);
      }
      if (updateData) {
        for (int i = 0; i < 8; i++) {
          for (int j = 0; j < 8; j++) {
            bool en = (sg.feature_mask[i] >> j) & 1;
            b.f(en, 1);
            if (en) {
              int bits = kSegFeatureBits[j];
              if (kSegFeatureSigned[j])
                b.su(sg.feature_data[i][j], 1 + bits);
              else if (bits)
                b.f(uint32_t(sg.feature_data[i][j]), bits);
            }
          }
        }
      }
    }

    /* ---- delta_q / delta_lf ---- */
    bool deltaQPresent = false;
    if (q.base_qindex > 0) {
      deltaQPresent = q.ControlFlags & 1;
      b.f(deltaQPresent, 1);
      if (deltaQPresent)
        b.f((q.ControlFlags >> 1) & 3, 2);
    }
    const auto& lf = pp.loop_filter;
    if (deltaQPresent && !allowIntrabc) {
      bool lfPresent = lf.ControlFlags & 8;
      b.f(lfPresent, 1);
      if (lfPresent) {
        b.f(lf.delta_lf_res, 2);
        b.f(!!(lf.ControlFlags & 4), 1);
      }
    }

    /* lossless */
    bool codedLossless = true;
    for (int seg = 0; seg < 8; seg++) {
      int qindex = q.base_qindex;
      if (segEnabled && (sg.feature_mask[seg] & 1))
        qindex = std::clamp(int(q.base_qindex) + sg.feature_data[seg][0], 0, 255);
      bool lossless = qindex == 0 && !q.y_dc_delta_q && !q.u_ac_delta_q && !q.u_dc_delta_q &&
                      !q.v_ac_delta_q && !q.v_dc_delta_q;
      codedLossless &= lossless;
    }
    bool allLossless = codedLossless;   /* no superres */

    /* ---- loop_filter_params ---- */
    if (!codedLossless && !allowIntrabc) {
      b.f(lf.filter_level[0], 6);
      b.f(lf.filter_level[1], 6);
      if (numPlanes > 1 && (lf.filter_level[0] || lf.filter_level[1])) {
        b.f(lf.filter_level_u, 6);
        b.f(lf.filter_level_v, 6);
      }
      b.f(lf.sharpness_level, 3);
      bool deltaEnabled = lf.ControlFlags & 1;
      b.f(deltaEnabled, 1);
      if (deltaEnabled) {
        b.f(1, 1);                       /* loop_filter_delta_update: send the effective values */
        for (int i = 0; i < 8; i++) {
          b.f(1, 1);
          b.su(lf.ref_deltas[i], 7);
        }
        for (int i = 0; i < 2; i++) {
          b.f(1, 1);
          b.su(lf.mode_deltas[i], 7);
        }
      }
    }

    /* ---- cdef_params ---- */
    if (!codedLossless && !allowIntrabc && (cf & C_CDEF)) {
      const auto& cd = pp.cdef;
      b.f(cd.ControlFlags & 3, 2);
      int cdefBits = (cd.ControlFlags >> 2) & 3;
      b.f(uint32_t(cdefBits), 2);
      for (int i = 0; i < (1 << cdefBits); i++) {
        b.f(cd.y_strengths[i] & 0x3f, 4);
        b.f(cd.y_strengths[i] >> 6, 2);
        if (numPlanes > 1) {
          b.f(cd.uv_strengths[i] & 0x3f, 4);
          b.f(cd.uv_strengths[i] >> 6, 2);
        }
      }
    }

    /* ---- lr_params ---- */
    if (!allLossless && !allowIntrabc && (cf & C_RESTORATION)) {
      static const uint8_t kCoded[4] = { 0, 2, 3, 1 };   /* DXVA None/Wiener/Sgr/Switchable -> lr_type */
      bool usesLr = false, usesChromaLr = false;
      for (int i = 0; i < numPlanes; i++) {
        uint8_t t = lf.frame_restoration_type[i] & 3;
        b.f(kCoded[t], 2);
        if (t) {
          usesLr = true;
          if (i > 0) usesChromaLr = true;
        }
      }
      if (usesLr) {
        int shift = int(lf.log2_restoration_unit_size[0]) - 6;
        if (sb128) {
          if (shift < 1 || shift > 2) return fail("lr unit size");
          b.f(uint32_t(shift - 1), 1);
        } else {
          if (shift < 0 || shift > 2) return fail("lr unit size");
          b.f(shift > 0, 1);
          if (shift > 0)
            b.f(shift == 2, 1);
        }
        if (ssx && ssy && usesChromaLr) {
          int uvShift = int(lf.log2_restoration_unit_size[0]) - int(lf.log2_restoration_unit_size[1]);
          b.f(uvShift > 0, 1);
        }
      }
    }

    /* ---- read_tx_mode ---- */
    if (!codedLossless)
      b.f(((cf >> 22) & 3) == 2, 1);     /* tx_mode_select */

    /* ---- frame_reference_mode / skip_mode ---- */
    bool referenceSelect = false;
    if (!frameIsIntra) {
      referenceSelect = cf & C_REFERENCE_MODE;
      b.f(referenceSelect, 1);
    }
    bool skipModeAllowed = false;
    if (!frameIsIntra && referenceSelect && orderHintOn) {
      int fwdIdx = -1, bwdIdx = -1, fwdHint = 0, bwdHint = 0;
      for (int i = 0; i < 7; i++) {
        int refHint = st.slotOrderHint[refSlot[i]];
        if (relDist(refHint, pp.order_hint, ohBits) < 0) {
          if (fwdIdx < 0 || relDist(refHint, fwdHint, ohBits) > 0) { fwdIdx = i; fwdHint = refHint; }
        } else if (relDist(refHint, pp.order_hint, ohBits) > 0) {
          if (bwdIdx < 0 || relDist(refHint, bwdHint, ohBits) < 0) { bwdIdx = i; bwdHint = refHint; }
        }
      }
      if (fwdIdx < 0) {
        skipModeAllowed = false;
      } else if (bwdIdx >= 0) {
        skipModeAllowed = true;
      } else {
        int secIdx = -1, secHint = 0;
        for (int i = 0; i < 7; i++) {
          int refHint = st.slotOrderHint[refSlot[i]];
          if (relDist(refHint, fwdHint, ohBits) < 0) {
            if (secIdx < 0 || relDist(refHint, secHint, ohBits) > 0) { secIdx = i; secHint = refHint; }
          }
        }
        skipModeAllowed = secIdx >= 0;
      }
    }
    if (skipModeAllowed)
      b.f(!!(cf & C_SKIP_MODE), 1);

    if (!frameIsIntra && !errorResilient)
      b.f(!!(cf & C_WARPED), 1);         /* allow_warped_motion (enable_warped_motion = 1) */
    b.f(!!(cf & C_REDUCED_TX), 1);

    /* ---- global_motion_params ---- */
    int32_t gm[8][6];
    for (int r = 1; r <= 7; r++) {
      gm[r][0] = gm[r][1] = gm[r][3] = gm[r][4] = 0;
      gm[r][2] = gm[r][5] = 1 << WARPEDMODEL_PREC_BITS;
    }
    if (!frameIsIntra) {
      int32_t prev[8][6];
      for (int r = 1; r <= 7; r++) {
        if (primaryRef == PRIMARY_REF_NONE) {
          prev[r][0] = prev[r][1] = prev[r][3] = prev[r][4] = 0;
          prev[r][2] = prev[r][5] = 1 << WARPEDMODEL_PREC_BITS;
        } else {
          std::memcpy(prev[r], st.slotGm[refSlot[primaryRef]][r], sizeof(prev[r]));
        }
      }
      for (int r = 1; r <= 7; r++) {
        const auto& fr = pp.frame_refs[r - 1];
        int type = (fr.GlobalMotionFlags >> 1) & 3;
        b.f(type != 0, 1);                       /* is_global */
        if (type != 0) {
          b.f(type == 2, 1);                     /* is_rot_zoom */
          if (type != 2)
            b.f(type == 1, 1);                   /* is_translation */
        }
        int32_t p[6];
        for (int k = 0; k < 6; k++)
          p[k] = fr.wmmat[k];
        if (type == 0) {
          p[0] = p[1] = p[3] = p[4] = 0;
          p[2] = p[5] = 1 << WARPEDMODEL_PREC_BITS;
        }
        if (type >= 2) {
          writeGlobalParam(b, type, 2, p[2], prev[r][2], allowHp);
          writeGlobalParam(b, type, 3, p[3], prev[r][3], allowHp);
          if (type == 3) {
            writeGlobalParam(b, type, 4, p[4], prev[r][4], allowHp);
            writeGlobalParam(b, type, 5, p[5], prev[r][5], allowHp);
          } else {
            p[4] = -p[3];
            p[5] = p[2];
          }
        }
        if (type >= 1) {
          writeGlobalParam(b, type, 0, p[0], prev[r][0], allowHp);
          writeGlobalParam(b, type, 1, p[1], prev[r][1], allowHp);
        }
        std::memcpy(gm[r], p, sizeof(p));
      }
    }

    /* ---- film_grain_params ---- */
    if ((cf & C_FILM_GRAIN) && (showFrame || showable)) {
      const auto& fg = pp.film_grain;
      bool apply = fg.ControlFlags & 1;
      b.f(apply, 1);
      if (apply) {
        b.f(fg.grain_seed, 16);
        if (frameType == INTER_FRAME)
          b.f(1, 1);                             /* update_grain */
        b.f(fg.num_y_points, 4);
        for (int i = 0; i < fg.num_y_points; i++) {
          b.f(fg.scaling_points_y[i][0], 8);
          b.f(fg.scaling_points_y[i][1], 8);
        }
        bool csFromLuma = (fg.ControlFlags >> 3) & 1;
        if (!mono)
          b.f(csFromLuma, 1);
        else
          csFromLuma = false;
        int numCb = 0, numCr = 0;
        if (!(mono || csFromLuma || (ssx && ssy && fg.num_y_points == 0))) {
          numCb = fg.num_cb_points;
          b.f(uint32_t(numCb), 4);
          for (int i = 0; i < numCb; i++) {
            b.f(fg.scaling_points_cb[i][0], 8);
            b.f(fg.scaling_points_cb[i][1], 8);
          }
          numCr = fg.num_cr_points;
          b.f(uint32_t(numCr), 4);
          for (int i = 0; i < numCr; i++) {
            b.f(fg.scaling_points_cr[i][0], 8);
            b.f(fg.scaling_points_cr[i][1], 8);
          }
        }
        b.f((fg.ControlFlags >> 1) & 3, 2);      /* grain_scaling_minus_8 */
        int lag = (fg.ControlFlags >> 4) & 3;
        b.f(uint32_t(lag), 2);
        int numPosLuma = 2 * lag * (lag + 1);
        int numPosChroma = numPosLuma;
        if (fg.num_y_points) {
          numPosChroma = numPosLuma + 1;
          for (int i = 0; i < numPosLuma; i++)
            b.f(fg.ar_coeffs_y[i], 8);
        }
        if (csFromLuma || numCb)
          for (int i = 0; i < numPosChroma; i++)
            b.f(fg.ar_coeffs_cb[i], 8);
        if (csFromLuma || numCr)
          for (int i = 0; i < numPosChroma; i++)
            b.f(fg.ar_coeffs_cr[i], 8);
        b.f((fg.ControlFlags >> 6) & 3, 2);      /* ar_coeff_shift_minus_6 */
        b.f((fg.ControlFlags >> 8) & 3, 2);      /* grain_scale_shift */
        if (numCb) {
          b.f(fg.cb_mult, 8);
          b.f(fg.cb_luma_mult, 8);
          b.f(uint32_t(fg.cb_offset) & 0x1ff, 9);
        }
        if (numCr) {
          b.f(fg.cr_mult, 8);
          b.f(fg.cr_luma_mult, 8);
          b.f(uint32_t(fg.cr_offset) & 0x1ff, 9);
        }
        b.f((fg.ControlFlags >> 10) & 1, 1);     /* overlap_flag */
        b.f((fg.ControlFlags >> 11) & 1, 1);     /* clip_to_restricted_range */
      }
    }
    b.trailing();
    obu(out, 3, b.data());               /* OBU_FRAME_HEADER */

    /* ---- tile group ---- */
    std::vector<uint8_t> tg;
    if (tileCount > 1)
      tg.push_back(0);                   /* tile_start_and_end_present_flag = 0 + byte alignment */
    for (uint32_t i = 0; i < tileCount; i++) {
      const auto& t = tiles[i];
      if (size_t(t.DataOffset) + t.DataSize > dataSize || !t.DataSize)
        return fail("tile data out of range");
      if (i + 1 < tileCount) {
        uint32_t s = t.DataSize - 1;
        for (int k = 0; k < tileSizeBytes; k++)
          tg.push_back(uint8_t(s >> (8 * k)));
      }
      tg.insert(tg.end(), data + t.DataOffset, data + t.DataOffset + t.DataSize);
    }
    obu(out, 4, tg);                     /* OBU_TILE_GROUP */

    /* ---- reference update (decoder side mirror) ---- */
    for (int s = 0; s < 8; s++) {
      if (refreshFlags & (1u << s)) {
        st.slotValid[s] = true;
        st.slotTex[s] = pp.CurrPicTextureIndex;
        st.slotOrderHint[s] = pp.order_hint;
        for (int r = 1; r <= 7; r++)
          std::memcpy(st.slotGm[s][r], gm[r], sizeof(gm[r]));
      }
    }
    return true;
  }

}
