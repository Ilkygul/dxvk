#include <algorithm>

#include "d3d11_context.h"
#include "d3d11_context_def.h"
#include "d3d11_context_imm.h"

namespace dxvk {

  template<typename ContextType>
  D3D11CommonContext<ContextType>::D3D11CommonContext(
          D3D11Device*            pParent,
    const Rc<DxvkDevice>&         Device,
          UINT                    ContextFlags,
          DxvkCsChunkFlags        CsFlags)
  : D3D11DeviceChild<ID3D11DeviceContext4>(pParent),
    m_contextExt(GetTypedContext()),
    m_annotation(GetTypedContext(), Device),
    m_device    (Device),
    m_flags     (ContextFlags),
    m_staging   (Device, StagingBufferSize),
    m_csFlags   (CsFlags),
    m_csChunk   (AllocCsChunk()) {
    // Create local allocation cache with the same properties
    // that we will use for common dynamic buffer types
    uint32_t cachedDynamic = pParent->GetOptions()->cachedDynamicResources;

    VkMemoryPropertyFlags memoryFlags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (cachedDynamic & D3D11_BIND_CONSTANT_BUFFER) {
      memoryFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      cachedDynamic = 0;
    }

    VkBufferUsageFlags bufferUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    if (!(cachedDynamic & D3D11_BIND_SHADER_RESOURCE)) {
      bufferUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                  |  VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    }

    if (!(cachedDynamic & D3D11_BIND_VERTEX_BUFFER))
      bufferUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    if (!(cachedDynamic & D3D11_BIND_INDEX_BUFFER))
      bufferUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    m_allocationCache = m_device->createAllocationCache(bufferUsage, memoryFlags);

    // Determine maximum tess factor based on device options
    int32_t tessFactorOption = m_parent->GetOptions()->maxTessFactor;
    m_maxTessFactor = std::min(m_device->properties().core.properties.limits.maxTessellationGenerationLevel, 64u);

    if (tessFactorOption > 0 && tessFactorOption < int32_t(m_maxTessFactor))
      m_maxTessFactor = tessFactorOption;
  }


  template<typename ContextType>
  D3D11CommonContext<ContextType>::~D3D11CommonContext() {

  }


  template<typename ContextType>
  HRESULT STDMETHODCALLTYPE D3D11CommonContext<ContextType>::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D11DeviceChild)
     || riid == __uuidof(ID3D11DeviceContext)
     || riid == __uuidof(ID3D11DeviceContext1)
     || riid == __uuidof(ID3D11DeviceContext2)
     || riid == __uuidof(ID3D11DeviceContext3)
     || riid == __uuidof(ID3D11DeviceContext4)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11VkExtContext)
     || riid == __uuidof(ID3D11VkExtContext1)) {
      *ppvObject = ref(&m_contextExt);
      return S_OK;
    }

    if (riid == __uuidof(ID3DUserDefinedAnnotation)
     || riid == __uuidof(IDXVKUserDefinedAnnotation)) {
      *ppvObject = ref(&m_annotation);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11DeviceContext), riid)) {
      Logger::warn("D3D11DeviceContext::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  template<typename ContextType>
  D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GetType() {
    return IsDeferred
      ? D3D11_DEVICE_CONTEXT_DEFERRED
      : D3D11_DEVICE_CONTEXT_IMMEDIATE;
  }


  template<typename ContextType>
  UINT STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GetContextFlags() {
    return m_flags;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ClearState() {
    D3D10DeviceLock lock = LockContext();

    ResetCommandListState();
    ResetContextState();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DiscardResource(ID3D11Resource* pResource) {
    D3D10DeviceLock lock = LockContext();

    if (!pResource)
      return;

    D3D11_RESOURCE_DIMENSION resType = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    pResource->GetType(&resType);

    if (resType == D3D11_RESOURCE_DIMENSION_BUFFER) {
      DiscardBuffer(pResource);
    } else {
      auto texture = GetCommonTexture(pResource);
      auto image = texture->GetImage();

      for (uint32_t i = 0; i < texture->CountSubresources(); i++)
        DiscardTexture(pResource, i);

      if (image) {
        EmitCs([cImage = std::move(image)] (DxvkContext* ctx) {
          ctx->discardImage(cImage);
        });
      }
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DiscardView(ID3D11View* pResourceView) {
    DiscardViewBase(pResourceView, nullptr, 0);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DiscardView1(
          ID3D11View*              pResourceView,
    const D3D11_RECT*              pRects,
          UINT                     NumRects) {
    DiscardViewBase(pResourceView, pRects, NumRects);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DiscardViewBase(
          ID3D11View*              pResourceView,
    const D3D11_RECT*              pRects,
          UINT                     NumRects) {
    D3D10DeviceLock lock = LockContext();

    // We don't support discarding individual rectangles
    if (!pResourceView || (NumRects && pRects))
      return;

    // ID3D11View has no methods to query the exact type of
    // the view, so we'll have to check each possible class
    auto dsv = dynamic_cast<D3D11DepthStencilView*>(pResourceView);
    auto rtv = dynamic_cast<D3D11RenderTargetView*>(pResourceView);
    auto uav = dynamic_cast<D3D11UnorderedAccessView*>(pResourceView);

    Rc<DxvkImageView> view;
    if (dsv) view = dsv->GetImageView();
    if (rtv) view = rtv->GetImageView();
    if (uav) view = uav->GetImageView();

    if (view == nullptr)
      return;

    // Get information about underlying resource
    Com<ID3D11Resource> resource;
    pResourceView->GetResource(&resource);

    uint32_t mipCount = GetCommonTexture(resource.ptr())->Desc()->MipLevels;

    // Discard mip levels one by one
    VkImageSubresourceRange sr = view->subresources();

    for (uint32_t layer = 0; layer < sr.layerCount; layer++) {
      for (uint32_t mip = 0; mip < sr.levelCount; mip++) {
        DiscardTexture(resource.ptr(), D3D11CalcSubresource(
          sr.baseMipLevel + mip, sr.baseArrayLayer + layer, mipCount));
      }
    }

    if (rtv || dsv) {
      EmitCs([cView = view] (DxvkContext* ctx) {
        ctx->clearRenderTarget(cView, 0, VkClearValue(), cView->info().aspects);
      });
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopySubresourceRegion(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
          UINT                              DstX,
          UINT                              DstY,
          UINT                              DstZ,
          ID3D11Resource*                   pSrcResource,
          UINT                              SrcSubresource,
    const D3D11_BOX*                        pSrcBox) {
    CopySubresourceRegionBase(
      pDstResource, DstSubresource, DstX, DstY, DstZ,
      pSrcResource, SrcSubresource, pSrcBox, 0);
    NptA8AfterCopy(pDstResource, DstSubresource);
  }


}


namespace dxvk {
  // NPT_SHARED_SYNC (default on): writes to a shared (MISC_SHARED / dmabuf-exported) resource are tracked per immediate
  // context; an explicit Flush then waits until the GPU has finished them, so a consumer in another process (dwm) never
  // samples an intermediate state.  NPT_SHARED_SYNC=0 disables.
  bool nptSharedSyncEnabled() { static const bool on = env::getEnvVar("NPT_SHARED_SYNC") != "0"; return on; }
  // NPT_SHARED_DRAW_SYNC (default on): submit + wait for the GPU right after every draw/clear/copy/update that writes a shared resource.
  // 0 = off, 1 = submit + wait, 2 (default) = 1-texel CopySubresourceRegion to a staging texture + Map(READ) through the real D3D11 paths
  int nptSharedDrawSyncMode() { static const int m = [] { std::string v = env::getEnvVar("NPT_SHARED_DRAW_SYNC"); return v == "0" ? 0 : (v == "1" ? 1 : 2); }(); return m; }
  bool nptSharedDrawSyncEnabled() { return nptSharedDrawSyncMode() != 0; }
  bool nptSharedReadSyncEnabled() { static const bool on = env::getEnvVar("NPT_SHARED_READ_SYNC") != "0"; return on; }
}

// ---------------------------------------------------------------------------
// NPT_DUMP_A8=1 (DroidVM debugging aid for the "white text boxes" problem)
//  1. after CopySubresourceRegion(1) into an A8/R8 MISC_SHARED texture, and for the first
//     few Flush() calls afterwards: read the destination back and write /data/adb/fold-vm/npt/dump/<pid>-<tex>-<n>.pgm
//  2. at draws that sample an A8/R8 SRV in the pixel shader (first few): log pixel-shader name/hash,
//     blend state per RT, sampler, SRV desc + format swizzle, and dump render target 0 (.ppm + alpha .pgm)
// Only for the immediate context. Nothing happens unless the env var is set.
// ---------------------------------------------------------------------------
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace dxvk {

  struct NptA8Dump {
    bool enabled = false;
    std::string dir = "/data/adb/fold-vm/npt/dump";
    std::atomic<int> copyDumps = { 0 }, drawLogs = { 0 }, flushDumps = { 0 }, fileSeq = { 0 }, bufDraws = { 0 }, bufSeen = { 0 }, atlasSeen = { 0 }, atlasDraws = { 0 }, atlasDumps = { 0 }, opSeq = { 0 }, cbLogs = { 0 }, gpuChecks = { 0 }, gpuCheckSeq = { 0 };
    std::string psFilter = "b413";
    std::mutex mtx;
    Com<ID3D11Texture2D> last[4];
    std::unordered_map<const void*, std::string> hist;
    int lastCount = 0;
    NptA8Dump() {
      enabled = env::getEnvVar("NPT_DUMP_A8") == "1";
      std::string d = env::getEnvVar("NPT_DUMP_A8_DIR");
      if (!d.empty()) dir = d;
      { std::string f = env::getEnvVar("NPT_DUMP_A8_PS"); if (!f.empty()) psFilter = f; }
      if (enabled) mkdir(dir.c_str(), 0777);
    }
  };

  static NptA8Dump& nptA8() { static NptA8Dump d; return d; }

  // texture history (how/when a texture got its data), keyed by D3D11CommonTexture*
  void nptA8Note(const void* key, const std::string& what) {
    auto& d = nptA8(); if (!d.enabled || !key) return;
    std::lock_guard<std::mutex> l(d.mtx);
    auto& h = d.hist[key]; if (what.compare(0, 7, "create ") == 0) h.clear(); if (h.size() < 900) { h += what; h += "; "; }
  }
  bool nptA8Enabled() { return nptA8().enabled; }
  static std::string nptA8History(const void* key) {
    auto& d = nptA8(); std::lock_guard<std::mutex> l(d.mtx);
    auto it = d.hist.find(key); return it == d.hist.end() ? std::string("(no history: created before logging or by another path)") : it->second;
  }

  // Diagnostic only: no barriers, waits, resources or publication behavior added.
  static void nptTraceSharedGuard(const char* op, const void* context,
      DxvkDevice* device, ID3D11Resource* resource, bool tinyDone) {
    static const bool enabled = env::getEnvVar("NPT_SHARED_GUARD_TRACE") == "1";
    static std::atomic<uint32_t> count { 0 };
    if (!enabled) return;
    const uint32_t seq = count.fetch_add(1, std::memory_order_relaxed);
    if (seq >= 256) return;
    auto emit = [] (const std::string& message) {
#if defined(__ANDROID__)
      __android_log_write(ANDROID_LOG_INFO, "NptSharedGuard", message.c_str());
#endif
      Logger::info(message);
    };
    auto* texture = resource ? GetCommonTexture(resource) : nullptr;
    auto image = texture ? texture->GetImage() : Rc<DxvkImage>();
    if (!image) {
      emit(str::format("NPT-SHARED-GUARD pid=", getpid(), " op=", op,
        " seq=", seq, " context=", context, " resource=", resource, " image=none tinyDone=", tinyDone));
      return;
    }
    const auto& info = image->info();
    const char* mode = info.sharing.mode == DxvkSharedHandleMode::Import ? "Import" :
      (info.sharing.mode == DxvkSharedHandleMode::Export ? "Export" : "None");
    emit(str::format("NPT-SHARED-GUARD pid=", getpid(), " op=", op,
      " seq=", seq, " role=", op, " context=", context, " resource=", resource, " image=", image->handle(),
      " device=", device, " sharing=", mode, " sharingEnum=", uint32_t(info.sharing.mode),
      " handleType=", uint32_t(info.sharing.type), " format=", uint32_t(info.format),
      " size=", info.extent.width, "x", info.extent.height,
      " defaultLayout=", uint32_t(info.layout),
      " graphicsFamily=", device->queues().graphics.queueFamily,
      " transferFamily=", device->queues().transfer.queueFamily,
      " syncMode=", nptSharedDrawSyncMode(),
      " foreignExtension=", bool(device->features().extQueueFamilyForeign),
      " tinyDone=", tinyDone));
  }

  // Independent bounded import-read diagnostic. Existing TinySync behavior is unchanged.
  struct NptImportReadOutputs {
    bool omShared = false;
    uint32_t rtvs = 0, sharedRtvs = 0, uavs = 0, sharedUavs = 0;
    VkImage firstRtvImage = VK_NULL_HANDLE;
    uint32_t firstRtvSharing = 0, firstRtvFormat = 0;
    uint32_t firstRtvWidth = 0, firstRtvHeight = 0;
  };

  static bool nptImportReadTraceEnabled() {
    static const bool enabled = env::getEnvVar("NPT_SHARED_GUARD_TRACE") == "1";
    return enabled;
  }

  // Per-process guarded-draw entry sequence, not a resource-generation counter.
  static uint64_t nptImportReadDrawSequence() {
    if (!nptImportReadTraceEnabled()) return 0;
    static std::atomic<uint64_t> draws { 0 };
    return draws.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  static void nptTraceImportRead(const void* context, DxvkDevice* device,
      ID3D11Resource* resource, uint64_t drawSeq,
      const NptImportReadOutputs& outputs, bool tinyDone) {
    if (!nptImportReadTraceEnabled()) return;
    auto* texture = resource ? GetCommonTexture(resource) : nullptr;
    auto image = texture ? texture->GetImage() : Rc<DxvkImage>();
    if (!image || image->info().sharing.mode != DxvkSharedHandleMode::Import) return;
    const auto& info = image->info();
    static std::atomic<uint64_t> imports { 0 }, lateTargets { 0 }, lateAlpha { 0 };
    const uint64_t importSeq = imports.fetch_add(1, std::memory_order_relaxed);
    const bool target960 = info.extent.width == 960 && info.extent.height == 640;
    uint64_t targetSeq = 0;
    bool sampledTarget = false;
    const bool alphaFormat = info.format == VK_FORMAT_R8_UNORM
                          || info.format == VK_FORMAT_A8_UNORM_KHR;
    // Keep an independent budget for alpha-mask candidates imported after
    // the compositor's initial burst. R8 may represent remapped A8.
    const bool sampledAlpha = importSeq >= 128 && alphaFormat
      && lateAlpha.fetch_add(1, std::memory_order_relaxed) < 64;
    if (importSeq >= 128 && !sampledAlpha) {
      if (!target960) return;
      targetSeq = lateTargets.fetch_add(1, std::memory_order_relaxed);
      // At most 64 later records: one per 120 qualifying target reads.
      sampledTarget = targetSeq < 120u * 64u && (targetSeq % 120u) == 0;
      if (!sampledTarget) return;
    }
    const bool derivedOmShared = outputs.sharedRtvs || outputs.sharedUavs;
    const auto message = str::format("NPT-IMPORT-READ pid=", getpid(),
      " context=", context, " device=", device, " drawSeq=", drawSeq,
      " importSeq=", importSeq, " target960=", target960,
      " sampledTarget=", sampledTarget, " sampledAlpha=", sampledAlpha, " targetSeq=", targetSeq,
      " resource=", resource, " image=", image->handle(),
      " sharing=Import sharingEnum=", uint32_t(info.sharing.mode),
      " handleType=", uint32_t(info.sharing.type), " format=", uint32_t(info.format),
      " size=", info.extent.width, "x", info.extent.height,
      " defaultLayout=", uint32_t(info.layout),
      " graphicsFamily=", device->queues().graphics.queueFamily,
      " transferFamily=", device->queues().transfer.queueFamily,
      " omShared=", outputs.omShared, " derivedOmShared=", derivedOmShared,
      " rtvCount=", outputs.rtvs, " sharedRtvCount=", outputs.sharedRtvs,
      " privateRtvCount=", outputs.rtvs - outputs.sharedRtvs,
      " uavCount=", outputs.uavs, " sharedUavCount=", outputs.sharedUavs,
      " firstRtvImage=", outputs.firstRtvImage,
      " firstRtvSharingEnum=", outputs.firstRtvSharing,
      " firstRtvFormat=", outputs.firstRtvFormat,
      " firstRtvSize=", outputs.firstRtvWidth, "x", outputs.firstRtvHeight,
      " syncMode=", nptSharedDrawSyncMode(), " tinyDone=", tinyDone,
      " phase=before-draw (binding-and-pre-readback-not-draw-completion)");
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_INFO, "NptImportRead", message.c_str());
#endif
    Logger::info(message);
  }

  static bool nptIsA8R8(DXGI_FORMAT f) { return f == DXGI_FORMAT_A8_UNORM || (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_R8_SINT); }

  // Compare completed GPU contents on each side of a shared atlas handoff.
  // This is intentionally bounded and unsuitable for performance measurements.
  static void nptTraceSharedAlphaPixels(ID3D11DeviceContext* ctx,
      ID3D11Resource* resource, bool consumer) {
    if (!nptImportReadTraceEnabled() || !resource)
      return;

    Com<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&texture))))
      return;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if ((desc.Format != DXGI_FORMAT_A8_UNORM && desc.Format != DXGI_FORMAT_R8_UNORM)
     || desc.Width < 256 || desc.Height < 256
     || uint64_t(desc.Width) * desc.Height > 4u * 1024u * 1024u
     || desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
      return;

    auto image = GetCommonTexture(resource)->GetImage();
    if (!image || image->info().sharing.mode != (consumer
        ? DxvkSharedHandleMode::Import : DxvkSharedHandleMode::Export))
      return;

    static std::atomic<uint32_t> writes { 0 }, reads { 0 };
    if ((consumer ? reads : writes).fetch_add(1, std::memory_order_relaxed) >= 8)
      return;

    Com<ID3D11Device> device;
    texture->GetDevice(&device);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
    D3D11_MAPPED_SUBRESOURCE map = { };
    if (SUCCEEDED(hr)) {
      ctx->CopyResource(staging.ptr(), texture.ptr());
      hr = ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &map);
    }

    uint64_t hash = 14695981039346656037ull;
    uint32_t nonzero = 0;
    if (SUCCEEDED(hr)) {
      for (uint32_t y = 0; y < desc.Height; y++) {
        const auto* row = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
        for (uint32_t x = 0; x < desc.Width; x++) {
          hash = (hash ^ row[x]) * 1099511628211ull;
          nonzero += row[x] != 0;
        }
      }
      ctx->Unmap(staging.ptr(), 0);
    }
    const auto message = str::format("NPT-ALPHA-PIXELS pid=", getpid(),
      " side=", consumer ? "import" : "export", " resource=", resource,
      " image=", image->handle(), " size=", desc.Width, "x", desc.Height,
      " hr=", std::hex, uint32_t(hr), " hash=", hash, std::dec,
      " nonzero=", nonzero);
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_INFO, "NptAlphaPixels", message.c_str());
#endif
    Logger::info(message);
  }

  static void nptDumpTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const char* tag) {
    auto& d = nptA8();
    D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) return;
    Com<ID3D11Device> dev; tex->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    Com<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) { Logger::warn("NPT_DUMP_A8: staging create failed"); return; }
    ctx->CopyResource(st.ptr(), tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st.ptr(), 0, D3D11_MAP_READ, 0, &m))) { Logger::warn("NPT_DUMP_A8: Map(READ) failed"); return; }
    const bool one = nptIsA8R8(desc.Format);
    const bool four = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM
                   || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (one || four) {
      const bool bgr = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
      std::string base = str::format(d.dir, "/", getpid(), "-", std::hex, uintptr_t(tex) & 0xffffff, std::dec, "-", d.fileSeq.fetch_add(1), ".", tag);
      std::string path = base + (one ? ".pgm" : ".ppm");
      uint64_t sum = 0, nz = 0, n255 = 0; uint32_t mx = 0;
      if (FILE* f = fopen(path.c_str(), "wb")) {
        fprintf(f, one ? "P5\n%u %u\n255\n" : "P6\n%u %u\n255\n", desc.Width, desc.Height);
        for (UINT y = 0; y < desc.Height; y++) {
          const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
          if (one) { fwrite(row, 1, desc.Width, f); for (UINT x = 0; x < desc.Width; x++) { sum += row[x]; nz += row[x] != 0; n255 += row[x] == 255; mx = std::max<uint32_t>(mx, row[x]); } }
          else for (UINT x = 0; x < desc.Width; x++) { uint8_t rgb[3] = { row[x*4 + (bgr ? 2 : 0)], row[x*4 + 1], row[x*4 + (bgr ? 0 : 2)] }; fwrite(rgb, 1, 3, f);
            if (row[x*4] == 255 && row[x*4+1] == 255 && row[x*4+2] == 255 && row[x*4+3] == 255) nz++; else if (row[x*4+3]) sum++; }
        }
        fclose(f);
      }
      if (four) {
        std::string apath = base + ".alpha.pgm";
        if (FILE* f = fopen(apath.c_str(), "wb")) {
          fprintf(f, "P5\n%u %u\n255\n", desc.Width, desc.Height);
          for (UINT y = 0; y < desc.Height; y++) { const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch; for (UINT x = 0; x < desc.Width; x++) fputc(row[x*4 + 3], f); }
          fclose(f);
        }
      }
      Logger::info(str::format("NPT_DUMP_A8: wrote ", path, " fmt=", uint32_t(desc.Format), " ", desc.Width, "x", desc.Height, " rowPitch=", m.RowPitch,
        one ? str::format(" nonzero=", nz, "/", uint64_t(desc.Width) * desc.Height, " max=", mx, " ==255:", n255, " mean=", sum / std::max<uint64_t>(1, uint64_t(desc.Width) * desc.Height)) : str::format(" opaqueWhitePixels=", nz, " otherAlphaNonzeroPixels=", sum, " of ", uint64_t(desc.Width) * desc.Height)));
    }
    ctx->Unmap(st.ptr(), 0);
  }

  static std::string nptSwz(VkComponentSwizzle c) {
    switch (c) { case VK_COMPONENT_SWIZZLE_IDENTITY: return "I"; case VK_COMPONENT_SWIZZLE_ZERO: return "0"; case VK_COMPONENT_SWIZZLE_ONE: return "1";
      case VK_COMPONENT_SWIZZLE_R: return "R"; case VK_COMPONENT_SWIZZLE_G: return "G"; case VK_COMPONENT_SWIZZLE_B: return "B"; case VK_COMPONENT_SWIZZLE_A: return "A"; default: return "?"; }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8AfterCopy(ID3D11Resource* pDst, UINT DstSubresource) {
    auto& d = nptA8();
    if (!d.enabled || GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !pDst) return;
    D3D11_RESOURCE_DIMENSION dim; pDst->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    Com<ID3D11Texture2D> t; pDst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
    if (!t) return;
    D3D11_TEXTURE2D_DESC desc; t->GetDesc(&desc);
    if (!nptIsA8R8(desc.Format) || !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) return;
    {
      std::lock_guard<std::mutex> l(d.mtx);
      bool have = false; for (int i = 0; i < d.lastCount; i++) have |= d.last[i].ptr() == t.ptr();
      if (!have) { d.last[d.lastCount < 4 ? d.lastCount++ : 3] = t; }
      d.flushDumps.store(0);
    }
    if (d.copyDumps.fetch_add(1) < 40)
      nptDumpTexture(static_cast<ID3D11DeviceContext*>(this), t.ptr(), "copy");
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8OnFlush() {
    auto& d = nptA8();
    if (!d.enabled || GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !d.lastCount) return;
    if (d.flushDumps.fetch_add(1) >= 5) return;
    Com<ID3D11Texture2D> l[4]; int n;
    { std::lock_guard<std::mutex> g(d.mtx); n = d.lastCount; for (int i = 0; i < n; i++) l[i] = d.last[i]; }
    for (int i = 0; i < n; i++) nptDumpTexture(static_cast<ID3D11DeviceContext*>(this), l[i].ptr(), "flush");
  }




  // VS/PS constant buffers (slot, buffer, FirstConstant/NumConstants as the app bound them, first 256 bytes of the bound range as
  // floats and hex) and VS SRVs.
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8DumpCB() {
    ID3D11DeviceContext* ic = static_cast<ID3D11DeviceContext*>(this);
    struct St { D3D11ShaderType t; const char* n; } stages[] = { { D3D11ShaderType::eVertex, "VS" }, { D3D11ShaderType::ePixel, "PS" } };
    for (auto& st : stages) {
      auto& cbv = m_state.cbv[st.t];
      for (uint32_t i = 0; i < cbv.maxCount; i++) {
        auto& b = cbv.buffers[i]; if (!b.buffer) continue;
        const D3D11_BUFFER_DESC bd = *b.buffer->Desc();
        const UINT offBytes = b.constantOffset * 16u, cntBytes = b.constantCount * 16u;
        Logger::info(str::format("NPT_DUMP_A8 cb: ", st.n, " slot=", i, " buffer ByteWidth=", bd.ByteWidth, " usage=", uint32_t(bd.Usage), " | FirstConstant=", b.constantOffset, " (", offBytes, " B) NumConstants=", b.constantCount, " (", cntBytes, " B) bound=", b.constantBound));
        if (bd.ByteWidth > (16u << 20)) continue;
        Com<ID3D11Device> dv; b.buffer->GetDevice(&dv);
        D3D11_BUFFER_DESC sb = bd; sb.Usage = D3D11_USAGE_STAGING; sb.BindFlags = 0; sb.MiscFlags = 0; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sb.StructureByteStride = 0;
        Com<ID3D11Buffer> sbuf; if (FAILED(dv->CreateBuffer(&sb, nullptr, &sbuf))) continue;
        ic->CopyResource(sbuf.ptr(), static_cast<ID3D11Buffer*>(b.buffer.ptr()));
        D3D11_MAPPED_SUBRESOURCE m; if (FAILED(ic->Map(sbuf.ptr(), 0, D3D11_MAP_READ, 0, &m))) continue;
        const uint8_t* p = (const uint8_t*)m.pData;
        for (UINT row = 0; row < 6 && offBytes + row * 16 + 16 <= bd.ByteWidth; row++) {
          float f[4]; memcpy(f, p + offBytes + row * 16, 16); uint32_t u[4]; memcpy(u, f, 16);
          Logger::info(str::format("NPT_DUMP_A8 cb:   ", st.n, " cb", i, "[", row, "] = ", f[0], ", ", f[1], ", ", f[2], ", ", f[3], "   hex ", std::hex, u[0], " ", u[1], " ", u[2], " ", u[3], std::dec));
        }
        // also the first 64 bytes of the whole buffer, in case the offset binding is being ignored somewhere
        if (offBytes) { float f[4]; memcpy(f, p, 16); Logger::info(str::format("NPT_DUMP_A8 cb:   ", st.n, " cb", i, " (buffer start, offset ignored) = ", f[0], ", ", f[1], ", ", f[2], ", ", f[3])); }
        ic->Unmap(sbuf.ptr(), 0);
      }
    }
    auto& vsrv = m_state.srv[D3D11ShaderType::eVertex];
    for (uint32_t i = 0; i < vsrv.maxCount; i++) if (vsrv.views[i]) { D3D11_SHADER_RESOURCE_VIEW_DESC1 vd; vsrv.views[i]->GetDesc1(&vd);
      Logger::info(str::format("NPT_DUMP_A8 cb: VS SRV slot=", i, " view fmt=", uint32_t(vd.Format), " dim=", uint32_t(vd.ViewDimension))); }
  }






  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptTouch(ID3D11Resource* pRes) {
    if (!pRes) return;
    for (auto& t : m_nptTouched) if (t.ptr() == pRes) return;
    if (m_nptTouched.size() < 8) m_nptTouched.push_back(pRes);
  }

  // Reads one texel (or 4 bytes of a buffer) of the resource back through the immediate context: the copy resolves
  // deferred clears, adds the transfer barriers and the Map waits for the GPU -- exactly what the NPT_DUMP_A8 readback did.
  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::NptTinySync(ID3D11Resource* pRes) {
    if constexpr (IsDeferred) {
      return false;
    } else {
      if (!pRes) return false;
      ID3D11DeviceContext* ic = static_cast<ID3D11DeviceContext*>(this);
      D3D11_RESOURCE_DIMENSION dim; pRes->GetType(&dim);
      ID3D11Resource* stage = nullptr;
      D3D11_BOX box = { 0, 0, 0, 1, 1, 1 };
      if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto* b = static_cast<D3D11Buffer*>(pRes);
        if (b->Desc()->ByteWidth < 4) return false;
        if (!m_nptTinyBuf) {
          D3D11_BUFFER_DESC bd = { }; bd.ByteWidth = 4; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          if (FAILED(m_parent->CreateBuffer(&bd, nullptr, &m_nptTinyBuf))) return false;
        }
        stage = m_nptTinyBuf.ptr(); box.right = 4;
      } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        auto* t = GetCommonTexture(pRes); if (!t) return false;
        const D3D11_COMMON_TEXTURE_DESC* d = t->Desc();
        if (d->SampleDesc.Count > 1) return false;
        const DXGI_FORMAT f = d->Format;
        if ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB)) return false;
        auto it = m_nptTinyStage.find(uint32_t(f));
        if (it == m_nptTinyStage.end()) {
          D3D11_TEXTURE2D_DESC td = { }; td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1;
          td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          Com<ID3D11Texture2D> nt; if (FAILED(m_parent->CreateTexture2D(&td, nullptr, &nt))) return false;
          it = m_nptTinyStage.emplace(uint32_t(f), std::move(nt)).first;
        }
        stage = it->second.ptr();
      } else return false;
      ic->CopySubresourceRegion(stage, 0, 0, 0, 0, pRes, 0, &box);
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(ic->Map(stage, 0, D3D11_MAP_READ, 0, &m))) return false;
      ic->Unmap(stage, 0);
      return true;
    }
  }

  // Consumer side: before a draw that samples a shared / imported image, read one texel of it back (see NptTinySync).
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptReadSyncBeforeDraw() {
    if constexpr (!IsDeferred) {
      if (!nptSharedReadSyncEnabled() || !nptSharedSyncEnabled()) return;
      const uint64_t traceDrawSeq = nptImportReadDrawSequence();
      small_vector<Com<ID3D11Resource>, 4> res;
      for (uint32_t st = 0; st < 2; st++) {
        auto& srvs = m_state.srv[st == 0 ? D3D11ShaderType::ePixel : D3D11ShaderType::eVertex];
        for (uint32_t i = 0; i < srvs.maxCount; i++) {
          if (!srvs.views[i]) continue;
          if (!NptViewShared(srvs.views[i]->GetImageView())) continue;
          Com<ID3D11Resource> r; srvs.views[i]->GetResource(&r);
          bool dup = false; for (auto& x : res) dup |= x.ptr() == r.ptr();
          if (!dup && res.size() < 4) res.push_back(std::move(r));
        }
      }
      NptImportReadOutputs outputs;
      if (!res.empty() && nptImportReadTraceEnabled()) {
        outputs.omShared = m_nptOmShared;
        for (const auto& rtv : m_state.om.rtvs) {
          if (!rtv) continue;
          auto view = rtv->GetImageView();
          if (!view) continue;
          outputs.rtvs++;
          if (NptViewShared(view)) outputs.sharedRtvs++;
          if (!outputs.firstRtvImage) {
            const auto& info = view->image()->info();
            outputs.firstRtvImage = view->image()->handle();
            outputs.firstRtvSharing = uint32_t(info.sharing.mode);
            outputs.firstRtvFormat = uint32_t(info.format);
            outputs.firstRtvWidth = info.extent.width;
            outputs.firstRtvHeight = info.extent.height;
          }
        }
        for (UINT i = m_state.om.minUav; i < m_state.om.maxUav && i < m_state.om.uavs.size(); i++) {
          if (!m_state.om.uavs[i]) continue;
          outputs.uavs++;
          if (NptViewShared(m_state.om.uavs[i]->GetImageView())) outputs.sharedUavs++;
        }
      }
      for (auto& r : res) {
        const bool done = NptTinySync(r.ptr());
        nptTraceSharedGuard("source-read-before-draw", this, m_device.ptr(), r.ptr(), done);
        nptTraceImportRead(this, m_device.ptr(), r.ptr(), traceDrawSeq, outputs, done);
        if (done)
          nptTraceSharedAlphaPixels(static_cast<ID3D11DeviceContext*>(this), r.ptr(), true);
      }
    }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptDrawSyncNow() {
    if constexpr (!IsDeferred) {
      if (m_nptSharedDirty && nptSharedDrawSyncEnabled() && nptSharedSyncEnabled()) {
        m_nptSharedDirty = false;
        bool done = false;
        if (nptSharedDrawSyncMode() == 2) {
          // The op's destinations (copy/clear/update/resolve targets) plus every shared render target / UAV bound for draws.
          auto touched = std::move(m_nptTouched);
          m_nptTouched.clear();
          if (m_nptOmShared) {
            for (UINT i = 0; i < m_state.om.rtvs.size(); i++)
              if (m_state.om.rtvs[i] != nullptr && NptViewShared(m_state.om.rtvs[i]->GetImageView())) {
                Com<ID3D11Resource> r; m_state.om.rtvs[i]->GetResource(&r); if (r != nullptr) touched.push_back(std::move(r)); }
            for (UINT i = m_state.om.minUav; i < m_state.om.maxUav && i < m_state.om.uavs.size(); i++)
              if (m_state.om.uavs[i] != nullptr && NptViewShared(m_state.om.uavs[i]->GetImageView())) {
                Com<ID3D11Resource> r; m_state.om.uavs[i]->GetResource(&r); if (r != nullptr) touched.push_back(std::move(r)); }
          }
          for (auto& r : touched) {
            const bool tinyDone = NptTinySync(r.ptr());
            done |= tinyDone;
            nptTraceSharedGuard("destination-write-after-op", this, m_device.ptr(), r.ptr(), tinyDone);
            if (tinyDone)
              nptTraceSharedAlphaPixels(static_cast<ID3D11DeviceContext*>(this), r.ptr(), false);
          }
        } else {
          m_nptTouched.clear();
        }
        if (!done)
          static_cast<ContextType*>(this)->NptSharedSyncNow();
      }
    }
  }

  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::NptViewShared(const Rc<DxvkImageView>& view) {
    return view != nullptr && view->image()->info().sharing.mode != DxvkSharedHandleMode::None;
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptMarkSharedRes(ID3D11Resource* pRes) {
    if (!pRes || !nptSharedSyncEnabled()) return;
    D3D11_RESOURCE_DIMENSION dim; pRes->GetType(&dim);
    UINT misc = 0;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) misc = static_cast<D3D11Buffer*>(pRes)->Desc()->MiscFlags;
    else if (auto* t = GetCommonTexture(pRes)) {
      misc = t->Desc()->MiscFlags;
      // Imported / exported images count even when the importer's desc carries no sharing flag
      // (e.g. synthesised descriptors of KMD standard allocations).
      Rc<DxvkImage> img = t->GetImage();
      if (img != nullptr && img->info().sharing.mode != DxvkSharedHandleMode::None)
        misc |= D3D11_RESOURCE_MISC_SHARED;
    }
    if (misc & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) {
      m_nptSharedDirty = true;
      NptTouch(pRes);
    }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptUpdateOmShared() {
    m_nptOmShared = false;
    if (!nptSharedSyncEnabled()) return;
    for (UINT i = 0; i < m_state.om.rtvs.size(); i++)
      if (m_state.om.rtvs[i] != nullptr && NptViewShared(m_state.om.rtvs[i]->GetImageView())) m_nptOmShared = true;
    for (UINT i = m_state.om.minUav; i < m_state.om.maxUav && i < m_state.om.uavs.size(); i++)
      if (m_state.om.uavs[i] != nullptr && NptViewShared(m_state.om.uavs[i]->GetImageView())) m_nptOmShared = true;
  }

  // ---- GPU-ordered snapshots around a draw (pre-draw copy, draw, post-draw copy) of VS cbuffers 0/1 and the vertex slices the draw reads.
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8BeforeDraw(uint32_t kind, uint32_t count, uint32_t instances, uint32_t start, int32_t base) {
    auto& d = nptA8();
    if (!d.enabled || GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || m_nptPre.valid) return;
    if (d.gpuChecks.load() >= 6 || !m_state.ps.ptr()) return;
    std::string ps = m_state.ps->GetCommonShader()->GetName();
    if (ps.find(d.psFilter) == std::string::npos) return;
    d.gpuChecks.fetch_add(1);
    m_nptDraw = { kind, count, instances, start, base };
    ID3D11DeviceContext* ic = static_cast<ID3D11DeviceContext*>(this);
    m_nptPre = NptPre(); m_nptPre.ps = ps; m_nptPre.valid = true;
    auto snap = [&](D3D11Buffer* b, UINT off, UINT len, const std::string& tag, Com<ID3D11Buffer>& out) {
      D3D11_BUFFER_DESC bd = *b->Desc(); if (off >= bd.ByteWidth) return; len = std::min<UINT>(len, bd.ByteWidth - off); if (!len) return;
      Com<ID3D11Device> dv; b->GetDevice(&dv); D3D11_BUFFER_DESC sb = { }; sb.ByteWidth = len; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(dv->CreateBuffer(&sb, nullptr, &out))) return;
      D3D11_BOX box = { off, 0, 0, off + len, 1, 1 }; ic->CopySubresourceRegion(out.ptr(), 0, 0, 0, 0, static_cast<ID3D11Buffer*>(b), 0, &box);
      m_nptPre.items.push_back({ tag, b, off, len, out, nullptr });
    };
    auto& cbv = m_state.cbv[D3D11ShaderType::eVertex];
    for (uint32_t i = 0; i < 2 && i < cbv.maxCount; i++) if (cbv.buffers[i].buffer) {
      Com<ID3D11Buffer> o; snap(cbv.buffers[i].buffer.ptr(), cbv.buffers[i].constantOffset * 16u, 256u, str::format("VS cb", i), o); }
    if (auto* il = m_state.ia.inputLayout.ptr()) {
      uint32_t na = il->GetAttributeCount(), nb = il->GetBindingCount();
      for (uint32_t i = 0; i < nb; i++) { auto bnd = il->GetInput(na + i).binding(); auto& vb = m_state.ia.vertexBuffers[bnd.binding]; if (!vb.buffer) continue;
        const bool inst = bnd.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE; const bool indexed = kind == 1 || kind == 3;
        UINT stride = vb.stride ? vb.stride : 4u; UINT first = vb.offset + (inst ? 0u : (indexed ? uint32_t(std::max(0, base)) : start) * stride);
        UINT n = inst ? instances : (indexed ? 64u : count); n = std::min<UINT>(std::max<UINT>(n, 1u), 4096u / stride + 1);
        Com<ID3D11Buffer> o; snap(vb.buffer.ptr(), first, n * stride, str::format("VB slot", bnd.binding, inst ? " (instance)" : " (vertex)", " stride=", stride), o); }
    }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8GpuCheck() {
    if (!m_nptPre.valid) return;
    auto& d = nptA8(); ID3D11DeviceContext* ic = static_cast<ID3D11DeviceContext*>(this);
    int n = d.gpuCheckSeq.fetch_add(1);
    for (auto& it : m_nptPre.items) {
      Com<ID3D11Device> dv; it.buf->GetDevice(&dv); D3D11_BUFFER_DESC sb = { }; sb.ByteWidth = it.len; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(dv->CreateBuffer(&sb, nullptr, &it.post))) continue;
      D3D11_BOX box = { it.off, 0, 0, it.off + it.len, 1, 1 }; ic->CopySubresourceRegion(it.post.ptr(), 0, 0, 0, 0, static_cast<ID3D11Buffer*>(it.buf), 0, &box);
    }
    static const char* kinds[] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced" };
    Logger::info(str::format("NPT_DUMP_A8 gpucheck #", n, " PS=", m_nptPre.ps, " ", kinds[m_nptDraw.kind], " count=", m_nptDraw.count, " instances=", m_nptDraw.instances, " start=", m_nptDraw.start));
    for (auto& it : m_nptPre.items) {
      if (!it.post) continue;
      D3D11_MAPPED_SUBRESOURCE a, b;
      if (FAILED(ic->Map(it.pre.ptr(), 0, D3D11_MAP_READ, 0, &a))) continue;
      if (FAILED(ic->Map(it.post.ptr(), 0, D3D11_MAP_READ, 0, &b))) { ic->Unmap(it.pre.ptr(), 0); continue; }
      const uint8_t* pa = (const uint8_t*)a.pData; const uint8_t* pb = (const uint8_t*)b.pData; int diff = -1;
      for (UINT k = 0; k < it.len; k++) if (pa[k] != pb[k]) { diff = int(k); break; }
      auto hx = [&](const uint8_t* p) { std::string h; for (UINT k = 0; k < std::min<UINT>(it.len, 64u); k++) { char t[4]; snprintf(t, sizeof t, "%02x", p[k]); h += t; } return h; };
      std::string line = str::format("NPT_DUMP_A8 gpucheck:   ", it.tag, " srcOffset=", it.off, " len=", it.len, diff < 0 ? " GPU-ORDERED MATCH" : str::format(" GPU-ORDERED MISMATCH at byte ", diff),
        "\n    pre-draw  first64=", hx(pa), "\n    post-draw first64=", hx(pb));
      if (it.tag.compare(0, 5, "VS cb") == 0) { float f[4]; memcpy(f, pa, 16); line += str::format("\n    pre-draw cb[0]=(", f[0], ",", f[1], ",", f[2], ",", f[3], ")"); if (it.len >= 32) { memcpy(f, pa + 16, 16); line += str::format(" cb[1]=(", f[0], ",", f[1], ",", f[2], ",", f[3], ")"); } }
      Logger::info(line);
      ic->Unmap(it.post.ptr(), 0); ic->Unmap(it.pre.ptr(), 0);
    }
    m_nptPre = NptPre();
  }

  // Draws that render into an A8/R8 render target (glyph atlas): full state log + atlas dump after the draw.
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8AtlasDraw() {
    auto& d = nptA8();
    if (!m_state.om.rtvs[0]) return;
    D3D11_RENDER_TARGET_VIEW_DESC1 rvd; m_state.om.rtvs[0]->GetDesc1(&rvd);
    if (!nptIsA8R8(rvd.Format)) return;
    int seen = d.atlasSeen.fetch_add(1);
    if (!(seen < 20 || seen % 3 == 0)) return;
    if (d.atlasDraws.fetch_add(1) >= 70) return;
    static const char* kinds[] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced" };
    std::string ps = m_state.ps.ptr() ? m_state.ps->GetCommonShader()->GetName() : "(no PS)";
    std::string vs = m_state.vs.ptr() ? m_state.vs->GetCommonShader()->GetName() : "(no VS)";
    Com<ID3D11Resource> res; m_state.om.rtvs[0]->GetResource(&res);
    Com<ID3D11Texture2D> rt; if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt));
    D3D11_TEXTURE2D_DESC td = { }; if (rt) rt->GetDesc(&td);
    Logger::info(str::format("NPT_DUMP_A8 atlasdraw #", seen, " seq=", d.opSeq.fetch_add(1), ": ", kinds[m_nptDraw.kind], " count=", m_nptDraw.count, " instances=", m_nptDraw.instances, " | PS=", ps, " VS=", vs,
      " | RT0 view fmt=", uint32_t(rvd.Format), " tex ", td.Width, "x", td.Height, " misc=0x", std::hex, td.MiscFlags, std::dec, " dim=", uint32_t(rvd.ViewDimension)));
    if (m_state.om.cbState.ptr()) { const auto& bd = m_state.om.cbState->Desc();
      for (uint32_t r = 0; r < (bd.IndependentBlendEnable ? 2u : 1u); r++) { const auto& b = bd.RenderTarget[r];
        Logger::info(str::format("NPT_DUMP_A8 atlasdraw:   blend RT[", r, "] en=", b.BlendEnable, " src=", uint32_t(b.SrcBlend), " dst=", uint32_t(b.DestBlend), " op=", uint32_t(b.BlendOp), " srcA=", uint32_t(b.SrcBlendAlpha), " dstA=", uint32_t(b.DestBlendAlpha), " opA=", uint32_t(b.BlendOpAlpha), " mask=0x", std::hex, uint32_t(b.RenderTargetWriteMask), std::dec)); } }
    else Logger::info("NPT_DUMP_A8 atlasdraw:   blend = default (disabled, mask all)");
    auto& rs = m_state.rs; (void)rs;
    for (uint32_t i = 0; i < m_state.rs.numViewports; i++) { const auto& v = m_state.rs.viewports[i]; Logger::info(str::format("NPT_DUMP_A8 atlasdraw:   viewport[", i, "] ", v.TopLeftX, ",", v.TopLeftY, " ", v.Width, "x", v.Height)); }
    for (uint32_t i = 0; i < m_state.rs.numScissors; i++) { const auto& r = m_state.rs.scissors[i]; Logger::info(str::format("NPT_DUMP_A8 atlasdraw:   scissor[", i, "] ", r.left, ",", r.top, " - ", r.right, ",", r.bottom)); }
    auto& psrv = m_state.srv[D3D11ShaderType::ePixel];
    for (uint32_t i = 0; i < psrv.maxCount; i++) if (psrv.views[i]) {
      D3D11_SHADER_RESOURCE_VIEW_DESC1 vd; psrv.views[i]->GetDesc1(&vd); D3D11_COMMON_RESOURCE_DESC rd = psrv.views[i]->GetResourceDesc();
      Com<ID3D11Resource> r2; psrv.views[i]->GetResource(&r2); std::string extra;
      if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) extra = str::format(" FirstElement=", vd.Buffer.FirstElement, " NumElements=", vd.Buffer.NumElements);
      else { Com<ID3D11Texture2D> t2; if (r2) r2->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2)); if (t2) { D3D11_TEXTURE2D_DESC x; t2->GetDesc(&x); extra = str::format(" tex ", x.Width, "x", x.Height, " history: ", nptA8History(GetCommonTexture(r2.ptr()))); } }
      Logger::info(str::format("NPT_DUMP_A8 atlasdraw:   PS SRV[", i, "] view fmt=", uint32_t(vd.Format), " dim=", uint32_t(vd.ViewDimension), " res fmt=", uint32_t(rd.Format), " usage=", uint32_t(rd.Usage), extra)); }
    NptA8DumpIA();
    NptA8DumpCB();
    if (rt && d.atlasDumps.fetch_add(1) < 40) nptDumpTexture(static_cast<ID3D11DeviceContext*>(this), rt.ptr(), "atlasdraw");
  }

  // IA / VS state at the draw: VS name, input layout (location/binding/format/offset), vertex buffer bindings + first vertices,
  // draw parameters, index buffer + first indices.
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8DumpIA() {
    ID3D11DeviceContext* ic = static_cast<ID3D11DeviceContext*>(this);
    std::string vsn = m_state.vs.ptr() ? m_state.vs->GetCommonShader()->GetName() : "(no VS)";
    static const char* kinds[] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced" };
    Logger::info(str::format("NPT_DUMP_A8 ia: VS=", vsn, " draw=", kinds[m_nptDraw.kind], " count=", m_nptDraw.count, " instances=", m_nptDraw.instances,
      " start=", m_nptDraw.start, " baseVertex=", m_nptDraw.base, " topology=", uint32_t(m_state.ia.primitiveTopology)));
    auto readBuf = [&](D3D11Buffer* b, UINT off, UINT len, std::vector<uint8_t>& out) -> bool {
      if (!b) return false; D3D11_BUFFER_DESC bd = *b->Desc(); if (bd.ByteWidth > (16u << 20) || off >= bd.ByteWidth) return false;
      len = std::min<UINT>(len, bd.ByteWidth - off);
      Com<ID3D11Device> dv; b->GetDevice(&dv);
      D3D11_BUFFER_DESC sb = bd; sb.Usage = D3D11_USAGE_STAGING; sb.BindFlags = 0; sb.MiscFlags = 0; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sb.StructureByteStride = 0;
      Com<ID3D11Buffer> st; if (FAILED(dv->CreateBuffer(&sb, nullptr, &st))) return false;
      ic->CopyResource(st.ptr(), static_cast<ID3D11Buffer*>(b));
      D3D11_MAPPED_SUBRESOURCE m; if (FAILED(ic->Map(st.ptr(), 0, D3D11_MAP_READ, 0, &m))) return false;
      out.assign((const uint8_t*)m.pData + off, (const uint8_t*)m.pData + off + len); ic->Unmap(st.ptr(), 0); return true; };
    auto hex = [](const std::vector<uint8_t>& v, size_t a, size_t n) { std::string h; for (size_t k = a; k < a + n && k < v.size(); k++) { char t[4]; snprintf(t, sizeof t, "%02x", v[k]); h += t; } return h; };
    auto* il = m_state.ia.inputLayout.ptr();
    if (il) {
      uint32_t na = il->GetAttributeCount(), nb = il->GetBindingCount();
      Logger::info(str::format("NPT_DUMP_A8 ia: input layout attributes=", na, " bindings=", nb));
      for (uint32_t i = 0; i < na; i++) { auto a = il->GetInput(i).attribute();
        Logger::info(str::format("NPT_DUMP_A8 ia:   attr[", i, "] location=", a.location, " binding=", a.binding, " vkFormat=", uint32_t(a.format), " offset=", a.offset)); }
      for (uint32_t i = 0; i < nb; i++) { auto b = il->GetInput(na + i).binding();
        Logger::info(str::format("NPT_DUMP_A8 ia:   binding[", i, "] slot=", b.binding, " extent=", b.extent, " inputRate=", uint32_t(b.inputRate), " divisor=", b.divisor)); }
    } else Logger::info("NPT_DUMP_A8 ia: no input layout");
    for (uint32_t slot = 0; slot < m_state.ia.maxVbCount; slot++) {
      auto& vb = m_state.ia.vertexBuffers[slot]; if (!vb.buffer) continue;
      const bool indexed = m_nptDraw.kind == 1 || m_nptDraw.kind == 3;
      UINT first = (indexed ? 0 : m_nptDraw.start) * vb.stride + vb.offset;
      std::vector<uint8_t> bytes; const UINT want = std::max<UINT>(8u * std::max<UINT>(vb.stride, 4u), 32u);
      Logger::info(str::format("NPT_DUMP_A8 ia:   VB slot=", slot, " stride=", vb.stride, " offset=", vb.offset, " ByteWidth=", vb.buffer->Desc()->ByteWidth, " usage=", uint32_t(vb.buffer->Desc()->Usage)));
      if (readBuf(vb.buffer.ptr(), first, want, bytes) && vb.stride)
        for (uint32_t v = 0; v < 8 && (v + 1) * vb.stride <= bytes.size(); v++)
          Logger::info(str::format("NPT_DUMP_A8 ia:     vtx[", v, "]=", hex(bytes, v * vb.stride, vb.stride)));
      else if (!bytes.empty()) Logger::info(str::format("NPT_DUMP_A8 ia:     raw=", hex(bytes, 0, 64)));
    }
    if (m_state.ia.indexBuffer.buffer && (m_nptDraw.kind == 1 || m_nptDraw.kind == 3)) {
      auto& ib = m_state.ia.indexBuffer; const UINT isz = ib.format == DXGI_FORMAT_R16_UINT ? 2 : 4; std::vector<uint8_t> bytes;
      Logger::info(str::format("NPT_DUMP_A8 ia:   IB format=", uint32_t(ib.format), " offset=", ib.offset, " ByteWidth=", ib.buffer->Desc()->ByteWidth));
      if (readBuf(ib.buffer.ptr(), ib.offset + m_nptDraw.start * isz, 12 * isz, bytes)) {
        std::string idx; for (size_t k = 0; k + isz <= bytes.size(); k += isz) idx += str::format(isz == 2 ? uint32_t(bytes[k] | bytes[k+1] << 8) : uint32_t(bytes[k] | bytes[k+1] << 8 | bytes[k+2] << 16 | bytes[k+3] << 24), " ");
        Logger::info(str::format("NPT_DUMP_A8 ia:     first indices: ", idx));
      }
    }
  }

  // Draw sampling a typed buffer SRV of R8_UINT: log desc + dump the bytes around the SRV range.
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8BufferDraw() {
    auto& d = nptA8();
    if (d.bufDraws.load() >= 60) return;
    auto& srvs = m_state.srv[D3D11ShaderType::ePixel];
    for (uint32_t i = 0; i < srvs.maxCount; i++) {
      if (!srvs.views[i]) continue;
      D3D11_SHADER_RESOURCE_VIEW_DESC1 vd; srvs.views[i]->GetDesc1(&vd);
      if (vd.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && vd.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) continue;
      if (vd.Format != DXGI_FORMAT_R8_UINT && vd.Format != DXGI_FORMAT_R8_UNORM) continue;
      { int seen = d.bufSeen.fetch_add(1); if (!(seen < 12 || seen % 5 == 0)) return; }
      if (d.bufDraws.fetch_add(1) >= 60) return;
      Com<ID3D11Resource> res; srvs.views[i]->GetResource(&res);
      Com<ID3D11Buffer> buf; if (res) res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
      if (!buf) continue;
      D3D11_BUFFER_DESC bd; buf->GetDesc(&bd);
      std::string ps = m_state.ps.ptr() ? m_state.ps->GetCommonShader()->GetName() : "(no PS)";
      const UINT first = vd.Buffer.FirstElement, num = vd.Buffer.NumElements;
      Logger::info(str::format("NPT_DUMP_A8 bufdraw: #", d.bufSeen.load() - 1, " seq=", d.opSeq.fetch_add(1), " PS=", ps, " SRV slot=", i, " view fmt=", uint32_t(vd.Format), " dim=", uint32_t(vd.ViewDimension),
        " FirstElement=", first, " NumElements=", num, " bufferFlags=0x", std::hex, vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX ? uint32_t(vd.BufferEx.Flags) : 0u, std::dec,
        " | buffer ByteWidth=", bd.ByteWidth, " usage=", uint32_t(bd.Usage), " bind=0x", std::hex, bd.BindFlags, " misc=0x", bd.MiscFlags, std::dec,
        " cpu=0x", std::hex, bd.CPUAccessFlags, std::dec));
      NptA8DumpIA();
      NptA8DumpCB();
      if (m_state.om.rtvs[0]) { Com<ID3D11Resource> rres; m_state.om.rtvs[0]->GetResource(&rres); Com<ID3D11Texture2D> rt2; if (rres) rres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt2));
        if (rt2) nptDumpTexture(static_cast<ID3D11DeviceContext*>(this), rt2.ptr(), "bufdraw-rt"); }
      if (m_state.om.rtvs[0]) { D3D11_RENDER_TARGET_VIEW_DESC1 rvd; m_state.om.rtvs[0]->GetDesc1(&rvd);
        Logger::info(str::format("NPT_DUMP_A8 bufdraw:  RT0 view fmt=", uint32_t(rvd.Format)));
        if (m_state.om.cbState.ptr()) { const auto& b = m_state.om.cbState->Desc().RenderTarget[0];
          Logger::info(str::format("NPT_DUMP_A8 bufdraw:  blend RT[0] en=", b.BlendEnable, " src=", uint32_t(b.SrcBlend), " dst=", uint32_t(b.DestBlend), " srcA=", uint32_t(b.SrcBlendAlpha), " dstA=", uint32_t(b.DestBlendAlpha), " mask=0x", std::hex, uint32_t(b.RenderTargetWriteMask), std::dec)); } }
      // read the buffer back through a staging copy
      Com<ID3D11Device> dev; buf->GetDevice(&dev);
      D3D11_BUFFER_DESC sb = bd; sb.Usage = D3D11_USAGE_STAGING; sb.BindFlags = 0; sb.MiscFlags = 0; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sb.StructureByteStride = 0;
      if (bd.ByteWidth > (8u << 20)) continue;
      Com<ID3D11Buffer> st;
      if (FAILED(dev->CreateBuffer(&sb, nullptr, &st))) { Logger::warn("NPT_DUMP_A8: staging buffer create failed"); continue; }
      ID3D11DeviceContext* ic = static_cast<ID3D11DeviceContext*>(this);
      ic->CopyResource(st.ptr(), buf.ptr());
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(ic->Map(st.ptr(), 0, D3D11_MAP_READ, 0, &m))) continue;
      const uint8_t* p = (const uint8_t*)m.pData;
      const UINT lo = first > 64 ? first - 64 : 0, hi = std::min<UINT>(bd.ByteWidth, first + num + 64);
      uint64_t h = 1469598103934665603ull, nz = 0; UINT last = 0; uint32_t mx = 0;
      for (UINT k = first; k < std::min<UINT>(bd.ByteWidth, first + num); k++) { h ^= p[k]; h *= 1099511628211ull; if (p[k]) { nz++; last = k - first; } mx = std::max<uint32_t>(mx, p[k]); }
      std::string hex; for (UINT k = first; k < std::min<UINT>(bd.ByteWidth, first + std::min<UINT>(num, 32)); k++) { char t[4]; snprintf(t, sizeof t, "%02x", p[k]); hex += t; }
      Logger::info(str::format("NPT_DUMP_A8 bufdraw:  srv-range fnv=", std::hex, h, std::dec, " nonzero=", nz, "/", num, " lastNonzeroIdx=", last, " max=", mx, " first32=", hex));
      std::string path = str::format(d.dir, "/", getpid(), "-buf", std::hex, uintptr_t(buf.ptr()) & 0xffffff, std::dec, "-", d.fileSeq.fetch_add(1), ".first", first, "-n", num, ".bin");
      if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(p + lo, 1, hi - lo, f); fclose(f); Logger::info(str::format("NPT_DUMP_A8 bufdraw:  wrote ", path, " (bytes [", lo, ",", hi, ") of buffer; SRV range starts at ", first - lo, " in file)")); }
      ic->Unmap(st.ptr(), 0);

      // all other pixel-shader SRVs (e.g. the t1 expansion LUT): desc, resource, swizzle, history, contents
      for (uint32_t j = 0; j < srvs.maxCount; j++) {
        if (j == i || !srvs.views[j]) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC1 v2; srvs.views[j]->GetDesc1(&v2);
        D3D11_COMMON_RESOURCE_DESC r2 = srvs.views[j]->GetResourceDesc();
        auto fi2 = m_parent->LookupFormat(v2.Format != DXGI_FORMAT_UNKNOWN ? v2.Format : r2.Format, DXGI_VK_FORMAT_MODE_ANY);
        Com<ID3D11Resource> res2; srvs.views[j]->GetResource(&res2);
        std::string line = str::format("NPT_DUMP_A8 bufdraw: other SRV slot=", j, " view fmt=", uint32_t(v2.Format), " dim=", uint32_t(v2.ViewDimension), " resource fmt=", uint32_t(r2.Format),
          " usage=", uint32_t(r2.Usage), " bind=0x", std::hex, r2.BindFlags, " cpu=0x", r2.CPUAccessFlags, " misc=0x", r2.MiscFlags, std::dec,
          " vkfmt=", uint32_t(fi2.Format), " swizzle=", nptSwz(fi2.Swizzle.r), nptSwz(fi2.Swizzle.g), nptSwz(fi2.Swizzle.b), nptSwz(fi2.Swizzle.a));
        Com<ID3D11Texture2D> t2; if (res2) res2->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2));
        if (t2) {
          D3D11_TEXTURE2D_DESC td; t2->GetDesc(&td);
          line += str::format(" | tex ", td.Width, "x", td.Height, " mips=", td.MipLevels, " arr=", td.ArraySize, " | history: ", nptA8History(GetCommonTexture(res2.ptr())));
          Logger::info(line);
          Logger::info(str::format("NPT_DUMP_A8 bufdraw: sampler0=", m_state.samplers[D3D11ShaderType::ePixel].maxCount ? uint32_t(m_state.samplers[D3D11ShaderType::ePixel].samplers[0]->Desc().Filter) : 99u));
          if (td.Width <= 4096 && td.SampleDesc.Count == 1) nptDumpTexture(ic, t2.ptr(), "bufdraw-t");
          // first row hex of the texture (4 bytes/texel or 1 byte/texel only)
          Com<ID3D11Device> dv; t2->GetDevice(&dv);
          D3D11_TEXTURE2D_DESC sd2 = td; sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd2.MiscFlags = 0;
          Com<ID3D11Texture2D> st2;
          if (SUCCEEDED(dv->CreateTexture2D(&sd2, nullptr, &st2))) {
            ic->CopyResource(st2.ptr(), t2.ptr());
            D3D11_MAPPED_SUBRESOURCE m2;
            if (SUCCEEDED(ic->Map(st2.ptr(), 0, D3D11_MAP_READ, 0, &m2))) {
              auto fi3 = lookupFormatInfo(fi2.Format);
              size_t rb = std::min<size_t>(64, size_t(td.Width) * (fi3 ? fi3->elementSize : 4));
              std::string hx; for (size_t k = 0; k < rb; k++) { char t[4]; snprintf(t, sizeof t, "%02x", ((const uint8_t*)m2.pData)[k]); hx += t; }
              uint64_t hh = 1469598103934665603ull; size_t rowb = size_t(td.Width) * (fi3 ? fi3->elementSize : 4);
              for (UINT y = 0; y < td.Height; y++) for (size_t k = 0; k < rowb; k++) { hh ^= ((const uint8_t*)m2.pData)[size_t(y) * m2.RowPitch + k]; hh *= 1099511628211ull; }
              Logger::info(str::format("NPT_DUMP_A8 bufdraw:   tex contents tightFnv=", std::hex, hh, std::dec, " rowPitch=", m2.RowPitch, " row0[first ", rb, "B]=", hx));
              ic->Unmap(st2.ptr(), 0);
            }
          }
        } else Logger::info(line);
      }
    }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::NptA8AfterDraw() {
    auto& d = nptA8();
    if (!d.enabled || GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    NptA8GpuCheck();
    NptA8BufferDraw();
    NptA8AtlasDraw();
    if (d.drawLogs.load() >= 12) return;
    auto& srvs = m_state.srv[D3D11ShaderType::ePixel];
    int slot = -1;
    for (uint32_t i = 0; i < srvs.maxCount; i++) {
      if (!srvs.views[i]) continue;
      D3D11_SHADER_RESOURCE_VIEW_DESC1 vd; srvs.views[i]->GetDesc1(&vd);
      D3D11_COMMON_RESOURCE_DESC rd = srvs.views[i]->GetResourceDesc();
      if (nptIsA8R8(vd.Format) || nptIsA8R8(rd.Format)) { slot = int(i); break; }
    }
    if (slot < 0) return;
    if (d.drawLogs.fetch_add(1) >= 12) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC1 vd; srvs.views[slot]->GetDesc1(&vd);
    D3D11_COMMON_RESOURCE_DESC rd = srvs.views[slot]->GetResourceDesc();
    auto fi = m_parent->LookupFormat(vd.Format != DXGI_FORMAT_UNKNOWN ? vd.Format : rd.Format, DXGI_VK_FORMAT_MODE_ANY);
    std::string ps = m_state.ps.ptr() ? m_state.ps->GetCommonShader()->GetName() : "(no PS)";
    Logger::info(str::format("NPT_DUMP_A8 draw: PS=", ps, " SRV slot=", slot, " view fmt=", uint32_t(vd.Format), " dim=", uint32_t(vd.ViewDimension),
      " resource fmt=", uint32_t(rd.Format), " vkfmt=", uint32_t(fi.Format), " swizzle(r,g,b,a)=",
      nptSwz(fi.Swizzle.r), nptSwz(fi.Swizzle.g), nptSwz(fi.Swizzle.b), nptSwz(fi.Swizzle.a)));
    auto& smp = m_state.samplers[D3D11ShaderType::ePixel];
    if (smp.maxCount && smp.samplers[0]) {
      const auto& sd = smp.samplers[0]->Desc();
      Logger::info(str::format("NPT_DUMP_A8 draw: sampler0 filter=", uint32_t(sd.Filter), " addr=", uint32_t(sd.AddressU), ",", uint32_t(sd.AddressV), ",", uint32_t(sd.AddressW),
        " mipLodBias=", sd.MipLODBias, " minLod=", sd.MinLOD, " maxLod=", sd.MaxLOD, " cmp=", uint32_t(sd.ComparisonFunc)));
    }
    if (m_state.om.cbState.ptr()) {
      const auto& bd = m_state.om.cbState->Desc();
      Logger::info(str::format("NPT_DUMP_A8 draw: blend alphaToCov=", bd.AlphaToCoverageEnable, " independent=", bd.IndependentBlendEnable));
      for (uint32_t r = 0; r < (bd.IndependentBlendEnable ? 8u : 1u); r++) {
        const auto& b = bd.RenderTarget[r];
        Logger::info(str::format("NPT_DUMP_A8 draw:  RT[", r, "] en=", b.BlendEnable, " src=", uint32_t(b.SrcBlend), " dst=", uint32_t(b.DestBlend), " op=", uint32_t(b.BlendOp),
          " srcA=", uint32_t(b.SrcBlendAlpha), " dstA=", uint32_t(b.DestBlendAlpha), " opA=", uint32_t(b.BlendOpAlpha), " mask=0x", std::hex, uint32_t(b.RenderTargetWriteMask), std::dec));
      }
    } else Logger::info("NPT_DUMP_A8 draw: blend state = default (blend disabled)");
    Logger::info(str::format("NPT_DUMP_A8 draw: blendFactor=", m_state.om.blendFactor[0], ",", m_state.om.blendFactor[1], ",", m_state.om.blendFactor[2], ",", m_state.om.blendFactor[3], " sampleMask=0x", std::hex, m_state.om.sampleMask, std::dec));
    if (m_state.om.rtvs[0]) {
      Com<ID3D11Resource> res; m_state.om.rtvs[0]->GetResource(&res);
      D3D11_RENDER_TARGET_VIEW_DESC1 rvd; m_state.om.rtvs[0]->GetDesc1(&rvd);
      Logger::info(str::format("NPT_DUMP_A8 draw: RT0 view fmt=", uint32_t(rvd.Format)));
      Com<ID3D11Texture2D> t; if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
      if (t) nptDumpTexture(static_cast<ID3D11DeviceContext*>(this), t.ptr(), "draw-rt");
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopySubresourceRegion1(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
          UINT                              DstX,
          UINT                              DstY,
          UINT                              DstZ,
          ID3D11Resource*                   pSrcResource,
          UINT                              SrcSubresource,
    const D3D11_BOX*                        pSrcBox,
          UINT                              CopyFlags) {
    CopySubresourceRegionBase(
      pDstResource, DstSubresource, DstX, DstY, DstZ,
      pSrcResource, SrcSubresource, pSrcBox, CopyFlags);
    NptA8AfterCopy(pDstResource, DstSubresource);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopySubresourceRegionBase(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
          UINT                              DstX,
          UINT                              DstY,
          UINT                              DstZ,
          ID3D11Resource*                   pSrcResource,
          UINT                              SrcSubresource,
    const D3D11_BOX*                        pSrcBox,
          UINT                              CopyFlags) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);

    if (!pDstResource || !pSrcResource)
      return;

    NptMarkSharedRes(pDstResource);

    if (pSrcBox
     && (pSrcBox->left  >= pSrcBox->right
      || pSrcBox->top   >= pSrcBox->bottom
      || pSrcBox->front >= pSrcBox->back))
      return;

    D3D11_RESOURCE_DIMENSION dstResourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_RESOURCE_DIMENSION srcResourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;

    pDstResource->GetType(&dstResourceDim);
    pSrcResource->GetType(&srcResourceDim);

    if (dstResourceDim == D3D11_RESOURCE_DIMENSION_BUFFER && srcResourceDim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      auto dstBuffer = static_cast<D3D11Buffer*>(pDstResource);
      auto srcBuffer = static_cast<D3D11Buffer*>(pSrcResource);

      VkDeviceSize dstOffset = DstX;
      VkDeviceSize srcOffset = 0;
      VkDeviceSize byteCount = -1;

      if (pSrcBox) {
        srcOffset = pSrcBox->left;
        byteCount = pSrcBox->right - pSrcBox->left;
      }

      CopyBuffer(dstBuffer, dstOffset, srcBuffer, srcOffset, byteCount);
    } else if (dstResourceDim != D3D11_RESOURCE_DIMENSION_BUFFER && srcResourceDim != D3D11_RESOURCE_DIMENSION_BUFFER) {
      auto dstTexture = GetCommonTexture(pDstResource);
      auto srcTexture = GetCommonTexture(pSrcResource);

      if (unlikely(nptA8Enabled()))
        nptA8Note(dstTexture, str::format("CopySubresourceRegion(dst ", DstX, ",", DstY, " src fmt=", uint32_t(srcTexture->Desc()->Format), " ", srcTexture->Desc()->Width, "x", srcTexture->Desc()->Height, pSrcBox ? " box" : " full", ")"));

      if (DstSubresource >= dstTexture->CountSubresources()
       || SrcSubresource >= srcTexture->CountSubresources())
        return;

      auto dstFormatInfo = lookupFormatInfo(dstTexture->GetPackedFormat());
      auto srcFormatInfo = lookupFormatInfo(srcTexture->GetPackedFormat());

      auto dstLayers = vk::makeSubresourceLayers(dstTexture->GetSubresourceFromIndex(dstFormatInfo->aspectMask, DstSubresource));
      auto srcLayers = vk::makeSubresourceLayers(srcTexture->GetSubresourceFromIndex(srcFormatInfo->aspectMask, SrcSubresource));

      VkOffset3D srcOffset = { 0, 0, 0 };
      VkOffset3D dstOffset = { int32_t(DstX), int32_t(DstY), int32_t(DstZ) };

      VkExtent3D srcExtent = srcTexture->MipLevelExtent(srcLayers.mipLevel);

      if (pSrcBox) {
        srcOffset.x = pSrcBox->left;
        srcOffset.y = pSrcBox->top;
        srcOffset.z = pSrcBox->front;

        srcExtent.width  = pSrcBox->right -  pSrcBox->left;
        srcExtent.height = pSrcBox->bottom - pSrcBox->top;
        srcExtent.depth  = pSrcBox->back -   pSrcBox->front;
      }

      CopyImage(
        dstTexture, &dstLayers, dstOffset,
        srcTexture, &srcLayers, srcOffset,
        srcExtent);
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopyResource(
          ID3D11Resource*                   pDstResource,
          ID3D11Resource*                   pSrcResource) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);

    if (!pDstResource || !pSrcResource || (pDstResource == pSrcResource))
      return;

    NptMarkSharedRes(pDstResource);

    D3D11_RESOURCE_DIMENSION dstResourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_RESOURCE_DIMENSION srcResourceDim = D3D11_RESOURCE_DIMENSION_UNKNOWN;

    pDstResource->GetType(&dstResourceDim);
    pSrcResource->GetType(&srcResourceDim);

    if (dstResourceDim != srcResourceDim)
      return;

    if (unlikely(nptA8Enabled()) && dstResourceDim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      nptA8Note(GetCommonTexture(pDstResource), str::format("CopyResource(src fmt=", uint32_t(GetCommonTexture(pSrcResource)->Desc()->Format), ")"));

    if (dstResourceDim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      auto dstBuffer = static_cast<D3D11Buffer*>(pDstResource);
      auto srcBuffer = static_cast<D3D11Buffer*>(pSrcResource);

      if (dstBuffer->Desc()->ByteWidth != srcBuffer->Desc()->ByteWidth)
        return;

      CopyBuffer(dstBuffer, 0, srcBuffer, 0, -1);
    } else {
      auto dstTexture = GetCommonTexture(pDstResource);
      auto srcTexture = GetCommonTexture(pSrcResource);

      auto dstDesc = dstTexture->Desc();
      auto srcDesc = srcTexture->Desc();

      // The subresource count must match as well
      if (dstDesc->ArraySize != srcDesc->ArraySize
       || dstDesc->MipLevels != srcDesc->MipLevels)
        return;

      auto dstFormatInfo = lookupFormatInfo(dstTexture->GetPackedFormat());
      auto srcFormatInfo = lookupFormatInfo(srcTexture->GetPackedFormat());

      for (uint32_t i = 0; i < dstDesc->MipLevels; i++) {
        VkImageSubresourceLayers dstLayers = { dstFormatInfo->aspectMask, i, 0, dstDesc->ArraySize };
        VkImageSubresourceLayers srcLayers = { srcFormatInfo->aspectMask, i, 0, srcDesc->ArraySize };

        CopyImage(
          dstTexture, &dstLayers, VkOffset3D(),
          srcTexture, &srcLayers, VkOffset3D(),
          srcTexture->MipLevelExtent(i));
      }
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopyStructureCount(
          ID3D11Buffer*                     pDstBuffer,
          UINT                              DstAlignedByteOffset,
          ID3D11UnorderedAccessView*        pSrcView) {
    D3D10DeviceLock lock = LockContext();

    auto buf = static_cast<D3D11Buffer*>(pDstBuffer);
    auto uav = static_cast<D3D11UnorderedAccessView*>(pSrcView);

    if (!buf || !uav)
      return;

    auto counterView = uav->GetCounterView();

    if (counterView == nullptr)
      return;

    AddCost(GpuCostEstimate::Transfer);

    EmitCs([
      cDstSlice = buf->GetBufferSlice(DstAlignedByteOffset),
      cSrcSlice = DxvkBufferSlice(counterView)
    ] (DxvkContext* ctx) {
      ctx->copyBuffer(
        cDstSlice.buffer(),
        cDstSlice.offset(),
        cSrcSlice.buffer(),
        cSrcSlice.offset(),
        sizeof(uint32_t));
    });

    if (buf->HasSequenceNumber())
      GetTypedContext()->TrackBufferSequenceNumber(buf);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ClearRenderTargetView(
          ID3D11RenderTargetView*           pRenderTargetView,
    const FLOAT                             ColorRGBA[4]) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);
    if (pRenderTargetView && nptSharedSyncEnabled() && NptViewShared(static_cast<D3D11RenderTargetView*>(pRenderTargetView)->GetImageView())) { m_nptSharedDirty = true; Com<ID3D11Resource> r; pRenderTargetView->GetResource(&r); NptTouch(r.ptr()); }

    auto rtv = static_cast<D3D11RenderTargetView*>(pRenderTargetView);

    if (!rtv)
      return;

    AddCost(GpuCostEstimate::Transfer);

    auto view  = rtv->GetImageView();
    auto color = ConvertColorValue(ColorRGBA, view->formatInfo());

    EmitCs([
      cClearValue = color,
      cImageView  = std::move(view)
    ] (DxvkContext* ctx) {
      ctx->clearRenderTarget(cImageView,
        VK_IMAGE_ASPECT_COLOR_BIT,
        cClearValue, 0u);
    });
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ClearUnorderedAccessViewUint(
          ID3D11UnorderedAccessView*        pUnorderedAccessView,
    const UINT                              Values[4]) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);
    if (pUnorderedAccessView && nptSharedSyncEnabled() && NptViewShared(static_cast<D3D11UnorderedAccessView*>(pUnorderedAccessView)->GetImageView())) { m_nptSharedDirty = true; Com<ID3D11Resource> r; pUnorderedAccessView->GetResource(&r); NptTouch(r.ptr()); }

    if (!pUnorderedAccessView)
      return;

    Com<ID3D11UnorderedAccessView> qiUav;

    if (FAILED(pUnorderedAccessView->QueryInterface(IID_PPV_ARGS(&qiUav))))
      return;

    AddCost(GpuCostEstimate::Transfer);

    auto uav = static_cast<D3D11UnorderedAccessView*>(qiUav.ptr());

    // Gather UAV format info. We'll use this to determine
    // whether we need to create a temporary view or not.
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    uav->GetDesc(&uavDesc);

    VkFormat uavFormat = m_parent->LookupFormat(uavDesc.Format, DXGI_VK_FORMAT_MODE_ANY).Format;
    VkFormat rawFormat = m_parent->LookupFormat(uavDesc.Format, DXGI_VK_FORMAT_MODE_RAW).Format;

    if (uavDesc.Format == DXGI_FORMAT_A8_UNORM)
      rawFormat = uavFormat;

    if (uavFormat && !rawFormat) {
      Logger::err(str::format("D3D11: ClearUnorderedAccessViewUint: No raw format found for ", uavFormat));
      return;
    }

    VkClearValue clearValue = { };

    if (uavDesc.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
      // R11G11B10 is a special case since there's no corresponding
      // integer format with the same bit layout. Use R32 instead.
      clearValue.color.uint32[0] = ((Values[0] & 0x7FF) <<  0)
                                 | ((Values[1] & 0x7FF) << 11)
                                 | ((Values[2] & 0x3FF) << 22);
      clearValue.color.uint32[1] = 0;
      clearValue.color.uint32[2] = 0;
      clearValue.color.uint32[3] = 0;
    } else if (uavDesc.Format == DXGI_FORMAT_A8_UNORM) {
      // Use the unorm format itself to execute the clear, regardless
      // of whether we use A8 or emulate the format with R8. This is
      // necessary because we cannot create R8_UINT views for A8.
      float a = float(Values[3] & 0xff) / 255.0f;

      clearValue.color.float32[0] = a;
      clearValue.color.float32[1] = a;
      clearValue.color.float32[2] = a;
      clearValue.color.float32[3] = a;
    } else {
      clearValue.color.uint32[0] = Values[0];
      clearValue.color.uint32[1] = Values[1];
      clearValue.color.uint32[2] = Values[2];
      clearValue.color.uint32[3] = Values[3];
    }

    if (uav->GetResourceType() == D3D11_RESOURCE_DIMENSION_BUFFER) {
      // In case of raw and structured buffers as well as typed
      // buffers that can be used for atomic operations, we can
      // use the fast Vulkan buffer clear function.
      Rc<DxvkBufferView> bufferView = uav->GetBufferView();

      if (bufferView->info().format == VK_FORMAT_R32_UINT
       || bufferView->info().format == VK_FORMAT_R32_SINT
       || bufferView->info().format == VK_FORMAT_R32_SFLOAT
       || bufferView->info().format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        EmitCs([
          cClearValue = clearValue.color.uint32[0],
          cDstSlice   = DxvkBufferSlice(bufferView)
        ] (DxvkContext* ctx) {
          ctx->clearBuffer(
            cDstSlice.buffer(),
            cDstSlice.offset(),
            cDstSlice.length(),
            cClearValue);
        });
      } else {
        // Create a view with an integer format if necessary
        if (uavFormat != rawFormat)  {
          DxvkBufferViewKey info = bufferView->info();
          info.format = rawFormat;

          bufferView = bufferView->buffer()->createView(info);
        }

        EmitCs([
          cClearValue = clearValue,
          cDstView    = bufferView
        ] (DxvkContext* ctx) {
          ctx->clearBufferView(
            cDstView, 0,
            cDstView->elementCount(),
            cClearValue.color);
        });
      }
    } else {
      Rc<DxvkImageView> imageView = uav->GetImageView();

      // If the clear value is zero, we can use the original view regardless of
      // the format since the bit pattern will not change in any supported format.
      bool isZeroClearValue = !(clearValue.color.uint32[0] | clearValue.color.uint32[1]
                              | clearValue.color.uint32[2] | clearValue.color.uint32[3]);

      EmitCs([
        cClearValue = clearValue,
        cDstView    = imageView,
        cDstFormat  = isZeroClearValue ? uavFormat : rawFormat
      ] (DxvkContext* ctx) {
        // Ensure that we can write to the image with the given format
        DxvkImageUsageInfo imageUsage = { };
        imageUsage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        imageUsage.viewFormatCount = 1;
        imageUsage.viewFormats = &cDstFormat;

        ctx->ensureImageCompatibility(cDstView->image(), imageUsage);

        // If necessary, recreate the view
        Rc<DxvkImageView> view = cDstView;

        if (view->info().format != cDstFormat) {
          DxvkImageViewKey key = cDstView->info();
          key.format = cDstFormat;

          view = cDstView->image()->createView(key);
        }

        ctx->clearImageView(view,
          VkOffset3D { 0, 0, 0 },
          cDstView->mipLevelExtent(0),
          VK_IMAGE_ASPECT_COLOR_BIT,
          cClearValue);
      });
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ClearUnorderedAccessViewFloat(
          ID3D11UnorderedAccessView*        pUnorderedAccessView,
    const FLOAT                             Values[4]) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);
    if (pUnorderedAccessView && nptSharedSyncEnabled() && NptViewShared(static_cast<D3D11UnorderedAccessView*>(pUnorderedAccessView)->GetImageView())) { m_nptSharedDirty = true; Com<ID3D11Resource> r; pUnorderedAccessView->GetResource(&r); NptTouch(r.ptr()); }

    auto uav = static_cast<D3D11UnorderedAccessView*>(pUnorderedAccessView);

    if (!uav)
      return;

    auto imgView = uav->GetImageView();
    auto bufView = uav->GetBufferView();

    const DxvkFormatInfo* info = nullptr;
    if (imgView != nullptr) info = imgView->formatInfo();
    if (bufView != nullptr) info = bufView->formatInfo();

    if (!info || info->flags.any(DxvkFormatFlag::SampledSInt, DxvkFormatFlag::SampledUInt))
      return;

    AddCost(GpuCostEstimate::Transfer);

    VkClearValue clearValue;
    clearValue.color.float32[0] = Values[0];
    clearValue.color.float32[1] = Values[1];
    clearValue.color.float32[2] = Values[2];
    clearValue.color.float32[3] = Values[3];

    if (uav->GetResourceType() == D3D11_RESOURCE_DIMENSION_BUFFER) {
      EmitCs([
        cClearValue = clearValue,
        cDstView    = std::move(bufView)
      ] (DxvkContext* ctx) {
        ctx->clearBufferView(
          cDstView, 0,
          cDstView->elementCount(),
          cClearValue.color);
      });
    } else {
      EmitCs([
        cClearValue = clearValue,
        cDstView    = std::move(imgView)
      ] (DxvkContext* ctx) {
        ctx->clearImageView(cDstView,
          VkOffset3D { 0, 0, 0 },
          cDstView->mipLevelExtent(0),
          VK_IMAGE_ASPECT_COLOR_BIT,
          cClearValue);
      });
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ClearDepthStencilView(
          ID3D11DepthStencilView*           pDepthStencilView,
          UINT                              ClearFlags,
          FLOAT                             Depth,
          UINT8                             Stencil) {
    D3D10DeviceLock lock = LockContext();

    auto dsv = static_cast<D3D11DepthStencilView*>(pDepthStencilView);

    if (!dsv)
      return;

    // Figure out which aspects to clear based on
    // the image view properties and clear flags.
    VkImageAspectFlags aspectMask = 0;

    if (ClearFlags & D3D11_CLEAR_DEPTH)
      aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;

    if (ClearFlags & D3D11_CLEAR_STENCIL)
      aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

    aspectMask &= dsv->GetWritableAspectMask();

    if (!aspectMask)
      return;

    AddCost(GpuCostEstimate::Transfer);

    VkClearValue clearValue;
    clearValue.depthStencil.depth   = Depth;
    clearValue.depthStencil.stencil = Stencil;

    EmitCs([
      cClearValue = clearValue,
      cAspectMask = aspectMask,
      cImageView  = dsv->GetImageView()
    ] (DxvkContext* ctx) {
      ctx->clearRenderTarget(cImageView,
        cAspectMask, cClearValue, 0u);
    });
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ClearView(
          ID3D11View*                       pView,
    const FLOAT                             Color[4],
    const D3D11_RECT*                       pRect,
          UINT                              NumRects) {
    D3D10DeviceLock lock = LockContext();

    if (NumRects && !pRect)
      return;

    AddCost(GpuCostEstimate::Transfer);

    // ID3D11View has no methods to query the exact type of
    // the view, so we'll have to check each possible class
    auto dsv = dynamic_cast<D3D11DepthStencilView*>(pView);
    auto rtv = dynamic_cast<D3D11RenderTargetView*>(pView);
    auto uav = dynamic_cast<D3D11UnorderedAccessView*>(pView);
    auto vov = dynamic_cast<D3D11VideoProcessorOutputView*>(pView);

    // Retrieve underlying resource view
    if (dsv) {
      Rc<DxvkImageView> imgView = dsv->GetImageView();

      if (imgView)
        ClearImageView(std::move(imgView), Color, pRect, NumRects);
    } else if (rtv) {
      Rc<DxvkImageView> imgView = rtv->GetImageView();

      if (imgView)
        ClearImageView(std::move(imgView), Color, pRect, NumRects);
    } else if (uav) {
      Rc<DxvkImageView> imgView = uav->GetImageView();
      Rc<DxvkBufferView> bufView = uav->GetBufferView();

      if (imgView)
        ClearImageView(std::move(imgView), Color, pRect, NumRects);

      if (bufView)
        ClearBufferView(std::move(bufView), Color, pRect, NumRects);
    } else if (vov) {
      auto views = vov->GetCommon().GetViews();
      auto shadow = vov->GetCommon().GetShadow();

      // If we have to assume that the image is only partially cleared,
      // make sure to properly sync it with the shadow image.
      VkImageSubresourceLayers imageLayers = vov->GetCommon().GetImageSubresource();

      VkImageSubresourceLayers shadowLayers = { };
      shadowLayers.aspectMask = imageLayers.aspectMask;
      shadowLayers.layerCount = imageLayers.layerCount;

      if (shadow && NumRects)
        SyncImage(shadow, shadowLayers, vov->GetCommon().GetImage(), imageLayers);

      // Assume that planar video formats use Y - Cb - Cr
      // order for the purpose of mapping color components.
      uint32_t component = 0u;
      FLOAT planeColor[4] = { };

      for (uint32_t i = 0u; i < views.size(); i++) {
        if (!views[i])
          break;

        // Extract relevant color components from the clear color
        // and shift the input array accordingly.
        uint32_t n = bit::popcnt(views[i]->formatInfo()->componentMask);

        for (uint32_t c = 0u; c < 4u; c++)
          planeColor[c] = c < n && component + c < 4u ? Color[c] : 0.0f;

        component += n;

        // Perform the actual clear. Rects will be adjusted by called method.
        ClearImageView(std::move(views[i]), planeColor, pRect, NumRects);
      }

      // If the video view has a shadow image, copy it back to the base image
      if (shadow)
        SyncImage(vov->GetCommon().GetImage(), imageLayers, shadow, shadowLayers);
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) {
    D3D10DeviceLock lock = LockContext();

    auto view = static_cast<D3D11ShaderResourceView*>(pShaderResourceView);

    if (!view || view->GetResourceType() == D3D11_RESOURCE_DIMENSION_BUFFER)
      return;

    D3D11_COMMON_RESOURCE_DESC resourceDesc = view->GetResourceDesc();

    if (!(resourceDesc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS))
      return;

    AddCost(GpuCostEstimate::Transfer);

    EmitCs([cDstImageView = view->GetImageView()]
    (DxvkContext* ctx) {
      ctx->generateMipmaps(cDstImageView, VK_FILTER_LINEAR);
    });
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ResolveSubresource(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
          ID3D11Resource*                   pSrcResource,
          UINT                              SrcSubresource,
          DXGI_FORMAT                       Format) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);
    NptMarkSharedRes(pDstResource);

    bool isSameSubresource = pDstResource   == pSrcResource
                          && DstSubresource == SrcSubresource;

    if (!pDstResource || !pSrcResource || isSameSubresource)
      return;

    D3D11_RESOURCE_DIMENSION dstResourceType;
    D3D11_RESOURCE_DIMENSION srcResourceType;

    pDstResource->GetType(&dstResourceType);
    pSrcResource->GetType(&srcResourceType);

    if (dstResourceType != D3D11_RESOURCE_DIMENSION_TEXTURE2D
     || srcResourceType != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;

    auto dstTexture = static_cast<D3D11Texture2D*>(pDstResource);
    auto srcTexture = static_cast<D3D11Texture2D*>(pSrcResource);

    D3D11_TEXTURE2D_DESC dstDesc;
    D3D11_TEXTURE2D_DESC srcDesc;

    dstTexture->GetDesc(&dstDesc);
    srcTexture->GetDesc(&srcDesc);

    if (dstDesc.SampleDesc.Count != 1)
      return;

    D3D11CommonTexture* dstTextureInfo = GetCommonTexture(pDstResource);
    D3D11CommonTexture* srcTextureInfo = GetCommonTexture(pSrcResource);

    const DXGI_VK_FORMAT_INFO dstFormatInfo = m_parent->LookupFormat(dstDesc.Format, DXGI_VK_FORMAT_MODE_ANY);
    const DXGI_VK_FORMAT_INFO srcFormatInfo = m_parent->LookupFormat(srcDesc.Format, DXGI_VK_FORMAT_MODE_ANY);

    auto dstVulkanFormatInfo = lookupFormatInfo(dstFormatInfo.Format);
    auto srcVulkanFormatInfo = lookupFormatInfo(srcFormatInfo.Format);

    if (DstSubresource >= dstTextureInfo->CountSubresources()
     || SrcSubresource >= srcTextureInfo->CountSubresources())
      return;

    AddCost(GpuCostEstimate::Transfer);

    const VkImageSubresource dstSubresource =
      dstTextureInfo->GetSubresourceFromIndex(
        dstVulkanFormatInfo->aspectMask, DstSubresource);

    const VkImageSubresource srcSubresource =
      srcTextureInfo->GetSubresourceFromIndex(
        srcVulkanFormatInfo->aspectMask, SrcSubresource);

    const VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };

    const VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };

    if (srcDesc.SampleDesc.Count == 1 || m_parent->GetOptions()->disableMsaa) {
      EmitCs([
        cDstImage  = dstTextureInfo->GetImage(),
        cSrcImage  = srcTextureInfo->GetImage(),
        cDstLayers = dstSubresourceLayers,
        cSrcLayers = srcSubresourceLayers
      ] (DxvkContext* ctx) {
        ctx->copyImage(
          cDstImage, cDstLayers, VkOffset3D { 0, 0, 0 },
          cSrcImage, cSrcLayers, VkOffset3D { 0, 0, 0 },
          cDstImage->mipLevelExtent(cDstLayers.mipLevel));
      });
    } else {
      VkFormat format = m_parent->LookupFormat(
        Format, DXGI_VK_FORMAT_MODE_ANY).Format;

      EmitCs([
        cDstImage  = dstTextureInfo->GetImage(),
        cSrcImage  = srcTextureInfo->GetImage(),
        cDstSubres = dstSubresourceLayers,
        cSrcSubres = srcSubresourceLayers,
        cFormat    = format
      ] (DxvkContext* ctx) {
        VkFormat format = cFormat ? cFormat : cSrcImage->info().format;

        VkImageResolve region;
        region.srcSubresource = cSrcSubres;
        region.srcOffset      = VkOffset3D { 0, 0, 0 };
        region.dstSubresource = cDstSubres;
        region.dstOffset      = VkOffset3D { 0, 0, 0 };
        region.extent         = cDstImage->mipLevelExtent(cDstSubres.mipLevel);

        ctx->resolveImage(cDstImage, cSrcImage, region, format,
          getDefaultResolveMode(format), VK_RESOLVE_MODE_NONE);
      });
    }

    if constexpr (!IsDeferred)
      GetTypedContext()->NotifyResolve();

    if (dstTextureInfo->HasSequenceNumber())
      GetTypedContext()->TrackTextureSequenceNumber(dstTextureInfo, DstSubresource);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::UpdateSubresource(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
    const D3D11_BOX*                        pDstBox,
    const void*                             pSrcData,
          UINT                              SrcRowPitch,
          UINT                              SrcDepthPitch) {
    if (IsDeferred && unlikely(pDstBox != nullptr) && unlikely(!m_parent->GetOptions()->exposeDriverCommandLists)) {
      // If called from a deferred context and native command list support is not
      // exposed, we need to apply the destination box to the source pointer. This
      // only applies to UpdateSubresource, not to UpdateSubresource1. See MSDN:
      // https://msdn.microsoft.com/en-us/library/windows/desktop/ff476486(v=vs.85).aspx)
      size_t srcOffset = pDstBox->left;

      // For textures, the offset logic needs to take the format into account.
      // Ignore that multi-planar images exist, this is hairy enough already.
      D3D11CommonTexture* dstTexture = GetCommonTexture(pDstResource);

      if (dstTexture) {
        auto dstFormat = dstTexture->GetPackedFormat();
        auto dstFormatInfo = lookupFormatInfo(dstFormat);

        size_t blockSize = dstFormatInfo->elementSize;

        VkOffset3D offset;
        offset.x = pDstBox->left / dstFormatInfo->blockSize.width;
        offset.y = pDstBox->top / dstFormatInfo->blockSize.height;
        offset.z = pDstBox->front / dstFormatInfo->blockSize.depth;

        srcOffset = offset.x * blockSize + offset.y * SrcRowPitch + offset.z * SrcDepthPitch;
      }

      pSrcData = reinterpret_cast<const char*>(pSrcData) + srcOffset;
    }

    UpdateResource(pDstResource, DstSubresource, pDstBox,
      pSrcData, SrcRowPitch, SrcDepthPitch, 0);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::UpdateSubresource1(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
    const D3D11_BOX*                        pDstBox,
    const void*                             pSrcData,
          UINT                              SrcRowPitch,
          UINT                              SrcDepthPitch,
          UINT                              CopyFlags) {
    UpdateResource(pDstResource, DstSubresource, pDstBox,
      pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawAuto() {
    D3D10DeviceLock lock = LockContext();

    D3D11Buffer* buffer = m_state.ia.vertexBuffers[0].buffer.ptr();

    if (!buffer)
      return;

    DxvkBufferSlice vtxBuf = buffer->GetBufferSlice();
    DxvkBufferSlice ctrBuf = buffer->GetSOCounter();

    if (!ctrBuf.defined())
      return;

    if (unlikely(HasDirtyGraphicsBindings()))
      ApplyDirtyGraphicsBindings();

    // We bind the SO counter as an indirect count buffer,
    // so reset any tracking we may have been doing here.
    m_state.id.reset();

    EmitCs([=] (DxvkContext* ctx) mutable {
      ctx->bindDrawBuffers(DxvkBufferSlice(),
        Forwarder::move(ctrBuf));

      ctx->drawIndirectXfb(0u,
        vtxBuf.buffer()->getXfbVertexStride(),
        vtxBuf.offset());

      // Reset draw buffer right away so we don't
      // keep the SO counter alive indefinitely
      ctx->bindDrawBuffers(DxvkBufferSlice(),
        DxvkBufferSlice());
    });
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::Draw(
          UINT            VertexCount,
          UINT            StartVertexLocation) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this, true);

    if (unlikely(!VertexCount))
      return;

    VkDrawIndirectCommand draw = { };
    draw.vertexCount   = VertexCount;
    draw.instanceCount = 1u;
    draw.firstVertex   = StartVertexLocation;
    draw.firstInstance = 0u;

    if (unlikely(nptA8Enabled())) m_nptDraw = { 0u, VertexCount, 1u, StartVertexLocation, 0 };
    if (unlikely(nptA8Enabled())) NptA8BeforeDraw(0u, VertexCount, 1u, StartVertexLocation, 0);
    BatchDraw(draw);
    NptA8AfterDraw();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawIndexed(
          UINT            IndexCount,
          UINT            StartIndexLocation,
          INT             BaseVertexLocation) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this, true);

    if (unlikely(!IndexCount))
      return;

    VkDrawIndexedIndirectCommand draw = { };
    draw.indexCount    = IndexCount;
    draw.instanceCount = 1u;
    draw.firstIndex    = StartIndexLocation;
    draw.vertexOffset  = BaseVertexLocation;
    draw.firstInstance = 0u;

    if (unlikely(nptA8Enabled())) m_nptDraw = { 1u, IndexCount, 1u, StartIndexLocation, BaseVertexLocation };
    if (unlikely(nptA8Enabled())) NptA8BeforeDraw(1u, IndexCount, 1u, StartIndexLocation, BaseVertexLocation);
    BatchDrawIndexed(draw);
    NptA8AfterDraw();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawInstanced(
          UINT            VertexCountPerInstance,
          UINT            InstanceCount,
          UINT            StartVertexLocation,
          UINT            StartInstanceLocation) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this, true);

    if (unlikely(!VertexCountPerInstance || !InstanceCount))
      return;

    VkDrawIndirectCommand draw = { };
    draw.vertexCount   = VertexCountPerInstance;
    draw.instanceCount = InstanceCount;
    draw.firstVertex   = StartVertexLocation;
    draw.firstInstance = StartInstanceLocation;

    if (unlikely(nptA8Enabled())) m_nptDraw = { 2u, VertexCountPerInstance, InstanceCount, StartVertexLocation, 0 };
    if (unlikely(nptA8Enabled())) NptA8BeforeDraw(2u, VertexCountPerInstance, InstanceCount, StartVertexLocation, 0);
    BatchDraw(draw);
    NptA8AfterDraw();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawIndexedInstanced(
          UINT            IndexCountPerInstance,
          UINT            InstanceCount,
          UINT            StartIndexLocation,
          INT             BaseVertexLocation,
          UINT            StartInstanceLocation) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this, true);

    if (unlikely(!IndexCountPerInstance || !InstanceCount))
      return;

    VkDrawIndexedIndirectCommand draw = { };
    draw.indexCount    = IndexCountPerInstance;
    draw.instanceCount = InstanceCount;
    draw.firstIndex    = StartIndexLocation;
    draw.vertexOffset  = BaseVertexLocation;
    draw.firstInstance = StartInstanceLocation;

    if (unlikely(nptA8Enabled())) m_nptDraw = { 3u, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation };
    if (unlikely(nptA8Enabled())) NptA8BeforeDraw(3u, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation);
    BatchDrawIndexed(draw);
    NptA8AfterDraw();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawIndexedInstancedIndirect(
          ID3D11Buffer*   pBufferForArgs,
          UINT            AlignedByteOffsetForArgs) {
    D3D10DeviceLock lock = LockContext();
    SetDrawBuffers(pBufferForArgs, nullptr);

    if (!ValidateDrawBufferSize(pBufferForArgs, AlignedByteOffsetForArgs, sizeof(VkDrawIndexedIndirectCommand)))
      return;

    if (unlikely(HasDirtyGraphicsBindings()))
      ApplyDirtyGraphicsBindings();

    // If possible, batch multiple indirect draw calls into one single multidraw call
    if (m_csDataType == D3D11CmdType::DrawIndirectIndexed) {
      auto cmdData = static_cast<D3D11CmdDrawIndirectData*>(m_csData->first());
      auto stride = GetIndirectCommandStride(cmdData, AlignedByteOffsetForArgs, sizeof(VkDrawIndexedIndirectCommand));

      if (stride) {
        cmdData->count += 1;
        cmdData->stride = stride;
        return;
      }
    }

    // Need to start a new draw sequence
    EmitCsCmd<D3D11CmdDrawIndirectData>(D3D11CmdType::DrawIndirectIndexed, 1u,
      [] (DxvkContext* ctx, const D3D11CmdDrawIndirectData* data, size_t) {
        ctx->drawIndexedIndirect(data->offset, data->count, data->stride, true);
      });

    auto cmdData = new (m_csData->first()) D3D11CmdDrawIndirectData();
    cmdData->offset = AlignedByteOffsetForArgs;
    cmdData->count  = 1;
    cmdData->stride = 0;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawInstancedIndirect(
          ID3D11Buffer*   pBufferForArgs,
          UINT            AlignedByteOffsetForArgs) {
    D3D10DeviceLock lock = LockContext();
    SetDrawBuffers(pBufferForArgs, nullptr);

    if (!ValidateDrawBufferSize(pBufferForArgs, AlignedByteOffsetForArgs, sizeof(VkDrawIndirectCommand)))
      return;

    if (unlikely(HasDirtyGraphicsBindings()))
      ApplyDirtyGraphicsBindings();

    // If possible, batch multiple indirect draw calls into one single multidraw call
    if (m_csDataType == D3D11CmdType::DrawIndirect) {
      auto cmdData = static_cast<D3D11CmdDrawIndirectData*>(m_csData->first());
      auto stride = GetIndirectCommandStride(cmdData, AlignedByteOffsetForArgs, sizeof(VkDrawIndirectCommand));

      if (stride) {
        cmdData->count += 1;
        cmdData->stride = stride;
        return;
      }
    }

    // Need to start a new draw sequence
    EmitCsCmd<D3D11CmdDrawIndirectData>(D3D11CmdType::DrawIndirect, 1u,
      [] (DxvkContext* ctx, const D3D11CmdDrawIndirectData* data, size_t) {
        ctx->drawIndirect(data->offset, data->count, data->stride, true);
      });

    auto cmdData = new (m_csData->first()) D3D11CmdDrawIndirectData();
    cmdData->offset = AlignedByteOffsetForArgs;
    cmdData->count  = 1;
    cmdData->stride = 0;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::Dispatch(
          UINT            ThreadGroupCountX,
          UINT            ThreadGroupCountY,
          UINT            ThreadGroupCountZ) {
    D3D10DeviceLock lock = LockContext();
    NptDrawSyncGuard nptSync(this);

    if (unlikely(!ThreadGroupCountX || !ThreadGroupCountY || !ThreadGroupCountZ))
      return;

    AddCost(GpuCostEstimate::Dispatch);

    if (unlikely(HasDirtyComputeBindings()))
      ApplyDirtyComputeBindings();

    EmitCs([=] (DxvkContext* ctx) {
      ctx->dispatch(
        ThreadGroupCountX,
        ThreadGroupCountY,
        ThreadGroupCountZ);
    });
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DispatchIndirect(
          ID3D11Buffer*   pBufferForArgs,
          UINT            AlignedByteOffsetForArgs) {
    D3D10DeviceLock lock = LockContext();
    SetDrawBuffers(pBufferForArgs, nullptr);

    if (!ValidateDrawBufferSize(pBufferForArgs, AlignedByteOffsetForArgs, sizeof(VkDispatchIndirectCommand)))
      return;

    AddCost(GpuCostEstimate::DispatchIndirect);

    if (unlikely(HasDirtyComputeBindings()))
      ApplyDirtyComputeBindings();

    EmitCs([cOffset = AlignedByteOffsetForArgs]
    (DxvkContext* ctx) {
      ctx->dispatchIndirect(cOffset);
    });
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IASetInputLayout(ID3D11InputLayout* pInputLayout) {
    D3D10DeviceLock lock = LockContext();

    auto inputLayout = static_cast<D3D11InputLayout*>(pInputLayout);

    if (m_state.ia.inputLayout != inputLayout) {
      bool equal = false;

      // Some games (e.g. Grim Dawn) create lots and lots of
      // identical input layouts, so we'll only apply the state
      // if the input layouts has actually changed between calls.
      if (m_state.ia.inputLayout != nullptr && inputLayout != nullptr)
        equal = m_state.ia.inputLayout->Compare(inputLayout);

      m_state.ia.inputLayout = inputLayout;

      if (!equal)
        ApplyInputLayout();
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) {
    D3D10DeviceLock lock = LockContext();

    if (m_state.ia.primitiveTopology != Topology) {
      m_state.ia.primitiveTopology = Topology;
      ApplyPrimitiveTopology();
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IASetVertexBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppVertexBuffers,
    const UINT*                             pStrides,
    const UINT*                             pOffsets) {
    D3D10DeviceLock lock = LockContext();

    for (uint32_t i = 0; i < NumBuffers; i++) {
      auto newBuffer = static_cast<D3D11Buffer*>(ppVertexBuffers[i]);

      if (m_state.ia.vertexBuffers[StartSlot + i].buffer != newBuffer) {
        m_state.ia.vertexBuffers[StartSlot + i].buffer = newBuffer;
        m_state.ia.vertexBuffers[StartSlot + i].offset = pOffsets[i];
        m_state.ia.vertexBuffers[StartSlot + i].stride = pStrides[i];

        BindVertexBuffer(StartSlot + i, newBuffer, pOffsets[i], pStrides[i]);
      } else if (m_state.ia.vertexBuffers[StartSlot + i].offset != pOffsets[i]
              || m_state.ia.vertexBuffers[StartSlot + i].stride != pStrides[i]) {
        m_state.ia.vertexBuffers[StartSlot + i].offset = pOffsets[i];
        m_state.ia.vertexBuffers[StartSlot + i].stride = pStrides[i];

        BindVertexBufferRange(StartSlot + i, newBuffer, pOffsets[i], pStrides[i]);
      }
    }

    m_state.ia.maxVbCount = std::clamp(StartSlot + NumBuffers,
      m_state.ia.maxVbCount, uint32_t(m_state.ia.vertexBuffers.size()));
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IASetIndexBuffer(
          ID3D11Buffer*                     pIndexBuffer,
          DXGI_FORMAT                       Format,
          UINT                              Offset) {
    D3D10DeviceLock lock = LockContext();

    auto newBuffer = static_cast<D3D11Buffer*>(pIndexBuffer);

    if (m_state.ia.indexBuffer.buffer != newBuffer) {
      m_state.ia.indexBuffer.buffer = newBuffer;
      m_state.ia.indexBuffer.offset = Offset;
      m_state.ia.indexBuffer.format = Format;

      BindIndexBuffer(newBuffer, Offset, Format);
    } else if (m_state.ia.indexBuffer.offset != Offset
            || m_state.ia.indexBuffer.format != Format) {
      m_state.ia.indexBuffer.offset = Offset;
      m_state.ia.indexBuffer.format = Format;

      BindIndexBufferRange(newBuffer, Offset, Format);
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IAGetInputLayout(ID3D11InputLayout** ppInputLayout) {
    D3D10DeviceLock lock = LockContext();

    *ppInputLayout = m_state.ia.inputLayout.ref();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) {
    D3D10DeviceLock lock = LockContext();

    *pTopology = m_state.ia.primitiveTopology;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IAGetVertexBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppVertexBuffers,
          UINT*                             pStrides,
          UINT*                             pOffsets) {
    D3D10DeviceLock lock = LockContext();

    for (uint32_t i = 0; i < NumBuffers; i++) {
      const bool inRange = StartSlot + i < m_state.ia.vertexBuffers.size();

      if (ppVertexBuffers) {
        ppVertexBuffers[i] = inRange
          ? m_state.ia.vertexBuffers[StartSlot + i].buffer.ref()
          : nullptr;
      }

      if (pStrides) {
        pStrides[i] = inRange
          ? m_state.ia.vertexBuffers[StartSlot + i].stride
          : 0u;
      }

      if (pOffsets) {
        pOffsets[i] = inRange
          ? m_state.ia.vertexBuffers[StartSlot + i].offset
          : 0u;
      }
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IAGetIndexBuffer(
          ID3D11Buffer**                    ppIndexBuffer,
          DXGI_FORMAT*                      pFormat,
          UINT*                             pOffset) {
    D3D10DeviceLock lock = LockContext();

    if (ppIndexBuffer)
      *ppIndexBuffer = m_state.ia.indexBuffer.buffer.ref();

    if (pFormat)
      *pFormat = m_state.ia.indexBuffer.format;

    if (pOffset)
      *pOffset = m_state.ia.indexBuffer.offset;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSSetShader(
          ID3D11VertexShader*               pVertexShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    auto shader = static_cast<D3D11VertexShader*>(pVertexShader);
    SetClassInstances<D3D11ShaderType::eVertex>(
      shader ? shader->GetCommonShader() : nullptr, ppClassInstances, NumClassInstances);

    if (m_state.vs != shader) {
      m_state.vs = shader;

      BindShader<D3D11ShaderType::eVertex>(GetCommonShader(shader));
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSSetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers<D3D11ShaderType::eVertex>(
      StartSlot, NumBuffers, ppConstantBuffers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSSetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers1<D3D11ShaderType::eVertex>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSSetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView* const*  ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    SetShaderResources<D3D11ShaderType::eVertex>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSSetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    SetSamplers<D3D11ShaderType::eVertex>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSGetShader(
          ID3D11VertexShader**              ppVertexShader,
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    if (ppVertexShader)
      *ppVertexShader = m_state.vs.ref();

    GetClassInstances<D3D11ShaderType::eVertex>(ppClassInstances, pNumClassInstances);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSGetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eVertex>(
      StartSlot, NumBuffers, ppConstantBuffers,
      nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSGetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eVertex>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSGetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    GetShaderResources<D3D11ShaderType::eVertex>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::VSGetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    GetSamplers<D3D11ShaderType::eVertex>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSSetShader(
          ID3D11HullShader*                 pHullShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    auto shader = static_cast<D3D11HullShader*>(pHullShader);
    SetClassInstances<D3D11ShaderType::eHull>(
      shader ? shader->GetCommonShader() : nullptr, ppClassInstances, NumClassInstances);

    if (m_state.hs != shader) {
      m_state.hs = shader;

      BindShader<D3D11ShaderType::eHull>(GetCommonShader(shader));
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSSetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers<D3D11ShaderType::eHull>(
      StartSlot, NumBuffers, ppConstantBuffers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSSetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers1<D3D11ShaderType::eHull>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSSetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView* const*  ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    SetShaderResources<D3D11ShaderType::eHull>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSSetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    SetSamplers<D3D11ShaderType::eHull>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSGetShader(
          ID3D11HullShader**                ppHullShader,
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    if (ppHullShader)
      *ppHullShader = m_state.hs.ref();

    GetClassInstances<D3D11ShaderType::eHull>(ppClassInstances, pNumClassInstances);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSGetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eHull>(
      StartSlot, NumBuffers, ppConstantBuffers,
      nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSGetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eHull>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSGetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    GetShaderResources<D3D11ShaderType::eHull>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::HSGetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    GetSamplers<D3D11ShaderType::eHull>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSSetShader(
          ID3D11DomainShader*               pDomainShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    auto shader = static_cast<D3D11DomainShader*>(pDomainShader);
    SetClassInstances<D3D11ShaderType::eDomain>(
      shader ? shader->GetCommonShader() : nullptr, ppClassInstances, NumClassInstances);

    if (m_state.ds != shader) {
      m_state.ds = shader;

      BindShader<D3D11ShaderType::eDomain>(GetCommonShader(shader));
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSSetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers<D3D11ShaderType::eDomain>(
      StartSlot, NumBuffers, ppConstantBuffers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSSetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers1<D3D11ShaderType::eDomain>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSSetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView* const*  ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    SetShaderResources<D3D11ShaderType::eDomain>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSSetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    SetSamplers<D3D11ShaderType::eDomain>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSGetShader(
          ID3D11DomainShader**              ppDomainShader,
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    if (ppDomainShader)
      *ppDomainShader = m_state.ds.ref();

    GetClassInstances<D3D11ShaderType::eDomain>(ppClassInstances, pNumClassInstances);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSGetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eDomain>(
      StartSlot, NumBuffers, ppConstantBuffers,
      nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSGetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eDomain>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSGetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    GetShaderResources<D3D11ShaderType::eDomain>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DSGetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    GetSamplers<D3D11ShaderType::eDomain>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSSetShader(
          ID3D11GeometryShader*             pShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    auto shader = static_cast<D3D11GeometryShader*>(pShader);
    SetClassInstances<D3D11ShaderType::eGeometry>(
      shader ? shader->GetCommonShader() : nullptr, ppClassInstances, NumClassInstances);

    if (m_state.gs != shader) {
      m_state.gs = shader;

      BindShader<D3D11ShaderType::eGeometry>(GetCommonShader(shader));
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSSetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers<D3D11ShaderType::eGeometry>(
      StartSlot, NumBuffers, ppConstantBuffers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSSetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {  
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers1<D3D11ShaderType::eGeometry>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSSetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView* const*  ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    SetShaderResources<D3D11ShaderType::eGeometry>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSSetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    SetSamplers<D3D11ShaderType::eGeometry>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSGetShader(
          ID3D11GeometryShader**            ppGeometryShader,
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    if (ppGeometryShader)
      *ppGeometryShader = m_state.gs.ref();

    GetClassInstances<D3D11ShaderType::eGeometry>(ppClassInstances, pNumClassInstances);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSGetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eGeometry>(
      StartSlot, NumBuffers, ppConstantBuffers,
      nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSGetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eGeometry>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSGetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    GetShaderResources<D3D11ShaderType::eGeometry>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GSGetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    GetSamplers<D3D11ShaderType::eGeometry>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSSetShader(
          ID3D11PixelShader*                pPixelShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    auto shader = static_cast<D3D11PixelShader*>(pPixelShader);
    SetClassInstances<D3D11ShaderType::ePixel>(
      shader ? shader->GetCommonShader() : nullptr, ppClassInstances, NumClassInstances);

    if (m_state.ps != shader) {
      m_state.ps = shader;

      BindShader<D3D11ShaderType::ePixel>(GetCommonShader(shader));
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSSetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers<D3D11ShaderType::ePixel>(
      StartSlot, NumBuffers, ppConstantBuffers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSSetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers1<D3D11ShaderType::ePixel>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSSetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView* const*  ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    SetShaderResources<D3D11ShaderType::ePixel>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSSetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    SetSamplers<D3D11ShaderType::ePixel>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSGetShader(
          ID3D11PixelShader**               ppPixelShader,
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    if (ppPixelShader)
      *ppPixelShader = m_state.ps.ref();

    GetClassInstances<D3D11ShaderType::ePixel>(ppClassInstances, pNumClassInstances);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSGetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::ePixel>(
      StartSlot, NumBuffers, ppConstantBuffers,
      nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSGetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::ePixel>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSGetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    GetShaderResources<D3D11ShaderType::ePixel>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::PSGetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    GetSamplers<D3D11ShaderType::ePixel>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSSetShader(
          ID3D11ComputeShader*              pComputeShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    auto shader = static_cast<D3D11ComputeShader*>(pComputeShader);
    SetClassInstances<D3D11ShaderType::eCompute>(
      shader ? shader->GetCommonShader() : nullptr, ppClassInstances, NumClassInstances);

    if (m_state.cs != shader) {
      m_state.cs = shader;

      BindShader<D3D11ShaderType::eCompute>(GetCommonShader(shader));
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSSetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers<D3D11ShaderType::eCompute>(
      StartSlot, NumBuffers, ppConstantBuffers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSSetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    SetConstantBuffers1<D3D11ShaderType::eCompute>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSSetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView* const*  ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    SetShaderResources<D3D11ShaderType::eCompute>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSSetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    SetSamplers<D3D11ShaderType::eCompute>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSSetUnorderedAccessViews(
          UINT                              StartSlot,
          UINT                              NumUAVs,
          ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT*                             pUAVInitialCounts) {
    D3D10DeviceLock lock = LockContext();

    if (TestRtvUavHazards(0, nullptr, NumUAVs, ppUnorderedAccessViews))
      return;

    // Unbind previously bound conflicting UAVs
    int32_t uavId = m_state.uav.mask.findNext(0);

    while (uavId >= 0) {
      if (uint32_t(uavId) < StartSlot || uint32_t(uavId) >= StartSlot + NumUAVs) {
        for (uint32_t i = 0; i < NumUAVs; i++) {
          auto uav = static_cast<D3D11UnorderedAccessView*>(ppUnorderedAccessViews[i]);

          if (CheckViewOverlap(uav, m_state.uav.views[uavId].ptr())) {
            m_state.uav.views[uavId] = nullptr;
            m_state.uav.mask.clr(uavId);

            if (!DirtyComputeUnorderedAccessView(uavId, true))
              BindUnorderedAccessView(D3D11ShaderType::eCompute, uavId, nullptr);
          }
        }

        uavId = m_state.uav.mask.findNext(uavId + 1);
      } else {
        uavId = m_state.uav.mask.findNext(StartSlot + NumUAVs);
      }
    }

    // Actually bind the given UAVs
    for (uint32_t i = 0; i < NumUAVs; i++) {
      auto uav = static_cast<D3D11UnorderedAccessView*>(ppUnorderedAccessViews[i]);
      auto ctr = pUAVInitialCounts ? pUAVInitialCounts[i] : ~0u;

      if (ctr != ~0u && uav && uav->HasCounter())
        UpdateUnorderedAccessViewCounter(uav, ctr);

      if (m_state.uav.views[StartSlot + i] != uav) {
        m_state.uav.views[StartSlot + i] = uav;
        m_state.uav.mask.set(StartSlot + i, uav != nullptr);

        if (!DirtyComputeUnorderedAccessView(StartSlot + i, !uav))
          BindUnorderedAccessView(D3D11ShaderType::eCompute, StartSlot + i, uav);

        ResolveCsSrvHazards(uav);
      }
    }

    m_state.uav.maxCount = std::clamp(StartSlot + NumUAVs,
      m_state.uav.maxCount, uint32_t(m_state.uav.views.size()));
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSGetShader(
          ID3D11ComputeShader**             ppComputeShader,
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    D3D10DeviceLock lock = LockContext();

    if (ppComputeShader)
      *ppComputeShader = m_state.cs.ref();

    GetClassInstances<D3D11ShaderType::eCompute>(ppClassInstances, pNumClassInstances);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSGetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eCompute>(
      StartSlot, NumBuffers, ppConstantBuffers,
      nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSGetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    D3D10DeviceLock lock = LockContext();

    GetConstantBuffers<D3D11ShaderType::eCompute>(
      StartSlot, NumBuffers, ppConstantBuffers,
      pFirstConstant, pNumConstants);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSGetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    D3D10DeviceLock lock = LockContext();

    GetShaderResources<D3D11ShaderType::eCompute>(
      StartSlot, NumViews, ppShaderResourceViews);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSGetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    D3D10DeviceLock lock = LockContext();

    GetSamplers<D3D11ShaderType::eCompute>(
      StartSlot, NumSamplers, ppSamplers);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CSGetUnorderedAccessViews(
          UINT                              StartSlot,
          UINT                              NumUAVs,
          ID3D11UnorderedAccessView**       ppUnorderedAccessViews) {
    D3D10DeviceLock lock = LockContext();

    for (uint32_t i = 0; i < NumUAVs; i++) {
      ppUnorderedAccessViews[i] = StartSlot + i < m_state.uav.views.size()
        ? m_state.uav.views[StartSlot + i].ref()
        : nullptr;
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMSetRenderTargets(
          UINT                              NumViews,
          ID3D11RenderTargetView* const*    ppRenderTargetViews,
          ID3D11DepthStencilView*           pDepthStencilView) {
    D3D10DeviceLock lock = LockContext();

    SetRenderTargetsAndUnorderedAccessViews(
      NumViews, ppRenderTargetViews, pDepthStencilView,
      NumViews, 0, nullptr, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMSetRenderTargetsAndUnorderedAccessViews(
          UINT                              NumRTVs,
          ID3D11RenderTargetView* const*    ppRenderTargetViews,
          ID3D11DepthStencilView*           pDepthStencilView,
          UINT                              UAVStartSlot,
          UINT                              NumUAVs,
          ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT*                             pUAVInitialCounts) {
    D3D10DeviceLock lock = LockContext();

    SetRenderTargetsAndUnorderedAccessViews(
      NumRTVs, ppRenderTargetViews, pDepthStencilView,
      UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMSetBlendState(
          ID3D11BlendState*                 pBlendState,
    const FLOAT                             BlendFactor[4],
          UINT                              SampleMask) {
    D3D10DeviceLock lock = LockContext();

    auto blendState = static_cast<D3D11BlendState*>(pBlendState);

    if (m_state.om.cbState    != blendState
     || m_state.om.sampleMask != SampleMask) {
      m_state.om.cbState    = blendState;
      m_state.om.sampleMask = SampleMask;

      ApplyBlendState();
    }

    if (BlendFactor != nullptr) {
      for (uint32_t i = 0; i < 4; i++)
        m_state.om.blendFactor[i] = BlendFactor[i];

      ApplyBlendFactor();
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMSetDepthStencilState(
          ID3D11DepthStencilState*          pDepthStencilState,
          UINT                              StencilRef) {
    D3D10DeviceLock lock = LockContext();

    auto depthStencilState = static_cast<D3D11DepthStencilState*>(pDepthStencilState);

    if (m_state.om.dsState != depthStencilState) {
      m_state.om.dsState = depthStencilState;
      ApplyDepthStencilState();
    }

    // The D3D11 runtime only appears to store the low 8 bits,
    // and some games rely on this behaviour. Do the same here.
    StencilRef &= 0xFF;

    if (m_state.om.stencilRef != StencilRef) {
      m_state.om.stencilRef = StencilRef;
      ApplyStencilRef();
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMGetRenderTargets(
          UINT                              NumViews,
          ID3D11RenderTargetView**          ppRenderTargetViews,
          ID3D11DepthStencilView**          ppDepthStencilView) {
    OMGetRenderTargetsAndUnorderedAccessViews(
      NumViews, ppRenderTargetViews, ppDepthStencilView,
      NumViews, 0, nullptr);
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMGetRenderTargetsAndUnorderedAccessViews(
          UINT                              NumRTVs,
          ID3D11RenderTargetView**          ppRenderTargetViews,
          ID3D11DepthStencilView**          ppDepthStencilView,
          UINT                              UAVStartSlot,
          UINT                              NumUAVs,
          ID3D11UnorderedAccessView**       ppUnorderedAccessViews) {
    D3D10DeviceLock lock = LockContext();

    if (ppRenderTargetViews) {
      for (UINT i = 0; i < NumRTVs; i++) {
        ppRenderTargetViews[i] = i < m_state.om.rtvs.size()
          ? m_state.om.rtvs[i].ref()
          : nullptr;
      }
    }

    if (ppDepthStencilView)
      *ppDepthStencilView = m_state.om.dsv.ref();

    if (ppUnorderedAccessViews) {
      for (UINT i = 0; i < NumUAVs; i++) {
        ppUnorderedAccessViews[i] = UAVStartSlot + i < m_state.om.uavs.size()
          ? m_state.om.uavs[UAVStartSlot + i].ref()
          : nullptr;
      }
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMGetBlendState(
          ID3D11BlendState**                ppBlendState,
          FLOAT                             BlendFactor[4],
          UINT*                             pSampleMask) {
    D3D10DeviceLock lock = LockContext();

    if (ppBlendState)
      *ppBlendState = m_state.om.cbState.ref();

    if (BlendFactor)
      std::memcpy(BlendFactor, m_state.om.blendFactor, sizeof(FLOAT) * 4);

    if (pSampleMask)
      *pSampleMask = m_state.om.sampleMask;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::OMGetDepthStencilState(
          ID3D11DepthStencilState**         ppDepthStencilState,
          UINT*                             pStencilRef) {
    D3D10DeviceLock lock = LockContext();

    if (ppDepthStencilState)
      *ppDepthStencilState = m_state.om.dsState.ref();

    if (pStencilRef)
      *pStencilRef = m_state.om.stencilRef;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::RSSetState(ID3D11RasterizerState* pRasterizerState) {
    D3D10DeviceLock lock = LockContext();

    auto newRasterizerState = static_cast<D3D11RasterizerState*>(pRasterizerState);

    if (m_state.rs.state != newRasterizerState) {
      // Need to keep the previous rasterizer state object alive for the time being
      auto oldRasterizerState = std::move(m_state.rs.state);

      m_state.rs.state = newRasterizerState;
      ApplyRasterizerState();

      // If necessary, update the rasterizer sample count push constant
      uint32_t oldSampleCount = oldRasterizerState ? oldRasterizerState->Desc().ForcedSampleCount : 0;
      uint32_t newSampleCount = newRasterizerState ? newRasterizerState->Desc().ForcedSampleCount : 0;

      if (oldSampleCount != newSampleCount)
        ApplyRasterizerSampleCount();

      // In D3D11, the rasterizer state defines whether the scissor test is
      // enabled, so if that changes, we need to update scissor rects as well.
      bool oldScissorEnable = oldRasterizerState && oldRasterizerState->Desc().ScissorEnable;
      bool newScissorEnable = newRasterizerState && newRasterizerState->Desc().ScissorEnable;

      if (oldScissorEnable != newScissorEnable)
        ApplyViewportState();
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::RSSetViewports(
          UINT                              NumViewports,
    const D3D11_VIEWPORT*                   pViewports) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(NumViewports > m_state.rs.viewports.size()))
      return;

    for (uint32_t i = 0; i < NumViewports; i++) {
      const D3D11_VIEWPORT& vp = pViewports[i];

      bool valid = vp.Width >= 0.0f && vp.Height >= 0.0f
                && vp.MinDepth >= 0.0f && vp.MaxDepth <= 1.0f
                && vp.MinDepth <= vp.MaxDepth;

      if (!valid)
        return;
    }

    bool dirty = m_state.rs.numViewports != NumViewports;
    m_state.rs.numViewports = NumViewports;

    for (uint32_t i = 0; i < NumViewports; i++) {
      const D3D11_VIEWPORT& vp = m_state.rs.viewports[i];

      dirty |= vp.TopLeftX != pViewports[i].TopLeftX
            || vp.TopLeftY != pViewports[i].TopLeftY
            || vp.Width    != pViewports[i].Width
            || vp.Height   != pViewports[i].Height
            || vp.MinDepth != pViewports[i].MinDepth
            || vp.MaxDepth != pViewports[i].MaxDepth;
      
      m_state.rs.viewports[i] = pViewports[i];
    }

    if (dirty)
      ApplyViewportState();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::RSSetScissorRects(
          UINT                              NumRects,
    const D3D11_RECT*                       pRects) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(NumRects > m_state.rs.scissors.size()))
      return;

    bool dirty = m_state.rs.numScissors != NumRects;
    m_state.rs.numScissors = NumRects;

    for (uint32_t i = 0; i < NumRects; i++) {
      if (pRects[i].bottom >= pRects[i].top
       && pRects[i].right  >= pRects[i].left) {
        const D3D11_RECT& sr = m_state.rs.scissors[i];

        dirty |= sr.top    != pRects[i].top
              || sr.left   != pRects[i].left
              || sr.bottom != pRects[i].bottom
              || sr.right  != pRects[i].right;

        m_state.rs.scissors[i] = pRects[i];
      }
    }

    if (dirty && m_state.rs.state && m_state.rs.state->Desc().ScissorEnable)
      ApplyViewportState();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::RSGetState(ID3D11RasterizerState** ppRasterizerState) {
    D3D10DeviceLock lock = LockContext();

    if (ppRasterizerState)
      *ppRasterizerState = m_state.rs.state.ref();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::RSGetViewports(
          UINT*                             pNumViewports,
          D3D11_VIEWPORT*                   pViewports) {
    D3D10DeviceLock lock = LockContext();
    uint32_t numWritten = m_state.rs.numViewports;

    if (pViewports) {
      numWritten = std::min(numWritten, *pNumViewports);

      for (uint32_t i = 0; i < *pNumViewports; i++) {
        if (i < m_state.rs.numViewports) {
          pViewports[i] = m_state.rs.viewports[i];
        } else {
          pViewports[i].TopLeftX = 0.0f;
          pViewports[i].TopLeftY = 0.0f;
          pViewports[i].Width    = 0.0f;
          pViewports[i].Height   = 0.0f;
          pViewports[i].MinDepth = 0.0f;
          pViewports[i].MaxDepth = 0.0f;
        }
      }
    }

    *pNumViewports = numWritten;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::RSGetScissorRects(
          UINT*                             pNumRects,
          D3D11_RECT*                       pRects) {
    D3D10DeviceLock lock = LockContext();
    uint32_t numWritten = m_state.rs.numScissors;

    if (pRects) {
      numWritten = std::min(numWritten, *pNumRects);

      for (uint32_t i = 0; i < *pNumRects; i++) {
        if (i < m_state.rs.numScissors) {
          pRects[i] = m_state.rs.scissors[i];
        } else {
          pRects[i].left   = 0;
          pRects[i].top    = 0;
          pRects[i].right  = 0;
          pRects[i].bottom = 0;
        }
      }
    }

    *pNumRects = m_state.rs.numScissors;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SOSetTargets(
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppSOTargets,
    const UINT*                             pOffsets) {
    D3D10DeviceLock lock = LockContext();

    for (uint32_t i = 0; i < NumBuffers; i++) {
      D3D11Buffer* buffer = static_cast<D3D11Buffer*>(ppSOTargets[i]);
      UINT         offset = pOffsets != nullptr ? pOffsets[i] : 0;

      m_state.so.targets[i].buffer = buffer;
      m_state.so.targets[i].offset = offset;
    }

    for (uint32_t i = NumBuffers; i < D3D11_SO_BUFFER_SLOT_COUNT; i++) {
      m_state.so.targets[i].buffer = nullptr;
      m_state.so.targets[i].offset = 0;
    }

    for (uint32_t i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT; i++) {
      BindXfbBuffer(i,
        m_state.so.targets[i].buffer.ptr(),
        m_state.so.targets[i].offset);
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SOGetTargets(
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppSOTargets) {
    D3D10DeviceLock lock = LockContext();

    for (uint32_t i = 0; i < NumBuffers; i++) {
      ppSOTargets[i] = i < m_state.so.targets.size()
        ? m_state.so.targets[i].buffer.ref()
        : nullptr;
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SOGetTargetsWithOffsets(
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppSOTargets,
          UINT*                             pOffsets) {
    D3D10DeviceLock lock = LockContext();

    for (uint32_t i = 0; i < NumBuffers; i++) {
      const bool inRange = i < m_state.so.targets.size();

      if (ppSOTargets) {
        ppSOTargets[i] = inRange
          ? m_state.so.targets[i].buffer.ref()
          : nullptr;
      }

      if (pOffsets) {
        pOffsets[i] = inRange
          ? m_state.so.targets[i].offset
          : 0u;
      }
    }
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SetPredication(
          ID3D11Predicate*                  pPredicate,
          BOOL                              PredicateValue) {
    D3D10DeviceLock lock = LockContext();

    auto predicate = D3D11Query::FromPredicate(pPredicate);
    m_state.pr.predicateObject = predicate;
    m_state.pr.predicateValue  = PredicateValue;

    static bool s_errorShown = false;

    if (pPredicate && !std::exchange(s_errorShown, true))
      Logger::err("D3D11DeviceContext::SetPredication: Stub");
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GetPredication(
          ID3D11Predicate**                 ppPredicate,
          BOOL*                             pPredicateValue) {
    D3D10DeviceLock lock = LockContext();

    if (ppPredicate)
      *ppPredicate = D3D11Query::AsPredicate(m_state.pr.predicateObject.ref());

    if (pPredicateValue)
      *pPredicateValue = m_state.pr.predicateValue;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SetResourceMinLOD(
          ID3D11Resource*                   pResource,
          FLOAT                             MinLOD) {
    bool s_errorShown = false;

    if (std::exchange(s_errorShown, true))
      Logger::err("D3D11DeviceContext::SetResourceMinLOD: Not implemented");
  }
  
  
  template<typename ContextType>
  FLOAT STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GetResourceMinLOD(ID3D11Resource* pResource) {
    bool s_errorShown = false;

    if (std::exchange(s_errorShown, true))
      Logger::err("D3D11DeviceContext::GetResourceMinLOD: Not implemented");

    return 0.0f;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopyTiles(
          ID3D11Resource*                   pTiledResource,
    const D3D11_TILED_RESOURCE_COORDINATE*  pTileRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE*           pTileRegionSize,
          ID3D11Buffer*                     pBuffer,
          UINT64                            BufferStartOffsetInBytes,
          UINT                              Flags) {
    D3D10DeviceLock lock = LockContext();

    if (!pTiledResource || !pBuffer)
      return;

    auto buffer = static_cast<D3D11Buffer*>(pBuffer);

    // Get buffer slice and just forward the call
    VkDeviceSize bufferSize = pTileRegionSize->NumTiles * SparseMemoryPageSize;

    if (buffer->Desc()->ByteWidth < BufferStartOffsetInBytes + bufferSize)
      return;

    DxvkBufferSlice slice = buffer->GetBufferSlice(BufferStartOffsetInBytes, bufferSize);

    CopyTiledResourceData(pTiledResource,
      pTileRegionStartCoordinate,
      pTileRegionSize, slice, Flags);

    if (buffer->HasSequenceNumber())
      GetTypedContext()->TrackBufferSequenceNumber(buffer);
  }


  template<typename ContextType>
  HRESULT STDMETHODCALLTYPE D3D11CommonContext<ContextType>::CopyTileMappings(
          ID3D11Resource*                   pDestTiledResource,
    const D3D11_TILED_RESOURCE_COORDINATE*  pDestRegionCoordinate,
          ID3D11Resource*                   pSourceTiledResource,
    const D3D11_TILED_RESOURCE_COORDINATE*  pSourceRegionCoordinate,
    const D3D11_TILE_REGION_SIZE*           pTileRegionSize,
          UINT                              Flags) {
    D3D10DeviceLock lock = LockContext();

    if (!pDestTiledResource || !pSourceTiledResource)
      return E_INVALIDARG;

    if constexpr (!IsDeferred)
      GetTypedContext()->ConsiderFlush(GpuFlushType::ImplicitWeakHint);

    DxvkSparseBindInfo bindInfo;
    bindInfo.dstResource = GetPagedResource(pDestTiledResource);
    bindInfo.srcResource = GetPagedResource(pSourceTiledResource);

    auto dstPageTable = bindInfo.dstResource->getSparsePageTable();
    auto srcPageTable = bindInfo.srcResource->getSparsePageTable();

    if (!dstPageTable || !srcPageTable)
      return E_INVALIDARG;

    if (pDestRegionCoordinate->Subresource >= dstPageTable->getSubresourceCount()
     || pSourceRegionCoordinate->Subresource >= srcPageTable->getSubresourceCount())
      return E_INVALIDARG;

    VkOffset3D dstRegionOffset = {
      int32_t(pDestRegionCoordinate->X),
      int32_t(pDestRegionCoordinate->Y),
      int32_t(pDestRegionCoordinate->Z) };

    VkOffset3D srcRegionOffset = {
      int32_t(pSourceRegionCoordinate->X),
      int32_t(pSourceRegionCoordinate->Y),
      int32_t(pSourceRegionCoordinate->Z) };

    VkExtent3D regionExtent = {
      uint32_t(pTileRegionSize->Width),
      uint32_t(pTileRegionSize->Height),
      uint32_t(pTileRegionSize->Depth) };

    for (uint32_t i = 0; i < pTileRegionSize->NumTiles; i++) {
      // We don't know the current tile mappings of either resource since
      // this may be called on a deferred context and tile mappings are
      // updated on the CS thread, so just resolve the copy in the backend
      uint32_t dstTile = dstPageTable->computePageIndex(
        pDestRegionCoordinate->Subresource, dstRegionOffset,
        regionExtent, !pTileRegionSize->bUseBox, i);

      uint32_t srcTile = srcPageTable->computePageIndex(
        pSourceRegionCoordinate->Subresource, srcRegionOffset,
        regionExtent, !pTileRegionSize->bUseBox, i);

      if (dstTile >= dstPageTable->getPageCount()
       || srcTile >= srcPageTable->getPageCount())
        return E_INVALIDARG;

      DxvkSparseBind bind;
      bind.mode = DxvkSparseBindMode::Copy;
      bind.dstPage = dstTile;
      bind.srcPage = srcTile;

      bindInfo.binds.push_back(bind);
    }

    DxvkSparseBindFlags flags = (Flags & D3D11_TILE_MAPPING_NO_OVERWRITE)
      ? DxvkSparseBindFlags(DxvkSparseBindFlag::SkipSynchronization)
      : DxvkSparseBindFlags();

    EmitCs([
      cBindInfo = std::move(bindInfo),
      cFlags    = flags
    ] (DxvkContext* ctx) {
      ctx->updatePageTable(cBindInfo, cFlags);
    });

    return S_OK;
  }


  template<typename ContextType>
  HRESULT STDMETHODCALLTYPE D3D11CommonContext<ContextType>::ResizeTilePool(
          ID3D11Buffer*                     pTilePool,
          UINT64                            NewSizeInBytes) {
    D3D10DeviceLock lock = LockContext();

    if (NewSizeInBytes % SparseMemoryPageSize)
      return E_INVALIDARG;

    auto buffer = static_cast<D3D11Buffer*>(pTilePool);

    if (!buffer->IsTilePool())
      return E_INVALIDARG;

    // Perform the resize operation. This is somewhat trivialized
    // since all lifetime tracking is done by the backend.
    EmitCs([
      cAllocator  = buffer->GetSparseAllocator(),
      cPageCount  = NewSizeInBytes / SparseMemoryPageSize
    ] (DxvkContext* ctx) {
      cAllocator->setCapacity(cPageCount);
    });

    return S_OK;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::TiledResourceBarrier(
          ID3D11DeviceChild*                pTiledResourceOrViewAccessBeforeBarrier,
          ID3D11DeviceChild*                pTiledResourceOrViewAccessAfterBarrier) {
    D3D10DeviceLock lock = LockContext();

    DxvkGlobalPipelineBarrier srcBarrier = GetTiledResourceDependency(pTiledResourceOrViewAccessBeforeBarrier);
    DxvkGlobalPipelineBarrier dstBarrier = GetTiledResourceDependency(pTiledResourceOrViewAccessAfterBarrier);

    if (srcBarrier.stages && dstBarrier.stages) {
      EmitCs([
        cSrcBarrier = srcBarrier,
        cDstBarrier = dstBarrier
      ] (DxvkContext* ctx) {
        ctx->emitGraphicsBarrier(
          cSrcBarrier.stages, cSrcBarrier.access,
          cDstBarrier.stages, cDstBarrier.access);
      });
    }
  }


  template<typename ContextType>
  HRESULT STDMETHODCALLTYPE D3D11CommonContext<ContextType>::UpdateTileMappings(
          ID3D11Resource*                   pTiledResource,
          UINT                              NumRegions,
    const D3D11_TILED_RESOURCE_COORDINATE*  pRegionCoordinates,
    const D3D11_TILE_REGION_SIZE*           pRegionSizes,
          ID3D11Buffer*                     pTilePool,
          UINT                              NumRanges,
    const UINT*                             pRangeFlags,
    const UINT*                             pRangeTileOffsets,
    const UINT*                             pRangeTileCounts,
          UINT                              Flags) {
    D3D10DeviceLock lock = LockContext();

    if (!pTiledResource || !NumRegions || !NumRanges)
      return E_INVALIDARG;

    if constexpr (!IsDeferred)
      GetTypedContext()->ConsiderFlush(GpuFlushType::ImplicitWeakHint);

    // Find sparse allocator if the tile pool is defined
    DxvkSparseBindInfo bindInfo;

    if (pTilePool) {
      auto tilePool = static_cast<D3D11Buffer*>(pTilePool);
      bindInfo.srcAllocator = tilePool->GetSparseAllocator();

      if (bindInfo.srcAllocator == nullptr)
        return E_INVALIDARG;
    }

    // Find resource and sparse page table for the given resource
    bindInfo.dstResource = GetPagedResource(pTiledResource);
    auto pageTable = bindInfo.dstResource->getSparsePageTable();

    if (!pageTable)
      return E_INVALIDARG;

    // Lookup table in case the app tries to bind the same
    // page multiple times. We should resolve that here and
    // only consider the last bind to any given page.
    std::vector<uint32_t> bindIndices(pageTable->getPageCount(), ~0u);

    // This function allows pretty much every parameter to be nullptr
    // in some way, so initialize some defaults as necessary
    D3D11_TILED_RESOURCE_COORDINATE regionCoord = { };
    D3D11_TILE_REGION_SIZE regionSize = { };

    if (!pRegionSizes) {
      regionSize.NumTiles = pRegionCoordinates
        ? 1 : pageTable->getPageCount();
    }

    uint32_t rangeFlag = 0u;
    uint32_t rangeTileOffset = 0u;
    uint32_t rangeTileCount = ~0u;

    // For now, just generate a simple list of tile index to
    // page index mappings, and let the backend optimize later
    uint32_t regionIdx = 0u;
    uint32_t regionTile = 0u;
    uint32_t rangeIdx = 0u;
    uint32_t rangeTile = 0u;

    while (regionIdx < NumRegions && rangeIdx < NumRanges) {
      if (!regionTile) {
        if (pRegionCoordinates)
          regionCoord = pRegionCoordinates[regionIdx];

        if (pRegionSizes)
          regionSize = pRegionSizes[regionIdx];
      }

      if (!rangeTile) {
        if (pRangeFlags)
          rangeFlag = pRangeFlags[rangeIdx];

        if (pRangeTileOffsets)
          rangeTileOffset = pRangeTileOffsets[rangeIdx];

        if (pRangeTileCounts)
          rangeTileCount = pRangeTileCounts[rangeIdx];
      }

      if (!(rangeFlag & D3D11_TILE_RANGE_SKIP)) {
        if (regionCoord.Subresource >= pageTable->getSubresourceCount())
          return E_INVALIDARG;

        if (regionSize.bUseBox && regionSize.NumTiles !=
            regionSize.Width * regionSize.Height * regionSize.Depth)
          return E_INVALIDARG;

        VkOffset3D regionOffset = {
          int32_t(regionCoord.X),
          int32_t(regionCoord.Y),
          int32_t(regionCoord.Z) };

        VkExtent3D regionExtent = {
          uint32_t(regionSize.Width),
          uint32_t(regionSize.Height),
          uint32_t(regionSize.Depth) };

        uint32_t resourceTile = pageTable->computePageIndex(regionCoord.Subresource,
          regionOffset, regionExtent, !regionSize.bUseBox, regionTile);

        // Fill in bind info for the current tile
        DxvkSparseBind bind = { };
        bind.dstPage = resourceTile;

        if (rangeFlag & D3D11_TILE_RANGE_NULL) {
          bind.mode = DxvkSparseBindMode::Null;
        } else if (pTilePool) {
          bind.mode = DxvkSparseBindMode::Bind;
          bind.srcPage = rangeFlag & D3D11_TILE_RANGE_REUSE_SINGLE_TILE
            ? rangeTileOffset
            : rangeTileOffset + rangeTile;
        } else {
          return E_INVALIDARG;
        }

        // Add bind info to the bind list, overriding
        // any existing bind for the same resource page
        if (resourceTile < pageTable->getPageCount()) {
          if (bindIndices[resourceTile] < bindInfo.binds.size())
            bindInfo.binds[bindIndices[resourceTile]] = bind;
          else
            bindInfo.binds.push_back(bind);
        }
      }

      if (++regionTile == regionSize.NumTiles) {
        regionTile = 0;
        regionIdx += 1;
      }

      if (++rangeTile == rangeTileCount) {
        rangeTile = 0;
        rangeIdx += 1;
      }
    }

    // Translate flags. The backend benefits from NO_OVERWRITE since
    // otherwise we have to serialize execution of the current command
    // buffer, the sparse binding operation, and subsequent commands.
    // With NO_OVERWRITE, we can execute it more or less asynchronously.
    DxvkSparseBindFlags flags = (Flags & D3D11_TILE_MAPPING_NO_OVERWRITE)
      ? DxvkSparseBindFlags(DxvkSparseBindFlag::SkipSynchronization)
      : DxvkSparseBindFlags();

    EmitCs([
      cBindInfo = std::move(bindInfo),
      cFlags    = flags
    ] (DxvkContext* ctx) {
      ctx->updatePageTable(cBindInfo, cFlags);
    });

    return S_OK;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::UpdateTiles(
          ID3D11Resource*                   pDestTiledResource,
    const D3D11_TILED_RESOURCE_COORDINATE*  pDestTileRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE*           pDestTileRegionSize,
    const void*                             pSourceTileData,
          UINT                              Flags) {
    D3D10DeviceLock lock = LockContext();

    if (!pDestTiledResource || !pSourceTileData)
      return;

    // Allocate staging memory and copy source data into it, at a
    // 64k page granularity. It is not clear whether this behaviour
    // is correct in case we're writing to incmplete pages.
    VkDeviceSize bufferSize = pDestTileRegionSize->NumTiles * SparseMemoryPageSize;

    DxvkBufferSlice slice = AllocStagingBuffer(bufferSize);
    std::memcpy(slice.mapPtr(0), pSourceTileData, bufferSize);

    // Fix up flags. The runtime probably validates this in some
    // way but our internal function relies on correct flags anyway.
    Flags &= D3D11_TILE_MAPPING_NO_OVERWRITE;
    Flags |= D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;

    CopyTiledResourceData(pDestTiledResource,
      pDestTileRegionStartCoordinate,
      pDestTileRegionSize, slice, Flags);

    if constexpr (!IsDeferred)
      static_cast<ContextType*>(this)->ThrottleAllocation();
  }


  template<typename ContextType>
  BOOL STDMETHODCALLTYPE D3D11CommonContext<ContextType>::IsAnnotationEnabled() {
    return m_annotation.GetStatus();
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SetMarkerInt(
          LPCWSTR                           pLabel,
          INT                               Data) {
    // Not implemented in the backend, ignore
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::BeginEventInt(
          LPCWSTR                           pLabel,
          INT                               Data) {
    // Not implemented in the backend, ignore
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::EndEvent() {
    // Not implemented in the backend, ignore
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::GetHardwareProtectionState(
          BOOL*                             pHwProtectionEnable) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::err("D3D11DeviceContext::GetHardwareProtectionState: Not implemented");

    if (pHwProtectionEnable)
      *pHwProtectionEnable = FALSE;
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::SetHardwareProtectionState(
          BOOL                              HwProtectionEnable) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::err("D3D11DeviceContext::SetHardwareProtectionState: Not implemented");
  }


  template<typename ContextType>
  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::TransitionSurfaceLayout(
          IDXGIVkInteropSurface*    pSurface,
    const VkImageSubresourceRange*  pSubresources,
          VkImageLayout             OldLayout,
          VkImageLayout             NewLayout) {
    D3D10DeviceLock lock = LockContext();

    // Get the underlying D3D11 resource
    Com<ID3D11Resource> resource;

    pSurface->QueryInterface(__uuidof(ID3D11Resource),
      reinterpret_cast<void**>(&resource));

    // Get the texture from that resource
    D3D11CommonTexture* texture = GetCommonTexture(resource.ptr());

    EmitCs([
      cImage        = texture->GetImage(),
      cSubresources = *pSubresources,
      cOldLayout    = OldLayout,
      cNewLayout    = NewLayout
    ] (DxvkContext* ctx) {
      ctx->transformImage(
        cImage, cSubresources,
        cOldLayout, cNewLayout);
    });
  }


  template<typename ContextType>
  DxvkCsChunkRef D3D11CommonContext<ContextType>::AllocCsChunk() {
    return m_parent->AllocCsChunk(m_csFlags);
  }


  template<typename ContextType>
  DxvkBufferSlice D3D11CommonContext<ContextType>::AllocStagingBuffer(
          VkDeviceSize                      Size) {
    return m_staging.alloc(Size);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDirtyConstantBuffers(
          D3D11ShaderType                   Stage,
    const D3D11BindingMask&                 BoundMask,
          D3D11BindingMask&                 DirtyMask) {
    uint32_t bindMask = BoundMask.cbvMask & DirtyMask.cbvMask;

    if (!bindMask)
      return;

    // Need to clear dirty bits before binding
    const auto& state = m_state.cbv[Stage];
    DirtyMask.cbvMask -= bindMask;

    for (uint32_t slot : bit::BitMask(bindMask)) {
      const auto& cbv = state.buffers[slot];

      BindConstantBuffer(Stage, slot, cbv.buffer.ptr(),
        cbv.constantOffset, cbv.constantBound);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDirtySamplers(
          D3D11ShaderType                   Stage,
    const D3D11BindingMask&                 BoundMask,
          D3D11BindingMask&                 DirtyMask) {
    uint32_t bindMask = BoundMask.samplerMask & DirtyMask.samplerMask;

    if (!bindMask)
      return;

    // Need to clear dirty bits before binding
    const auto& state = m_state.samplers[Stage];
    DirtyMask.samplerMask -= bindMask;

    for (uint32_t slot : bit::BitMask(bindMask))
      BindSampler(Stage, slot, state.samplers[slot].ptr());
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDirtyShaderResources(
          D3D11ShaderType                   Stage,
    const D3D11BindingMask&                 BoundMask,
          D3D11BindingMask&                 DirtyMask) {
    const auto& state = m_state.srv[Stage];

    for (uint32_t i = 0; i < state.maxCount; i += 64u) {
      uint32_t maskIndex = i / 64u;
      uint64_t bindMask = BoundMask.srvMask[maskIndex] & DirtyMask.srvMask[maskIndex];

      if (!bindMask)
        continue;

    // Need to clear dirty bits before binding
      DirtyMask.srvMask[maskIndex] -= bindMask;

      for (uint32_t slot : bit::BitMask(bindMask))
        BindShaderResource(Stage, slot + i, state.views[slot + i].ptr());
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDirtyUnorderedAccessViews(
          D3D11ShaderType                   Stage,
    const D3D11BindingMask&                 BoundMask,
          D3D11BindingMask&                 DirtyMask) {
    uint64_t bindMask = BoundMask.uavMask & DirtyMask.uavMask;

    if (!bindMask)
      return;

    const auto& views = Stage == D3D11ShaderType::eCompute
      ? m_state.uav.views
      : m_state.om.uavs;

    // Need to clear dirty bits before binding
    DirtyMask.uavMask -= bindMask;

    for (uint32_t slot : bit::BitMask(bindMask))
      BindUnorderedAccessView(Stage, slot, views[slot].ptr());
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDirtyGraphicsBindings() {
    auto dirtyMask = m_state.lazy.shadersDirty & m_state.lazy.shadersUsed;
    dirtyMask.clr(D3D11ShaderType::eCompute);

    if (unlikely(!(dirtyMask & m_state.lazy.graphicsUavShaders).isClear())) {
      D3D11ShaderType stage = D3D11ShaderType::ePixel;

      auto& boundMask = m_state.lazy.bindingsUsed[stage];
      auto& dirtyMask = m_state.lazy.bindingsDirty[stage];

      ApplyDirtyUnorderedAccessViews(stage, boundMask, dirtyMask);
    }

    for (uint32_t stageIndex : bit::BitMask(uint32_t(dirtyMask.raw()))) {
      D3D11ShaderType stage = D3D11ShaderType(stageIndex);

      auto& boundMask = m_state.lazy.bindingsUsed[stage];
      auto& dirtyMask = m_state.lazy.bindingsDirty[stage];

      ApplyDirtySamplers(stage, boundMask, dirtyMask);
      ApplyDirtyConstantBuffers(stage, boundMask, dirtyMask);
      ApplyDirtyShaderResources(stage, boundMask, dirtyMask);

      m_state.lazy.shadersDirty.clr(stage);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDirtyComputeBindings() {
    D3D11ShaderType stage = D3D11ShaderType::eCompute;

    auto& boundMask = m_state.lazy.bindingsUsed[stage];
    auto& dirtyMask = m_state.lazy.bindingsDirty[stage];

    ApplyDirtySamplers(stage, boundMask, dirtyMask);
    ApplyDirtyConstantBuffers(stage, boundMask, dirtyMask);
    ApplyDirtyShaderResources(stage, boundMask, dirtyMask);
    ApplyDirtyUnorderedAccessViews(stage, boundMask, dirtyMask);

    m_state.lazy.shadersDirty.clr(stage);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyInputLayout() {
    if (likely(m_state.ia.inputLayout != nullptr)) {
      uint32_t attributeCount = m_state.ia.inputLayout->GetAttributeCount();
      uint32_t bindingCount = m_state.ia.inputLayout->GetBindingCount();

      EmitCsCmd<DxvkVertexInput>(D3D11CmdType::None, attributeCount + bindingCount, [
        cAttributeCount   = attributeCount,
        cBindingCount     = bindingCount
      ] (DxvkContext* ctx, const DxvkVertexInput* layout, size_t) {
        ctx->setInputLayout(cAttributeCount, &layout[0],
          cBindingCount, &layout[cAttributeCount]);
      });

      for (uint32_t i = 0; i < attributeCount + bindingCount; i++)
        new (m_csData->at(i)) DxvkVertexInput(m_state.ia.inputLayout->GetInput(i));
    } else {
      EmitCs([] (DxvkContext* ctx) {
        ctx->setInputLayout(0, nullptr, 0, nullptr);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyPrimitiveTopology() {
    D3D11_PRIMITIVE_TOPOLOGY topology = m_state.ia.primitiveTopology;
    DxvkInputAssemblyState iaState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false);

    if (topology <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) {
      static const std::array<DxvkInputAssemblyState, 14> s_iaStates = {{
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false),
        // Regular topologies
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_POINT_LIST,     false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_LINE_LIST,      false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,     true),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,  false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, true),
        // Gap. This includes triangle fan which isn't supported in D3D11
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false),
        // Adjacency topologies
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,       false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,      true),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,   false),
        DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,  true),
      }};

      iaState = s_iaStates[uint32_t(topology)];
    } else if (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
            && topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
      // The number of control points per patch can be inferred from the enum value in D3D11
      iaState = DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, false);
      iaState.setPatchVertexCount(topology - D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
    }

    EmitCs([iaState] (DxvkContext* ctx) {
      ctx->setInputAssemblyState(iaState);
    });
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyBlendState() {
    if (m_state.om.cbState != nullptr) {
      EmitCs([
        cBlendState = m_state.om.cbState->GetBlendState(),
        cMsState    = m_state.om.cbState->GetMsState(m_state.om.sampleMask),
        cLoState    = m_state.om.cbState->GetLoState()
      ] (DxvkContext* ctx) {
        for (uint32_t i = 0; i < cBlendState.size(); i++)
          ctx->setBlendMode(i, cBlendState[i]);

        ctx->setMultisampleState(cMsState);
        ctx->setLogicOpState(cLoState);
      });
    } else {
      EmitCs([
        cSampleMask = m_state.om.sampleMask
      ] (DxvkContext* ctx) {
        DxvkBlendMode cbState = InitDefaultBlendState();

        for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
          ctx->setBlendMode(i, cbState);

        ctx->setMultisampleState(InitDefaultMultisampleState(cSampleMask));
        ctx->setLogicOpState(InitDefaultLogicOpState());
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyBlendFactor() {
    EmitCs([
      cBlendConstants = DxvkBlendConstants {
        m_state.om.blendFactor[0], m_state.om.blendFactor[1],
        m_state.om.blendFactor[2], m_state.om.blendFactor[3] }
    ] (DxvkContext* ctx) {
      ctx->setBlendConstants(cBlendConstants);
    });
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyDepthStencilState() {
    if (m_state.om.dsState != nullptr) {
      EmitCs([
        cState = m_state.om.dsState->GetState()
      ] (DxvkContext* ctx) {
        ctx->setDepthStencilState(cState);
      });
    } else {
      EmitCs([] (DxvkContext* ctx) {
        ctx->setDepthStencilState(InitDefaultDepthStencilState());
      });
    }
  }
  
  
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyStencilRef() {
    EmitCs([
      cStencilRef = m_state.om.stencilRef
    ] (DxvkContext* ctx) {
      ctx->setStencilReference(cStencilRef);
    });
  }
  
  
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyRasterizerState() {
    if (m_state.rs.state != nullptr) {
      EmitCs([
        cState      = m_state.rs.state->GetState(),
        cDepthBias  = m_state.rs.state->GetDepthBias()
      ] (DxvkContext* ctx) {
        ctx->setRasterizerState(cState);
        ctx->setDepthBias(cDepthBias);
      });
    } else {
      EmitCs([] (DxvkContext* ctx) {
        ctx->setRasterizerState(InitDefaultRasterizerState());
        ctx->setDepthBias(DxvkDepthBias());
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyRasterizerSampleCount() {
    uint32_t sampleCount = m_state.om.sampleCount;

    if (unlikely(!sampleCount)) {
      sampleCount = m_state.rs.state
        ? m_state.rs.state->Desc().ForcedSampleCount
        : 0u;

      if (!sampleCount)
        sampleCount = 1u;
    }

    EmitCs([
      cPushConstants = DxvkBuiltInPushData(sampleCount, m_maxTessFactor)
    ] (DxvkContext* ctx) {
      ctx->pushData(VK_SHADER_STAGE_ALL_GRAPHICS,
        0u, sizeof(cPushConstants), &cPushConstants);
    });
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ApplyViewportState() {
    uint32_t viewportCount = m_state.rs.numViewports;

    if (likely(viewportCount)) {
      EmitCsCmd<DxvkViewport>(D3D11CmdType::None, viewportCount,
        [] (DxvkContext* ctx, const DxvkViewport* viewports, size_t count) {
          ctx->setViewports(count, viewports);
        });

      // Vulkan does not provide an easy way to disable the scissor test,
      // Set scissor rects that are at least as large as the framebuffer.
      bool enableScissorTest = m_state.rs.state && m_state.rs.state->Desc().ScissorEnable;

      // D3D11's coordinate system has its origin in the bottom left,
      // but the viewport coordinates are aligned to the top-left
      // corner so we can get away with flipping the viewport.
      for (uint32_t i = 0; i < viewportCount; i++) {
        const auto& vp = m_state.rs.viewports[i];

        auto* dst = new (m_csData->at(i)) DxvkViewport();
        dst->viewport.x = vp.TopLeftX;
        dst->viewport.y = vp.Height + vp.TopLeftY;
        dst->viewport.width = vp.Width;
        dst->viewport.height = -vp.Height;
        dst->viewport.minDepth = vp.MinDepth;
        dst->viewport.maxDepth = vp.MaxDepth;

        if (!enableScissorTest) {
          dst->scissor.offset = VkOffset2D { 0, 0 };
          dst->scissor.extent = VkExtent2D {
            D3D11_VIEWPORT_BOUNDS_MAX,
            D3D11_VIEWPORT_BOUNDS_MAX };
        } else if (i < m_state.rs.numScissors) {
          const auto& sr = m_state.rs.scissors[i];

          dst->scissor.offset = VkOffset2D { sr.left, sr.top };
          dst->scissor.extent = VkExtent2D {
            uint32_t(std::max(sr.left, sr.right) - sr.left),
            uint32_t(std::max(sr.top, sr.bottom) - sr.top) };
        }
      }
    } else {
      // The backend can't handle a viewport count of zero,
      // so we should at least specify one empty viewport
      EmitCs([] (DxvkContext* ctx) {
        DxvkViewport viewport = { };
        ctx->setViewports(1, &viewport);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BatchDraw(
    const VkDrawIndirectCommand&            draw) {
    if (unlikely(HasDirtyGraphicsBindings()))
      ApplyDirtyGraphicsBindings();

    if (unlikely(m_nptOmShared)) m_nptSharedDirty = true;

    // Batch consecutive draws if there are no state changes
    if (m_csDataType == D3D11CmdType::Draw) {
      auto* drawInfo = m_csChunk->pushData(m_csData, 1u);

      if (likely(drawInfo)) {
        new (drawInfo) VkDrawIndirectCommand(draw);
        return;
      }
    }

    EmitCsCmd<VkDrawIndirectCommand>(D3D11CmdType::Draw, 1u,
      [] (DxvkContext* ctx, const VkDrawIndirectCommand* draws, size_t count) {
        ctx->draw(count, draws);
      });

    new (m_csData->first()) VkDrawIndirectCommand(draw);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BatchDrawIndexed(
    const VkDrawIndexedIndirectCommand&     draw) {
    if (unlikely(HasDirtyGraphicsBindings()))
      ApplyDirtyGraphicsBindings();

    if (unlikely(m_nptOmShared)) m_nptSharedDirty = true;

    // Batch consecutive draws if there are no state changes
    if (m_csDataType == D3D11CmdType::DrawIndexed) {
      auto* drawInfo = m_csChunk->pushData(m_csData, 1u);

      if (likely(drawInfo)) {
        new (drawInfo) VkDrawIndexedIndirectCommand(draw);
        return;
      }
    }

    EmitCsCmd<VkDrawIndexedIndirectCommand>(D3D11CmdType::DrawIndexed, 1u,
      [] (DxvkContext* ctx, const VkDrawIndexedIndirectCommand* draws, size_t count) {
        ctx->drawIndexed(count, draws);
      });

    new (m_csData->first()) VkDrawIndexedIndirectCommand(draw);
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::BindShader(
    const D3D11CommonShader*    pShaderModule) {
    uint64_t oldUavMask = m_state.lazy.bindingsUsed[ShaderStage].uavMask;

    if (pShaderModule) {
      auto buffer = pShaderModule->GetIcb();
      auto shader = pShaderModule->GetShader();

      if (unlikely(shader->needsCompile()))
        m_device->requestCompileShader(shader);

      // If this shader activates any bindings that have not yet been applied,
      // mark the shader stage as dirty so it gets applied on the next draw.
      // Don't apply it right away since any dirty bindings are likely redundant.
      m_state.lazy.shadersUsed.set(ShaderStage);
      m_state.lazy.bindingsUsed[ShaderStage] = pShaderModule->GetBindingMask();

      if (!m_state.lazy.shadersDirty.test(ShaderStage) && (DebugLazyBinding != Tristate::False)) {
        if (!(m_state.lazy.bindingsDirty[ShaderStage] & m_state.lazy.bindingsUsed[ShaderStage]).empty())
          m_state.lazy.shadersDirty.set(ShaderStage);
      }

      EmitCs([
        cBuffer = std::move(buffer),
        cShader = std::move(shader)
      ] (DxvkContext* ctx) mutable {
        constexpr VkShaderStageFlagBits stage = GetShaderStage(ShaderStage);

        uint32_t slotId = D3D11ShaderResourceMapping::computeCbvBinding(ShaderStage,
          D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);

        ctx->bindShader<stage>(
          Forwarder::move(cShader));
        ctx->bindUniformBuffer(stage, slotId,
          Forwarder::move(cBuffer));
      });
    } else {
      // Mark shader stage as inactive and clean since we'll have no active
      // bindings. This works because if the app changes any binding at all
      // for this stage, it will get flagged as dirty, and if another shader
      // gets bound, it will check for any dirty bindings again.
      m_state.lazy.shadersUsed.clr(ShaderStage);
      m_state.lazy.shadersDirty.clr(ShaderStage);

      m_state.lazy.bindingsUsed[ShaderStage].reset();

      EmitCs([] (DxvkContext* ctx) {
        constexpr VkShaderStageFlagBits stage = GetShaderStage(ShaderStage);

        uint32_t slotId = D3D11ShaderResourceMapping::computeCbvBinding(ShaderStage,
          D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);

        ctx->bindShader<stage>(nullptr);
        ctx->bindUniformBuffer(stage, slotId, DxvkBufferSlice());
      });
    }

    // On graphics, UAVs are available to all stages, but we treat them as part
    // of the pixel shader binding set. Re-compute the active UAV mask. We don't
    // need to set the PS as active or dirty here though since the UAV update
    // code will mark all other stages that access UAVs as dirty, too.
    uint64_t newUavMask = m_state.lazy.bindingsUsed[ShaderStage].uavMask;

    if (ShaderStage != D3D11ShaderType::eCompute && oldUavMask != newUavMask) {
      constexpr D3D11ShaderType ps = D3D11ShaderType::ePixel;

      // Since dirty UAVs are only tracked on the PS mask, we need to mark the
      // stage as dirty if any of the used UAVs overlap with the dirty PS mask.
      if (m_state.lazy.bindingsDirty[ps].uavMask & newUavMask)
        m_state.lazy.shadersDirty.set(ShaderStage);

      // Accumulate graphics UAV mask and write it back to the pixel shader mask.
      m_state.lazy.graphicsUavShaders.clr(ShaderStage);

      for (uint32_t stageIndex : bit::BitMask(uint32_t(m_state.lazy.graphicsUavShaders.raw())))
        newUavMask |= m_state.lazy.bindingsUsed[D3D11ShaderType(stageIndex)].uavMask;

      m_state.lazy.bindingsUsed[ps].uavMask = newUavMask;

      // Update bit mask of shaders actively accessing graphics UAVs
      if (newUavMask)
        m_state.lazy.graphicsUavShaders.set(ShaderStage);
    }
  }


  static VkDepthBiasRepresentationEXT FormatToDepthBiasRepresentation(DXGI_FORMAT format) {
    switch (format) {
      default:
      case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      case DXGI_FORMAT_D32_FLOAT:
        return VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;

      case DXGI_FORMAT_D24_UNORM_S8_UINT:
      case DXGI_FORMAT_D16_UNORM:
        return VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
    }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindFramebuffer() {
    DxvkDepthBiasRepresentation depthBiasRepresentation =
      { VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT,
        m_device->features().extDepthBiasControl.depthBiasExact };
    NptUpdateOmShared();
    DxvkRenderTargets attachments;
    uint32_t sampleCount = 0;

    // D3D11 doesn't have the concept of a framebuffer object,
    // so we'll just create a new one every time the render
    // target bindings are updated. Set up the attachments.
    for (UINT i = 0; i < m_state.om.rtvs.size(); i++) {
      if (m_state.om.rtvs[i] != nullptr) {
        attachments.color[i].view = m_state.om.rtvs[i]->GetImageView();
        sampleCount = m_state.om.rtvs[i]->GetSampleCount();
      }
    }

    if (m_state.om.dsv != nullptr) {
      attachments.depth.view = m_state.om.dsv->GetImageView();
      sampleCount = m_state.om.dsv->GetSampleCount();

      if (m_device->features().extDepthBiasControl.leastRepresentableValueForceUnormRepresentation)
        depthBiasRepresentation.depthBiasRepresentation = FormatToDepthBiasRepresentation(m_state.om.dsv->GetViewFormat());
    }

    // Create and bind the framebuffer object to the context
    EmitCs([
      cAttachments    = std::move(attachments),
      cRepresentation = depthBiasRepresentation
    ] (DxvkContext* ctx) mutable {
      ctx->setDepthBiasRepresentation(cRepresentation);
      ctx->bindRenderTargets(Forwarder::move(cAttachments), 0u);
    });

    // If necessary, update push constant for the sample count
    if (m_state.om.sampleCount != sampleCount) {
      m_state.om.sampleCount = sampleCount;
      ApplyRasterizerSampleCount();
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindDrawBuffers(
          D3D11Buffer*                     pBufferForArgs,
          D3D11Buffer*                     pBufferForCount) {
    EmitCs([
      cArgBuffer = pBufferForArgs  ? pBufferForArgs->GetBufferSlice()  : DxvkBufferSlice(),
      cCntBuffer = pBufferForCount ? pBufferForCount->GetBufferSlice() : DxvkBufferSlice()
    ] (DxvkContext* ctx) mutable {
      ctx->bindDrawBuffers(
        Forwarder::move(cArgBuffer),
        Forwarder::move(cCntBuffer));
    });
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindVertexBuffer(
          UINT                              Slot,
          D3D11Buffer*                      pBuffer,
          UINT                              Offset,
          UINT                              Stride) {
    if (pBuffer) {
      EmitCs([
        cSlotId       = Slot,
        cBufferSlice  = pBuffer->GetBufferSlice(Offset),
        cStride       = Stride
      ] (DxvkContext* ctx) mutable {
        ctx->bindVertexBuffer(cSlotId,
          Forwarder::move(cBufferSlice),
          cStride);
      });
    } else {
      EmitCs([
        cSlotId       = Slot
      ] (DxvkContext* ctx) {
        ctx->bindVertexBuffer(cSlotId, DxvkBufferSlice(), 0);
      });
    }
  }
  
  
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindVertexBufferRange(
          UINT                              Slot,
          D3D11Buffer*                      pBuffer,
          UINT                              Offset,
          UINT                              Stride) {
    if (pBuffer) {
      VkDeviceSize offset = Offset;
      VkDeviceSize length = pBuffer->GetRemainingSize(Offset);

      EmitCs([
        cSlotId       = Slot,
        cBufferOffset = offset,
        cBufferLength = length,
        cStride       = Stride
      ] (DxvkContext* ctx) mutable {
        ctx->bindVertexBufferRange(cSlotId,
          cBufferOffset, cBufferLength, cStride);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindIndexBuffer(
          D3D11Buffer*                      pBuffer,
          UINT                              Offset,
          DXGI_FORMAT                       Format) {
    VkIndexType indexType = Format == DXGI_FORMAT_R16_UINT
      ? VK_INDEX_TYPE_UINT16
      : VK_INDEX_TYPE_UINT32;

    if (pBuffer) {
      EmitCs([
        cBufferSlice  = pBuffer->GetBufferSlice(Offset),
        cIndexType    = indexType
      ] (DxvkContext* ctx) mutable {
        ctx->bindIndexBuffer(
          Forwarder::move(cBufferSlice),
          cIndexType);
      });
    } else {
      EmitCs([
        cIndexType    = indexType
      ] (DxvkContext* ctx) {
        ctx->bindIndexBuffer(DxvkBufferSlice(), cIndexType);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindIndexBufferRange(
          D3D11Buffer*                      pBuffer,
          UINT                              Offset,
          DXGI_FORMAT                       Format) {
    if (pBuffer) {
      VkIndexType indexType = Format == DXGI_FORMAT_R16_UINT
        ? VK_INDEX_TYPE_UINT16
        : VK_INDEX_TYPE_UINT32;

      VkDeviceSize offset = Offset;
      VkDeviceSize length = pBuffer->GetRemainingSize(Offset);

      EmitCs([
        cBufferOffset = offset,
        cBufferLength = length,
        cIndexType    = indexType
      ] (DxvkContext* ctx) mutable {
        ctx->bindIndexBufferRange(
          cBufferOffset, cBufferLength,
          cIndexType);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindXfbBuffer(
          UINT                              Slot,
          D3D11Buffer*                      pBuffer,
          UINT                              Offset) {
    if (pBuffer) {
      EmitCs([
        cSlotId       = Slot,
        cOffset       = Offset,
        cBufferSlice  = pBuffer->GetBufferSlice(),
        cCounterSlice = pBuffer->GetSOCounter()
      ] (DxvkContext* ctx) mutable {
        if (cCounterSlice.defined() && cOffset != ~0u) {
          ctx->updateBuffer(
            cCounterSlice.buffer(),
            cCounterSlice.offset(),
            sizeof(cOffset),
            &cOffset);
        }

        ctx->bindXfbBuffer(cSlotId,
          Forwarder::move(cBufferSlice),
          Forwarder::move(cCounterSlice));
      });
    } else {
      EmitCs([
        cSlotId       = Slot
      ] (DxvkContext* ctx) {
        ctx->bindXfbBuffer(cSlotId,
          DxvkBufferSlice(),
          DxvkBufferSlice());
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindConstantBuffer(
          D3D11ShaderType                   ShaderStage,
          UINT                              Slot,
          D3D11Buffer*                      pBuffer,
          UINT                              Offset,
          UINT                              Length) {
    uint32_t slotId = D3D11ShaderResourceMapping::computeCbvBinding(ShaderStage, Slot);

    if (pBuffer) {
      EmitCs([
        cSlotId      = slotId,
        cStage       = GetShaderStage(ShaderStage),
        cBufferSlice = pBuffer->GetBufferSlice(16 * Offset, 16 * Length)
      ] (DxvkContext* ctx) mutable {
        ctx->bindUniformBuffer(cStage, cSlotId,
          Forwarder::move(cBufferSlice));
      });
    } else {
      EmitCs([
        cSlotId      = slotId,
        cStage       = GetShaderStage(ShaderStage)
      ] (DxvkContext* ctx) {
        ctx->bindUniformBuffer(cStage, cSlotId, DxvkBufferSlice());
      });
    }
  }
  
  
  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindConstantBufferRange(
          D3D11ShaderType                   ShaderStage,
          UINT                              Slot,
          UINT                              Offset,
          UINT                              Length) {
    uint32_t slotId = D3D11ShaderResourceMapping::computeCbvBinding(ShaderStage, Slot);

    EmitCs([
      cSlotId = slotId,
      cStage  = GetShaderStage(ShaderStage),
      cOffset = 16u * Offset,
      cLength = 16u * Length
    ] (DxvkContext* ctx) {
      ctx->bindUniformBufferRange(cStage, cSlotId, cOffset, cLength);
    });
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindSampler(
          D3D11ShaderType                   ShaderStage,
          UINT                              Slot,
          D3D11SamplerState*                pSampler) {
    uint32_t slotId = D3D11ShaderResourceMapping::computeSamplerBinding(ShaderStage, Slot);

    if (pSampler) {
      EmitCs([
        cSlotId   = slotId,
        cStage    = GetShaderStage(ShaderStage),
        cSampler  = pSampler->GetDXVKSampler()
      ] (DxvkContext* ctx) mutable {
        ctx->bindResourceSampler(cStage, cSlotId,
          Forwarder::move(cSampler));
      });
    } else {
      EmitCs([
        cSlotId   = slotId,
        cStage    = GetShaderStage(ShaderStage)
      ] (DxvkContext* ctx) {
        ctx->bindResourceSampler(cStage, cSlotId, nullptr);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindShaderResource(
          D3D11ShaderType                   ShaderStage,
          UINT                              Slot,
          D3D11ShaderResourceView*          pResource) {
    uint32_t slotId = D3D11ShaderResourceMapping::computeSrvBinding(ShaderStage, Slot);

    if (pResource) {
      if (pResource->GetViewInfo().Dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
        EmitCs([
          cSlotId = slotId,
          cStage  = GetShaderStage(ShaderStage),
          cView   = pResource->GetImageView()
        ] (DxvkContext* ctx) mutable {
          ctx->bindResourceImageView(cStage, cSlotId,
            Forwarder::move(cView));
        });
      } else {
        EmitCs([
          cSlotId = slotId,
          cStage  = GetShaderStage(ShaderStage),
          cView   = pResource->GetBufferView()
        ] (DxvkContext* ctx) mutable {
          ctx->bindResourceBufferView(cStage, cSlotId,
            Forwarder::move(cView));
        });
      }
    } else {
      EmitCs([
        cSlotId = slotId,
        cStage  = GetShaderStage(ShaderStage)
      ] (DxvkContext* ctx) {
        ctx->bindResourceImageView(cStage, cSlotId, nullptr);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::BindUnorderedAccessView(
          D3D11ShaderType                   ShaderStage,
          UINT                              Slot,
          D3D11UnorderedAccessView*         pUav) {
    uint32_t uavSlotId = D3D11ShaderResourceMapping::computeUavBinding(ShaderStage, Slot);
    uint32_t ctrSlotId = D3D11ShaderResourceMapping::computeUavCounterBinding(ShaderStage, Slot);

    VkShaderStageFlags stages = ShaderStage == D3D11ShaderType::eCompute
      ? VK_SHADER_STAGE_COMPUTE_BIT
      : VK_SHADER_STAGE_ALL_GRAPHICS;

    if (pUav) {
      if (pUav->GetViewInfo().Dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        EmitCs([
          cUavSlotId    = uavSlotId,
          cCtrSlotId    = ctrSlotId,
          cStages       = stages,
          cBufferView   = pUav->GetBufferView(),
          cCounterView  = pUav->GetCounterView()
        ] (DxvkContext* ctx) mutable {
          ctx->bindResourceBufferView(cStages, cUavSlotId,
            Forwarder::move(cBufferView));
          ctx->bindResourceBufferView(cStages, cCtrSlotId,
            Forwarder::move(cCounterView));
        });
      } else {
        EmitCs([
          cUavSlotId    = uavSlotId,
          cCtrSlotId    = ctrSlotId,
          cStages       = stages,
          cImageView    = pUav->GetImageView()
        ] (DxvkContext* ctx) mutable {
          ctx->bindResourceImageView(cStages, cUavSlotId,
            Forwarder::move(cImageView));
          ctx->bindResourceBufferView(cStages, cCtrSlotId, nullptr);
        });
      }
    } else {
      EmitCs([
        cUavSlotId    = uavSlotId,
        cCtrSlotId    = ctrSlotId,
        cStages       = stages
      ] (DxvkContext* ctx) {
        ctx->bindResourceImageView(cStages, cUavSlotId, nullptr);
        ctx->bindResourceBufferView(cStages, cCtrSlotId, nullptr);
      });
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ClearImageView(
          Rc<DxvkImageView>                 View,
    const FLOAT                             Color[4],
    const D3D11_RECT*                       pRects,
          UINT                              NumRects) {
    // 3D views are unsupported
    if (View->info().viewType == VK_IMAGE_VIEW_TYPE_3D)
      return;

    // Convert clear value
    auto clearValue = ConvertColorValue(Color, View->formatInfo());

    VkExtent3D extent3D = View->mipLevelExtent(0);
    VkExtent2D extent2D = { extent3D.width, extent3D.height };

    // Figure out which plane we're clearing for subsampling
    const DxvkPlaneFormatInfo* plane = nullptr;
    auto imageFormatInfo = View->image()->formatInfo();

    if (imageFormatInfo->flags.test(DxvkFormatFlag::MultiPlane))
      plane = &imageFormatInfo->planes[vk::getPlaneIndex(View->info().aspects)];

    // Clear all non-empty rectangles
    EmitCsCmd<VkRect2D>(D3D11CmdType::None, std::max(NumRects, 1u), [
      cView       = std::move(View),
      cClearValue = clearValue
    ] (DxvkContext* ctx, const VkRect2D* rects, size_t count) {
      constexpr VkImageUsageFlags rtUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      VkImageAspectFlags clearAspect = cView->formatInfo()->aspectMask & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT);

      for (size_t i = 0; i < count; i++) {
        VkOffset3D offset = { rects[i].offset.x, rects[i].offset.y, 0 };
        VkExtent3D extent = { rects[i].extent.width, rects[i].extent.height, 1u };

        if (extent.width && extent.height) {
          bool isFullSize = cView->mipLevelExtent(0) == extent;

          if ((cView->info().usage & rtUsage) && isFullSize)
            ctx->clearRenderTarget(cView, clearAspect, cClearValue, 0u);
          else
            ctx->clearImageView(cView, offset, extent, clearAspect, cClearValue);
        }
      }
    });

    if (NumRects) {
      for (uint32_t i = 0; i < NumRects; i++) {
        D3D11_RECT subsampledRect = pRects[i];

        if (plane) {
          subsampledRect.left   /= plane->blockSize.width;
          subsampledRect.top    /= plane->blockSize.height;
          subsampledRect.right  /= plane->blockSize.width;
          subsampledRect.bottom /= plane->blockSize.height;
        }

        new (m_csData->at(i)) VkRect2D(ConvertRect(subsampledRect, extent2D));
      }
    } else {
      auto vkRect = new (m_csData->first()) VkRect2D();
      vkRect->offset = VkOffset2D { 0, 0 };
      vkRect->extent = extent2D;
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ClearBufferView(
          Rc<DxvkBufferView>                View,
    const FLOAT                             Color[4],
    const D3D11_RECT*                       pRects,
          UINT                              NumRects) {
    // Convert clear value
    auto formatInfo = View->formatInfo();
    auto clearValue = ConvertColorValue(Color, formatInfo);

    // Just pass the rectangles through, even though we only need one dimension
    VkExtent2D extent2D = { uint32_t(View->info().size / formatInfo->elementSize), 1u };

    EmitCsCmd<VkRect2D>(D3D11CmdType::None, std::max(NumRects, 1u), [
      cView       = std::move(View),
      cClearValue = clearValue
    ] (DxvkContext* ctx, const VkRect2D* rects, size_t count) {
      for (size_t i = 0; i < count; i++) {
        if (rects[i].extent.width) {
          ctx->clearBufferView(cView,
            rects[i].offset.x, rects[i].extent.width,
            cClearValue.color);
        }
      }
    });

    if (NumRects) {
      for (uint32_t i = 0; i < NumRects; i++)
        new (m_csData->at(i)) VkRect2D(ConvertRect(pRects[i], extent2D));
    } else {
      auto vkRect = new (m_csData->first()) VkRect2D();
      vkRect->offset = VkOffset2D { 0, 0 };
      vkRect->extent = extent2D;
    }
  }


  template<typename ContextType>
  VkClearValue D3D11CommonContext<ContextType>::ConvertColorValue(
    const FLOAT                             Color[4],
    const DxvkFormatInfo*                   pFormatInfo) {
    VkClearValue result;

    if (pFormatInfo->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
      for (uint32_t i = 0; i < 4; i++) {
        if (pFormatInfo->flags.test(DxvkFormatFlag::SampledUInt))
          result.color.uint32[i] = uint32_t(std::max(0.0f, Color[i]));
        else if (pFormatInfo->flags.test(DxvkFormatFlag::SampledSInt))
          result.color.int32[i] = int32_t(Color[i]);
        else
          result.color.float32[i] = Color[i];
      }
    } else {
      result.depthStencil.depth = Color[0];
      result.depthStencil.stencil = 0;
    }

    return result;
  }


  template<typename ContextType>
  VkRect2D D3D11CommonContext<ContextType>::ConvertRect(
          D3D11_RECT                        Rect,
          VkExtent2D                        Extent) {
    Rect.left   = std::max<int32_t>(Rect.left,   0);
    Rect.top    = std::max<int32_t>(Rect.top,    0);
    Rect.right  = std::min<int32_t>(Rect.right,  Extent.width);
    Rect.bottom = std::min<int32_t>(Rect.bottom, Extent.height);

    if (Rect.left >= Rect.right || Rect.top >= Rect.bottom)
      return VkRect2D();

    VkRect2D result = { };
    result.offset.x = Rect.left;
    result.offset.y = Rect.top;
    result.extent.width = Rect.right - Rect.left;
    result.extent.height = Rect.bottom - Rect.top;
    return result;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::CopyBuffer(
          D3D11Buffer*                      pDstBuffer,
          VkDeviceSize                      DstOffset,
          D3D11Buffer*                      pSrcBuffer,
          VkDeviceSize                      SrcOffset,
          VkDeviceSize                      ByteCount) {
    // Clamp copy region to prevent out-of-bounds access
    VkDeviceSize dstLength = pDstBuffer->Desc()->ByteWidth;
    VkDeviceSize srcLength = pSrcBuffer->Desc()->ByteWidth;

    if (SrcOffset >= srcLength || DstOffset >= dstLength || !ByteCount)
      return;

    AddCost(GpuCostEstimate::Transfer);

    ByteCount = std::min(dstLength - DstOffset, ByteCount);
    ByteCount = std::min(srcLength - SrcOffset, ByteCount);

    EmitCs([
      cDstBuffer = pDstBuffer->GetBufferSlice(DstOffset, ByteCount),
      cSrcBuffer = pSrcBuffer->GetBufferSlice(SrcOffset, ByteCount)
    ] (DxvkContext* ctx) {
      if (cDstBuffer.buffer() != cSrcBuffer.buffer()) {
        ctx->copyBuffer(
          cDstBuffer.buffer(),
          cDstBuffer.offset(),
          cSrcBuffer.buffer(),
          cSrcBuffer.offset(),
          cSrcBuffer.length());
      } else {
        ctx->copyBufferRegion(
          cDstBuffer.buffer(),
          cDstBuffer.offset(),
          cSrcBuffer.offset(),
          cSrcBuffer.length());
      }
    });

    if (pDstBuffer->HasSequenceNumber())
      GetTypedContext()->TrackBufferSequenceNumber(pDstBuffer);
    if (pSrcBuffer->HasSequenceNumber())
      GetTypedContext()->TrackBufferSequenceNumber(pSrcBuffer);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::CopyImage(
          D3D11CommonTexture*               pDstTexture,
    const VkImageSubresourceLayers*         pDstLayers,
          VkOffset3D                        DstOffset,
          D3D11CommonTexture*               pSrcTexture,
    const VkImageSubresourceLayers*         pSrcLayers,
          VkOffset3D                        SrcOffset,
          VkExtent3D                        SrcExtent) {
    // Image formats must be size-compatible
    auto dstFormatInfo = lookupFormatInfo(pDstTexture->GetPackedFormat());
    auto srcFormatInfo = lookupFormatInfo(pSrcTexture->GetPackedFormat());

    if (dstFormatInfo->elementSize != srcFormatInfo->elementSize)
      return;

    // Sample counts must match
    if (pDstTexture->Desc()->SampleDesc.Count != pSrcTexture->Desc()->SampleDesc.Count)
      return;

    // Obviously, the copy region must not be empty
    VkExtent3D dstMipExtent = pDstTexture->MipLevelExtent(pDstLayers->mipLevel);
    VkExtent3D srcMipExtent = pSrcTexture->MipLevelExtent(pSrcLayers->mipLevel);

    if (uint32_t(DstOffset.x) >= dstMipExtent.width
     || uint32_t(DstOffset.y) >= dstMipExtent.height
     || uint32_t(DstOffset.z) >= dstMipExtent.depth)
      return;

    if (uint32_t(SrcOffset.x) >= srcMipExtent.width
     || uint32_t(SrcOffset.y) >= srcMipExtent.height
     || uint32_t(SrcOffset.z) >= srcMipExtent.depth)
      return;

    // Don't perform the copy if the offsets aren't block-aligned
    if (!util::isBlockAligned(SrcOffset, srcFormatInfo->blockSize)
     || !util::isBlockAligned(DstOffset, dstFormatInfo->blockSize))
      return;

    // Clamp the image region in order to avoid out-of-bounds access
    VkExtent3D blockCount    = util::computeBlockCount(SrcExtent, srcFormatInfo->blockSize);
    VkExtent3D dstBlockCount = util::computeMaxBlockCount(DstOffset, dstMipExtent, dstFormatInfo->blockSize);
    VkExtent3D srcBlockCount = util::computeMaxBlockCount(SrcOffset, srcMipExtent, srcFormatInfo->blockSize);

    blockCount = util::minExtent3D(blockCount, dstBlockCount);
    blockCount = util::minExtent3D(blockCount, srcBlockCount);

    SrcExtent = util::computeBlockExtent(blockCount, srcFormatInfo->blockSize);
    SrcExtent = util::snapExtent3D(SrcOffset, SrcExtent, srcMipExtent);

    if (!SrcExtent.width || !SrcExtent.height || !SrcExtent.depth)
      return;

    AddCost(GpuCostEstimate::Transfer);

    // While copying between 2D and 3D images is allowed in CopySubresourceRegion,
    // copying more than one slice at a time is not suppoted. Layer counts are 1.
    if ((pDstTexture->GetVkImageType() == VK_IMAGE_TYPE_3D)
     != (pSrcTexture->GetVkImageType() == VK_IMAGE_TYPE_3D))
      SrcExtent.depth = 1;

    // Certain types of copies require us to pass the destination extent to
    // the backend. This may be different when copying between compressed
    // and uncompressed image formats.
    VkExtent3D dstExtent = util::computeBlockExtent(blockCount, dstFormatInfo->blockSize);
    dstExtent = util::snapExtent3D(DstOffset, dstExtent, dstMipExtent);

    // It is possible for any of the given images to be a staging image with
    // no actual image, so we need to account for all possibilities here.
    bool dstIsImage = pDstTexture->HasImage();
    bool srcIsImage = pSrcTexture->HasImage();

    if (dstIsImage && srcIsImage) {
      EmitCs([
        cDstImage  = pDstTexture->GetImage(),
        cSrcImage  = pSrcTexture->GetImage(),
        cDstLayers = *pDstLayers,
        cSrcLayers = *pSrcLayers,
        cDstOffset = DstOffset,
        cSrcOffset = SrcOffset,
        cExtent    = SrcExtent
      ] (DxvkContext* ctx) {
        // CopyResource can only copy between different images, and
        // CopySubresourceRegion can only copy data from one single
        // subresource at a time, so this check is safe.
        if (cDstImage != cSrcImage || cDstLayers != cSrcLayers) {
          ctx->copyImage(
            cDstImage, cDstLayers, cDstOffset,
            cSrcImage, cSrcLayers, cSrcOffset,
            cExtent);
        } else {
          ctx->copyImageRegion(
            cDstImage, cDstLayers, cDstOffset,
            cSrcOffset, cExtent);
        }
      });
    } else {
      // Since each subresource uses a dedicated buffer, we are going
      // to need one call per subresource for staging resource copies
      for (uint32_t i = 0; i < pDstLayers->layerCount; i++) {
        uint32_t dstSubresource = D3D11CalcSubresource(pDstLayers->mipLevel, pDstLayers->baseArrayLayer + i, pDstTexture->Desc()->MipLevels);
        uint32_t srcSubresource = D3D11CalcSubresource(pSrcLayers->mipLevel, pSrcLayers->baseArrayLayer + i, pSrcTexture->Desc()->MipLevels);

        // For multi-plane image data stored in a buffer, the backend
        // assumes that the second plane immediately follows the first
        // plane in memory, which is only true if we copy the full image.
        uint32_t planeCount = 1;

        if (dstFormatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
          bool needsSeparateCopies = !dstIsImage && !srcIsImage;

          if (!dstIsImage)
            needsSeparateCopies |= pDstTexture->MipLevelExtent(pDstLayers->mipLevel) != SrcExtent;
          if (!srcIsImage)
            needsSeparateCopies |= pSrcTexture->MipLevelExtent(pSrcLayers->mipLevel) != SrcExtent;

          if (needsSeparateCopies)
            planeCount = vk::getPlaneCount(srcFormatInfo->aspectMask);
        }

        for (uint32_t j = 0; j < planeCount; j++) {
          VkImageAspectFlags dstAspectMask = dstFormatInfo->aspectMask;
          VkImageAspectFlags srcAspectMask = srcFormatInfo->aspectMask;

          if (planeCount > 1) {
            dstAspectMask = vk::getPlaneAspect(j);
            srcAspectMask = dstAspectMask;
          }

          if (dstIsImage) {
            VkImageSubresourceLayers dstLayer = { dstAspectMask,
              pDstLayers->mipLevel, pDstLayers->baseArrayLayer + i, 1 };

            EmitCs([
              cDstImage   = pDstTexture->GetImage(),
              cDstLayers  = dstLayer,
              cDstOffset  = DstOffset,
              cDstExtent  = dstExtent,
              cDstFormat  = pDstTexture->GetPackedFormat(),
              cSrcBuffer  = pSrcTexture->GetMappedBuffer(srcSubresource),
              cSrcLayout  = pSrcTexture->GetSubresourceLayout(srcAspectMask, srcSubresource),
              cSrcOffset  = pSrcTexture->ComputeMappedOffset(srcSubresource, j, SrcOffset),
              cSrcCoord   = SrcOffset,
              cSrcExtent  = srcMipExtent
            ] (DxvkContext* ctx) {
              ctx->copyBufferToImage(cDstImage, cDstLayers, cDstOffset, cDstExtent,
                cSrcBuffer, cSrcOffset, cSrcLayout.RowPitch, cSrcLayout.DepthPitch,
                cDstFormat);
            });
          } else if (srcIsImage) {
            VkImageSubresourceLayers srcLayer = { srcAspectMask,
              pSrcLayers->mipLevel, pSrcLayers->baseArrayLayer + i, 1 };

            EmitCs([
              cSrcImage   = pSrcTexture->GetImage(),
              cSrcFormat  = pSrcTexture->GetPackedFormat(),
              cSrcLayers  = srcLayer,
              cSrcOffset  = SrcOffset,
              cSrcExtent  = SrcExtent,
              cDstBuffer  = pDstTexture->GetMappedBuffer(dstSubresource),
              cDstLayout  = pDstTexture->GetSubresourceLayout(dstAspectMask, dstSubresource),
              cDstOffset  = pDstTexture->ComputeMappedOffset(dstSubresource, j, DstOffset),
              cDstCoord   = DstOffset,
              cDstExtent  = dstMipExtent
            ] (DxvkContext* ctx) {
              ctx->copyImageToBuffer(cDstBuffer, cDstOffset, cDstLayout.RowPitch,
                cDstLayout.DepthPitch, cSrcFormat, cSrcImage, cSrcLayers, cSrcOffset, cSrcExtent);
            });
          } else {
            // The backend is not aware of image metadata in this case,
            // so we need to handle image planes and block sizes here
            VkDeviceSize elementSize = dstFormatInfo->elementSize;
            VkExtent3D dstBlockSize = dstFormatInfo->blockSize;
            VkExtent3D srcBlockSize = srcFormatInfo->blockSize;
            VkExtent3D planeBlockSize = { 1u, 1u, 1u };

            if (planeCount > 1) {
              auto plane = &dstFormatInfo->planes[j];
              dstBlockSize.width  *= plane->blockSize.width;
              dstBlockSize.height *= plane->blockSize.height;
              srcBlockSize.width  *= plane->blockSize.width;
              srcBlockSize.height *= plane->blockSize.height;

              planeBlockSize.width  = plane->blockSize.width;
              planeBlockSize.height = plane->blockSize.height;
              elementSize = plane->elementSize;
            }

            EmitCs([
              cPixelSize = elementSize,
              cSrcBuffer = pSrcTexture->GetMappedBuffer(srcSubresource),
              cSrcStart  = pSrcTexture->GetSubresourceLayout(srcAspectMask, srcSubresource).Offset,
              cSrcOffset = util::computeBlockOffset(SrcOffset, srcBlockSize),
              cSrcSize   = util::computeBlockCount(srcMipExtent, srcBlockSize),
              cDstBuffer = pDstTexture->GetMappedBuffer(dstSubresource),
              cDstStart  = pDstTexture->GetSubresourceLayout(dstAspectMask, dstSubresource).Offset,
              cDstOffset = util::computeBlockOffset(DstOffset, dstBlockSize),
              cDstSize   = util::computeBlockCount(dstMipExtent, dstBlockSize),
              cExtent    = util::computeBlockCount(blockCount, planeBlockSize)
            ] (DxvkContext* ctx) {
              ctx->copyPackedBufferImage(
                cDstBuffer, cDstStart, cDstOffset, cDstSize,
                cSrcBuffer, cSrcStart, cSrcOffset, cSrcSize,
                cExtent, cPixelSize);
            });
          }
        }
      }
    }

    if (pDstTexture->HasSequenceNumber()) {
      for (uint32_t i = 0; i < pDstLayers->layerCount; i++) {
        GetTypedContext()->TrackTextureSequenceNumber(pDstTexture, D3D11CalcSubresource(
          pDstLayers->mipLevel, pDstLayers->baseArrayLayer + i, pDstTexture->Desc()->MipLevels));
      }
    }

    if (pSrcTexture->HasSequenceNumber()) {
      for (uint32_t i = 0; i < pSrcLayers->layerCount; i++) {
        GetTypedContext()->TrackTextureSequenceNumber(pSrcTexture, D3D11CalcSubresource(
          pSrcLayers->mipLevel, pSrcLayers->baseArrayLayer + i, pSrcTexture->Desc()->MipLevels));
      }
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::CopyTiledResourceData(
          ID3D11Resource*                   pResource,
    const D3D11_TILED_RESOURCE_COORDINATE*  pRegionCoordinate,
    const D3D11_TILE_REGION_SIZE*           pRegionSize,
          DxvkBufferSlice                   BufferSlice,
          UINT                              Flags) {
    Rc<DxvkPagedResource> resource = GetPagedResource(pResource);

    // Do some validation based on page table properties
    auto pageTable = resource->getSparsePageTable();

    if (!pageTable)
      return;

    if (pRegionSize->bUseBox && pRegionSize->NumTiles !=
        pRegionSize->Width * pRegionSize->Height * pRegionSize->Depth)
      return;

    if (pRegionSize->NumTiles > pageTable->getPageCount())
      return;

    // Ignore call if buffer access would be out of bounds
    VkDeviceSize bufferSize = pRegionSize->NumTiles * SparseMemoryPageSize;

    if (BufferSlice.length() < bufferSize)
      return;

    // Compute list of tile indices to copy
    std::vector<uint32_t> tiles(pRegionSize->NumTiles);

    for (uint32_t i = 0; i < pRegionSize->NumTiles; i++) {
      VkOffset3D regionOffset = {
        int32_t(pRegionCoordinate->X),
        int32_t(pRegionCoordinate->Y),
        int32_t(pRegionCoordinate->Z) };

      VkExtent3D regionExtent = {
        uint32_t(pRegionSize->Width),
        uint32_t(pRegionSize->Height),
        uint32_t(pRegionSize->Depth) };

      uint32_t tile = pageTable->computePageIndex(
        pRegionCoordinate->Subresource, regionOffset,
        regionExtent, !pRegionSize->bUseBox, i);

      // Check that the tile is valid and not part of the mip tail
      auto tileInfo = pageTable->getPageInfo(tile);

      if (tileInfo.type != DxvkSparsePageType::Buffer
       && tileInfo.type != DxvkSparsePageType::Image)
        return;

      tiles[i] = tile;
    }

    AddCost(GpuCostEstimate::Transfer);

    // If D3D12 is anything to go by, not passing this flag will trigger
    // the other code path, regardless of whether TO_LINEAR_BUFFER is set.
    if (Flags & D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) {
      EmitCs([
        cResource = std::move(resource),
        cTiles    = std::move(tiles),
        cBuffer   = std::move(BufferSlice)
      ] (DxvkContext* ctx) {
        ctx->copySparsePagesFromBuffer(
          cResource,
          cTiles.size(),
          cTiles.data(),
          cBuffer.buffer(),
          cBuffer.offset());
      });
    } else {
      EmitCs([
        cResource = std::move(resource),
        cTiles    = std::move(tiles),
        cBuffer   = std::move(BufferSlice)
      ] (DxvkContext* ctx) {
        ctx->copySparsePagesToBuffer(
          cBuffer.buffer(),
          cBuffer.offset(),
          cResource,
          cTiles.size(),
          cTiles.data());
      });
    }
  }


  template<typename ContextType>
  template<typename T>
  bool D3D11CommonContext<ContextType>::DirtyBindingGeneric(
          D3D11ShaderType                   ShaderStage,
          T                                 BoundMask,
          T&                                DirtyMask,
          T                                 DirtyBit,
          bool                              IsNull) {
    // Forward immediately if lazy binding is forced off
    if (DebugLazyBinding == Tristate::False)
      return false;

    if ((BoundMask & ~DirtyMask) & DirtyBit) {
      // If we're binding a non-null resource to an active slot that has not been
      // marked for lazy binding yet, forward the call immediately in order to
      // avoid tracking overhead. This is by far the most common case.
      if (likely(!IsNull && DebugLazyBinding != Tristate::True))
        return false;

      // If we are binding a null resource to an active slot, the app will likely
      // either bind something else or bind a shader that does not use this slot.
      // In that case, avoid likely redundant CS traffic and apply the binding on
      // the next draw.
      m_state.lazy.shadersDirty.set(ShaderStage);
    }

    // Binding is either inactive or already dirty. In the inactive case, there
    // is no need to mark the shader stage as dirty since binding a shader that
    // activates the binding will implicitly do so.
    DirtyMask |= DirtyBit;
    return true;
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::DirtyConstantBuffer(
          D3D11ShaderType                   ShaderStage,
          uint32_t                          Slot,
          bool                              IsNull) {
    return DirtyBindingGeneric(ShaderStage,
      m_state.lazy.bindingsUsed[ShaderStage].cbvMask,
      m_state.lazy.bindingsDirty[ShaderStage].cbvMask,
      1u << Slot, IsNull);
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::DirtySampler(
          D3D11ShaderType                   ShaderStage,
          uint32_t                          Slot,
          bool                              IsNull) {
    return DirtyBindingGeneric(ShaderStage,
      m_state.lazy.bindingsUsed[ShaderStage].samplerMask,
      m_state.lazy.bindingsDirty[ShaderStage].samplerMask,
      1u << Slot, IsNull);
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::DirtyShaderResource(
          D3D11ShaderType                   ShaderStage,
          uint32_t                          Slot,
          bool                              IsNull) {
    uint32_t idx = Slot / 64u;

    return DirtyBindingGeneric(ShaderStage,
      m_state.lazy.bindingsUsed[ShaderStage].srvMask[idx],
      m_state.lazy.bindingsDirty[ShaderStage].srvMask[idx],
      uint64_t(1u) << Slot, IsNull);
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::DirtyComputeUnorderedAccessView(
          uint32_t                          Slot,
          bool                              IsNull) {
    constexpr D3D11ShaderType ShaderStage = D3D11ShaderType::eCompute;

    return DirtyBindingGeneric(ShaderStage,
      m_state.lazy.bindingsUsed[ShaderStage].uavMask,
      m_state.lazy.bindingsDirty[ShaderStage].uavMask,
      uint64_t(1u) << Slot, IsNull);
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::DirtyGraphicsUnorderedAccessView(
          uint32_t                          Slot) {
    constexpr D3D11ShaderType ShaderStage = D3D11ShaderType::ePixel;

    if (DebugLazyBinding == Tristate::False)
      return false;

    // Use different logic here and always use lazy binding for graphics UAVs.
    // Since graphics UAVs are generally bound together with render targets,
    // looking at the active binding mask doesn't really help us here.
    uint64_t dirtyBit = uint64_t(1u) << Slot;

    if (m_state.lazy.bindingsUsed[ShaderStage].uavMask & dirtyBit) {
      // Need to mark all graphics stages that use UAVs as dirty here to
      // make sure that bindings actually get reapplied properly. There
      // may be no pixel shader bound in this case, even though we do
      // all the tracking on the pixel shader bit mask.
      m_state.lazy.shadersDirty.set(m_state.lazy.graphicsUavShaders);
    }

    m_state.lazy.bindingsDirty[ShaderStage].uavMask |= dirtyBit;
    return true;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::DiscardBuffer(
          ID3D11Resource*                   pResource) {
    auto buffer = static_cast<D3D11Buffer*>(pResource);

    if (buffer->GetMapMode() != D3D11_COMMON_BUFFER_MAP_MODE_NONE) {
      D3D11_MAPPED_SUBRESOURCE sr;

      Map(pResource, 0, D3D11_MAP_WRITE_DISCARD, 0, &sr);
      Unmap(pResource, 0);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::DiscardTexture(
          ID3D11Resource*                   pResource,
          UINT                              Subresource) {
    auto texture = GetCommonTexture(pResource);

    if (texture->GetMapMode() != D3D11_COMMON_TEXTURE_MAP_MODE_NONE) {
      D3D11_MAPPED_SUBRESOURCE sr;

      Map(pResource, Subresource, D3D11_MAP_WRITE_DISCARD, 0, &sr);
      Unmap(pResource, Subresource);
    }
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::GetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer**                    ppConstantBuffers,
          UINT*                             pFirstConstant,
          UINT*                             pNumConstants) {
    const auto& bindings = m_state.cbv[ShaderStage];

    for (uint32_t i = 0; i < NumBuffers; i++) {
      const bool inRange = StartSlot + i < bindings.buffers.size();

      if (ppConstantBuffers) {
        ppConstantBuffers[i] = inRange
          ? bindings.buffers[StartSlot + i].buffer.ref()
          : nullptr;
      }

      if (pFirstConstant) {
        pFirstConstant[i] = inRange
          ? bindings.buffers[StartSlot + i].constantOffset
          : 0u;
      }

      if (pNumConstants) {
        pNumConstants[i] = inRange
          ? bindings.buffers[StartSlot + i].constantCount
          : 0u;
      }
    }
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::GetShaderResources(
          UINT                              StartSlot,
          UINT                              NumViews,
          ID3D11ShaderResourceView**        ppShaderResourceViews) {
    const auto& bindings = m_state.srv[ShaderStage];

    for (uint32_t i = 0; i < NumViews; i++) {
      ppShaderResourceViews[i] = StartSlot + i < bindings.views.size()
        ? bindings.views[StartSlot + i].ref()
        : nullptr;
    }
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::GetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState**              ppSamplers) {
    const auto& bindings = m_state.samplers[ShaderStage];

    for (uint32_t i = 0; i < NumSamplers; i++) {
      ppSamplers[i] = StartSlot + i < bindings.samplers.size()
        ? bindings.samplers[StartSlot + i].ref()
        : nullptr;
    }
  }


  template<typename ContextType>
  DxvkGlobalPipelineBarrier D3D11CommonContext<ContextType>::GetTiledResourceDependency(
          ID3D11DeviceChild*                pObject) {
    if (!pObject) {
      DxvkGlobalPipelineBarrier result;
      result.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      result.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
      return result;
    } else {
      Com<ID3D11Resource> resource;

      if (FAILED(pObject->QueryInterface(IID_PPV_ARGS(&resource)))) {
        Com<ID3D11View> view;

        if (FAILED(pObject->QueryInterface(IID_PPV_ARGS(&view))))
          return DxvkGlobalPipelineBarrier();

        view->GetResource(&resource);
      }

      D3D11CommonTexture* texture = GetCommonTexture(resource.ptr());

      if (texture) {
        Rc<DxvkImage> image = texture->GetImage();

        DxvkGlobalPipelineBarrier result;
        result.stages = image->info().stages;
        result.access = image->info().access;
        return result;
      } else {
        Rc<DxvkBuffer> buffer = static_cast<D3D11Buffer*>(resource.ptr())->GetBuffer();

        if (buffer == nullptr)
          return DxvkGlobalPipelineBarrier();

        DxvkGlobalPipelineBarrier result;
        result.stages = buffer->info().stages;
        result.access = buffer->info().access;
        return result;
      }
    }
  }


  template<typename ContextType>
  D3D11MaxUsedBindings D3D11CommonContext<ContextType>::GetMaxUsedBindings() {
    D3D11MaxUsedBindings result;

    for (uint32_t i = 0; i < result.stages.size(); i++) {
      auto stage = D3D11ShaderType(i);

      result.stages[i].cbvCount = m_state.cbv[stage].maxCount;
      result.stages[i].srvCount = m_state.srv[stage].maxCount;
      result.stages[i].uavCount = 0;
      result.stages[i].samplerCount = m_state.samplers[stage].maxCount;
      result.stages[i].reserved = 0;
    }

    result.stages[uint32_t(D3D11ShaderType::ePixel)].uavCount = m_state.om.maxUav;
    result.stages[uint32_t(D3D11ShaderType::eCompute)].uavCount = m_state.uav.maxCount;

    result.vbCount = m_state.ia.maxVbCount;
    result.soCount = D3D11_SO_BUFFER_SLOT_COUNT;
    return result;
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::HasDirtyComputeBindings() {
    return m_state.lazy.shadersDirty.test(D3D11ShaderType::eCompute);
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::HasDirtyGraphicsBindings() {
    return (m_state.lazy.shadersDirty & m_state.lazy.shadersUsed).any(
      D3D11ShaderType::eVertex, D3D11ShaderType::eGeometry,
      D3D11ShaderType::eHull,   D3D11ShaderType::eDomain,
      D3D11ShaderType::ePixel);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ResetCommandListState() {
    EmitCs([
      cUsedBindings  = GetMaxUsedBindings(),
      cMaxTessFactor = m_maxTessFactor
    ] (DxvkContext* ctx) {
      // Reset render targets
      ctx->bindRenderTargets(DxvkRenderTargets(), 0u);

      // Reset vertex input state
      ctx->setInputLayout(0, nullptr, 0, nullptr);

      // Reset render states
      ctx->setInputAssemblyState(InitDefaultPrimitiveTopology());
      ctx->setDepthStencilState(InitDefaultDepthStencilState());
      ctx->setRasterizerState(InitDefaultRasterizerState());
      ctx->setDepthBias(DxvkDepthBias());
      ctx->setLogicOpState(InitDefaultLogicOpState());
      ctx->setMultisampleState(InitDefaultMultisampleState(D3D11_DEFAULT_SAMPLE_MASK));

      DxvkBlendMode cbState = InitDefaultBlendState();

      for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        ctx->setBlendMode(i, cbState);

      // Reset dynamic states
      ctx->setBlendConstants(DxvkBlendConstants { 1.0f, 1.0f, 1.0f, 1.0f });
      ctx->setStencilReference(D3D11_DEFAULT_STENCIL_REFERENCE);

      // Reset viewports
      DxvkViewport viewport = { };
      ctx->setViewports(1, &viewport);

      // Unbind indirect draw buffer
      ctx->bindDrawBuffers(DxvkBufferSlice(), DxvkBufferSlice());

      // Unbind index and vertex buffers
      ctx->bindIndexBuffer(DxvkBufferSlice(), VK_INDEX_TYPE_UINT32);

      for (uint32_t i = 0; i < cUsedBindings.vbCount; i++)
        ctx->bindVertexBuffer(i, DxvkBufferSlice(), 0);

      // Unbind transform feedback buffers
      for (uint32_t i = 0; i < cUsedBindings.soCount; i++)
        ctx->bindXfbBuffer(i, DxvkBufferSlice(), DxvkBufferSlice());

      // Unbind all shaders
      ctx->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(nullptr);
      ctx->bindShader<VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT>(nullptr);
      ctx->bindShader<VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT>(nullptr);
      ctx->bindShader<VK_SHADER_STAGE_GEOMETRY_BIT>(nullptr);
      ctx->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(nullptr);
      ctx->bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(nullptr);

      // Unbind per-shader stage resources
      for (uint32_t i = 0; i < 6; i++) {
        auto programType = D3D11ShaderType(i);
        auto stage = GetShaderStage(programType);

        // Unbind constant buffers, including the shader's ICB and instance data buffer
        auto cbSlotId = D3D11ShaderResourceMapping::computeCbvBinding(programType, 0);
        ctx->bindUniformBuffer(stage, cbSlotId + D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT + 0u, DxvkBufferSlice());
        ctx->bindUniformBuffer(stage, cbSlotId + D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT + 1u, DxvkBufferSlice());

        for (uint32_t j = 0; j < cUsedBindings.stages[i].cbvCount; j++)
          ctx->bindUniformBuffer(stage, cbSlotId + j, DxvkBufferSlice());

        // Unbind shader resource views
        auto srvSlotId = D3D11ShaderResourceMapping::computeSrvBinding(programType, 0);

        for (uint32_t j = 0; j < cUsedBindings.stages[i].srvCount; j++)
          ctx->bindResourceImageView(stage, srvSlotId + j, nullptr);

        // Unbind texture samplers
        auto samplerSlotId = D3D11ShaderResourceMapping::computeSamplerBinding(programType, 0);

        for (uint32_t j = 0; j < cUsedBindings.stages[i].samplerCount; j++)
          ctx->bindResourceSampler(stage, samplerSlotId + j, nullptr);

        // Unbind UAVs for supported stages
        if (programType == D3D11ShaderType::ePixel
         || programType == D3D11ShaderType::eCompute) {
          VkShaderStageFlags stages = programType == D3D11ShaderType::ePixel
            ? VK_SHADER_STAGE_ALL_GRAPHICS
            : VK_SHADER_STAGE_COMPUTE_BIT;

          auto uavSlotId = D3D11ShaderResourceMapping::computeUavBinding(programType, 0);
          auto ctrSlotId = D3D11ShaderResourceMapping::computeUavCounterBinding(programType, 0);

          for (uint32_t j = 0; j < cUsedBindings.stages[i].uavCount; j++) {
            ctx->bindResourceImageView(stages, uavSlotId, nullptr);
            ctx->bindResourceBufferView(stages, ctrSlotId, nullptr);
          }
        }
      }

      // Initialize push constants
      DxvkBuiltInPushData pc(1u, cMaxTessFactor);
      ctx->pushData(VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
    });
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ResetContextState() {
    // Reset shaders
    m_state.vs = nullptr;
    m_state.hs = nullptr;
    m_state.ds = nullptr;
    m_state.gs = nullptr;
    m_state.ps = nullptr;
    m_state.cs = nullptr;

    // Reset render state
    m_state.id.reset();
    m_state.ia.reset();
    m_state.om.reset();
    m_state.rs.reset();
    m_state.so.reset();
    m_state.pr.reset();

    // Reset resource bindings
    m_state.cbv.reset();
    m_state.srv.reset();
    m_state.uav.reset();
    m_state.samplers.reset();

    // Reset dirty tracking
    m_state.lazy.reset();

    // Reset class instances
    m_state.instances.reset();
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ResetDirtyTracking() {
    // Must only be called when all bindings are guaranteed to get applied
    // to the DXVK context before the next draw or dispatch command.
    m_state.lazy.bindingsDirty.reset();
    m_state.lazy.shadersDirty = 0u;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ResetStagingBuffer() {
    m_staging.reset();
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage, typename T>
  void D3D11CommonContext<ContextType>::ResolveSrvHazards(
          T*                                pView) {
    auto& bindings = m_state.srv[ShaderStage];
    int32_t srvId = bindings.hazardous.findNext(0);

    while (srvId >= 0) {
      auto srv = bindings.views[srvId].ptr();

      if (likely(srv && srv->TestHazards())) {
        bool hazard = CheckViewOverlap(pView, srv);

        if (unlikely(hazard)) {
          bindings.views[srvId] = nullptr;
          bindings.hazardous.clr(srvId);

          if (!DirtyShaderResource(ShaderStage, srvId, true))
            BindShaderResource(ShaderStage, srvId, nullptr);
        }
      } else {
        // Avoid further redundant iterations
        bindings.hazardous.clr(srvId);
      }

      srvId = bindings.hazardous.findNext(srvId + 1);
    }
  }


  template<typename ContextType>
  template<typename T>
  void D3D11CommonContext<ContextType>::ResolveCsSrvHazards(
          T*                                pView) {
    if (!pView) return;
    ResolveSrvHazards<D3D11ShaderType::eCompute>(pView);
  }


  template<typename ContextType>
  template<typename T>
  void D3D11CommonContext<ContextType>::ResolveOmSrvHazards(
          T*                                pView) {
    if (!pView) return;
    ResolveSrvHazards<D3D11ShaderType::eVertex>(pView);
    ResolveSrvHazards<D3D11ShaderType::eHull>(pView);
    ResolveSrvHazards<D3D11ShaderType::eDomain>(pView);
    ResolveSrvHazards<D3D11ShaderType::eGeometry>(pView);
    ResolveSrvHazards<D3D11ShaderType::ePixel>(pView);
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::ResolveOmRtvHazards(
          D3D11UnorderedAccessView*         pView) {
    if (!pView || !pView->HasBindFlag(D3D11_BIND_RENDER_TARGET))
      return false;

    bool hazard = false;

    if (CheckViewOverlap(pView, m_state.om.dsv.ptr())) {
      m_state.om.dsv = nullptr;
      hazard = true;
    }

    for (uint32_t i = 0; i < m_state.om.maxRtv; i++) {
      if (CheckViewOverlap(pView, m_state.om.rtvs[i].ptr())) {
        m_state.om.rtvs[i] = nullptr;
        hazard = true;
      }
    }

    return hazard;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::ResolveOmUavHazards(
          D3D11RenderTargetView*            pView) {
    if (!pView || !pView->HasBindFlag(D3D11_BIND_UNORDERED_ACCESS))
      return;

    for (uint32_t i = 0; i < m_state.om.maxUav; i++) {
      if (CheckViewOverlap(pView, m_state.om.uavs[i].ptr())) {
        m_state.om.uavs[i] = nullptr;

        if (!DirtyGraphicsUnorderedAccessView(i))
          BindUnorderedAccessView(D3D11ShaderType::ePixel, i, nullptr);
      }
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::RestoreCommandListState() {
    BindFramebuffer();

    BindShader<D3D11ShaderType::eVertex>(GetCommonShader(m_state.vs.ptr()));
    BindShader<D3D11ShaderType::eHull>(GetCommonShader(m_state.hs.ptr()));
    BindShader<D3D11ShaderType::eDomain>(GetCommonShader(m_state.ds.ptr()));
    BindShader<D3D11ShaderType::eGeometry>(GetCommonShader(m_state.gs.ptr()));
    BindShader<D3D11ShaderType::ePixel>(GetCommonShader(m_state.ps.ptr()));
    BindShader<D3D11ShaderType::eCompute>(GetCommonShader(m_state.cs.ptr()));

    ApplyInputLayout();
    ApplyPrimitiveTopology();
    ApplyBlendState();
    ApplyBlendFactor();
    ApplyDepthStencilState();
    ApplyStencilRef();
    ApplyRasterizerState();
    ApplyRasterizerSampleCount();
    ApplyViewportState();

    BindIndexBuffer(
      m_state.ia.indexBuffer.buffer.ptr(),
      m_state.ia.indexBuffer.offset,
      m_state.ia.indexBuffer.format);

    for (uint32_t i = 0; i < m_state.ia.maxVbCount; i++) {
      BindVertexBuffer(i,
        m_state.ia.vertexBuffers[i].buffer.ptr(),
        m_state.ia.vertexBuffers[i].offset,
        m_state.ia.vertexBuffers[i].stride);
    }

    for (uint32_t i = 0; i < m_state.so.targets.size(); i++)
      BindXfbBuffer(i, m_state.so.targets[i].buffer.ptr(), ~0u);

    // Reset dirty binding and shader masks before applying
    // bindings to avoid implicit null binding overrids.
    ResetDirtyTracking();

    for (uint32_t i = 0; i < D3D11ShaderTypeCount; i++) {
      auto stage = D3D11ShaderType(i);

      RestoreConstantBuffers(stage);
      RestoreShaderResources(stage);
      RestoreSamplers(stage);
    }

    RestoreUnorderedAccessViews(D3D11ShaderType::ePixel);
    RestoreUnorderedAccessViews(D3D11ShaderType::eCompute);

    // Draw buffer bindings aren't persistent at the API level, and
    // we can't meaningfully track them. Just reset this state here
    // and reapply on the next indirect draw.
    SetDrawBuffers(nullptr, nullptr);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::RestoreConstantBuffers(
          D3D11ShaderType                   Stage) {
    const auto& bindings = m_state.cbv[Stage];

    for (uint32_t i = 0; i < bindings.maxCount; i++) {
      BindConstantBuffer(Stage, i, bindings.buffers[i].buffer.ptr(),
        bindings.buffers[i].constantOffset, bindings.buffers[i].constantBound);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::RestoreSamplers(
          D3D11ShaderType                   Stage) {
    const auto& bindings = m_state.samplers[Stage];

    for (uint32_t i = 0; i < bindings.maxCount; i++)
      BindSampler(Stage, i, bindings.samplers[i].ptr());
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::RestoreShaderResources(
          D3D11ShaderType                   Stage) {
    const auto& bindings = m_state.srv[Stage];

    for (uint32_t i = 0; i < bindings.maxCount; i++)
      BindShaderResource(Stage, i, bindings.views[i].ptr());
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::RestoreUnorderedAccessViews(
          D3D11ShaderType                   Stage) {
    const auto& views = Stage == D3D11ShaderType::eCompute
      ? m_state.uav.views
      : m_state.om.uavs;

    uint32_t maxCount = Stage == D3D11ShaderType::eCompute
      ? m_state.uav.maxCount
      : m_state.om.maxUav;

    for (uint32_t i = 0; i < maxCount; i++)
      BindUnorderedAccessView(Stage, i, views[i].ptr());
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::SetConstantBuffers(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers) {
    auto& bindings = m_state.cbv[ShaderStage];

    for (uint32_t i = 0; i < NumBuffers; i++) {
      auto newBuffer = static_cast<D3D11Buffer*>(ppConstantBuffers[i]);

      uint32_t constantCount = newBuffer
        ? std::min(newBuffer->Desc()->ByteWidth / 16, UINT(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT))
        : 0u;

      if (bindings.buffers[StartSlot + i].buffer         != newBuffer
       || bindings.buffers[StartSlot + i].constantOffset != 0
       || bindings.buffers[StartSlot + i].constantCount  != constantCount) {
        bindings.buffers[StartSlot + i].buffer         = newBuffer;
        bindings.buffers[StartSlot + i].constantOffset = 0;
        bindings.buffers[StartSlot + i].constantCount  = constantCount;
        bindings.buffers[StartSlot + i].constantBound  = constantCount;

        if (!DirtyConstantBuffer(ShaderStage, StartSlot + i, !newBuffer))
          BindConstantBuffer(ShaderStage, StartSlot + i, newBuffer, 0, constantCount);
      }
    }

    bindings.maxCount = std::clamp(StartSlot + NumBuffers,
      bindings.maxCount, uint32_t(bindings.buffers.size()));
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::SetConstantBuffers1(
          UINT                              StartSlot,
          UINT                              NumBuffers,
          ID3D11Buffer* const*              ppConstantBuffers,
    const UINT*                             pFirstConstant,
    const UINT*                             pNumConstants) {
    auto& bindings = m_state.cbv[ShaderStage];

    for (uint32_t i = 0; i < NumBuffers; i++) {
      auto newBuffer = static_cast<D3D11Buffer*>(ppConstantBuffers[i]);

      UINT constantOffset;
      UINT constantCount;
      UINT constantBound;

      if (likely(newBuffer != nullptr)) {
        UINT bufferConstantsCount = newBuffer->Desc()->ByteWidth / 16;
        constantBound = std::min(bufferConstantsCount, UINT(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));

        if (likely(pFirstConstant && pNumConstants)) {
          constantOffset  = pFirstConstant[i];
          constantCount   = pNumConstants [i];

          if (unlikely(constantCount > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT))
            continue;

          constantBound = (constantOffset + constantCount > bufferConstantsCount)
            ? bufferConstantsCount - std::min(constantOffset, bufferConstantsCount)
            : constantCount;
        } else {
          constantOffset  = 0;
          constantCount   = constantBound;
        }
      } else {
        constantOffset  = 0;
        constantCount   = 0;
        constantBound   = 0;
      }

      // Do a full rebind if either the buffer changes
      if (bindings.buffers[StartSlot + i].buffer != newBuffer) {
        bindings.buffers[StartSlot + i].buffer = newBuffer;
        bindings.buffers[StartSlot + i].constantOffset = constantOffset;
        bindings.buffers[StartSlot + i].constantCount  = constantCount;
        bindings.buffers[StartSlot + i].constantBound  = constantBound;

        if (!DirtyConstantBuffer(ShaderStage, StartSlot + i, !newBuffer))
          BindConstantBuffer(ShaderStage, StartSlot + i, newBuffer, constantOffset, constantBound);
      } else if (bindings.buffers[StartSlot + i].constantOffset != constantOffset
              || bindings.buffers[StartSlot + i].constantCount  != constantCount) {
        bindings.buffers[StartSlot + i].constantOffset = constantOffset;
        bindings.buffers[StartSlot + i].constantCount  = constantCount;
        bindings.buffers[StartSlot + i].constantBound  = constantBound;

        if (!DirtyConstantBuffer(ShaderStage, StartSlot + i, !newBuffer))
          BindConstantBufferRange(ShaderStage, StartSlot + i, constantOffset, constantBound);
      }
    }

    bindings.maxCount = std::clamp(StartSlot + NumBuffers,
      bindings.maxCount, uint32_t(bindings.buffers.size()));
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::SetShaderResources(
          UINT                              StartSlot,
          UINT                              NumResources,
          ID3D11ShaderResourceView* const*  ppResources) {
    auto& bindings = m_state.srv[ShaderStage];

    for (uint32_t i = 0; i < NumResources; i++) {
      auto resView = static_cast<D3D11ShaderResourceView*>(ppResources[i]);

      if (bindings.views[StartSlot + i] != resView) {
        if (likely(resView != nullptr)) {
          if (unlikely(resView->TestHazards())) {
            if (TestSrvHazards<ShaderStage>(resView))
              resView = nullptr;

            // Only set if necessary, but don't reset it on every
            // bind as this would be more expensive than a few
            // redundant checks in OMSetRenderTargets and friends.
            bindings.hazardous.set(StartSlot + i, resView);
          }
        }

        bindings.views[StartSlot + i] = resView;

        if (!DirtyShaderResource(ShaderStage, StartSlot + i, !resView))
          BindShaderResource(ShaderStage, StartSlot + i, resView);
      }
    }

    bindings.maxCount = std::clamp(StartSlot + NumResources,
      bindings.maxCount, uint32_t(bindings.views.size()));
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::SetSamplers(
          UINT                              StartSlot,
          UINT                              NumSamplers,
          ID3D11SamplerState* const*        ppSamplers) {
    auto& bindings = m_state.samplers[ShaderStage];

    for (uint32_t i = 0; i < NumSamplers; i++) {
      auto sampler = static_cast<D3D11SamplerState*>(ppSamplers[i]);

      if (bindings.samplers[StartSlot + i] != sampler) {
        bindings.samplers[StartSlot + i] = sampler;

        if (!DirtySampler(ShaderStage, StartSlot + i, !sampler))
          BindSampler(ShaderStage, StartSlot + i, sampler);
      }
    }

    bindings.maxCount = std::clamp(StartSlot + NumSamplers,
      bindings.maxCount, uint32_t(bindings.samplers.size()));
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::SetRenderTargetsAndUnorderedAccessViews(
          UINT                              NumRTVs,
          ID3D11RenderTargetView* const*    ppRenderTargetViews,
          ID3D11DepthStencilView*           pDepthStencilView,
          UINT                              UAVStartSlot,
          UINT                              NumUAVs,
          ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT*                             pUAVInitialCounts) {
    if (TestRtvUavHazards(NumRTVs, ppRenderTargetViews, NumUAVs, ppUnorderedAccessViews))
      return;

    bool needsUpdate = false;
    bool isMultisampled = false;

    if (likely(NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)) {
      // Native D3D11 does not change the render targets if
      // the parameters passed to this method are invalid.
      if (!ValidateRenderTargets(NumRTVs, ppRenderTargetViews, pDepthStencilView))
        return;

      for (uint32_t i = 0; i < m_state.om.rtvs.size(); i++) {
        auto rtv = i < NumRTVs
          ? static_cast<D3D11RenderTargetView*>(ppRenderTargetViews[i])
          : nullptr;

        if (m_state.om.rtvs[i] != rtv) {
          m_state.om.rtvs[i] = rtv;
          needsUpdate = true;
          ResolveOmSrvHazards(rtv);

          if (NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
            ResolveOmUavHazards(rtv);
        }

        isMultisampled = isMultisampled || (rtv && rtv->GetSampleCount() > 1u);
      }

      auto dsv = static_cast<D3D11DepthStencilView*>(pDepthStencilView);

      if (m_state.om.dsv != dsv) {
        m_state.om.dsv = dsv;
        needsUpdate = true;
        ResolveOmSrvHazards(dsv);
      }

      m_state.om.maxRtv = NumRTVs;
    }

    if (unlikely(NumUAVs || m_state.om.maxUav)) {
      if (likely(NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)) {
        uint32_t newMinUav = NumUAVs ? UAVStartSlot : D3D11_1_UAV_SLOT_COUNT;
        uint32_t newMaxUav = NumUAVs ? UAVStartSlot + NumUAVs : 0u;

        uint32_t oldMinUav = std::exchange(m_state.om.minUav, newMinUav);
        uint32_t oldMaxUav = std::exchange(m_state.om.maxUav, newMaxUav);

        for (uint32_t i = std::min(oldMinUav, newMinUav);
                      i < std::max(oldMaxUav, newMaxUav); i++) {
          D3D11UnorderedAccessView* uav = nullptr;
          uint32_t                  ctr = ~0u;

          if (i >= UAVStartSlot && i < UAVStartSlot + NumUAVs) {
            uav = static_cast<D3D11UnorderedAccessView*>(ppUnorderedAccessViews[i - UAVStartSlot]);
            ctr = pUAVInitialCounts ? pUAVInitialCounts[i - UAVStartSlot] : ~0u;
          }

          if (ctr != ~0u && uav && uav->HasCounter())
            UpdateUnorderedAccessViewCounter(uav, ctr);

          if (m_state.om.uavs[i] != uav) {
            m_state.om.uavs[i] = uav;

            if (!DirtyGraphicsUnorderedAccessView(i))
              BindUnorderedAccessView(D3D11ShaderType::ePixel, i, uav);

            ResolveOmSrvHazards(uav);

            if (NumRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
              needsUpdate |= ResolveOmRtvHazards(uav);
          }
        }
      }
    }

    if (needsUpdate) {
      AddCost(GpuCostEstimate::RenderPass);
      BindFramebuffer();

      if constexpr (!IsDeferred)
        GetTypedContext()->NotifyRenderPassBoundary(isMultisampled);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::SetDrawBuffers(
          ID3D11Buffer*                     pBufferForArgs,
          ID3D11Buffer*                     pBufferForCount) {
    auto argBuffer = static_cast<D3D11Buffer*>(pBufferForArgs);
    auto cntBuffer = static_cast<D3D11Buffer*>(pBufferForCount);

    auto argBufferCookie = argBuffer ? argBuffer->GetCookie() : 0u;
    auto cntBufferCookie = cntBuffer ? cntBuffer->GetCookie() : 0u;

    if (m_state.id.argBufferCookie != argBufferCookie
     || m_state.id.cntBufferCookie != cntBufferCookie) {
      m_state.id.argBufferCookie = argBufferCookie;
      m_state.id.cntBufferCookie = cntBufferCookie;

      BindDrawBuffers(argBuffer, cntBuffer);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::SyncImage(
    const Rc<DxvkImage>&                    DstImage,
    const VkImageSubresourceLayers&         DstLayers,
    const Rc<DxvkImage>&                    SrcImage,
    const VkImageSubresourceLayers&         SrcLayers) {
    EmitCs([
      cDstImage = DstImage,
      cDstLayers = DstLayers,
      cSrcImage = SrcImage,
      cSrcLayers = SrcLayers
    ] (DxvkContext* ctx) {
      ctx->copyImage(
        cDstImage, cDstLayers, VkOffset3D(),
        cSrcImage, cSrcLayers, VkOffset3D(),
        cDstImage->mipLevelExtent(cDstLayers.mipLevel));
    });
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::TestRtvUavHazards(
          UINT                              NumRTVs,
          ID3D11RenderTargetView* const*    ppRTVs,
          UINT                              NumUAVs,
          ID3D11UnorderedAccessView* const* ppUAVs) {
    if (NumRTVs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) NumRTVs = 0;
    if (NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)           NumUAVs = 0;

    for (uint32_t i = 0; i < NumRTVs; i++) {
      auto rtv = static_cast<D3D11RenderTargetView*>(ppRTVs[i]);

      if (!rtv)
        continue;

      for (uint32_t j = 0; j < i; j++) {
        if (CheckViewOverlap(rtv, static_cast<D3D11RenderTargetView*>(ppRTVs[j])))
          return true;
      }

      if (rtv->HasBindFlag(D3D11_BIND_UNORDERED_ACCESS)) {
        for (uint32_t j = 0; j < NumUAVs; j++) {
          if (CheckViewOverlap(rtv, static_cast<D3D11UnorderedAccessView*>(ppUAVs[j])))
            return true;
        }
      }
    }

    for (uint32_t i = 0; i < NumUAVs; i++) {
      auto uav = static_cast<D3D11UnorderedAccessView*>(ppUAVs[i]);

      if (!uav)
        continue;

      for (uint32_t j = 0; j < i; j++) {
        if (CheckViewOverlap(uav, static_cast<D3D11UnorderedAccessView*>(ppUAVs[j])))
          return true;
      }
    }

    return false;
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  bool D3D11CommonContext<ContextType>::TestSrvHazards(
          D3D11ShaderResourceView*          pView) {
    bool hazard = false;

    if (ShaderStage == D3D11ShaderType::eCompute) {
      int32_t uav = m_state.uav.mask.findNext(0);

      while (uav >= 0 && !hazard) {
        hazard = CheckViewOverlap(pView, m_state.uav.views[uav].ptr());
        uav = m_state.uav.mask.findNext(uav + 1);
      }
    } else {
      hazard = CheckViewOverlap(pView, m_state.om.dsv.ptr());

      for (uint32_t i = 0; !hazard && i < m_state.om.maxRtv; i++)
        hazard = CheckViewOverlap(pView, m_state.om.rtvs[i].ptr());

      for (uint32_t i = 0; !hazard && i < m_state.om.maxUav; i++)
        hazard = CheckViewOverlap(pView, m_state.om.uavs[i].ptr());
    }

    return hazard;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::TrackResourceSequenceNumber(
          ID3D11Resource*             pResource) {
    if (!pResource)
      return;

    D3D11CommonTexture* texture = GetCommonTexture(pResource);

    if (texture) {
      if (texture->HasSequenceNumber()) {
        for (uint32_t i = 0; i < texture->CountSubresources(); i++)
          GetTypedContext()->TrackTextureSequenceNumber(texture, i);
      }
    } else {
      D3D11Buffer* buffer = static_cast<D3D11Buffer*>(pResource);

      if (buffer->HasSequenceNumber())
        GetTypedContext()->TrackBufferSequenceNumber(buffer);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::UpdateBuffer(
          D3D11Buffer*                      pDstBuffer,
          UINT                              Offset,
          UINT                              Length,
    const void*                             pSrcData) {
    constexpr uint32_t MaxDirectUpdateSize = 64u;

    DxvkBufferSlice bufferSlice = pDstBuffer->GetBufferSlice(Offset, Length);

    if (Length <= MaxDirectUpdateSize && !((Offset | Length) & 0x3)) {
      // The backend has special code paths for small buffer updates,
      // however both offset and size must be aligned to four bytes.
      // Write the data directly to the CS chunk.
      uint32_t dwordCount = Length / sizeof(uint32_t);

      EmitCsCmd<uint32_t>(D3D11CmdType::None, dwordCount, [
        cBufferSlice = std::move(bufferSlice)
      ] (DxvkContext* ctx, const uint32_t* data, size_t) {
        ctx->updateBuffer(
          cBufferSlice.buffer(),
          cBufferSlice.offset(),
          cBufferSlice.length(), data);
      });

      // Compiler should be able to vectorize here, but GCC only does
      // if we cast the destination pointer to the correct type first
      auto src = reinterpret_cast<const uint32_t*>(pSrcData);
      auto dst = reinterpret_cast<uint32_t*>(m_csData->first());

      for (uint32_t i = 0; i < dwordCount; i++)
        new (dst + i) uint32_t(src[i]);
    } else {
      // Write directly to a staging buffer and dispatch a copy
      DxvkBufferSlice stagingSlice = AllocStagingBuffer(Length);
      std::memcpy(stagingSlice.mapPtr(0), pSrcData, Length);

      EmitCs([
        cStagingSlice = std::move(stagingSlice),
        cBufferSlice  = std::move(bufferSlice)
      ] (DxvkContext* ctx) {
        ctx->copyBuffer(
          cBufferSlice.buffer(),
          cBufferSlice.offset(),
          cStagingSlice.buffer(),
          cStagingSlice.offset(),
          cBufferSlice.length());
      });
    }

    if (pDstBuffer->HasSequenceNumber())
      GetTypedContext()->TrackBufferSequenceNumber(pDstBuffer);

    if constexpr (!IsDeferred)
      static_cast<ContextType*>(this)->ThrottleAllocation();
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::UpdateTexture(
          D3D11CommonTexture*               pDstTexture,
          UINT                              DstSubresource,
    const D3D11_BOX*                        pDstBox,
    const void*                             pSrcData,
          UINT                              SrcRowPitch,
          UINT                              SrcDepthPitch) {
    if (DstSubresource >= pDstTexture->CountSubresources())
      return;

    VkFormat packedFormat = pDstTexture->GetPackedFormat();

    auto formatInfo = lookupFormatInfo(packedFormat);
    auto subresource = pDstTexture->GetSubresourceFromIndex(
        formatInfo->aspectMask, DstSubresource);

    VkExtent3D mipExtent = pDstTexture->MipLevelExtent(subresource.mipLevel);

    VkOffset3D offset = { 0, 0, 0 };
    VkExtent3D extent = mipExtent;

    if (pDstBox != nullptr) {
      if (pDstBox->left >= pDstBox->right
        || pDstBox->top >= pDstBox->bottom
        || pDstBox->front >= pDstBox->back)
        return;  // no-op, but legal

      offset.x = pDstBox->left;
      offset.y = pDstBox->top;
      offset.z = pDstBox->front;

      extent.width  = pDstBox->right - pDstBox->left;
      extent.height = pDstBox->bottom - pDstBox->top;
      extent.depth  = pDstBox->back - pDstBox->front;
    }

    if (!util::isBlockAligned(offset, extent, formatInfo->blockSize, mipExtent))
      return;

    auto stagingSlice = AllocStagingBuffer(util::computeImageDataSize(packedFormat, extent));

    util::packImageData(stagingSlice.mapPtr(0),
      pSrcData, SrcRowPitch, SrcDepthPitch, 0, 0,
      pDstTexture->GetVkImageType(), extent, 1,
      formatInfo, formatInfo->aspectMask);

    UpdateImage(pDstTexture, &subresource,
      offset, extent, std::move(stagingSlice));

    if constexpr (!IsDeferred)
      static_cast<ContextType*>(this)->ThrottleAllocation();
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::UpdateImage(
          D3D11CommonTexture*               pDstTexture,
    const VkImageSubresource*               pDstSubresource,
          VkOffset3D                        DstOffset,
          VkExtent3D                        DstExtent,
          DxvkBufferSlice                   StagingBuffer) {
    bool dstIsImage = pDstTexture->HasImage();

    uint32_t dstSubresource = D3D11CalcSubresource(pDstSubresource->mipLevel,
      pDstSubresource->arrayLayer, pDstTexture->Desc()->MipLevels);

    if (dstIsImage) {
      EmitCs([
        cDstImage         = pDstTexture->GetImage(),
        cDstLayers        = vk::makeSubresourceLayers(*pDstSubresource),
        cDstOffset        = DstOffset,
        cDstExtent        = DstExtent,
        cStagingSlice     = std::move(StagingBuffer),
        cPackedFormat     = pDstTexture->GetPackedFormat()
      ] (DxvkContext* ctx) {
        ctx->copyBufferToImage(cDstImage,
          cDstLayers, cDstOffset, cDstExtent,
          cStagingSlice.buffer(),
          cStagingSlice.offset(), 0, 0,
          cPackedFormat);
      });
    } else {
      // If the destination image is backed only by a buffer, we need to use
      // the packed buffer copy function which does not know about planes and
      // format metadata, so deal with it manually here.
      VkExtent3D dstMipExtent = pDstTexture->MipLevelExtent(pDstSubresource->mipLevel);

      auto dstFormat = pDstTexture->GetPackedFormat();
      auto dstFormatInfo = lookupFormatInfo(dstFormat);

      uint32_t planeCount = 1;

      if (dstFormatInfo->flags.test(DxvkFormatFlag::MultiPlane))
        planeCount = vk::getPlaneCount(dstFormatInfo->aspectMask);

      // The source data isn't stored in an image so we'll also need to
      // track the offset for that while iterating over the planes.
      VkDeviceSize srcPlaneOffset = 0;

      for (uint32_t i = 0; i < planeCount; i++) {
        VkImageAspectFlags dstAspectMask = dstFormatInfo->aspectMask;
        VkDeviceSize elementSize = dstFormatInfo->elementSize;
        VkExtent3D blockSize = dstFormatInfo->blockSize;

        if (dstFormatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
          dstAspectMask = vk::getPlaneAspect(i);

          auto plane = &dstFormatInfo->planes[i];
          blockSize.width  *= plane->blockSize.width;
          blockSize.height *= plane->blockSize.height;
          elementSize = plane->elementSize;
        }

        VkExtent3D blockCount = util::computeBlockCount(DstExtent, blockSize);

        EmitCs([
          cDstBuffer      = pDstTexture->GetMappedBuffer(dstSubresource),
          cDstStart       = pDstTexture->GetSubresourceLayout(dstAspectMask, dstSubresource).Offset,
          cDstOffset      = util::computeBlockOffset(DstOffset, blockSize),
          cDstSize        = util::computeBlockCount(dstMipExtent, blockSize),
          cDstExtent      = blockCount,
          cSrcBuffer      = StagingBuffer.buffer(),
          cSrcStart       = StagingBuffer.offset() + srcPlaneOffset,
          cPixelSize      = elementSize
        ] (DxvkContext* ctx) {
          ctx->copyPackedBufferImage(
            cDstBuffer, cDstStart, cDstOffset, cDstSize,
            cSrcBuffer, cSrcStart, VkOffset3D(), cDstExtent,
            cDstExtent, cPixelSize);
        });

        srcPlaneOffset += util::flattenImageExtent(blockCount) * elementSize;
      }
    }

    if (pDstTexture->HasSequenceNumber())
      GetTypedContext()->TrackTextureSequenceNumber(pDstTexture, dstSubresource);
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::UpdateResource(
          ID3D11Resource*                   pDstResource,
          UINT                              DstSubresource,
    const D3D11_BOX*                        pDstBox,
    const void*                             pSrcData,
          UINT                              SrcRowPitch,
          UINT                              SrcDepthPitch,
          UINT                              CopyFlags) {
    if (unlikely(nptA8Enabled()) && pDstResource) {
      D3D11_RESOURCE_DIMENSION nd; pDstResource->GetType(&nd);
      if (nd == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        nptA8Note(GetCommonTexture(pDstResource), str::format("UpdateSubresource(sub=", DstSubresource, " box=", pDstBox ? str::format(pDstBox->left, ",", pDstBox->top, "-", pDstBox->right, ",", pDstBox->bottom) : std::string("full"),
          " rowPitch=", SrcRowPitch, " flags=", CopyFlags, IsDeferred ? " deferred" : "", ")"));
    }
    auto context = static_cast<ContextType*>(this);
    D3D10DeviceLock lock = context->LockContext();
    NptDrawSyncGuard nptSync(this);

    if (!pDstResource)
      return;

    NptMarkSharedRes(pDstResource);

    // We need a different code path for buffers
    D3D11_RESOURCE_DIMENSION resourceType;
    pDstResource->GetType(&resourceType);

    if (likely(resourceType == D3D11_RESOURCE_DIMENSION_BUFFER)) {
      const auto bufferResource = static_cast<D3D11Buffer*>(pDstResource);
      uint64_t bufferSize = bufferResource->Desc()->ByteWidth;

      // DroidVM/Windows guests: real drivers execute UpdateSubresource1 on buffers in order and keep the
      // bytes outside the updated box, whatever COPY_DISCARD / COPY_NO_OVERWRITE say; Direct2D relies on
      // that for its colour table (cbuffer, DEFAULT, 4000 B, updated in parts).  Renaming the buffer on a
      // partial DISCARD loses the other entries, and a direct NO_OVERWRITE write races with queued draws.
      // DXVK_UPDATE_ORDERED=0 restores the stock behaviour.
      {
        static const bool ordered = env::getEnvVar("DXVK_UPDATE_ORDERED") != "0";
        if (unlikely(nptA8Enabled()) && (bufferResource->Desc()->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bufferSize >= 1024) {
          auto& d = nptA8();
          if (d.cbLogs.fetch_add(1) < 400) {
            std::string hx; for (UINT k = 0; k < 16 && pSrcData; k++) { char t[4]; snprintf(t, sizeof t, "%02x", ((const uint8_t*)pSrcData)[k]); hx += t; }
            Logger::info(str::format("NPT_DUMP_A8 cbupdate seq=", d.opSeq.fetch_add(1), ": buf=", std::hex, uintptr_t(bufferResource) & 0xffffff, std::dec, " ByteWidth=", bufferSize, " usage=", uint32_t(bufferResource->Desc()->Usage),
              " box=", pDstBox ? str::format("[", pDstBox->left, ",", pDstBox->right, ")") : std::string("full"), " flags=", CopyFlags, IsDeferred ? " DEFERRED" : " immediate", " first16=", hx));
          }
        }
        if (ordered)
          CopyFlags = 0;
      }

      // Provide a fast path for mapped buffer updates since some
      // games use UpdateSubresource to update constant buffers.
      if (likely(bufferResource->GetMapMode() == D3D11_COMMON_BUFFER_MAP_MODE_DIRECT) && likely(!pDstBox)) {
        context->UpdateMappedBuffer(bufferResource, 0, bufferSize, pSrcData, 0);
        return;
      }

      // Validate buffer range to update
      uint64_t offset = 0;
      uint64_t length = bufferSize;

      if (pDstBox) {
        offset = pDstBox->left;
        length = pDstBox->right - offset;
      }

      if (unlikely(offset + length > bufferSize))
        return;

      // Still try to be fast if a box is provided but we update the full buffer
      if (likely(bufferResource->GetMapMode() == D3D11_COMMON_BUFFER_MAP_MODE_DIRECT)) {
        CopyFlags &= D3D11_COPY_DISCARD | D3D11_COPY_NO_OVERWRITE;

        if (likely(length == bufferSize) || unlikely(CopyFlags != 0)) {
          context->UpdateMappedBuffer(bufferResource, offset, length, pSrcData, CopyFlags);
          return;
        }
      }

      // Otherwise we can't really do anything fancy, so just do a GPU copy
      if (likely(length))
        context->UpdateBuffer(bufferResource, offset, length, pSrcData);
    } else {
      D3D11CommonTexture* textureResource = GetCommonTexture(pDstResource);

      context->UpdateTexture(textureResource,
        DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::UpdateUnorderedAccessViewCounter(
          D3D11UnorderedAccessView*         pUav,
          uint32_t                          CounterValue) {
    EmitCs([
      cView    = pUav->GetCounterView(),
      cCounter = CounterValue
    ] (DxvkContext* ctx) {
      ctx->updateBuffer(cView->buffer(),
        cView->info().offset, sizeof(cCounter), &cCounter);
    });
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::ValidateRenderTargets(
          UINT                              NumViews,
          ID3D11RenderTargetView* const*    ppRenderTargetViews,
          ID3D11DepthStencilView*           pDepthStencilView) {
    Rc<DxvkImageView> refView;

    VkExtent3D dsvExtent = { 0u, 0u, 0u };
    VkExtent3D rtvExtent = { 0u, 0u, 0u };

    if (pDepthStencilView != nullptr) {
      refView = static_cast<D3D11DepthStencilView*>(
        pDepthStencilView)->GetImageView();
      dsvExtent = refView->mipLevelExtent(0);
    }

    for (uint32_t i = 0; i < NumViews; i++) {
      if (ppRenderTargetViews[i] != nullptr) {
        auto curView = static_cast<D3D11RenderTargetView*>(
          ppRenderTargetViews[i])->GetImageView();

        if (!rtvExtent.width)
          rtvExtent = curView->mipLevelExtent(0);

        if (refView != nullptr) {
          // Render target views must all have the same sample count,
          // layer count, and type. The size can mismatch under certain
          // conditions, the D3D11 documentation is wrong here.
          if (curView->info().viewType != refView->info().viewType
           || curView->info().layerCount != refView->info().layerCount)
            return false;

          if (curView->image()->info().sampleCount
           != refView->image()->info().sampleCount)
            return false;

          // Color targets must all be the same size
          VkExtent3D curExtent = curView->mipLevelExtent(0);

          if (curExtent.width  != rtvExtent.width
           || curExtent.height != rtvExtent.height)
            return false;
        } else {
          // Set reference view. All remaining views
          // must be compatible to the reference view.
          refView = curView;
        }
      }
    }

    // Based on testing, the depth-stencil target is allowed
    // to be larger than all color targets, but not smaller
    if (rtvExtent.width && dsvExtent.width) {
      if (rtvExtent.width  > dsvExtent.width
       || rtvExtent.height > dsvExtent.height)
        return false;
    }

    return true;
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::SetClassInstances(
    const D3D11CommonShader*                pShader,
          ID3D11ClassInstance* const*       ppClassInstances,
          UINT                              NumClassInstances) {
    auto& state = m_state.instances[ShaderStage];

    if (likely(!NumClassInstances) && likely(!state.instanceCount))
      return;

    // Not sure if this is the right thing to do, but if we don't
    // have a shader we also don't really have anything to bind
    if (!pShader)
      NumClassInstances = 0u;

    // Assign class instances for state tracking and populate
    // constant buffer containing class instance data */
    for (uint32_t i = 0u; i < NumClassInstances && i < D3D11ClassInstanceState::MaxInstances; i++)
      state.instances[i] = static_cast<D3D11ClassInstance*>(ppClassInstances[i]);

    // Unset previously bound class instances, if any
    for (uint32_t i = NumClassInstances; i < state.instanceCount; i++)
      state.instances[i] = nullptr;

    uint32_t slotId = D3D11ShaderResourceMapping::computeCbvBinding(ShaderStage,
      D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT + 1u);

    if (NumClassInstances) {
      auto buffer = AllocInstanceDataBuffer(ShaderStage);
      auto slice = buffer->allocateStorage();

      auto data = reinterpret_cast<D3D11InstanceData*>(slice->mapPtr());

      for (uint32_t i = 0u; i < D3D11ClassInstanceState::MaxInstances; i++)
        data[i] = pShader->GetClassInstanceData(i, state.instances[i].ptr());

      EmitCs([
        cSlotId = slotId,
        cStage  = GetShaderStage(ShaderStage),
        cBuffer = std::move(buffer),
        cSlice  = std::move(slice)
      ] (DxvkContext* ctx) mutable {
        ctx->invalidateBuffer(cBuffer, Forwarder::move(cSlice));
        ctx->bindUniformBuffer(cStage, cSlotId, DxvkBufferSlice(Forwarder::move(cBuffer)));
      });
    } else {
      EmitCs([
        cSlotId = slotId,
        cStage  = GetShaderStage(ShaderStage)
      ] (DxvkContext* ctx) mutable {
        ctx->bindUniformBuffer(cStage, cSlotId, DxvkBufferSlice());
      });
    }
  }


  template<typename ContextType>
  template<D3D11ShaderType ShaderStage>
  void D3D11CommonContext<ContextType>::GetClassInstances(
          ID3D11ClassInstance**             ppClassInstances,
          UINT*                             pNumClassInstances) {
    if (unlikely(pNumClassInstances)) {
      const auto& state = m_state.instances[ShaderStage];

      if (ppClassInstances) {
        for (uint32_t i = 0u; i < *pNumClassInstances; i++) {
          ppClassInstances[i] = i < state.instanceCount
            ? state.instances[i].ref()
            : nullptr;
        }
      }

      *pNumClassInstances = state.instanceCount;
    }
  }


  template<typename ContextType>
  Rc<DxvkBuffer> D3D11CommonContext<ContextType>::AllocInstanceDataBuffer(
          D3D11ShaderType                   ShaderStage) {
    if (!m_instanceData[ShaderStage]) {
      DxvkBufferCreateInfo bufferInfo = { };
      bufferInfo.size = sizeof(D3D11InstanceData) * D3D11ClassInstanceState::MaxInstances;
      bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      bufferInfo.stages = GetShaderStage(ShaderStage);
      bufferInfo.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
      bufferInfo.debugName = "Instance data";

      m_instanceData[ShaderStage] = m_device->createBuffer(bufferInfo,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }

    return m_instanceData[ShaderStage];
  }


  template<typename ContextType>
  DxvkInputAssemblyState D3D11CommonContext<ContextType>::InitDefaultPrimitiveTopology() {
    return DxvkInputAssemblyState(VK_PRIMITIVE_TOPOLOGY_MAX_ENUM, false);
  }


  template<typename ContextType>
  DxvkRasterizerState D3D11CommonContext<ContextType>::InitDefaultRasterizerState() {
    DxvkRasterizerState rsState = { };
    rsState.setPolygonMode(VK_POLYGON_MODE_FILL);
    rsState.setCullMode(VK_CULL_MODE_BACK_BIT);
    rsState.setFrontFace(VK_FRONT_FACE_CLOCKWISE);
    rsState.setDepthClip(true);
    rsState.setConservativeMode(VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
    rsState.setSampleCount(0);
    rsState.setFlatShading(false);
    rsState.setLineMode(VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT);
    return rsState;
  }


  template<typename ContextType>
  DxvkDepthStencilState D3D11CommonContext<ContextType>::InitDefaultDepthStencilState() {
    DxvkDepthStencilState dsState = { };
    dsState.setDepthTest(true);
    dsState.setDepthWrite(true);
    dsState.setDepthCompareOp(VK_COMPARE_OP_LESS);
    return dsState;
  }


  template<typename ContextType>
  DxvkMultisampleState D3D11CommonContext<ContextType>::InitDefaultMultisampleState(
          UINT                              SampleMask) {
    DxvkMultisampleState msState = { };
    msState.setSampleMask(SampleMask);
    return msState;
  }


  template<typename ContextType>
  DxvkLogicOpState D3D11CommonContext<ContextType>::InitDefaultLogicOpState() {
    DxvkLogicOpState loState = { };
    loState.setLogicOp(false, VK_LOGIC_OP_NO_OP);
    return loState;
  }


  template<typename ContextType>
  DxvkBlendMode D3D11CommonContext<ContextType>::InitDefaultBlendState() {
    DxvkBlendMode cbState = { };
    cbState.setBlendEnable(false);
    cbState.setColorOp(VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
    cbState.setAlphaOp(VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
    cbState.setWriteMask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    return cbState;
  }

  // Explicitly instantiate here
  template class D3D11CommonContext<D3D11DeferredContext>;
  template class D3D11CommonContext<D3D11ImmediateContext>;

}
