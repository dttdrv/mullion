// contract: D3D12CreateDevice takes the selected DXCore or DXGI adapter by its LUID, checks the minimum feature
// level, and allows a null output to probe creation. "The minimum D3D_FEATURE_LEVEL required for successful device
// creation." and "S_FALSE is returned." (Microsoft Learn, D3D12CreateDevice, MinimumFeatureLevel/ppDevice).
// DXGI AdapterLuid is "A unique value that identifies the adapter."; DXCore InstanceLuid "contains a locally
// unique identifier representing the adapter"; DISPLAYCONFIG_PATH_SOURCE_INFO adapterId is "The identifier of
// the adapter that this source information relates to." D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME says "The LUID is
// returned from the D3DKMTOpenAdapterFromGdiDisplayName call." (Microsoft Learn, respective members). Wine's
// d3d12 test_create_device witnesses creation from an IDXCoreAdapter on Windows.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include "../feature_levels.hpp"
#include <cstdint>
#include <climits>

// D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME and D3DKMT_CLOSEADAPTER, d3dkmthk.h (Microsoft Learn).
using NTSTATUS = LONG;
struct D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME {
  WCHAR DeviceName[CCHDEVICENAME];
  UINT hAdapter;
  LUID AdapterLuid;
  UINT VidPnSourceId;
};
struct D3DKMT_CLOSEADAPTER { UINT hAdapter; };

// DXCore Interface
// Copyright (C) Microsoft Corporation.
// Licensed under the MIT license.
// declarations from Microsoft DirectX-Headers, include/directx/dxcore_interface.h
enum class DXCoreAdapterProperty : uint32_t { InstanceLuid = 0 };
enum class DXCoreAdapterState : uint32_t;
enum class DXCoreAdapterPreference : uint32_t;
enum class DXCoreNotificationType : uint32_t;
using PFN_DXCORE_NOTIFICATION_CALLBACK = void(STDMETHODCALLTYPE *)(DXCoreNotificationType, IUnknown *, void *);

struct IDXCoreAdapter : public IUnknown {
  virtual bool STDMETHODCALLTYPE IsValid() = 0;
  virtual bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID attributeGUID) = 0;
  virtual bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty property) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty property, size_t bufferSize,
                                                void *propertyData) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty property, size_t *bufferSize) = 0;
  virtual bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState property) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                               const void *inputStateDetails, size_t outputBufferSize,
                                               void *outputBuffer) = 0;
  virtual bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState property) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState state, size_t inputStateDetailsSize,
                                             const void *inputStateDetails, size_t inputDataSize,
                                             const void *inputData) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapter, 0xf0db4c7f, 0xfe5a, 0x42a2, 0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e)

struct IDXCoreAdapterList : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetAdapter(uint32_t index, REFIID riid, void **ppvAdapter) = 0;
  virtual uint32_t STDMETHODCALLTYPE GetAdapterCount() = 0;
  virtual bool STDMETHODCALLTYPE IsStale() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFactory(REFIID riid, void **ppvFactory) = 0;
  virtual HRESULT STDMETHODCALLTYPE Sort(uint32_t numPreferences, const DXCoreAdapterPreference *preferences) = 0;
  virtual bool STDMETHODCALLTYPE IsAdapterPreferenceSupported(DXCoreAdapterPreference preference) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapterList, 0x526c7776, 0x40e9, 0x459b, 0xb7, 0x11, 0xf3, 0x2a, 0xd7, 0x6d, 0xfc, 0x28)
const GUID DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS = {
    0x0c9ece4d, 0x2f6e, 0x4f01, {0x8c, 0x96, 0xe8, 0x9e, 0x33, 0x1b, 0x47, 0xb1}};

struct IDXCoreAdapterFactory : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateAdapterList(uint32_t numAttributes, const GUID *filterAttributes, REFIID riid,
                                                      void **ppvAdapterList) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAdapterByLuid(const LUID &adapterLUID, REFIID riid, void **ppvAdapter) = 0;
  virtual bool STDMETHODCALLTYPE IsNotificationTypeSupported(DXCoreNotificationType notificationType) = 0;
  virtual HRESULT STDMETHODCALLTYPE RegisterEventNotification(IUnknown *dxCoreObject,
                                                              DXCoreNotificationType notificationType,
                                                              PFN_DXCORE_NOTIFICATION_CALLBACK callbackFunction,
                                                              void *callbackContext, uint32_t *eventCookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnregisterEventNotification(uint32_t eventCookie) = 0;
};
__CRT_UUID_DECL(IDXCoreAdapterFactory, 0x78ee5945, 0xc36e, 0x4b13, 0xa6, 0x69, 0x00, 0x5d, 0xd1, 0x1c, 0x0f, 0x06)

// a real adapter with a chosen LUID or property error, to tell failure from selecting the default adapter
struct Adapter : IDXCoreAdapter {
  ComPtr<IDXCoreAdapter> core;
  LUID luid{};
  HRESULT property = S_OK;
  ULONG refs = 1;
  bool exposed = true;

  Adapter(IDXCoreAdapter *core, LUID luid) : core(core), luid(luid) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && (iid != __uuidof(IDXCoreAdapter) || !exposed))
      return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  bool STDMETHODCALLTYPE IsValid() override { return core->IsValid(); }
  bool STDMETHODCALLTYPE IsAttributeSupported(REFGUID attribute) override {
    return core->IsAttributeSupported(attribute);
  }
  bool STDMETHODCALLTYPE IsPropertySupported(DXCoreAdapterProperty p) override { return core->IsPropertySupported(p); }
  HRESULT STDMETHODCALLTYPE GetProperty(DXCoreAdapterProperty p, size_t size, void *data) override {
    if (p != DXCoreAdapterProperty::InstanceLuid)
      return core->GetProperty(p, size, data);
    if (!data)
      return E_POINTER;
    if (size < sizeof(luid))
      return E_INVALIDARG;
    memcpy(data, &luid, sizeof(luid));
    return property;
  }
  HRESULT STDMETHODCALLTYPE GetPropertySize(DXCoreAdapterProperty p, size_t *size) override {
    return core->GetPropertySize(p, size);
  }
  bool STDMETHODCALLTYPE IsQueryStateSupported(DXCoreAdapterState p) override { return core->IsQueryStateSupported(p); }
  HRESULT STDMETHODCALLTYPE QueryState(DXCoreAdapterState p, size_t details_size, const void *details, size_t size,
                                       void *data) override {
    return core->QueryState(p, details_size, details, size, data);
  }
  bool STDMETHODCALLTYPE IsSetStateSupported(DXCoreAdapterState p) override { return core->IsSetStateSupported(p); }
  HRESULT STDMETHODCALLTYPE SetState(DXCoreAdapterState p, size_t details_size, const void *details, size_t size,
                                     const void *data) override {
    return core->SetState(p, details_size, details, size, data);
  }
  HRESULT STDMETHODCALLTYPE GetFactory(REFIID iid, void **out) override { return core->GetFactory(iid, out); }
};

int
main(int, char **) {
  auto dll = LoadLibraryA("dxcore.dll");
  auto create = dll ? (HRESULT(WINAPI *)(REFIID, void **))GetProcAddress(dll, "DXCoreCreateAdapterFactory") : nullptr;
  if (!create) {
    if (dll)
      FreeLibrary(dll);
    printf("skipped: DXCoreCreateAdapterFactory unavailable\n");
    return 77;
  }
  ComPtr<IDXCoreAdapterFactory> core_factory;
  ComPtr<IDXGIFactory4> factory;
  CHECK(create(IID_PPV_ARGS(&core_factory)));
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXCoreAdapterList> core_list;
  CHECK(core_factory->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, IID_PPV_ARGS(&core_list)));
  std::vector<LUID> core_luids;
  for (UINT i = 0; i < core_list->GetAdapterCount(); i++) {
    step("DXCore adapter %u: independent enumeration", i);
    ComPtr<IDXCoreAdapter> core;
    LUID luid{};
    CHECK(core_list->GetAdapter(i, IID_PPV_ARGS(&core)));
    CHECK(core->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(luid), &luid));
    core_luids.push_back(luid);
  }
  if (!expect(!core_luids.empty(), "DXCore enumerated no graphics adapters"))
    return verdict();
  core_list.Reset();
  step("primary display: D3DKMT adapter identity");
  auto gdi = GetModuleHandleA("gdi32.dll");
  auto open_adapter = gdi ? reinterpret_cast<NTSTATUS(WINAPI *)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *)>(
                                GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"))
                          : nullptr;
  auto close_adapter = gdi ? reinterpret_cast<NTSTATUS(WINAPI *)(const D3DKMT_CLOSEADAPTER *)>(
                                 GetProcAddress(gdi, "D3DKMTCloseAdapter"))
                           : nullptr;
  if (open_adapter && close_adapter) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!expect(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor), "no primary monitor"))
      return verdict();
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open{};
    memcpy(open.DeviceName, monitor.szDevice, sizeof(open.DeviceName));
    NTSTATUS status = open_adapter(&open);
    if (!expect(status == 0, "display identity query returned %08lx", status))
      return verdict();
    ComPtr<IDXGIAdapter> dxgi;
    HRESULT hr = factory->EnumAdapterByLuid(open.AdapterLuid, IID_PPV_ARGS(&dxgi));
    D3DKMT_CLOSEADAPTER close{open.hAdapter};
    expect(close_adapter(&close) == 0, "display identity handle was not closed");
    if (!expect(hr == S_OK && dxgi, "D3DKMT LUID %08lx:%08lx absent from DXGI (%08lx)",
                open.AdapterLuid.HighPart, open.AdapterLuid.LowPart, hr)) {
      ComPtr<IDXGIAdapter> first;
      DXGI_ADAPTER_DESC desc{};
      CHECK(factory->EnumAdapters(0, &first));
      CHECK(first->GetDesc(&desc));
      printf("DXGI LUID %08lx:%08lx, D3DKMT LUID %08lx:%08lx\n", desc.AdapterLuid.HighPart,
             desc.AdapterLuid.LowPart, open.AdapterLuid.HighPart, open.AdapterLuid.LowPart);
      for (auto luid : core_luids)
        printf("DXCore LUID %08lx:%08lx\n", luid.HighPart, luid.LowPart);
      return verdict();
    }
  } else {
    printf("skipped: D3DKMT display identity query unavailable\n");
  }
  step("display paths: QueryDisplayConfig adapter identity");
  auto user = GetModuleHandleA("user32.dll");
  auto sizes = user ? reinterpret_cast<decltype(&GetDisplayConfigBufferSizes)>(
                          GetProcAddress(user, "GetDisplayConfigBufferSizes"))
                    : nullptr;
  auto query = user ? reinterpret_cast<decltype(&QueryDisplayConfig)>(GetProcAddress(user, "QueryDisplayConfig"))
                    : nullptr;
  if (sizes && query) {
    UINT path_count, mode_count;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status;
    do {
      if (!expect(sizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) == ERROR_SUCCESS,
                  "display path sizing failed"))
        return verdict();
      paths.resize(path_count);
      modes.resize(mode_count);
      status = query(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    } while (status == ERROR_INSUFFICIENT_BUFFER);
    if (!expect(status == ERROR_SUCCESS && path_count != 0, "display path query returned %ld, paths %u", status,
                path_count))
      return verdict();
    for (UINT i = 0; i < path_count; i++) {
      step("display path %u: source and target adapter identities", i);
      for (auto luid : {paths[i].sourceInfo.adapterId, paths[i].targetInfo.adapterId}) {
        ComPtr<IDXGIAdapter> dxgi;
        HRESULT hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&dxgi));
        if (!expect(hr == S_OK && dxgi, "display path LUID %08lx:%08lx absent from DXGI (%08lx)", luid.HighPart,
                    luid.LowPart, hr))
          return verdict();
        DXGI_ADAPTER_DESC desc{};
        CHECK(dxgi->GetDesc(&desc));
        expect(desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart,
               "DXGI LUID %08lx:%08lx, display path LUID %08lx:%08lx", desc.AdapterLuid.HighPart,
               desc.AdapterLuid.LowPart, luid.HighPart, luid.LowPart);
      }
    }
  } else {
    printf("skipped: QueryDisplayConfig unavailable\n");
  }
  UINT count = 0;
  for (;; count++) {
    step("adapter %u: DXGI and DXCore enumeration", count);
    ComPtr<IDXGIAdapter> dxgi;
    HRESULT hr = factory->EnumAdapters(count, &dxgi);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    CHECK(hr);
    DXGI_ADAPTER_DESC desc{};
    CHECK(dxgi->GetDesc(&desc));
    ComPtr<IDXCoreAdapter> core;
    hr = core_factory->GetAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&core));
    if (!expect(hr == S_OK && core, "DXCore lookup returned %08lx for DXGI LUID %08lx:%08lx", hr,
                desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart)) {
      for (UINT i = 0; i < core_luids.size(); i++)
        printf("DXGI adapter %u LUID %08lx:%08lx, DXCore adapter %u LUID %08lx:%08lx\n", count,
               desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, i, core_luids[i].HighPart, core_luids[i].LowPart);
      return verdict();
    }
    LUID core_luid{};
    CHECK(core->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(core_luid), &core_luid));
    expect(core_luid.HighPart == desc.AdapterLuid.HighPart && core_luid.LowPart == desc.AdapterLuid.LowPart,
           "DXGI LUID %08lx:%08lx, DXCore LUID %08lx:%08lx", desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart,
           core_luid.HighPart, core_luid.LowPart);
    Adapter wrapped(core.Get(), core_luid);
    ComPtr<IUnknown> dxgi_unknown, core_unknown;
    CHECK(dxgi.As(&dxgi_unknown));
    CHECK(core.As(&core_unknown));
    std::vector<D3D_FEATURE_LEVEL> requested = {D3D12_TEST_FEATURE_LEVELS};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{(UINT)requested.size(), requested.data()};
    ComPtr<ID3D12Device> reference;
    CHECK(make_device(dxgi.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&reference)));
    CHECK(reference->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
    reference.Reset();
    expect(levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0, "no D3D12 feature level reported");
    requested.push_back(static_cast<D3D_FEATURE_LEVEL>(~0u));
    for (auto *input : {dxgi_unknown.Get(), core_unknown.Get(), (IUnknown *)&wrapped}) {
      const char *kind = input == dxgi_unknown.Get()   ? "DXGI"
                         : input == core_unknown.Get() ? "DXCore"
                                                       : "wrapped DXCore";
      for (bool probe : {false, true}) {
        ComPtr<ID3D12Device> device;
        for (auto level : requested) {
          step("adapter %u: %s, feature level %04x, %s", count, kind, level, probe ? "probe" : "create");
          bool supported = level >= D3D_FEATURE_LEVEL_11_0 && level <= levels.MaxSupportedFeatureLevel;
          hr = D3D12CreateDevice(input, level, __uuidof(ID3D12Device),
                                 probe ? nullptr : (void **)device.GetAddressOf());
          expect(hr == (supported ? (probe ? S_FALSE : S_OK) : E_INVALIDARG), "creation returned %08lx", hr);
          if (!probe && supported && expect(device != nullptr, "creation returned no device")) {
            auto got = device->GetAdapterLuid();
            expect(got.HighPart == core_luid.HighPart && got.LowPart == core_luid.LowPart,
                   "device LUID %08lx:%08lx, expected %08lx:%08lx", got.HighPart, got.LowPart, core_luid.HighPart,
                   core_luid.LowPart);
          } else {
            expect(!device, "probe or unsupported level returned a device");
          }
          device.Reset();
          expect(wrapped.refs == 1, "adapter retained %lu references", wrapped.refs);
        }
        step("adapter %u: %s, unsupported device interface, probe %u", count, kind, probe);
        hr = D3D12CreateDevice(input, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12DeviceChild),
                              probe ? nullptr : (void **)device.GetAddressOf());
        expect(hr == (probe ? S_FALSE : E_NOINTERFACE) && !device, "unsupported interface returned %08lx", hr);
        expect(wrapped.refs == 1, "unsupported interface retained %lu references", wrapped.refs);
      }
    }
    if (!count) {
      for (bool probe : {false, true}) {
        step("default adapter: %s", probe ? "probe" : "create");
        ComPtr<ID3D12Device> device;
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                               probe ? nullptr : (void **)device.GetAddressOf());
        expect(hr == (probe ? S_FALSE : S_OK), "default creation returned %08lx", hr);
        if (device) {
          auto got = device->GetAdapterLuid();
          expect(got.HighPart == core_luid.HighPart && got.LowPart == core_luid.LowPart, "wrong default adapter LUID");
        }
      }
    }
    for (bool probe : {false, true}) {
      ComPtr<ID3D12Device> device;
      for (HRESULT error : {E_INVALIDARG, E_OUTOFMEMORY}) {
        Adapter wrapped(core.Get(), core_luid);
        step("adapter %u: property error %08lx, probe %u", count, error, probe);
        wrapped.property = error;
        hr = D3D12CreateDevice(&wrapped, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                               probe ? nullptr : (void **)device.GetAddressOf());
        expect(FAILED(hr) && !device, "property error returned %08lx", hr);
        expect(wrapped.refs == 1, "property error retained %lu references", wrapped.refs);
        device.Reset();
      }
      for (size_t bit = 0; bit < sizeof(LUID) * CHAR_BIT; bit++) {
        Adapter wrapped(core.Get(), core_luid);
        reinterpret_cast<unsigned char *>(&wrapped.luid)[bit / CHAR_BIT] ^= 1u << (bit % CHAR_BIT);
        ComPtr<IDXGIAdapter> found;
        step("adapter %u: LUID bit %zu differs, probe %u", count, bit, probe);
        hr = factory->EnumAdapterByLuid(wrapped.luid, IID_PPV_ARGS(&found));
        if (hr == S_OK) {
          DXGI_ADAPTER_DESC got{};
          CHECK(found->GetDesc(&got));
          if (!expect(got.AdapterLuid.HighPart == wrapped.luid.HighPart &&
                          got.AdapterLuid.LowPart == wrapped.luid.LowPart,
                      "lookup returned LUID %08lx:%08lx for %08lx:%08lx", got.AdapterLuid.HighPart,
                      got.AdapterLuid.LowPart, wrapped.luid.HighPart, wrapped.luid.LowPart))
            return verdict();
          continue;
        }
        if (!expect(hr == DXGI_ERROR_NOT_FOUND, "missing LUID lookup returned %08lx", hr))
          return verdict();
        hr = D3D12CreateDevice(&wrapped, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                               probe ? nullptr : (void **)device.GetAddressOf());
        expect(FAILED(hr) && !device, "missing adapter returned %08lx", hr);
        expect(wrapped.refs == 1, "missing adapter retained %lu references", wrapped.refs);
        device.Reset();
      }
      Adapter wrapped(core.Get(), core_luid);
      wrapped.exposed = false;
      step("adapter %u: IUnknown without an adapter interface, probe %u", count, probe);
      hr = D3D12CreateDevice(&wrapped, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                             probe ? nullptr : (void **)device.GetAddressOf());
      expect(FAILED(hr) && !device, "non-adapter returned %08lx", hr);
      expect(wrapped.refs == 1, "non-adapter retained %lu references", wrapped.refs);
      device.Reset();
    }
  }
  expect(count != 0, "no adapters exercised");
  core_factory.Reset();
  FreeLibrary(dll);
  return verdict();
}
