#if defined(__ANDROID__)
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

#include <algorithm>
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "d3d11_video_mc.h"

#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_texture.h"

namespace dxvk {

  /* Decoder messages also go to logcat (tag NptHwDec, with pid): the DXVK log
   * file is shared by every render-server worker and gets overwritten. */
  static void mcLog(int prio, const std::string& msg) {
#if defined(__ANDROID__)
    __android_log_write(prio, "NptHwDec", msg.c_str());
#endif
    if (prio >= 6) Logger::err(msg); else if (prio >= 5) Logger::warn(msg); else Logger::info(msg);
  }
#define MC_INFO(x) mcLog(4, str::format x)
#define MC_WARN(x) mcLog(5, str::format x)
#define MC_ERR(x)  mcLog(6, str::format x)

  /* Per-call trace when /data/adb/fold-vm/npt/hwdec-trace exists (checked
   * every 2 s), to logcat only. */
  static bool mcTrace() {
    static double s_next = 0.0;
    static bool s_on = false;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    if (now >= s_next) {
      s_on = access("/data/adb/fold-vm/npt/hwdec-trace", F_OK) == 0;
      s_next = now + 2.0;
    }
    return s_on;
  }
#define MC_TRACE(x) do { if (mcTrace()) { std::string _m = str::format x; __android_log_write(3, "NptHwDec", _m.c_str()); } } while (0)

  /* <cutils/native_handle.h> is not in the NDK */
  struct native_handle_t { int version, numFds, numInts; int data[]; };

  /* ------------------------------------------------------------------ */
  /* DXVA structures (dxva.h, all byte-packed)                           */
  /* ------------------------------------------------------------------ */

#pragma pack(push, 1)
  struct DxvaPicEntry { uint8_t bPicEntry; };

  struct DxvaPicParamsH264 {
    uint16_t wFrameWidthInMbsMinus1;
    uint16_t wFrameHeightInMbsMinus1;
    DxvaPicEntry CurrPic;
    uint8_t  num_ref_frames;
    uint16_t wBitFields;
    uint8_t  bit_depth_luma_minus8;
    uint8_t  bit_depth_chroma_minus8;
    uint16_t Reserved16Bits;
    uint32_t StatusReportFeedbackNumber;
    DxvaPicEntry RefFrameList[16];
    int32_t  CurrFieldOrderCnt[2];
    int32_t  FieldOrderCntList[16][2];
    int8_t   pic_init_qs_minus26;
    int8_t   chroma_qp_index_offset;
    int8_t   second_chroma_qp_index_offset;
    uint8_t  ContinuationFlag;
    int8_t   pic_init_qp_minus26;
    uint8_t  num_ref_idx_l0_active_minus1;
    uint8_t  num_ref_idx_l1_active_minus1;
    uint8_t  Reserved8BitsA;
    uint16_t FrameNumList[16];
    uint32_t UsedForReferenceFlags;
    uint16_t NonExistingFrameFlags;
    uint16_t frame_num;
    uint8_t  log2_max_frame_num_minus4;
    uint8_t  pic_order_cnt_type;
    uint8_t  log2_max_pic_order_cnt_lsb_minus4;
    uint8_t  delta_pic_order_always_zero_flag;
    uint8_t  direct_8x8_inference_flag;
    uint8_t  entropy_coding_mode_flag;
    uint8_t  pic_order_present_flag;
    uint8_t  num_slice_groups_minus1;
    uint8_t  slice_group_map_type;
    uint8_t  deblocking_filter_control_present_flag;
    uint8_t  redundant_pic_cnt_present_flag;
    uint8_t  Reserved8BitsB;
    uint16_t slice_group_change_rate_minus1;
    uint8_t  SliceGroupMap[810];
  };

  struct DxvaQmatrixH264 {
    uint8_t bScalingLists4x4[6][16];
    uint8_t bScalingLists8x8[2][64];
  };

  struct DxvaSliceShort {
    uint32_t BSNALunitDataLocation;
    uint32_t SliceBytesInBuffer;
    uint16_t wBadSliceChopping;
  };
#pragma pack(pop)

  static_assert(sizeof(DxvaSliceShort) == 10, "DXVA slice layout");

  /* wBitFields of DXVA_PicParams_H264 */
  enum : uint16_t {
    H264_FIELD_PIC        = 1u << 0,
    H264_MBAFF            = 1u << 1,
    H264_CHROMA_SHIFT     = 4,
    H264_CONSTRAINED_INTRA= 1u << 7,
    H264_WEIGHTED_PRED    = 1u << 8,
    H264_WBIPRED_SHIFT    = 9,
    H264_FRAME_MBS_ONLY   = 1u << 12,
    H264_TRANSFORM_8X8    = 1u << 13,
    H264_INTRA_PIC        = 1u << 15,
  };

  /* {1b81be68-a0c7-11d3-b984-00c04f2e73c5} */
  static const GUID GUID_H264_VLD_NOFGT =
    { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } };
  /* {463707f8-a1d0-4585-876d-83aa6d60b89e} */
  static const GUID GUID_VP9_VLD_PROFILE0 =
    { 0x463707f8, 0xa1d0, 0x4585, { 0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e } };

  /* {b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a} */
  static const GUID GUID_AV1_VLD_PROFILE0 =
    { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } };

  static double nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
  }

  static bool envFlag(const char* name, bool def) {
    const char* v = getenv(name);
    if (!v || !*v)
      return def;
    return v[0] != '0';
  }

  /* ------------------------------------------------------------------ */
  /* Profiles                                                           */
  /* ------------------------------------------------------------------ */

  bool D3D11VideoMcAvailable() {
#if defined(__ANDROID__)
    static const bool s_on = envFlag("NPT_HWDEC", true);
    return s_on;
#else
    return false;
#endif
  }

  uint32_t D3D11VideoMcProfileCount() {
    /* AV1 can be switched off (software decode in the guest):
     * NPT_HWDEC_AV1=0 or the flag file hwdec-noav1 */
    static const bool s_av1 = envFlag("NPT_HWDEC_AV1", true)
      && access("/data/adb/fold-vm/npt/hwdec-noav1", F_OK) != 0;
    return D3D11VideoMcAvailable() ? (s_av1 ? 3u : 2u) : 0u;
  }

  bool D3D11VideoMcProfile(uint32_t index, GUID* pGuid) {
    if (!D3D11VideoMcAvailable())
      return false;
    switch (index) {
      case 0: *pGuid = GUID_H264_VLD_NOFGT; return true;
      case 1: *pGuid = GUID_VP9_VLD_PROFILE0; return true;
      case 2: if (D3D11VideoMcProfileCount() < 3) return false;
              *pGuid = GUID_AV1_VLD_PROFILE0; return true;
      default: return false;
    }
  }

  D3D11VideoCodec D3D11VideoMcCodecFor(const GUID& guid) {
    if (!D3D11VideoMcAvailable())
      return D3D11VideoCodec::None;
    if (guid == GUID_H264_VLD_NOFGT)
      return D3D11VideoCodec::H264;
    if (guid == GUID_VP9_VLD_PROFILE0)
      return D3D11VideoCodec::VP9;
    if (guid == GUID_AV1_VLD_PROFILE0 && D3D11VideoMcProfileCount() >= 3)
      return D3D11VideoCodec::AV1;
    return D3D11VideoCodec::None;
  }

  /* ------------------------------------------------------------------ */
  /* Bit writer for H.264 parameter sets                                */
  /* ------------------------------------------------------------------ */

  class BitWriter {
  public:
    void bits(uint32_t v, int n) {
      while (n--) {
        if (m_pos % 8 == 0)
          m_data.push_back(0);
        if ((v >> n) & 1)
          m_data.back() |= 0x80 >> (m_pos % 8);
        m_pos++;
      }
    }
    void ue(uint32_t v) {
      uint32_t x = v + 1;
      int len = 32 - __builtin_clz(x);
      bits(0, len - 1);
      bits(x, len);
    }
    void se(int32_t v) {
      ue(v > 0 ? 2 * uint32_t(v) - 1 : 2 * uint32_t(-v));
    }
    /* rbsp_trailing_bits + emulation prevention, prefixed with a start code */
    void finishNal(std::vector<uint8_t>& out) {
      bits(1, 1);
      while (m_pos % 8)
        bits(0, 1);
      static const uint8_t sc[] = { 0, 0, 0, 1 };
      out.insert(out.end(), sc, sc + 4);
      int zeros = 0;
      for (uint8_t b : m_data) {
        if (zeros >= 2 && b <= 3) {
          out.push_back(3);
          zeros = 0;
        }
        out.push_back(b);
        zeros = b ? 0 : zeros + 1;
      }
    }
  private:
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
  };

  /* Reads ue(v) from an escaped NAL payload (enough for slice headers). */
  class BitReader {
  public:
    BitReader(const uint8_t* p, size_t n) {
      for (size_t i = 0; i < n && m_rbsp.size() < 64; i++) {
        if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0)
          continue;
        m_rbsp.push_back(p[i]);
      }
    }
    uint32_t bit() {
      if (m_pos / 8 >= m_rbsp.size()) { m_pos++; return 0; }
      uint32_t b = (m_rbsp[m_pos / 8] >> (7 - m_pos % 8)) & 1;
      m_pos++;
      return b;
    }
    uint32_t bits(int n) { uint32_t v = 0; while (n--) v = v << 1 | bit(); return v; }
    uint32_t ue() {
      int z = 0;
      while (!bit() && z < 31) z++;
      return ((1u << z) - 1) + bits(z);
    }
  private:
    std::vector<uint8_t> m_rbsp;
    size_t m_pos = 0;
  };

  static void writeScalingList(BitWriter& w, const uint8_t* list, int size) {
    /* DXVA lists are in zig-zag scan order, which is the coding order */
    int last = 8;
    for (int j = 0; j < size; j++) {
      int delta = int(list[j]) - last;
      if (delta > 127) delta -= 256;
      if (delta < -128) delta += 256;
      w.se(delta);
      last = list[j];
    }
  }

  static void h264Sps(const DxvaPicParamsH264& pp, std::vector<uint8_t>& out) {
    BitWriter w;
    w.bits(0x67, 8);                 /* nal_ref_idc 3, type 7 */
    w.bits(100, 8);                  /* profile_idc: High */
    w.bits(0, 8);                    /* constraint flags */
    w.bits(51, 8);                   /* level_idc 5.1 */
    w.ue(0);                         /* seq_parameter_set_id */
    uint32_t chroma = (pp.wBitFields >> H264_CHROMA_SHIFT) & 3;
    w.ue(chroma);
    if (chroma == 3)
      w.bits(0, 1);
    w.ue(pp.bit_depth_luma_minus8);
    w.ue(pp.bit_depth_chroma_minus8);
    w.bits(0, 1);                    /* qpprime_y_zero_transform_bypass */
    w.bits(0, 1);                    /* seq_scaling_matrix_present (PPS has it) */
    w.ue(pp.log2_max_frame_num_minus4);
    w.ue(pp.pic_order_cnt_type);
    if (pp.pic_order_cnt_type == 0) {
      w.ue(pp.log2_max_pic_order_cnt_lsb_minus4);
    } else if (pp.pic_order_cnt_type == 1) {
      /* offsets are not in DXVA; streams with POC type 1 are rare */
      w.bits(pp.delta_pic_order_always_zero_flag, 1);
      w.se(0);
      w.se(0);
      w.ue(0);
    }
    w.ue(pp.num_ref_frames);
    w.bits(0, 1);                    /* gaps_in_frame_num_value_allowed */
    w.ue(pp.wFrameWidthInMbsMinus1);
    bool frameMbsOnly = pp.wBitFields & H264_FRAME_MBS_ONLY;
    uint32_t mapUnits = frameMbsOnly ? pp.wFrameHeightInMbsMinus1
                                     : (pp.wFrameHeightInMbsMinus1 + 1) / 2 - 1;
    w.ue(mapUnits);
    w.bits(frameMbsOnly, 1);
    if (!frameMbsOnly)
      w.bits((pp.wBitFields & H264_MBAFF) ? 1 : 0, 1);
    w.bits(pp.direct_8x8_inference_flag, 1);
    w.bits(0, 1);                    /* frame_cropping */
    w.bits(0, 1);                    /* vui_parameters_present */
    w.finishNal(out);
  }

  static void h264Pps(const DxvaPicParamsH264& pp, const DxvaQmatrixH264* qm,
                      uint32_t ppsId, std::vector<uint8_t>& out) {
    BitWriter w;
    w.bits(0x68, 8);                 /* nal_ref_idc 3, type 8 */
    w.ue(ppsId);
    w.ue(0);                         /* seq_parameter_set_id */
    w.bits(pp.entropy_coding_mode_flag, 1);
    w.bits(pp.pic_order_present_flag, 1);
    w.ue(0);                         /* num_slice_groups_minus1 (no FMO) */
    w.ue(pp.num_ref_idx_l0_active_minus1);
    w.ue(pp.num_ref_idx_l1_active_minus1);
    w.bits((pp.wBitFields & H264_WEIGHTED_PRED) ? 1 : 0, 1);
    w.bits((pp.wBitFields >> H264_WBIPRED_SHIFT) & 3, 2);
    w.se(pp.pic_init_qp_minus26);
    w.se(pp.pic_init_qs_minus26);
    w.se(pp.chroma_qp_index_offset);
    w.bits(pp.deblocking_filter_control_present_flag, 1);
    w.bits((pp.wBitFields & H264_CONSTRAINED_INTRA) ? 1 : 0, 1);
    w.bits(pp.redundant_pic_cnt_present_flag, 1);

    bool t8x8 = pp.wBitFields & H264_TRANSFORM_8X8;
    bool flat = true;
    if (qm) {
      for (int i = 0; i < 6 && flat; i++)
        for (int j = 0; j < 16; j++)
          if (qm->bScalingLists4x4[i][j] != 16) { flat = false; break; }
      for (int i = 0; i < 2 && flat; i++)
        for (int j = 0; j < 64; j++)
          if (qm->bScalingLists8x8[i][j] != 16) { flat = false; break; }
    }
    if (t8x8 || !flat || pp.second_chroma_qp_index_offset != pp.chroma_qp_index_offset) {
      w.bits(t8x8, 1);
      w.bits(!flat, 1);              /* pic_scaling_matrix_present_flag */
      if (!flat) {
        for (int i = 0; i < 6; i++) {
          w.bits(1, 1);
          writeScalingList(w, qm->bScalingLists4x4[i], 16);
        }
        if (t8x8) {
          for (int i = 0; i < 2; i++) {
            w.bits(1, 1);
            writeScalingList(w, qm->bScalingLists8x8[i], 64);
          }
        }
      }
      w.se(pp.second_chroma_qp_index_offset);
    }
    w.finishNal(out);
  }

  /* ------------------------------------------------------------------ */
  /* Output view                                                        */
  /* ------------------------------------------------------------------ */

  D3D11VideoDecoderOutputView::D3D11VideoDecoderOutputView(
          D3D11Device*                          pDevice,
          ID3D11Resource*                       pResource,
    const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC& Desc)
  : D3D11DeviceChild<ID3D11VideoDecoderOutputView>(pDevice),
    m_resource(pResource), m_desc(Desc) {
    if (!GetCommonTexture(pResource))
      throw DxvkError("D3D11VideoDecoderOutputView: not a texture");
  }


  D3D11VideoDecoderOutputView::~D3D11VideoDecoderOutputView() {

  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDecoderOutputView::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D11DeviceChild)
     || riid == __uuidof(ID3D11View)
     || riid == __uuidof(ID3D11VideoDecoderOutputView)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }


  void STDMETHODCALLTYPE D3D11VideoDecoderOutputView::GetResource(
          ID3D11Resource**        ppResource) {
    *ppResource = m_resource.ref();
  }


  void STDMETHODCALLTYPE D3D11VideoDecoderOutputView::GetDesc(
          D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC* pDesc) {
    *pDesc = m_desc;
  }


  Rc<DxvkImage> D3D11VideoDecoderOutputView::GetImage() const {
    return GetCommonTexture(m_resource.ptr())->GetImage();
  }

  /* ------------------------------------------------------------------ */
  /* Decoder                                                            */
  /* ------------------------------------------------------------------ */

  D3D11VideoDecoder::D3D11VideoDecoder(
          D3D11Device*                pDevice,
    const D3D11_VIDEO_DECODER_DESC&   Desc,
    const D3D11_VIDEO_DECODER_CONFIG& Config)
  : D3D11DeviceChild<ID3D11VideoDecoder>(pDevice),
    m_desc(Desc), m_config(Config), m_codec(D3D11VideoMcCodecFor(Desc.Guid)) {
    if (m_codec == D3D11VideoCodec::None)
      throw DxvkError("D3D11VideoDecoder: unsupported profile");
    MC_INFO(("D3D11VideoDecoder: ", m_codec == D3D11VideoCodec::H264 ? "H.264" : m_codec == D3D11VideoCodec::AV1 ? "AV1" : "VP9",
      " ", Desc.SampleWidth, "x", Desc.SampleHeight, " raw=", Config.ConfigBitstreamRaw));
  }


  D3D11VideoDecoder::~D3D11VideoDecoder() {
    /* Queued CS commands hold a private reference, so none can still be
     * pending here; this may run on the CS thread. */
    m_destroying = true;
    DestroyCodec();
  }


  void D3D11VideoDecoder::FailDecode(const char* reason, uint64_t pts) {
    // CS and decoder submission threads can both report failure. Publish the
    // device-visible status before a following ordered native fence can pass.
    m_parent->GetDXVKDevice()->reportVideoDecodeFailure();
    if (!m_failed.exchange(true, std::memory_order_acq_rel))
      MC_ERR(("D3D11VideoDecoder: terminal decode failure, pts ", pts, ": ", reason));
  }


  void D3D11VideoDecoder::SyncCs() {
    if (!m_seq)
      return;
    auto ctx = m_parent->GetContext();
    D3D10DeviceLock lock = ctx->LockContext();
    ctx->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDecoder::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D11DeviceChild)
     || riid == __uuidof(ID3D11VideoDecoder)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDecoder::GetCreationParameters(
          D3D11_VIDEO_DECODER_DESC*   pVideoDesc,
          D3D11_VIDEO_DECODER_CONFIG* pConfig) {
    if (pVideoDesc) *pVideoDesc = m_desc;
    if (pConfig) *pConfig = m_config;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11VideoDecoder::GetDriverHandle(
          HANDLE*                 pDriverHandle) {
    if (pDriverHandle)
      *pDriverHandle = nullptr;
    return S_OK;
  }


  HRESULT D3D11VideoDecoder::GetBuffer(
          D3D11_VIDEO_DECODER_BUFFER_TYPE Type,
          UINT*                   pSize,
          void**                  ppBuffer) {
    if (uint32_t(Type) >= m_apiBufs.size())
      return E_INVALIDARG;
    auto& b = m_apiBufs[Type];
    size_t sz = Type == D3D11_VIDEO_DECODER_BUFFER_BITSTREAM
      ? std::max<size_t>(4u << 20, size_t(m_desc.SampleWidth) * m_desc.SampleHeight)
      : 64u << 10;
    if (b.size() < sz)
      b.resize(sz);
    *pSize = UINT(b.size());
    *ppBuffer = b.data();
    return S_OK;
  }


  HRESULT D3D11VideoDecoder::SubmitApiBuffers(
          UINT                    Count,
    const D3D11_VIDEO_DECODER_BUFFER_DESC* pDescs) {
    for (UINT i = 0; i < Count; i++) {
      uint32_t t = pDescs[i].BufferType;
      if (t >= m_bufs.size())
        continue;
      auto& src = m_apiBufs[t];
      size_t off = pDescs[i].DataOffset, sz = pDescs[i].DataSize;
      if (off + sz > src.size())
        return E_INVALIDARG;
      m_bufs[t].insert(m_bufs[t].end(), src.data() + off, src.data() + off + sz);
    }
    return S_OK;
  }


  HRESULT D3D11VideoDecoder::SubmitInline(
    const void*                   pData,
          UINT                    Size) {
    auto base = reinterpret_cast<const uint8_t*>(pData);
    if (Size < sizeof(NptDxvaSubmitHeader))
      return E_INVALIDARG;
    NptDxvaSubmitHeader hdr;
    memcpy(&hdr, base, sizeof(hdr));
    if (hdr.magic != NPT_DXVA_SUBMIT_MAGIC
     || sizeof(hdr) + size_t(hdr.count) * sizeof(NptDxvaSubmitEntry) > Size)
      return E_INVALIDARG;
    for (uint32_t i = 0; i < hdr.count; i++) {
      NptDxvaSubmitEntry e;
      memcpy(&e, base + sizeof(hdr) + i * sizeof(e), sizeof(e));
      if (size_t(e.offset) + e.size > Size || e.type >= m_bufs.size())
        return E_INVALIDARG;
      {
        const uint8_t* d = base + e.offset;
        MC_TRACE(("D3D11VideoDecoder: inline buffer type ", e.type, " size ", e.size, " off ", e.offset,
          " of ", Size, ": ", std::hex, e.size > 0 ? d[0] : 0, " ", e.size > 1 ? d[1] : 0, " ", e.size > 2 ? d[2] : 0,
          " ", e.size > 3 ? d[3] : 0));
      }
      /* a slice-control / bitstream pair must stay consistent: slices index
       * into the bitstream buffer of the same submission */
      m_bufs[e.type].insert(m_bufs[e.type].end(), base + e.offset, base + e.offset + e.size);
    }
    return S_OK;
  }


  HRESULT D3D11VideoDecoder::BeginFrame(
          D3D11VideoDecoderOutputView* pView) {
    if (!pView)
      return E_INVALIDARG;
    MC_TRACE(("Begin ", this, " seq ", m_seq + 1, " layer ", pView->GetLayer(), m_inFrame ? " (previous frame not ended)" : ""));
    if (pView->GetLayer() + 1 > m_maxLayer) {
      m_maxLayer = pView->GetLayer() + 1;
      if (m_maxLayer >= 4)
        MC_INFO(("D3D11VideoDecoder: app uses ", m_maxLayer, "+ output surfaces"));
    }
    m_target = pView;
    for (auto& b : m_bufs)
      b.clear();
    m_inFrame = true;
    return S_OK;
  }


  HRESULT D3D11VideoDecoder::EndFrame(
          D3D11ImmediateContext*  pContext) {
    MC_TRACE(("End ", this, " seq ", m_seq + 1, " inFrame ", m_inFrame, " bs ",
      m_bufs[D3D11_VIDEO_DECODER_BUFFER_BITSTREAM].size(), " pending ", m_pending.size()));
    if (!m_inFrame)
      return E_FAIL;
    m_inFrame = false;
    if (m_failed)
      return E_FAIL;

    Capture();
    if (m_codec == D3D11VideoCodec::AV1)
      return EndFrameAV1(pContext);

    bool expectOutput = true;
    uint32_t w = m_desc.SampleWidth, h = m_desc.SampleHeight;
    if (!BuildAccessUnit(&expectOutput, &w, &h))
      return S_OK;  /* nothing to decode (e.g. VP9 show_existing_frame) */

    if (!EnsureCodec(w, h)) {
      FailDecode("codec initialization/reconfiguration failed", m_seq + 1);
      return E_FAIL;
    }

    uint64_t pts = ++m_seq;
    if (const char* dump = getenv("NPT_HWDEC_DUMP")) {
      /* debug: append the access units sent to MediaCodec to a file */
      if (FILE* f = fopen(dump, pts == 1 ? "wb" : "ab")) {
        fwrite(m_au.data(), 1, m_au.size(), f);
        fclose(f);
      }
    }
    double t0 = nowMs();
    if (!Queue(m_au, pts))
      return E_FAIL;

    /* Wait for the decoded picture on the CS thread, in command order: the
     * ring (and the app's decode thread) never wait for MediaCodec, and every
     * later command reading the output texture still sees the picture. */
    if (expectOutput) {
      pContext->EmitCs([
        cThis  = Com<D3D11VideoDecoder, false>(this),
        cPts   = pts,
        cDst   = m_target->GetImage(),
        cLayer = m_target->GetLayer()
      ] (DxvkContext* ctx) {
        cThis->CompleteFrame(ctx, cPts, cDst, cLayer);
      });
      /* hand the chunk to the CS thread now: it drains MediaCodec's output,
       * without which the codec runs out of input buffers; and submit the
       * copy promptly so its AImage can go back to the codec */
      pContext->FlushCsChunk();
      pContext->ConsiderFlush(GpuFlushType::ImplicitStrongHint);
    }

    double el = nowMs() - t0;
    m_statFrames++;
    m_statMs += el;
    if (el > m_statMax) m_statMax = el;
    if (m_statFrames == 1 || m_statFrames % 600 == 0)
      MC_INFO(("D3D11VideoDecoder: ", m_statFrames, " frames, EndFrame avg ", m_statMs / m_statFrames, " ms, max ", m_statMax,
        " ms, ", m_codecWidth, "x", m_codecHeight, ", cached buffers ", m_cache.size()));
    MC_TRACE(("Queued ", this, " seq ", pts, " ", el, " ms out ", expectOutput));

    m_target = nullptr;
    return S_OK;
  }


  /* Debug: with /data/adb/fold-vm/npt/hwdec-capture present, every frame's
   * DXVA buffers are appended to /data/adb/fold-vm/npt/dxva-<pid>.bin:
   * 'DXVF' codec count {type size bytes}... (replayed by videotest). */
  void D3D11VideoDecoder::Capture() {
    static double s_next = 0.0;
    static bool s_on = false;
    double now = nowMs();
    if (now >= s_next) {
      s_on = access("/data/adb/fold-vm/npt/hwdec-capture", F_OK) == 0;
      s_next = now + 2000.0;
    }
    if (!s_on)
      return;
    char path[96];
    snprintf(path, sizeof(path), "/data/adb/fold-vm/npt/dxva-%d.bin", int(getpid()));
    FILE* f = fopen(path, "ab");
    if (!f)
      return;
    uint32_t hdr[3] = { 0x46565844u, uint32_t(m_codec), 0 };
    for (auto& b : m_bufs) if (!b.empty()) hdr[2]++;
    fwrite(hdr, sizeof(hdr), 1, f);
    for (uint32_t t = 0; t < m_bufs.size(); t++) {
      if (m_bufs[t].empty()) continue;
      uint32_t th[2] = { t, uint32_t(m_bufs[t].size()) };
      fwrite(th, sizeof(th), 1, f);
      fwrite(m_bufs[t].data(), 1, m_bufs[t].size(), f);
    }
    fclose(f);
  }


  HRESULT D3D11VideoDecoder::EndFrameAV1(D3D11ImmediateContext* pContext) {
    auto& ppb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS];
    auto& scb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL];
    auto& bsb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_BITSTREAM];
    if (ppb.size() < sizeof(DxvaPicParamsAV1) || scb.size() < sizeof(DxvaTileAV1) || bsb.empty()) {
      MC_WARN(("D3D11VideoDecoder: AV1 frame without picture parameters/tiles/bitstream (",
        ppb.size(), "/", scb.size(), "/", bsb.size(), ")"));
      return S_OK;
    }
    auto f = std::make_unique<Av1Frame>();
    memcpy(&f->pp, ppb.data(), sizeof(f->pp));
    f->tiles.resize(scb.size() / sizeof(DxvaTileAV1));
    memcpy(f->tiles.data(), scb.data(), f->tiles.size() * sizeof(DxvaTileAV1));
    f->data = bsb;

    if (f->pp.bitdepth != 8) {
      MC_WARN(("D3D11VideoDecoder: AV1 bit depth ", uint32_t(f->pp.bitdepth), " not supported"));
      return E_FAIL;
    }
    if (!EnsureCodec(std::max(f->pp.width, 64u), std::max(f->pp.height, 64u))) {
      FailDecode("AV1 codec initialization/reconfiguration failed", m_seq + 1);
      return E_FAIL;
    }

    /* A hidden frame the stream may show later (show_existing_frame) must
     * end up in its texture: Chromium then presents that texture without a
     * decode call. MediaCodec only outputs shown frames, so code it as
     * shown; for an inter or intra-only frame that changes nothing but the
     * output (film grain comes from the same parameters either way). */
    uint8_t& fl = f->pp.FormatAndPictureInfoFlags;
    if (!(fl & 4) && (fl & 8) && (fl & 3) != 0)
      fl = uint8_t((fl | 4) & ~8);

    uint64_t pts = ++m_seq;
    f->pts = pts;
    bool show = av1ShowFrame(f->pp);
    {
      std::lock_guard<std::mutex> lock(m_av1Mutex);
      FinalizeAv1Locked(std::move(f));
    }

    if (show) {
      pContext->EmitCs([
        cThis  = Com<D3D11VideoDecoder, false>(this),
        cPts   = pts,
        cDst   = m_target->GetImage(),
        cLayer = m_target->GetLayer()
      ] (DxvkContext* ctx) {
        cThis->CompleteFrame(ctx, cPts, cDst, cLayer);
      });
      pContext->FlushCsChunk();
      pContext->ConsiderFlush(GpuFlushType::ImplicitStrongHint);
    }
    m_statFrames++;
    if (m_statFrames == 1 || m_statFrames % 600 == 0)
      MC_INFO(("D3D11VideoDecoder: AV1 ", m_statFrames, " frames, ", m_codecWidth, "x", m_codecHeight,
        ", cached buffers ", m_cache.size()));
    m_target = nullptr;
    return S_OK;
  }


  /* A slot of ours whose picture the app no longer references, or a second
   * copy of a picture (a key frame fills all slots); -1 when all eight hold
   * different live pictures. */
  int D3D11VideoDecoder::FreeAv1Slot(const bool* live) const {
    const auto& st = m_av1;
    int copies[256] = { };
    for (int s = 0; s < 8; s++)
      if (st.slotValid[s])
        copies[st.slotTex[s]]++;
    for (int s = 0; s < 8; s++)
      if (!st.slotValid[s] || !live[st.slotTex[s]])
        return s;
    for (int s = 0; s < 8; s++)
      if (copies[st.slotTex[s]] > 1)
        return s;
    return -1;
  }


  bool D3D11VideoDecoder::WriteAv1Locked(const Av1Frame& f, const DxvaPicParamsAV1& pp,
                                         uint8_t refresh, std::vector<uint8_t>& obus) {
    auto& st = m_av1;
    const char* err = nullptr;
    if (!av1WriteFrame(st, pp, f.tiles.data(), uint32_t(f.tiles.size()),
                       f.data.data(), f.data.size(), refresh, f.refSlots, obus, &err)) {
      static uint32_t s_n;
      if (s_n++ < 20)
        MC_WARN(("D3D11VideoDecoder: AV1 frame ", f.pts, " not expressible: ", err ? err : "?"));
      return false;
    }
    /* the texture now holds this picture: slots still naming it hold the
     * old picture, which the app can no longer reference */
    for (int s = 0; s < 8; s++)
      if (!(refresh & (1u << s)) && st.slotTex[s] == int(pp.CurrPicTextureIndex))
        st.slotValid[s] = false;
    MC_TRACE(("AV1 frame ", f.pts, " type ", uint32_t(pp.FormatAndPictureInfoFlags & 3), " show ",
      av1ShowFrame(pp), " tex ", uint32_t(pp.CurrPicTextureIndex), " refresh 0x", std::hex, uint32_t(refresh)));
    return true;
  }


  /* DXVA does not carry refresh_frame_flags: the app's slot map of the NEXT
   * frame says where a picture went.  Our own slot layout keeps one copy of
   * every picture the app still references, which decides most frames right
   * away.  When all eight slots hold different live pictures, the app drops
   * one we cannot name yet:
   *  - a hidden frame waits for the next frame (no output is waited for);
   *  - a shown frame is decoded with refresh 0 and, if the next map shows the
   *    app kept it, decoded again as a hidden frame into the slot the app
   *    freed (the reference state is unchanged, so the picture is identical). */
  void D3D11VideoDecoder::FinalizeAv1Locked(std::unique_ptr<Av1Frame> f) {
    const auto& pp = f->pp;
    auto& st = m_av1;

    bool live[256] = { };
    for (int s = 0; s < 8; s++)
      if (pp.RefFrameMapTextureIndex[s] != 0xff)
        live[pp.RefFrameMapTextureIndex[s]] = true;

    if (m_av1Pending) {
      auto p = std::move(m_av1Pending);
      if (live[p->pp.CurrPicTextureIndex]) {
        int slot = FreeAv1Slot(live);
        if (slot < 0) {
          static uint32_t s_n;
          if (s_n++ < 10)
            MC_WARN(("D3D11VideoDecoder: AV1 no free slot for frame ", p->pts));
          slot = 7;
        }
        DxvaPicParamsAV1 ppw = p->pp;
        if (p->restore)
          ppw.FormatAndPictureInfoFlags &= ~uint8_t(4 | 8);   /* hidden, not showable */
        std::vector<uint8_t> obus;
        if (WriteAv1Locked(*p, ppw, uint8_t(1u << slot), obus))
          m_av1Acc.insert(m_av1Acc.end(), obus.begin(), obus.end());
      }
    }

    /* our slot for each reference: the slot holding that picture */
    bool intra = (pp.FormatAndPictureInfoFlags & 3) == 0 || (pp.FormatAndPictureInfoFlags & 3) == 2;
    if (!intra) {
      for (int i = 0; i < 7; i++) {
        int appSlot = pp.frame_refs[i].Index;
        int tex = appSlot < 8 ? pp.RefFrameMapTextureIndex[appSlot] : 0xff;
        for (int s = 0; s < 8; s++)
          if (st.slotValid[s] && st.slotTex[s] == tex) { f->refSlots[i] = s; break; }
      }
    }

    bool show = av1ShowFrame(pp);
    uint8_t refresh = 0;
    if (av1IsKeyShow(pp)) {
      refresh = 0xff;
    } else {
      int slot = FreeAv1Slot(live);
      if (slot >= 0) {
        refresh = uint8_t(1u << slot);
      } else if (!show) {
        m_av1Pending = std::move(f);
        return;
      } else {
        f->restore = true;
      }
    }

    std::vector<uint8_t> obus;
    if (!WriteAv1Locked(*f, pp, refresh, obus)) {
      if (show)
        m_av1Dropped.insert(f->pts);
      return;
    }
    if (!show) {
      m_av1Acc.insert(m_av1Acc.end(), obus.begin(), obus.end());
      return;
    }
    std::vector<uint8_t> au = { uint8_t((2 << 3) | 0x2), 0 };   /* temporal delimiter */
    au.insert(au.end(), m_av1Acc.begin(), m_av1Acc.end());
    au.insert(au.end(), obus.begin(), obus.end());
    m_av1Acc.clear();
    if (const char* dump = getenv("NPT_HWDEC_DUMP")) {
      static bool s_first = true;
      if (FILE* df = fopen(dump, std::exchange(s_first, false) ? "wb" : "ab")) {
        fwrite(au.data(), 1, au.size(), df);
        fclose(df);
      }
    }
    if (!Queue(au, f->pts))
      m_av1Dropped.insert(f->pts);
    if (f->restore)
      m_av1Pending = std::move(f);
  }


  /* CS thread: the frame being waited for is still held back for its
   * refresh flags (the app has not sent the next frame): send it with a
   * guess, or report that the frame was dropped. Returns false when no
   * output will ever come for pts. */
  bool D3D11VideoDecoder::FlushAv1Pending(uint64_t pts) {
    std::lock_guard<std::mutex> lock(m_av1Mutex);
    return !m_av1Dropped.count(pts);
  }


  bool D3D11VideoDecoder::BuildAccessUnit(bool* pExpectOutput, uint32_t* pWidth, uint32_t* pHeight) {
    m_au.clear();
    switch (m_codec) {
      case D3D11VideoCodec::H264: return BuildH264(pExpectOutput, pWidth, pHeight);
      case D3D11VideoCodec::VP9:  return BuildVP9(pExpectOutput, pWidth, pHeight);
      default: return false;
    }
  }


  bool D3D11VideoDecoder::BuildH264(bool* pExpectOutput, uint32_t* pWidth, uint32_t* pHeight) {
    auto& ppb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS];
    auto& qmb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX];
    auto& scb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL];
    auto& bsb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_BITSTREAM];
    if (ppb.size() < sizeof(DxvaPicParamsH264) || bsb.empty()) {
      MC_WARN(("D3D11VideoDecoder: H.264 frame without picture parameters/bitstream (",
        ppb.size(), "/", bsb.size(), ")"));
      return false;
    }
    DxvaPicParamsH264 pp;
    memcpy(&pp, ppb.data(), sizeof(pp));
    const DxvaQmatrixH264* qm = qmb.size() >= sizeof(DxvaQmatrixH264)
      ? reinterpret_cast<const DxvaQmatrixH264*>(qmb.data()) : nullptr;

    if (pp.wBitFields & H264_FIELD_PIC) {
      static bool s_warned;
      if (!std::exchange(s_warned, true))
        MC_WARN(("D3D11VideoDecoder: H.264 field pictures are not supported"));
    }

    /* Collect slice NAL units; pps id from the first slice header. */
    std::vector<std::pair<uint32_t, uint32_t>> slices;
    size_t nsl = scb.size() / sizeof(DxvaSliceShort);
    for (size_t i = 0; i < nsl; i++) {
      DxvaSliceShort s;
      memcpy(&s, scb.data() + i * sizeof(s), sizeof(s));
      if (size_t(s.BSNALunitDataLocation) + s.SliceBytesInBuffer > bsb.size())
        continue;
      slices.emplace_back(s.BSNALunitDataLocation, s.SliceBytesInBuffer);
    }
    if (slices.empty())
      slices.emplace_back(0u, uint32_t(bsb.size()));

    uint32_t ppsId = 0;
    bool idr = false;
    {
      const uint8_t* p = bsb.data() + slices[0].first;
      size_t n = slices[0].second, i = 0;
      while (i + 3 < n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
        i++;
      if (i + 4 < n) {
        idr = (p[i + 3] & 0x1f) == 5;
        BitReader r(p + i + 4, n - i - 4);
        r.ue();             /* first_mb_in_slice */
        r.ue();             /* slice_type */
        ppsId = r.ue();
        if (ppsId > 255)
          ppsId = 0;
      }
    }

    std::vector<uint8_t> sps, pps;
    h264Sps(pp, sps);
    h264Pps(pp, qm, ppsId, pps);
    if (idr || sps != m_lastSps || pps != m_lastPps || m_seq == 0) {
      m_au.insert(m_au.end(), sps.begin(), sps.end());
      m_au.insert(m_au.end(), pps.begin(), pps.end());
      m_lastSps = std::move(sps);
      m_lastPps = std::move(pps);
    }

    for (auto& s : slices) {
      const uint8_t* p = bsb.data() + s.first;
      size_t n = s.second;
      /* Short-format slices begin with a 00 00 01 start code already. */
      if (!(n >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (n >= 4 && p[2] == 0 && p[3] == 1)))) {
        static const uint8_t sc[] = { 0, 0, 1 };
        m_au.insert(m_au.end(), sc, sc + 3);
      }
      m_au.insert(m_au.end(), p, p + n);
    }

    *pExpectOutput = true;
    *pWidth  = (pp.wFrameWidthInMbsMinus1 + 1u) * 16u;
    *pHeight = (pp.wFrameHeightInMbsMinus1 + 1u) * 16u;
    return true;
  }


  bool D3D11VideoDecoder::BuildVP9(bool* pExpectOutput, uint32_t* pWidth, uint32_t* pHeight) {
    auto& scb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL];
    auto& bsb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_BITSTREAM];
    if (bsb.empty())
      return false;

    size_t off = 0, n = bsb.size();
    if (scb.size() >= sizeof(DxvaSliceShort)) {
      DxvaSliceShort s;
      memcpy(&s, scb.data(), sizeof(s));
      if (size_t(s.BSNALunitDataLocation) + s.SliceBytesInBuffer <= bsb.size() && s.SliceBytesInBuffer) {
        off = s.BSNALunitDataLocation;
        n = s.SliceBytesInBuffer;
      }
    }
    const uint8_t* p = bsb.data() + off;

    /* uncompressed header: frame_marker(2) profile(2) [reserved] show_existing(1) ... */
    BitReader r(p, std::min<size_t>(n, 16));
    if (r.bits(2) != 2) {
      MC_WARN(("D3D11VideoDecoder: VP9 frame without frame marker"));
      return false;
    }
    uint32_t profile = r.bit();
    profile |= r.bit() << 1;
    if (profile == 3)
      r.bit();
    if (r.bit()) /* show_existing_frame: the app shows an earlier surface */
      return false;
    uint32_t frameType = r.bit();
    uint32_t showFrame = r.bit();
    (void)frameType;

    m_au.assign(p, p + n);
    *pExpectOutput = showFrame != 0;

    auto& ppb = m_bufs[D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS];
    if (ppb.size() >= 3 + 2 + 8) {
      /* DXVA_PicParams_VP9: CurrPic(1) profile(1) flags(2) width(4) height(4) */
      uint32_t w, h;
      memcpy(&w, ppb.data() + 4, 4);
      memcpy(&h, ppb.data() + 8, 4);
      if (w && h && w <= 16384 && h <= 16384) {
        *pWidth = w;
        *pHeight = h;
      }
    }
    return true;
  }

  /* ------------------------------------------------------------------ */
  /* MediaCodec                                                         */
  /* ------------------------------------------------------------------ */

#if defined(__ANDROID__)

  static void startBinderPool() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
      void* bn = dlopen("libbinder_ndk.so", RTLD_NOW);
      auto setMax = bn ? reinterpret_cast<bool (*)(uint32_t)>(dlsym(bn, "ABinderProcess_setThreadPoolMaxThreadCount")) : nullptr;
      auto start  = bn ? reinterpret_cast<void (*)()>(dlsym(bn, "ABinderProcess_startThreadPool")) : nullptr;
      if (setMax) setMax(4);
      if (start) start();
      MC_INFO(("D3D11VideoDecoder: binder thread pool ", start ? "started" : "unavailable"));
    });
  }

  bool D3D11VideoDecoder::EnsureCodec(uint32_t width, uint32_t height) {
    if (m_codecCleanupUnknown)
      return false; /* an uncertain codec handle must never be used again */
    if (m_started && width == m_codecWidth && height == m_codecHeight)
      return true;
    if (m_started) {
      /* VP9 can change resolution in-stream; MediaCodec follows on its own,
       * keep the session unless the buffers would be too small. */
      if (width <= m_codecWidth && height <= m_codecHeight)
        return true;
      if (!DestroyCodec())
        return false;
    }

    startBinderPool();

    const char* mime = m_codec == D3D11VideoCodec::H264 ? "video/avc"
                     : m_codec == D3D11VideoCodec::AV1 ? "video/av01" : "video/x-vnd.on2.vp9";
    m_codecHandle = AMediaCodec_createDecoderByType(mime);
    if (!m_codecHandle) {
      MC_ERR(("D3D11VideoDecoder: no MediaCodec for ", mime));
      return false;
    }

    uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    if (AImageReader_newWithUsage(int32_t(width), int32_t(height), AIMAGE_FORMAT_YUV_420_888,
                                  usage, int32_t(kReaderImages), &m_reader) != AMEDIA_OK) {
      MC_ERR(("D3D11VideoDecoder: AImageReader failed"));
      DestroyCodec();
      return false;
    }
    ANativeWindow* window = nullptr;
    AImageReader_getWindow(m_reader, &window);

    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, int32_t(width));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, int32_t(height));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, int32_t(std::max<uint32_t>(width * height, 2u << 20)));
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-forceNonUBWC.value", 1);
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PRIORITY, 0);
    media_status_t st = AMediaCodec_configure(m_codecHandle, f, window, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(m_codecHandle) != AMEDIA_OK) {
      MC_ERR(("D3D11VideoDecoder: MediaCodec configure/start failed: ", int(st)));
      DestroyCodec();
      return false;
    }
    m_started = true;
    m_codecWidth = width;
    m_codecHeight = height;
    m_lastSps.clear();
    m_lastPps.clear();
    MC_INFO(("D3D11VideoDecoder: MediaCodec ", mime, " ", width, "x", height, " started"));
    return true;
  }


  bool D3D11VideoDecoder::DestroyCodec() {
    if (m_codecCleanupUnknown)
      return false;
    bool clean = true;
    if (!m_destroying)
      SyncCs();
    if (!m_pending.empty() || !m_cache.empty()) {
      const VkResult idle = m_parent->GetDXVKDevice()->waitForIdleStatus();
      if (idle != VK_SUCCESS) {
        FailDecode("graphics copy teardown quiescence unknown", m_seq);
        m_codecCleanupUnknown = true;
        return false; /* keep images/import handles; no proof of GPU read release */
      }
    }
    for (auto& p : m_pending)
      AImage_delete(p.image);
    m_pending.clear();
    for (auto& r : m_ready)
      AImage_delete(r.second);
    m_ready.clear();
    if (m_codecHandle) {
      if (m_started && AMediaCodec_stop(m_codecHandle) != AMEDIA_OK) {
        FailDecode("codec stop did not complete successfully", m_seq);
        clean = false;
      }
      if (AMediaCodec_delete(m_codecHandle) != AMEDIA_OK) {
        FailDecode("codec delete ownership is unknown", m_seq);
        // No retry or new codec can reuse an uncertain handle. Keep the
        // reader and raw import handles instead of abandoning its producer
        // surface here. On final object destruction these host-only handles
        // deliberately remain until native-worker process exit. Prior copies
        // were CS-drained and had a successful idle result above;
        // no VPU API was given a guest target or guest-memory FD.
        m_codecCleanupUnknown = true;
        return false;
      }
      m_codecHandle = nullptr;
    }
    m_started = false;
    {
      std::lock_guard<std::mutex> lock(m_av1Mutex);
      m_av1 = Av1State();
      m_av1Pending.reset();
      m_av1Acc.clear();
    }
    if (m_reader) {
      AImageReader_delete(m_reader);
      m_reader = nullptr;
    }
    if (!m_cache.empty()) {
      auto dev = m_parent->GetDXVKDevice();
      auto vkd = dev->vkd();
      for (auto& c : m_cache) {
        c.buffer = nullptr;
        vkd->vkDestroyBuffer(vkd->device(), c.vkBuffer, nullptr);
        vkd->vkFreeMemory(vkd->device(), c.vkMemory, nullptr);
      }
      m_cache.clear();
    }
    return clean;
  }


  bool D3D11VideoDecoder::Queue(const std::vector<uint8_t>& au, uint64_t pts) {
    double t0 = nowMs();
    while (nowMs() - t0 < 500.0) {
      ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codecHandle, 10000);
      if (idx < 0)
        continue;
      size_t cap = 0;
      uint8_t* buf = AMediaCodec_getInputBuffer(m_codecHandle, idx, &cap);
      if (!buf || au.size() > cap) {
        MC_ERR(("D3D11VideoDecoder: access unit too large (", au.size(), " > ", cap, ")"));
        AMediaCodec_queueInputBuffer(m_codecHandle, idx, 0, 0, pts * 1000, 0);
        FailDecode("input access unit exceeds codec buffer", pts);
        return false;
      }
      memcpy(buf, au.data(), au.size());
      const media_status_t status = AMediaCodec_queueInputBuffer(
        m_codecHandle, idx, 0, au.size(), pts * 1000, 0);
      if (status != AMEDIA_OK) {
        FailDecode("queueInputBuffer rejected access unit", pts);
        return false;
      }
      return true;
    }
    MC_ERR(("D3D11VideoDecoder: no MediaCodec input buffer"));
    FailDecode("input dequeue deadline", pts);
    return false;
  }


  void D3D11VideoDecoder::CompleteFrame(DxvkContext* ctx, uint64_t pts, const Rc<DxvkImage>& dst, uint32_t layer) {
    double t0 = nowMs();
    RetirePending(false);
    AImage* image = AcquireOutput(pts);
    if (image == nullptr && m_pending.size()) {
      /* the reader may be out of images: hand the copies so far to the GPU
       * and give their images back, then try once more */
      ctx->flushCommandList(nullptr, nullptr);
      RetirePending(true);
      image = AcquireOutput(pts);
    }
    if (!image) {
      MC_WARN(("D3D11VideoDecoder: no output for frame ", pts, " (pending ", m_pending.size(), ")"));
      // Only expected-output frames enqueue CompleteFrame. Hidden/no-output
      // frames and VP9 show_existing keep their existing paths. Exhausting
      // this existing deadline is unknown/failure, never an empty success.
      FailDecode("expected output unavailable", pts);
      return;
    }
    if (!CopyImage(image, ctx, dst, layer)) {
      // CopyImage consumes/deletes the image on every failure path; preserve
      // that ownership and do not manufacture a successful copied frame.
      FailDecode("decoded output import/copy rejected", pts);
      return;
    }
    ctx->flushCommandList(nullptr, nullptr);
    double el = nowMs() - t0;
    if (el > 100.0)
      MC_WARN(("D3D11VideoDecoder: frame ", pts, " took ", el, " ms on the CS thread"));
    MC_TRACE(("Done ", this, " seq ", pts, " ", el, " ms"));
  }


  /* CS thread.  Each rendered output buffer is paired with the next image
   * the reader delivers, strictly in order, keyed by the codec's pts; before
   * releasing an output, room is made in the reader so that pairing can
   * never slip.  Outputs for other frames are parked in m_ready. */
  AImage* D3D11VideoDecoder::AcquireOutput(uint64_t pts) {
    for (auto it = m_ready.begin(); it != m_ready.end(); ++it) {
      if (it->first == pts) {
        AImage* img = it->second;
        m_ready.erase(it);
        return img;
      }
    }

    double t0 = nowMs();
    bool av1Flushed = false;
    while (nowMs() - t0 < 1000.0) {
      if (m_codec == D3D11VideoCodec::AV1 && !av1Flushed && nowMs() - t0 > 40.0) {
        av1Flushed = true;
        if (!FlushAv1Pending(pts))
          return nullptr;
      }
      AMediaCodecBufferInfo info;
      ssize_t idx = AMediaCodec_dequeueOutputBuffer(m_codecHandle, &info, 5000);
      if (idx < 0)
        continue;
      uint64_t outPts = uint64_t(info.presentationTimeUs) / 1000;
      bool render = info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      if (!render) {
        if (AMediaCodec_releaseOutputBuffer(m_codecHandle, idx, false) != AMEDIA_OK) {
          FailDecode("releaseOutputBuffer rejected non-rendered output", pts);
          return nullptr;
        }
        continue;
      }
      /* keep at least two reader slots free */
      while (m_pending.size() + m_ready.size() + 2 >= kReaderImages) {
        size_t before = m_pending.size();
        RetirePending(true);
        if (m_pending.size() == before)
          break;
      }
      if (AMediaCodec_releaseOutputBuffer(m_codecHandle, idx, true) != AMEDIA_OK) {
        // Do not pair an unrelated already-queued AImage with this PTS after
        // a failed render submission.
        FailDecode("releaseOutputBuffer rejected rendered output", pts);
        return nullptr;
      }

      AImage* image = nullptr;
      double ti = nowMs();
      while (nowMs() - ti < 500.0) {
        if (AImageReader_acquireNextImage(m_reader, &image) == AMEDIA_OK && image)
          break;
        image = nullptr;
        usleep(200);
      }
      if (!image) {
        MC_WARN(("D3D11VideoDecoder: no image for output ", outPts));
        continue;
      }
      if (outPts == pts)
        return image;
      if (outPts < pts) {
        AImage_delete(image);           /* a frame nobody waits for any more */
        continue;
      }
      m_ready.emplace_back(outPts, image);
    }
    return nullptr;
  }


  Rc<DxvkBuffer> D3D11VideoDecoder::ImportBuffer(AHardwareBuffer* ahb, size_t* pSize) {
    using GetId = int (*)(const AHardwareBuffer*, uint64_t*);
    using GetHandle = const native_handle_t* (*)(const AHardwareBuffer*);
    /* Resolve both functions from the same held library handle. A wrapper
     * address is not an allocation identity: AImageReader may recycle it. */
    static void* s_nativeWindow = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
    static GetId s_getId = s_nativeWindow
      ? reinterpret_cast<GetId>(dlsym(s_nativeWindow, "AHardwareBuffer_getId")) : nullptr;
    static GetHandle s_getHandle = s_nativeWindow
      ? reinterpret_cast<GetHandle>(dlsym(s_nativeWindow, "AHardwareBuffer_getNativeHandle")) : nullptr;

    uint64_t id = 0;
    int idResult = s_getId ? s_getId(ahb, &id) : -1;
    bool cacheable = idResult == 0 && id != 0;
    const native_handle_t* nh = s_getHandle ? s_getHandle(ahb) : nullptr;
    int fd = nh && nh->numFds >= 1 ? nh->data[0] : -1;

    /* Opt-in, bounded per decoder. A failed/absent ID query must never
     * silently reuse an import keyed by an ephemeral AHB pointer. */
    static const bool s_identityTrace = envFlag("NPT_HWDEC_IDENTITY_TRACE", false);
    bool traceIdentity = (s_identityTrace || mcTrace()) && m_identityTraceCount++ < 128;
    struct stat fdStat = { };
    int statResult = traceIdentity && fd >= 0 ? fstat(fd, &fdStat) : -1;
    auto trace = [&] (bool hit) {
      if (traceIdentity)
        MC_INFO(("D3D11VideoDecoder: AHB identity ptr ", static_cast<const void*>(ahb),
          " getId ", s_getId != nullptr, " rc ", idResult, " id ", id,
          " cacheable ", cacheable, " cacheHit ", hit, " fd ", fd,
          " statRc ", statResult, " dev ", uint64_t(fdStat.st_dev),
          " ino ", uint64_t(fdStat.st_ino), " size ", int64_t(fdStat.st_size)));
    };
    for (auto& c : m_cache) {
      if (cacheable && c.cacheable && c.id == id) {
        trace(true);
        *pSize = c.size;
        return c.buffer;
      }
    }
    trace(false);
    if (fd < 0)
      return nullptr;
    off_t end = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (end <= 0)
      return nullptr;

    auto dev = m_parent->GetDXVKDevice();
    auto vkd = dev->vkd();
    CachedBuffer c;
    c.cacheable = cacheable;
    c.id = id;
    c.size = size_t(end);

    int dfd = dup(fd);
    VkMemoryFdPropertiesKHR fdProps = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (vkd->vkGetMemoryFdPropertiesKHR(vkd->device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dfd, &fdProps)) {
      close(dfd);
      return nullptr;
    }
    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext };
    bi.size = c.size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkd->vkCreateBuffer(vkd->device(), &bi, nullptr, &c.vkBuffer)) {
      close(dfd);
      return nullptr;
    }
    VkMemoryRequirements req;
    vkd->vkGetBufferMemoryRequirements(vkd->device(), c.vkBuffer, &req);
    const auto& mp = dev->adapter()->memoryProperties();
    uint32_t bits = req.memoryTypeBits & fdProps.memoryTypeBits, type = ~0u;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if (bits & (1u << i)) { type = i; break; }
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = dfd;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp };
    ai.allocationSize = std::max<VkDeviceSize>(req.size, c.size);
    ai.memoryTypeIndex = type;
    if (type == ~0u || vkd->vkAllocateMemory(vkd->device(), &ai, nullptr, &c.vkMemory)) {
      close(dfd);
      vkd->vkDestroyBuffer(vkd->device(), c.vkBuffer, nullptr);
      MC_ERR(("D3D11VideoDecoder: dmabuf import failed"));
      return nullptr;
    }
    const VkResult bindStatus = vkd->vkBindBufferMemory(vkd->device(), c.vkBuffer, c.vkMemory, 0);
    if (bindStatus != VK_SUCCESS) {
      /* Successful vkAllocateMemory already consumed dfd. The buffer has
       * never been submitted or wrapped; do not close the imported fd twice. */
      vkd->vkDestroyBuffer(vkd->device(), c.vkBuffer, nullptr);
      vkd->vkFreeMemory(vkd->device(), c.vkMemory, nullptr);
      MC_ERR(("D3D11VideoDecoder: dmabuf bind failed: ", bindStatus));
      return nullptr;
    }

    DxvkBufferCreateInfo info = { };
    info.size = c.size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT;
    info.debugName = "MediaCodec frame";
    DxvkBufferImportInfo importInfo = { };
    importInfo.buffer = c.vkBuffer;
    c.buffer = dev->importBuffer(info, importInfo, 0);

    m_cache.push_back(c);
    *pSize = c.size;
    MC_TRACE(("D3D11VideoDecoder: imported frame buffer ", m_cache.size(), " (", c.size, " bytes)"));
    return c.buffer;
  }


  bool D3D11VideoDecoder::CopyImage(AImage* image, DxvkContext* ctx, const Rc<DxvkImage>& dst, uint32_t layer) {
    AHardwareBuffer* ahb = nullptr;
    if (AImage_getHardwareBuffer(image, &ahb) != AMEDIA_OK || !ahb) {
      AImage_delete(image);
      return false;
    }
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(ahb, &d);
    if (d.format != 0x7fa30c04) {
      static bool s_warned;
      if (!std::exchange(s_warned, true))
        MC_ERR(("D3D11VideoDecoder: unexpected decoder buffer format 0x", std::hex, d.format));
      AImage_delete(image);
      return false;
    }

    /* Do not create a transient import unless it will have a pending
     * image owner and a GPU retirement point. */
    if (dst == nullptr) {
      AImage_delete(image);
      return false;
    }
    size_t size = 0;
    Rc<DxvkBuffer> buffer = ImportBuffer(ahb, &size);
    if (buffer == nullptr) {
      AImage_delete(image);
      return false;
    }

    /* Venus NV12: stride ALIGN(w,128), Y scanlines ALIGN(h,32), UV after */
    uint32_t stride = std::max<uint32_t>(d.stride, (d.width + 127u) & ~127u);
    VkDeviceSize uvOffset = VkDeviceSize(stride) * ((d.height + 31u) & ~31u);

    VkExtent3D ext = dst->info().extent;
    uint32_t w = std::min(d.width, ext.width) & ~1u;
    uint32_t h = std::min(d.height, ext.height) & ~1u;

    VkImageSubresourceLayers y = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0u, layer, 1u };
    VkImageSubresourceLayers uv = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0u, layer, 1u };
    ctx->copyBufferToImage(dst, y, VkOffset3D(), VkExtent3D { w, h, 1u },
      buffer, 0, stride, 0, dst->info().format);
    ctx->copyBufferToImage(dst, uv, VkOffset3D(), VkExtent3D { w, h, 1u },
      buffer, uvOffset, stride, 0, dst->info().format);

    PendingImage p;
    p.image = image;
    p.buffer = buffer;
    m_pending.push_back(std::move(p));
    return true;
  }


  /* CS thread (or after a CS sync): gives an AImage back to the codec once
   * the GPU copy that read it has completed. */
  void D3D11VideoDecoder::RetirePending(bool force) {
    double t0 = nowMs();
    while (!m_pending.empty()) {
      auto& p = m_pending.front();
      if (p.buffer->isInUse(DxvkAccess::Read)) {
        if (!force || nowMs() - t0 > 200.0)
          return;
        usleep(250);
        continue;
      }
      /* Invalid/missing allocation IDs use a fresh import for this image.
       * m_cache owns its raw Vulkan handles only until this GPU read retires;
       * those entries are never eligible for lookup/reuse. */
      auto transient = std::find_if(m_cache.begin(), m_cache.end(), [&] (const CachedBuffer& c) {
        return !c.cacheable && c.buffer.ptr() == p.buffer.ptr();
      });
      if (transient != m_cache.end()) {
        p.buffer = nullptr;
        transient->buffer = nullptr;
        auto vkd = m_parent->GetDXVKDevice()->vkd();
        vkd->vkDestroyBuffer(vkd->device(), transient->vkBuffer, nullptr);
        vkd->vkFreeMemory(vkd->device(), transient->vkMemory, nullptr);
        m_cache.erase(transient);
      }
      AImage_delete(p.image);
      m_pending.pop_front();
    }
  }

#else

  bool D3D11VideoDecoder::EnsureCodec(uint32_t, uint32_t) { return false; }
  bool D3D11VideoDecoder::DestroyCodec() { return true; }
  bool D3D11VideoDecoder::Queue(const std::vector<uint8_t>&, uint64_t) { return false; }
  void D3D11VideoDecoder::CompleteFrame(DxvkContext*, uint64_t, const Rc<DxvkImage>&, uint32_t) { }
  AImage* D3D11VideoDecoder::AcquireOutput(uint64_t) { return nullptr; }
  bool D3D11VideoDecoder::CopyImage(AImage*, DxvkContext*, const Rc<DxvkImage>&, uint32_t) { return false; }
  Rc<DxvkBuffer> D3D11VideoDecoder::ImportBuffer(AHardwareBuffer*, size_t*) { return nullptr; }
  void D3D11VideoDecoder::RetirePending(bool) { }

#endif

}
