#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "com/com_guid.hpp"
#include "log/log.hpp"
#include "util_hotpatch.h"
#include "util_string.hpp"
#include "wsi_window.hpp"
#include "Metal.hpp"
#include "mullion_format.hpp"
#include <unordered_map>
#include <unordered_set>

namespace mullion {

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config);

// the HWND reservation must outlive the factory call and cover both frontends (CreateSwapChainForHwnd, Remarks).
class FlipWindow final : public ComObjectWithInitialRef<IUnknown> {
  inline static mullion::mutex mutex_;
  inline static std::unordered_set<HWND> windows_;
  HWND window_;
  bool acquired_;

public:
  FlipWindow(HWND window) : window_(window) {
    std::lock_guard lock(mutex_);
    acquired_ = windows_.insert(window).second;
  }

  ~FlipWindow() override {
    if (acquired_) {
      std::lock_guard lock(mutex_);
      windows_.erase(window_);
    }
  }

  bool acquired() const { return acquired_; }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **object) final {
    InitReturnPtr(object);
    if (!object)
      return E_POINTER;
    if (riid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *object = ref(this);
    return S_OK;
  }
};

// status notifications belong to the process, as the windows whose status they report.
// occlusion is a window's minimization (DXGI_STATUS_OCCLUDED from Present); stereo never changes, as no output has it.
class StatusNotifications {
  struct Target {
    HANDLE event;
    HWND window;
    UINT message;
  };
  inline static mullion::mutex mutex_;
  inline static std::unordered_map<DWORD, Target> occlusion_, stereo_;
  inline static DWORD cookie_ = 0;
  inline static HWINEVENTHOOK hook_ = nullptr;

  static void CALLBACK
  changed(HWINEVENTHOOK, DWORD, HWND, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW)
      return;
    std::lock_guard lock(mutex_);
    for (auto &[cookie, target] : occlusion_)
      target.event ? SetEvent(target.event) : PostMessageW(target.window, target.message, 0, 0);
  }

  static bool
  valid(const Target &target) {
    if (target.window) {
      DWORD process = 0;
      return GetWindowThreadProcessId(target.window, &process) && process == GetCurrentProcessId();
    }
    // NtQueryObject's ObjectTypeInformation (2 in winternl.h, whose definitions util_d3dkmt.h has) begins with the
    // type's name, its characters after the structure
    using Query = LONG(WINAPI *)(HANDLE, int, void *, ULONG, ULONG *);
    static auto query = (Query)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    alignas(UNICODE_STRING) char info[512];
    auto &name = *(UNICODE_STRING *)info;
    return target.event && query(target.event, 2, info, sizeof(info), nullptr) >= 0 &&
           std::wstring_view(name.Buffer, name.Length / sizeof(WCHAR)) == L"Event";
  }

public:
  static HRESULT
  add(bool occlusion, Target target, DWORD *cookie) {
    if (!cookie || (occlusion && !valid(target)))
      return DXGI_ERROR_INVALID_CALL;
    std::lock_guard lock(mutex_);
    auto &targets = occlusion ? occlusion_ : stereo_;
    // RegisterOcclusionStatusEvent, Remarks: the same event twice fails with INVALID_CALL
    for (auto &[_, registered] : targets)
      if (occlusion && target.event && registered.event == target.event)
        return DXGI_ERROR_INVALID_CALL;
    if (occlusion && !hook_) {
      HMODULE module;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)&changed, &module);
      hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, module, changed,
                              GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
      if (!hook_)
        return HRESULT_FROM_WIN32(GetLastError());
    }
    *cookie = ++cookie_;
    targets.emplace(*cookie, target);
    return S_OK;
  }

  static void
  remove(bool occlusion, DWORD cookie) {
    std::lock_guard lock(mutex_);
    (occlusion ? occlusion_ : stereo_).erase(cookie);
    if (occlusion && occlusion_.empty() && hook_) {
      UnhookWinEvent(hook_);
      hook_ = nullptr;
    }
  }
};

class MTLDXGIFactory : public MTLDXGIObject<IDXGIFactory6> {

public:
  MTLDXGIFactory(UINT Flags, bool Extended) : flags_(Flags), extended_(Extended) {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    // Windows hides only IDXGIFactory1 on legacy factories (Wine's test_create_factory).
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || (riid == __uuidof(IDXGIFactory1) && extended_) ||
        riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIFactory2), riid)) {
      WARN("DXGIFactory: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);

    WARN("DXGIFactory::GetParent: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() final {
    // We don't support Stereo 3D at the moment
    return FALSE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return FAIL(DXGI_ERROR_INVALID_CALL);

    ERR("Software adapters not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  MULLION_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain) final {
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr)
      return FAIL(DXGI_ERROR_INVALID_CALL);
    // the window is this call's argument: its refusal names this call in the log, not the function both entries share
    *ppSwapChain = nullptr;
    if (!pDesc->OutputWindow)
      return FAIL(DXGI_ERROR_INVALID_CALL);

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width = pDesc->BufferDesc.Width;
    desc.Height = pDesc->BufferDesc.Height;
    desc.Format = pDesc->BufferDesc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling = pDesc->BufferDesc.Scaling;
    descFs.Windowed = pDesc->Windowed;

    IDXGISwapChain1 *swapChain = nullptr;
    HRESULT hr = CreateSwapChainForWindow(pDevice, pDesc->OutputWindow, &desc, &descFs, &swapChain);

    *ppSwapChain = swapChain;
    return hr;
  }

  MULLION_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    return CreateSwapChainForWindow(pDevice, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  // what CreateSwapChain and CreateSwapChainForHwnd do, called by both. CreateSwapChain does not go through
  // CreateSwapChainForHwnd, as DXGI's does not: on ARM64EC a hybrid-patchable function is entered through its patchable
  // x64 entry even from inside the module, so a hook on both entries (the Steam overlay's) saw a swap chain made by
  // CreateSwapChain twice, took two references to it and dropped one. Streamline's DLSS Frame Generation then could
  // not replace it: the window stayed held, its second swap chain was refused, and CONTROL Resonant crashed
  HRESULT
  CreateSwapChainForWindow(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain) {
    InitReturnPtr(ppSwapChain);

    if (!ppSwapChain || !pDesc || !hWnd || !pDevice)
      return FAIL(DXGI_ERROR_INVALID_CALL);

    if ((pDesc->Flags & (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) &&
        pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD)
      return DXGI_ERROR_INVALID_CALL;

    if (pDesc->Format != DXGI_FORMAT_UNKNOWN &&
        (pDesc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || pDesc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD) &&
        !IsFlipSwapChainFormat(pDesc->Format))
      return FAIL(DXGI_ERROR_INVALID_CALL);

    // every effect but DISCARD "cannot be used with multisampling" (Microsoft Learn, DXGI_SWAP_EFFECT)
    if (pDesc->SwapEffect != DXGI_SWAP_EFFECT_DISCARD && (pDesc->SampleDesc.Count != 1 || pDesc->SampleDesc.Quality))
      return FAIL(DXGI_ERROR_INVALID_CALL);

    Com<IMTLSwapChainFactory> swapchain_factory;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&swapchain_factory)))) {
      ERR("Unsupported device type");
      return DXGI_ERROR_UNSUPPORTED;
    }

    // Make sure the back buffer size is not zero
    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    wsi::getWindowSize(hWnd, desc.Width ? nullptr : &desc.Width,
                       desc.Height ? nullptr : &desc.Height);

    // If necessary, set up a default set of
    // fullscreen parameters for the swap chain
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc;

    if (pFullscreenDesc) {
      fsDesc = *pFullscreenDesc;
    } else {
      fsDesc.RefreshRate = {0, 0};
      fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
      fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
      fsDesc.Windowed = TRUE;
    }

    try {
      Com<FlipWindow> window;
      if (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
        window = Com<FlipWindow>::transfer(new FlipWindow(hWnd));
        if (!window->acquired())
          return DXGI_ERROR_INVALID_CALL;
      }
      Com<IDXGISwapChain1> chain;
      HRESULT hr = swapchain_factory->CreateSwapChain(this, hWnd, &desc, &fsDesc, &chain);
      if (FAILED(hr))
        return hr;
      if (window) {
        Com<IMTLDXGISwapChain> owned;
        if (FAILED(hr = chain->QueryInterface(IID_PPV_ARGS(&owned))) ||
            FAILED(hr = owned->SetWindowReservation(window.ptr())))
          return hr;
      }
      *ppSwapChain = chain.takeOwnership();
      return hr;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  MULLION_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    ERR("Not implemented");
    return E_NOTIMPL;
  }

  MULLION_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                         IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return FAIL(DXGI_ERROR_INVALID_CALL);

    IDXGIAdapter1 *handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                          IDXGIAdapter1 **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    auto devices = WMT::CopyAllDevices();
    UINT adapter_count = devices.count();

    if (Adapter >= adapter_count)
      return DXGI_ERROR_NOT_FOUND;

    UINT adjusted_adapter = Adapter;
    if (adapter_count > 1) {
      UINT preferred_adapter = 0;
      for (unsigned i = 0; i < adapter_count; i++) {
        if (!devices.object(i).hasUnifiedMemory())
          preferred_adapter = i;
      }
      if (Adapter == 0)
        adjusted_adapter = preferred_adapter;
      else
        adjusted_adapter = Adapter <= preferred_adapter ? Adapter - 1 : Adapter;
    }

    auto device = devices.object(adjusted_adapter);

    *ppAdapter = CreateAdapter(device, this, Config::getInstance());
    // devices->release(); // no you should not release it...
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) final {
    if (pWindowHandle == nullptr)
      return FAIL(DXGI_ERROR_INVALID_CALL);

    // Wine's test_window_association records NULL even after a successful association.
    *pWindowHandle = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource,
                                                         LUID *pLuid) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                  UINT Flags) final {
    if ((Flags & ~DXGI_MWA_VALID) || (WindowHandle && !wsi::isWindow(WindowHandle)))
      return FAIL(DXGI_ERROR_INVALID_CALL);
    if (Flags) {
      WARN("MakeWindowAssociation: Ignoring flags ", Flags);
    }
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsCurrent() final { return TRUE; }

  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(
      HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) final {
    return StatusNotifications::add(true, {nullptr, WindowHandle, wMsg}, pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent,
                                                      DWORD *pdwCookie) final {
    return StatusNotifications::add(false, {hEvent, nullptr, 0}, pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle,
                                                       UINT wMsg,
                                                       DWORD *pdwCookie) final {
    return StatusNotifications::add(false, {nullptr, WindowHandle, wMsg}, pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE
  RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) final {
    return StatusNotifications::add(true, {hEvent, nullptr, 0}, pdwCookie);
  }

  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) final {
    StatusNotifications::remove(false, dwCookie);
  }

  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) final {
    StatusNotifications::remove(true, dwCookie);
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

  HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid,
                                              void **ppAdapter) override {
    InitReturnPtr(ppAdapter);

    Com<IDXGIAdapter1> adapter;
    for (UINT i = 0; SUCCEEDED(this->EnumAdapters1(i, &adapter)); i++) {
      DXGI_ADAPTER_DESC desc;
      adapter->GetDesc(&desc);
      if (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart)
        return adapter->QueryInterface(riid, ppAdapter);
      adapter = nullptr;
    }
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid,
                                            void **adapter) override {
    // no software rasterizer here: the GPU's adapter is the one that makes a device, as in Wine and DXVK
    return EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, iid, adapter);
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
      auto info = static_cast<BOOL *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return FAIL(E_INVALIDARG);

      *info = TRUE;
      return S_OK;
    }
    default: {
      ERR("DXGIFactory::CheckFeatureSupport: unknown feature ", Feature);
      return E_INVALIDARG;
    }
    }
  };

  HRESULT STDMETHODCALLTYPE
  EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                             REFIID riid, void **ppvAdapter) override {
    InitReturnPtr(ppvAdapter);
    if (!ppvAdapter)
      return DXGI_ERROR_INVALID_CALL;
    // GpuPreference ignored, since Apple Silicon has only 1 GPU anyway
    // FIXME: support Intel Mac with dedicated GPU
    Com<IDXGIAdapter1> adapter;
    HRESULT hr = this->EnumAdapters1(Adapter, &adapter);

    if (FAILED(hr))
      return hr;
    return adapter->QueryInterface(riid, ppvAdapter);
  };

private:
  UINT flags_;
  bool extended_;
};

static HRESULT CreateFactory(UINT Flags, bool Extended, REFIID riid, void **ppFactory) {
  InitializeVendorExtension();
  try {
    MTLDXGIFactory* factory = new MTLDXGIFactory(Flags, Extended);
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();

    if (FAILED(hr))
      return hr;

    return S_OK;
  } catch (const MTLD3DError &e) {
    Logger::err(e.message());
    return E_FAIL;
  }
}

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
  return CreateFactory(Flags, true, riid, ppFactory);
}

extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  return CreateDXGIFactory2(0, riid, ppFactory);
}

extern "C" HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **factory) {
  return CreateFactory(0, false, riid, factory);
}

} // namespace mullion
