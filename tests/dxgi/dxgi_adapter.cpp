// contract: a DXGI factory's adapters, and what an adapter says of its memory.
// - EnumAdapters1 gives adapter 0, and "DXGI_ERROR_NOT_FOUND if the index is greater than or equal to the number of
//   local adapters" (IDXGIFactory1::EnumAdapters1); an adapter's parent is its factory (IDXGIObject::GetParent), and
//   its three descriptions are of one adapter.
// - EnumAdapterByLuid "outputs the IDXGIAdapter for the specified LUID", "designed to be paired with
//   ID3D12Device::GetAdapterLuid": the device's LUID finds its adapter, and a LUID of no adapter finds none.
// - EnumWarpAdapter "provides an adapter which can be provided to D3D12CreateDevice" (IDXGIFactory4::EnumWarpAdapter,
//   https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgifactory4-enumwarpadapter).
// - CheckInterfaceSupport: "S_OK indicates that the interface is supported, otherwise DXGI_ERROR_UNSUPPORTED".
// - QueryVideoMemoryInfo: a budget for the local group, the application's usage in it, which grows by a texture's
//   size when one is made, and no more to reserve than the budget. "When the adapter is UMA, D3D12_MEMORY_POOL_L0 and
//   DXGI_MEMORY_SEGMENT_GROUP_LOCAL refer to the same memory" (D3D12_MEMORY_POOL), so the non-local group of such an
//   adapter has no budget and no usage. a reservation set (SetVideoMemoryReservation) is the current one; one above
//   what is available is refused, and a budget event is set when registered, as Windows drivers do (DXVK's
//   dxgi_adapter.cpp records both).
// - the adapter has the GPU's name and, for applications that leave unless they know the vendor, AMD's vendor ID;
//   the configuration names another (dxgi.customVendorId), Apple's own included, and changes nothing else. the
//   environment's (MULLION_CONFIG) overrides the file's, also after the file opens another executable's section.
//   dxgi.maxDeviceMemory caps the memory the adapter reports and the local budget, in MB, as DXVK's option does.
// - a gamma ramp is an output's, a monitor's (IDXGIOutput::SetGammaControl, GetGammaControl): set through one object
//   of the output, it is what another one gets.
//   gamma capabilities are "only supported while in full-screen mode" (IDXGIOutput::GetGammaControlCapabilities).
//   https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-getgammacontrolcapabilities
#include "../d3d12/d3d12_test.hpp"
#include <d3d10.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <limits>

int
main(int argc, char **argv) {
  // a second argument sets the vendor through the configuration before anything reads it
  UINT vendor = 0x1002;
  // a fourth caps the memory the adapter reports, in MB (dxgi.maxDeviceMemory)
  const UINT64 memory_cap = argc > 4 ? strtoull(argv[4], nullptr, 10) << 20 : 0;
  if (argc > 2) {
    vendor = strtoul(argv[2], nullptr, 16);
    std::string line = std::string("dxgi.customVendorId=") + argv[2];
    if (memory_cap)
      line += std::string("; dxgi.maxDeviceMemory=") + argv[4];
    SetEnvironmentVariableA("MULLION_CONFIG", line.c_str());
  }
  // a third, when it is "-", names a file that sets NVIDIA's vendor and ends in another executable's section
  char file[MAX_PATH] = {};
  if (argc > 3 && !strcmp(argv[3], "-")) {
    char dir[MAX_PATH];
    FILE *conf = GetTempPathA(MAX_PATH, dir) && GetTempFileNameA(dir, "mul", 0, file) ? fopen(file, "w") : nullptr;
    if (!conf || fputs("dxgi.customVendorId=10de\n[another.exe]\n", conf) < 0 || fclose(conf))
      return printf("the configuration file\n"), 1;
    SetEnvironmentVariableA("MULLION_CONFIG_FILE", file);
  }
  unsigned failures = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && failures++ < 16)
      printf("%s\n", what);
  };
  ComPtr<IDXGIFactory6> factory;
  CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> first, past;
  CHECK(factory->EnumAdapters1(0, &first));
  UINT count = 1;
  for (ComPtr<IDXGIAdapter1> next; SUCCEEDED(factory->EnumAdapters1(count, &next)); count++)
    ;
  expect(factory->EnumAdapters1(count, &past) == DXGI_ERROR_NOT_FOUND && !past, "an adapter past the last");
  ComPtr<IDXGIFactory> parent;
  expect(SUCCEEDED(first->GetParent(IID_PPV_ARGS(&parent))) && parent.Get() == factory.Get(), "an adapter's parent is not its factory");

  ComPtr<IDXGIAdapter4> adapter;
  CHECK(first.As(&adapter));
  DXGI_ADAPTER_DESC desc;
  DXGI_ADAPTER_DESC1 desc1;
  DXGI_ADAPTER_DESC3 desc3;
  CHECK(adapter->GetDesc(&desc));
  CHECK(adapter->GetDesc1(&desc1));
  CHECK(adapter->GetDesc3(&desc3));
  auto same = [](const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; };
  expect(same(desc.AdapterLuid, desc1.AdapterLuid) && same(desc.AdapterLuid, desc3.AdapterLuid) &&
             desc.VendorId == desc3.VendorId && desc.DeviceId == desc3.DeviceId &&
             desc.DedicatedVideoMemory == desc3.DedicatedVideoMemory && !wcscmp(desc.Description, desc3.Description),
         "the adapter's descriptions differ");
  expect(desc.Description[0] && desc.DedicatedVideoMemory, "an adapter without a name or memory");
  // AMD's (PCI-SIG 0x1002), or the one the test is run with (argv: a vendor ID as the configuration takes it)
  expect(desc.VendorId == vendor, "the adapter's vendor is not the one expected");
  if (*file)
    DeleteFileA(file);
  expect(wcsstr(desc.Description, L"Apple"), "the adapter's name is not the GPU's");

  // each descriptor describes the same physical adapter (DXGI_ADAPTER_DESC, DESC1, DESC2 and DESC3).
  // GetDesc2: "Gets a DXGI 1.2 description of an adapter (or video card)."
  // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiadapter2-getdesc2
  for (UINT index = 0;; index++) {
    step("adapter %u complete descriptor fields and null outputs", index);
    ComPtr<IDXGIAdapter1> enumerated;
    HRESULT hr = factory->EnumAdapters1(index, &enumerated);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    CHECK(hr);
    ComPtr<IDXGIAdapter4> current;
    CHECK(enumerated.As(&current));
    DXGI_ADAPTER_DESC a{};
    DXGI_ADAPTER_DESC1 b{};
    DXGI_ADAPTER_DESC2 c{};
    DXGI_ADAPTER_DESC3 d{};
    CHECK(current->GetDesc(&a));
    CHECK(current->GetDesc1(&b));
    CHECK(current->GetDesc2(&c));
    CHECK(current->GetDesc3(&d));
    auto fields = [&](const auto &left, const auto &right) {
      return !wcsncmp(left.Description, right.Description, ARRAYSIZE(left.Description)) &&
             left.VendorId == right.VendorId && left.DeviceId == right.DeviceId &&
             left.SubSysId == right.SubSysId && left.Revision == right.Revision &&
             left.DedicatedVideoMemory == right.DedicatedVideoMemory &&
             left.DedicatedSystemMemory == right.DedicatedSystemMemory &&
             left.SharedSystemMemory == right.SharedSystemMemory && same(left.AdapterLuid, right.AdapterLuid);
    };
    ::expect(fields(a, b) && fields(a, c) && fields(a, d), "descriptor versions disagree on a field");
    ::expect(b.Flags == c.Flags && b.Flags == (d.Flags & (DXGI_ADAPTER_FLAG_REMOTE | DXGI_ADAPTER_FLAG_SOFTWARE)),
             "descriptor flags disagree");
    ::expect(c.GraphicsPreemptionGranularity == d.GraphicsPreemptionGranularity &&
                 c.ComputePreemptionGranularity == d.ComputePreemptionGranularity &&
                 d.GraphicsPreemptionGranularity <= DXGI_GRAPHICS_PREEMPTION_INSTRUCTION_BOUNDARY &&
                 d.ComputePreemptionGranularity <= DXGI_COMPUTE_PREEMPTION_INSTRUCTION_BOUNDARY,
             "descriptor preemption values");
    ::expect(current->GetDesc(nullptr) == E_INVALIDARG && current->GetDesc1(nullptr) == E_INVALIDARG &&
                 current->GetDesc2(nullptr) == E_INVALIDARG && current->GetDesc3(nullptr) == E_INVALIDARG,
             "NULL descriptor outputs");

    // CheckInterfaceSupport: "only to check whether a Direct3D 10.x interface is supported";
    // later Direct3D versions return DXGI_ERROR_UNSUPPORTED even when device creation works.
    // https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-checkinterfacesupport
    step("adapter %u interface support and driver-version output", index);
    for (const GUID &iid : {__uuidof(IDXGIDevice), __uuidof(ID3D10Device), __uuidof(ID3D10Device1)}) {
      LARGE_INTEGER first{}, repeated{};
      repeated.QuadPart = -1;
      HRESULT a = current->CheckInterfaceSupport(iid, &first);
      HRESULT b = current->CheckInterfaceSupport(iid, &repeated);
      ::expect(a == S_OK && b == S_OK && first.QuadPart == repeated.QuadPart,
               "D3D10 interface/version output: %#lx / %#lx", a, b);
    }
    for (const GUID &iid : {__uuidof(IUnknown), __uuidof(IDXGIFactory), __uuidof(ID3D11Device),
                           __uuidof(ID3D11Device1), __uuidof(ID3D11Device2), __uuidof(ID3D11Device3),
                           __uuidof(ID3D11Device4), __uuidof(ID3D11Device5), __uuidof(ID3D12Device)}) {
      LARGE_INTEGER ignored{};
      ::expect(current->CheckInterfaceSupport(iid, &ignored) == DXGI_ERROR_UNSUPPORTED,
               "interface outside D3D10 accepted");
    }
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  LUID luid = device->GetAdapterLuid(), other = luid;
  other.LowPart = ~other.LowPart;
  ComPtr<IDXGIAdapter> found, missing;
  expect(SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&found))) && found && same(luid, desc.AdapterLuid),
         "the device's LUID does not find its adapter");
  expect(FAILED(factory->EnumAdapterByLuid(other, IID_PPV_ARGS(&missing))) && !missing, "a LUID of no adapter finds one");

  ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> software;
  expect(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)) == S_OK && warp &&
             SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&software))) && software,
         "the WARP adapter makes no device");

  LARGE_INTEGER version;
  expect(adapter->CheckInterfaceSupport(__uuidof(ID3D10Device), &version) == S_OK, "no Direct3D 10 on the adapter");
  expect(adapter->CheckInterfaceSupport(__uuidof(IDXGIFactory), &version) == DXGI_ERROR_UNSUPPORTED, "the adapter supports a factory");

  // an output is its monitor, whichever object stands for it
  ComPtr<IDXGIOutput> one, twin;
  if (SUCCEEDED(adapter->EnumOutputs(0, &one)) && SUCCEEDED(adapter->EnumOutputs(0, &twin))) {
    DXGI_OUTPUT_DESC output_desc{};
    CHECK(one->GetDesc(&output_desc));
    const auto &bounds = output_desc.DesktopCoordinates;
    HWND window = CreateWindowW(L"static", L"gamma", 0, bounds.left, bounds.top, bounds.right - bounds.left,
                                bounds.bottom - bounds.top, nullptr, nullptr, nullptr, nullptr);
    if (!window)
      return 1;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    DXGI_SWAP_CHAIN_DESC swapchain_desc{};
    swapchain_desc.BufferDesc = {UINT(bounds.right - bounds.left), UINT(bounds.bottom - bounds.top), {},
                                 DXGI_FORMAT_R8G8B8A8_UNORM};
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2;
    swapchain_desc.OutputWindow = window;
    swapchain_desc.Windowed = TRUE;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain> swapchain;
    CHECK(factory->CreateSwapChain(queue.Get(), &swapchain_desc, &swapchain));
    CHECK(swapchain->SetFullscreenState(TRUE, one.Get()));
    one.Reset();
    twin.Reset();
    CHECK(swapchain->GetContainingOutput(&one));
    CHECK(swapchain->GetContainingOutput(&twin));
    DXGI_GAMMA_CONTROL_CAPABILITIES caps{};
    DXGI_GAMMA_CONTROL set{}, got{}, original{};
    HRESULT gamma = one->GetGammaControlCapabilities(&caps);
    expect(gamma == S_OK || gamma == DXGI_ERROR_UNSUPPORTED, "fullscreen gamma capabilities");
    if (gamma == S_OK) {
      bool bounded = caps.NumGammaControlPoints > 0 && caps.NumGammaControlPoints <= ARRAYSIZE(caps.ControlPointPositions) &&
                     caps.NumGammaControlPoints <= ARRAYSIZE(set.GammaCurve) && std::isfinite(caps.MinConvertedValue) &&
                     std::isfinite(caps.MaxConvertedValue) && caps.MinConvertedValue <= caps.MaxConvertedValue;
      expect(bounded, "invalid gamma point count");
      HRESULT saved = one->GetGammaControl(&original);
      expect(saved == S_OK, "save original gamma");
      if (bounded && saved == S_OK) {
        set.Scale = {1, 1, 1};
        for (UINT i = 0; i < caps.NumGammaControlPoints; i++) {
          float u = caps.NumGammaControlPoints > 1 ? float(i) / (caps.NumGammaControlPoints - 1) : 0;
          set.GammaCurve[i] = {std::lerp(caps.MinConvertedValue, caps.MaxConvertedValue, u),
                               std::lerp(caps.MinConvertedValue, caps.MaxConvertedValue, 1 - u),
                               std::lerp(caps.MinConvertedValue, caps.MaxConvertedValue, u * u / 2)};
        }
        bool same = SUCCEEDED(one->SetGammaControl(&set)) && SUCCEEDED(twin->GetGammaControl(&got));
        for (UINT i = 0; same && i < caps.NumGammaControlPoints; i++)
          same = !memcmp(&set.GammaCurve[i], &got.GammaCurve[i], sizeof(DXGI_RGB));
        expect(same, "a gamma ramp set on a monitor's output is not what another output of it has");
        expect(one->SetGammaControl(&original) == S_OK, "restore original gamma");
      }
    }
    expect(swapchain->SetFullscreenState(FALSE, nullptr) == S_OK, "leave gamma fullscreen");
    swapchain.Reset();
    DestroyWindow(window);
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO local, non_local, after;
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local));
  expect(local.Budget && local.AvailableForReservation && local.AvailableForReservation <= local.Budget && !local.CurrentReservation,
         "the local group's budget and reservation");
  // the configuration's cap is the adapter's memory and the local budget, where the GPU has more; usage stays what it is
  if (memory_cap) {
    DXGI_ADAPTER_DESC1 capped;
    CHECK(adapter->GetDesc1(&capped));
    expect(capped.DedicatedVideoMemory == memory_cap && capped.SharedSystemMemory == memory_cap,
           "the adapter's memory is not the configured cap");
    expect(local.Budget == memory_cap, "the local budget is not the configured cap");
  }
  D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)));
  if (architecture.UMA)
    expect(!non_local.Budget && !non_local.CurrentUsage && !non_local.AvailableForReservation,
           "unified memory has a non-local group");
  // a texture's memory is the application's usage
  const UINT side = 2048;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)
  ));
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after));
  UINT64 texture_bytes = device->GetResourceAllocationInfo(0, 1, &texture_desc).SizeInBytes;
  expect(after.CurrentUsage >= local.CurrentUsage + side * side * 4 && texture_bytes >= side * side * 4,
         "a texture's memory is not in the local group's usage");

  expect(adapter->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, local.AvailableForReservation) == S_OK,
         "what is available is not reserved");
  expect(FAILED(adapter->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, local.AvailableForReservation + 1)),
         "more than is available is reserved");
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after));
  expect(after.CurrentReservation == local.AvailableForReservation, "the reservation set is not the current one");

  // QueryVideoMemoryInfo: "The index of the device's physical adapter for which the video memory information
  // is queried." node indices are bounded by ID3D12Device::GetNodeCount, not the number of adapters in a factory.
  // https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo
  for (auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
    step("memory group %u query/reservation boundaries", group);
    DXGI_QUERY_VIDEO_MEMORY_INFO initial{}, got{};
    CHECK(adapter->QueryVideoMemoryInfo(0, group, &initial));
    ::expect(adapter->QueryVideoMemoryInfo(0, group, nullptr) == E_INVALIDARG, "NULL memory info");
    for (UINT node : {device->GetNodeCount(), std::numeric_limits<UINT>::max()}) {
      ::expect(adapter->QueryVideoMemoryInfo(node, group, &got) == E_INVALIDARG, "invalid memory node");
      ::expect(adapter->SetVideoMemoryReservation(node, group, 0) == E_INVALIDARG, "invalid reservation node");
    }
    for (UINT64 value : {UINT64(0), std::min(UINT64(1), initial.AvailableForReservation),
                         initial.AvailableForReservation / 2, initial.AvailableForReservation}) {
      CHECK(adapter->SetVideoMemoryReservation(0, group, value));
      CHECK(adapter->QueryVideoMemoryInfo(0, group, &got));
      ::expect(got.CurrentReservation == value, "reservation roundtrip");
      if (got.AvailableForReservation != std::numeric_limits<UINT64>::max()) {
        ::expect(FAILED(adapter->SetVideoMemoryReservation(0, group, got.AvailableForReservation + 1)),
                 "above-available reservation accepted");
        CHECK(adapter->QueryVideoMemoryInfo(0, group, &got));
        ::expect(got.CurrentReservation == value, "refused reservation changed state");
      }
    }
    CHECK(adapter->SetVideoMemoryReservation(0, group, initial.CurrentReservation));
  }
  for (UINT value : {UINT(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL) + 1, std::numeric_limits<UINT>::max()}) {
    auto group = DXGI_MEMORY_SEGMENT_GROUP(value);
    ::expect(adapter->QueryVideoMemoryInfo(0, group, &after) == E_INVALIDARG, "invalid segment group");
    ::expect(adapter->SetVideoMemoryReservation(0, group, 0) == E_INVALIDARG, "invalid reservation group");
  }
  CHECK(adapter->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, local.CurrentReservation));

  HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  DWORD cookie = 0, second = 0;
  step("memory budget event invalid arguments");
  ::expect(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(nullptr, &cookie) == DXGI_ERROR_INVALID_CALL,
           "NULL budget event");
  ::expect(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(event, nullptr) == DXGI_ERROR_INVALID_CALL,
           "NULL budget cookie");
  expect(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(event, &cookie) == S_OK &&
             WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
         "a registered budget event is not set");
  expect(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(event, &second) == S_OK && second != cookie,
         "two registrations have one cookie");
  adapter->UnregisterVideoMemoryBudgetChangeNotification(cookie);
  adapter->UnregisterVideoMemoryBudgetChangeNotification(second);
  CloseHandle(event);

  printf("%s: %u wrong of %u checks over %u adapters\n", failures ? "failed" : "passed", failures, checks, count);
  ::expect(!failures, "%u existing adapter checks failed", failures);
  return verdict();
}
