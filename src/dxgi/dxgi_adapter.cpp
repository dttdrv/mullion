#include <atomic>
#include <algorithm>
#include <limits>
#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_options.hpp"
#include "util_string.hpp"
#include "log/log.hpp"
#include "log/trace.hpp"
#include "wsi_monitor.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "d3d10_1.h"
#include "Metal.hpp"

namespace mullion {

const GUID kWineDXGIAdapterUUID = {0x17399d75,
                                   0x964e,
                                   0x4c03,
                                   {0x99, 0xf8, 0x9d, 0x4f, 0xd1, 0x96, 0xdd, 0x62}};

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options);

LUID GetAdapterLuid(WMT::Device device) {
#ifdef _WIN32
  // one Metal device makes Wine's primary display an unambiguous match (D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME).
  if (WMT::CopyAllDevices().count() == 1) {
    auto gdi = GetModuleHandleA("gdi32.dll");
    auto open_adapter = gdi ? reinterpret_cast<NTSTATUS(WINAPI *)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *)>(
                                  GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"))
                            : nullptr;
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open = {};
    if (open_adapter && wsi::getDisplayName(wsi::getDefaultMonitor(), open.DeviceName) && !open_adapter(&open)) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = open.hAdapter;
      if (D3DKMTCloseAdapter(&close))
        WARN("Failed to close D3DKMT identity query adapter");
      return open.AdapterLuid;
    }
  }
#endif
  WARN("Wine adapter identity unavailable; using Metal registry ID");
  // without a host mapping, keep distinct Metal devices distinct, in MoltenVK's byte order.
  return std::bit_cast<LUID>(__builtin_bswap64(device.registryID()));
}

class MTLDXGIAdapter : public MTLDXGIObject<IMTLDXGIAdapter> {
public:
  MTLDXGIAdapter(WMT::Device device, IDXGIFactory *factory, Config &config)
      : device_(device), adapter_luid_(GetAdapterLuid(device)), factory_(factory), options_(config) {
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = adapter_luid_;
    if (D3DKMTOpenAdapterFromLuid(&open))
      WARN("Failed to open D3DKMT adapter");
    else
      local_kmt_ = open.hAdapter;
  };

  ~MTLDXGIAdapter() {
    if (local_kmt_) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = local_kmt_;
      if (D3DKMTCloseAdapter(&close))
        WARN("Failed to close D3DKMT adapter");
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1) ||
        riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) ||
        riid == __uuidof(IDXGIAdapter4) || riid == __uuidof(IMTLDXGIAdapter)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == kWineDXGIAdapterUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(IDXGIAdapter), riid)) {
      WARN("DXGIAdapter: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    return factory_->QueryInterface(riid, ppParent);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) final {
    if (pDesc == nullptr)
      return FAIL(E_INVALIDARG);

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
    }

    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) final {
    if (pDesc == nullptr)
      return FAIL(E_INVALIDARG);

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *pDesc) final {
    if (pDesc == nullptr)
      return FAIL(E_INVALIDARG);

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
      pDesc->GraphicsPreemptionGranularity = desc.GraphicsPreemptionGranularity;
      pDesc->ComputePreemptionGranularity = desc.ComputePreemptionGranularity;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3 *pDesc) final {
    if (pDesc == nullptr)
      return FAIL(E_INVALIDARG);

    std::memset(pDesc->Description, 0, sizeof(pDesc->Description));

    if (!options_.customDeviceDesc.empty()) {
      str::transcodeString(
          pDesc->Description,
          sizeof(pDesc->Description) / sizeof(pDesc->Description[0]) - 1,
          options_.customDeviceDesc.c_str(), options_.customDeviceDesc.size());
    } else {
      device_.name().getCString((char *)pDesc->Description, sizeof(pDesc->Description), WMTUTF16StringEncoding);
    }

    // applications know the vendors of PC graphics cards and some leave when they meet another (God of War Ragnarök
    // exits at startup unless it is AMD, Intel or NVIDIA: measured 2026-10-04 over eight IDs). the vendor is AMD's
    // unless the configuration names one (dxgi.customVendorId; Apple's own is 106b); the name is the GPU's
    if (options_.customVendorId >= 0) {
      pDesc->VendorId = options_.customVendorId;
    } else {
      pDesc->VendorId = 0x1002;
      if (g_extension_enabled == VendorExtension::Nvidia) {
        pDesc->VendorId = 0x10DE;
      }
    }

    if (options_.customDeviceId >= 0) {
      pDesc->DeviceId = options_.customDeviceId;
    } else {
      pDesc->DeviceId = 0;
    }

    pDesc->SubSysId = 0;
    pDesc->Revision = 0;
    // as much of it as a SIZE_T holds, which in a 32-bit process is less than there is
    pDesc->DedicatedVideoMemory = std::min<uint64_t>(DeviceMemory(), std::numeric_limits<SIZE_T>::max());
    pDesc->DedicatedSystemMemory = 0;
    // the memory is unified (D3D12_FEATURE_DATA_ARCHITECTURE1::UMA), and applications size an integrated GPU's by its
    // shared system memory, as Windows drivers of integrated GPUs report most of theirs: Unity's D3D12 device filter
    // saw 0 MB and fell back to D3D11 (PEAK, 2026-10-24)
    pDesc->SharedSystemMemory = pDesc->DedicatedVideoMemory;
    pDesc->AdapterLuid = adapter_luid_;
    pDesc->Flags = DXGI_ADAPTER_FLAG3_NONE;
    pDesc->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    pDesc->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output,
                                        IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);

    if (ppOutput == nullptr)
      return FAIL(E_INVALIDARG);

    HMONITOR monitor = wsi::enumMonitors(Output);
    if (monitor == nullptr)
      return DXGI_ERROR_NOT_FOUND;

    *ppOutput = CreateOutput(this, monitor, options_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CheckInterfaceSupport(const GUID &guid, LARGE_INTEGER *umd_version) final {
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;

    if (guid == __uuidof(IDXGIDevice) || guid == __uuidof(ID3D10Device) ||
        guid == __uuidof(ID3D10Device1))
      hr = S_OK;

    // We can't really reconstruct the version numbers
    // returned by Windows drivers from Metal
    if (SUCCEEDED(hr) && umd_version)
      umd_version->QuadPart = ~0ull;

    if (FAILED(hr)) {
      Logger::err("DXGI: CheckInterfaceSupport: Unsupported interface");
      Logger::err(str::format(guid));
    }

    return hr;
  }

  // there is no hardware content protection, so none is ever torn down: the event stays as it is
  HRESULT STDMETHODCALLTYPE
  RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE event,
                                                       DWORD *cookie) override {
    if (!event || !cookie)
      return FAIL(DXGI_ERROR_INVALID_CALL);
    *cookie = ++event_cookie_;
    return S_OK;
  }

  void STDMETHODCALLTYPE
  UnregisterHardwareContentProtectionTeardownStatus(DWORD cookie) override {}

  // what Metal recommends the device's working set be, or less where the configuration says
  uint64_t
  DeviceMemory() {
    uint64_t memory = device_.recommendedMaxWorkingSetSize();
    return options_.maxDeviceMemory ? std::min(memory, options_.maxDeviceMemory) : memory;
  }

  HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo) override {
    if (NodeIndex > 0 || !pVideoMemoryInfo)
      return FAIL(E_INVALIDARG);

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return FAIL(E_INVALIDARG);

    // unified memory is all local (D3D12_MEMORY_POOL, Remarks): the other group has nothing. Windows drivers let
    // half of a group be reserved
    bool local = MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
    pVideoMemoryInfo->Budget = local ? DeviceMemory() : 0;
    pVideoMemoryInfo->CurrentUsage = local ? device_.currentAllocatedSize() : 0;
    pVideoMemoryInfo->AvailableForReservation = pVideoMemoryInfo->Budget / 2;
    // MULLION_TRACE_MEMORY=heavy: every query; a game polls it each frame
    if (trace::Heavy(trace::Memory) && local)
      TRACE_IN(Memory, "QueryVideoMemoryInfo: usage ", pVideoMemoryInfo->CurrentUsage >> 20, " MB of a budget of ",
               pVideoMemoryInfo->Budget >> 20, " MB");
    pVideoMemoryInfo->CurrentReservation =
        mem_reserved_[uint32_t(MemorySegmentGroup)];
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      UINT64 Reservation) override {
    DXGI_QUERY_VIDEO_MEMORY_INFO info;
    if (HRESULT hr = QueryVideoMemoryInfo(NodeIndex, MemorySegmentGroup, &info); FAILED(hr))
      return hr;
    if (Reservation > info.AvailableForReservation)
      return FAIL(DXGI_ERROR_INVALID_CALL);

    mem_reserved_[uint32_t(MemorySegmentGroup)] = Reservation;
    return S_OK;
  }

  // the budget is the device's, which does not change: the event is set once, as Windows sets it on registration
  HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(
      HANDLE event, DWORD *cookie) override {
    if (!event || !cookie)
      return FAIL(DXGI_ERROR_INVALID_CALL);
    SetEvent(event);
    *cookie = ++event_cookie_;
    return S_OK;
  }

  void STDMETHODCALLTYPE
  UnregisterVideoMemoryBudgetChangeNotification(DWORD cookie) override {
    // no op
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() final { return device_; }
  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  WMT::Reference<WMT::Device> device_;
  D3DKMT_HANDLE local_kmt_ = 0;
  const LUID adapter_luid_;
  Com<IDXGIFactory> factory_;
  DxgiOptions options_;
  uint64_t mem_reserved_[2] = {0, 0};
  std::atomic<DWORD> event_cookie_{0};
};

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config) {
  return Com<IMTLDXGIAdapter>::transfer(
      new MTLDXGIAdapter(Device, pFactory, config));
}

} // namespace mullion