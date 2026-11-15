#pragma once

/*
 * Hardware video decode for D3D11 video (DXVA) on Android: ID3D11VideoDecoder
 * backed by MediaCodec (Fold / DroidVM Neptune host).
 *
 * The DXVA guest (Windows apps through Triton/Neptune) hands over decode-order
 * pictures. Per codec the DXVA buffers are turned into a bitstream MediaCodec
 * accepts (VP9: the frame as is; H.264: SPS/PPS synthesised from the picture
 * parameters + the slice NAL units). MediaCodec runs with Qualcomm's
 * decode-order output and linear NV12, renders into an AImageReader; each
 * decoded AHardwareBuffer is imported as a dmabuf VkBuffer (cached per
 * buffer) and copied into the decoder output view's NV12 array slice.
 *
 * Data path from the guest: Triton keeps decoder buffers in guest memory and
 * submits them inline through DecoderExtension(NPT_DXVA_SUBMIT_FUNCTION),
 * since GetDecoderBuffer's host pointer means nothing to the guest. The
 * regular GetDecoderBuffer/SubmitDecoderBuffers path works too (host tests).
 */

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <memory>
#include <set>

#include "../dxvk/dxvk_device.h"
#include "d3d11_video_av1.h"
#include "d3d11_device_child.h"
#include "../util/com/com_private_data.h"

struct AMediaCodec;
struct AImageReader;
struct AImage;
struct AHardwareBuffer;

namespace dxvk {

  class D3D11Device;
  class D3D11ImmediateContext;
  class DxvkBuffer;
  class DxvkImage;

  /* DecoderExtension function id + blob layout shared with Triton. */
  constexpr UINT NPT_DXVA_SUBMIT_FUNCTION = 0x4e505644u; /* 'NPVD' */
  constexpr uint32_t NPT_DXVA_SUBMIT_MAGIC = 0x31565044u; /* 'DPV1' */

  struct NptDxvaSubmitHeader {
    uint32_t magic;
    uint32_t count;     /* number of entries that follow */
  };

  struct NptDxvaSubmitEntry {
    uint32_t type;      /* D3D11_VIDEO_DECODER_BUFFER_TYPE */
    uint32_t offset;    /* data offset from the start of the blob */
    uint32_t size;      /* data size in bytes */
    uint32_t reserved;
  };

  enum class D3D11VideoCodec : uint32_t { None, H264, VP9, HEVC, AV1 };

  /* Profiles we expose, in order. */
  uint32_t D3D11VideoMcProfileCount();
  bool     D3D11VideoMcProfile(uint32_t index, GUID* pGuid);
  D3D11VideoCodec D3D11VideoMcCodecFor(const GUID& guid);
  bool     D3D11VideoMcAvailable();


  class D3D11VideoDecoderOutputView : public D3D11DeviceChild<ID3D11VideoDecoderOutputView> {

  public:

    D3D11VideoDecoderOutputView(
            D3D11Device*                          pDevice,
            ID3D11Resource*                       pResource,
      const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC& Desc);

    ~D3D11VideoDecoderOutputView();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                  riid,
            void**                  ppvObject);

    void STDMETHODCALLTYPE GetResource(
            ID3D11Resource**        ppResource);

    void STDMETHODCALLTYPE GetDesc(
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC* pDesc);

    Rc<DxvkImage> GetImage() const;

    uint32_t GetLayer() const {
      return m_desc.Texture2D.ArraySlice;
    }

  private:

    Com<ID3D11Resource>                   m_resource;
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC  m_desc;

  };


  class D3D11VideoDecoder : public D3D11DeviceChild<ID3D11VideoDecoder> {

  public:

    D3D11VideoDecoder(
            D3D11Device*                pDevice,
      const D3D11_VIDEO_DECODER_DESC&   Desc,
      const D3D11_VIDEO_DECODER_CONFIG& Config);

    ~D3D11VideoDecoder();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                  riid,
            void**                  ppvObject);

    HRESULT STDMETHODCALLTYPE GetCreationParameters(
            D3D11_VIDEO_DECODER_DESC*   pVideoDesc,
            D3D11_VIDEO_DECODER_CONFIG* pConfig);

    HRESULT STDMETHODCALLTYPE GetDriverHandle(
            HANDLE*                 pDriverHandle);

    /* API-level buffer path (host-native apps) */
    HRESULT GetBuffer(
            D3D11_VIDEO_DECODER_BUFFER_TYPE Type,
            UINT*                   pSize,
            void**                  ppBuffer);

    HRESULT SubmitApiBuffers(
            UINT                    Count,
      const D3D11_VIDEO_DECODER_BUFFER_DESC* pDescs);

    /* Inline buffers from Triton */
    HRESULT SubmitInline(
      const void*                   pData,
            UINT                    Size);

    HRESULT BeginFrame(
            D3D11VideoDecoderOutputView* pView);

    /* Decodes the frame; on output, records the copy into the target view
     * on the immediate context (caller holds the context lock). */
    HRESULT EndFrame(
            D3D11ImmediateContext*  pContext);

  private:

    struct CachedBuffer {
      bool            cacheable = false; /* otherwise owned until PendingImage retirement */
      uint64_t        id = 0;
      VkBuffer        vkBuffer = VK_NULL_HANDLE;
      VkDeviceMemory  vkMemory = VK_NULL_HANDLE;
      Rc<DxvkBuffer>  buffer;
      size_t          size = 0;
    };

    struct PendingImage {
      AImage*         image = nullptr;
      Rc<DxvkBuffer>  buffer;
    };

    D3D11_VIDEO_DECODER_DESC    m_desc;
    D3D11_VIDEO_DECODER_CONFIG  m_config;
    D3D11VideoCodec             m_codec;

    /* frame being assembled */
    Com<D3D11VideoDecoderOutputView> m_target;
    std::array<std::vector<uint8_t>, 16> m_bufs;   /* indexed by buffer type */
    std::array<std::vector<uint8_t>, 16> m_apiBufs;
    bool                        m_inFrame = false;

    /* MediaCodec */
    AMediaCodec*                m_codecHandle = nullptr;
    AImageReader*               m_reader = nullptr;
    bool                        m_started = false;
    std::atomic<bool>           m_failed = { false };
    uint32_t                    m_codecWidth = 0;
    uint32_t                    m_codecHeight = 0;
    uint64_t                    m_seq = 0;
    std::vector<uint8_t>        m_au;
    std::vector<uint8_t>        m_lastSps;
    std::vector<uint8_t>        m_lastPps;

    std::vector<CachedBuffer>   m_cache;
    std::deque<PendingImage>    m_pending;
    std::vector<std::pair<uint64_t, AImage*>> m_ready;  /* acquired, not yet copied */
    static constexpr size_t     kReaderImages = 16;
    bool                        m_destroying = false;
    bool                        m_codecCleanupUnknown = false;
    uint32_t                    m_maxLayer = 0;
    uint32_t                    m_identityTraceCount = 0;
    uint64_t                    m_statFrames = 0;
    double                      m_statMs = 0.0;
    double                      m_statMax = 0.0;

    bool EnsureCodec(uint32_t width, uint32_t height);
    bool DestroyCodec();
    bool BuildAccessUnit(bool* pExpectOutput, uint32_t* pWidth, uint32_t* pHeight);
    bool BuildH264(bool* pExpectOutput, uint32_t* pWidth, uint32_t* pHeight);
    bool BuildVP9(bool* pExpectOutput, uint32_t* pWidth, uint32_t* pHeight);

    /* AV1: a frame is held until the next one shows where it was stored
     * (refresh_frame_flags); hidden frames travel with the next shown one. */
    struct Av1Frame {
      DxvaPicParamsAV1          pp;
      std::vector<DxvaTileAV1>  tiles;
      std::vector<uint8_t>      data;
      uint64_t                  pts = 0;
      int                       refSlots[7] = { -1, -1, -1, -1, -1, -1, -1 };
      bool                      restore = false;  /* shown with refresh 0: store it once the app's map says where */
    };
    std::mutex                  m_av1Mutex;
    Av1State                    m_av1;
    std::unique_ptr<Av1Frame>   m_av1Pending;     /* previous frame, its slot known only from the next map */
    std::vector<uint8_t>        m_av1Acc;
    std::set<uint64_t>          m_av1Dropped;
    HRESULT EndFrameAV1(D3D11ImmediateContext* pContext);
    void FinalizeAv1Locked(std::unique_ptr<Av1Frame> f);
    bool WriteAv1Locked(const Av1Frame& f, const DxvaPicParamsAV1& pp, uint8_t refresh, std::vector<uint8_t>& obus);
    int  FreeAv1Slot(const bool* live) const;
    bool FlushAv1Pending(uint64_t pts);
    void Capture();
    void FailDecode(const char* reason, uint64_t pts);
    bool Queue(const std::vector<uint8_t>& au, uint64_t pts);
    /* CS thread: waits for frame pts and records the copy into dst/layer */
    void CompleteFrame(DxvkContext* ctx, uint64_t pts, const Rc<DxvkImage>& dst, uint32_t layer);
    AImage* AcquireOutput(uint64_t pts);
    bool CopyImage(AImage* image, DxvkContext* ctx, const Rc<DxvkImage>& dst, uint32_t layer);
    Rc<DxvkBuffer> ImportBuffer(AHardwareBuffer* ahb, size_t* pSize);
    void RetirePending(bool force);
    void SyncCs();

  };

}
