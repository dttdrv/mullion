// contract: a DXGI factory's adapters, and what an adapter says of its memory.
// - EnumAdapters1 gives adapter 0, and "DXGI_ERROR_NOT_FOUND if the index is greater than or equal to the number of
//   local adapters" (IDXGIFactory1::EnumAdapters1); an adapter's parent is its factory (IDXGIObject::GetParent), and
//   its three descriptions are of one adapter.
// - EnumAdapterByLuid "outputs the IDXGIAdapter for the specified LUID", "designed to be paired with
//   ID3D12Device::GetAdapterLuid": the device's LUID finds its adapter, and a LUID of no adapter finds none.
// - CheckInterfaceSupport: "S_OK indicates that the interface is supported, otherwise DXGI_ERROR_UNSUPPORTED".
// - QueryVideoMemoryInfo: a budget for the local group, the application's usage in it, which grows by a texture's
//   size when one is made, and no more to reserve than the budget. "When the adapter is UMA, D3D12_MEMORY_POOL_L0 and
//   DXGI_MEMORY_SEGMENT_GROUP_LOCAL refer to the same memory" (D3D12_MEMORY_POOL), so the non-local group of such an
//   adapter has no budget and no usage. a reservation set (SetVideoMemoryReservation) is the current one; one above
//   what is available is refused, and a budget event is set when registered, as Windows drivers do (DXVK's
//   dxgi_adapter.cpp records both).
// - the adapter has the GPU's name and, for applications that leave unless they know the vendor, AMD's vendor ID;
//   the configuration names another (dxgi.customVendorId), Apple's own included, and changes nothing else.
// - a gamma ramp is an output's, a monitor's (IDXGIOutput::SetGammaControl, GetGammaControl): set through one object
//   of the output, it is what another one gets.
#include "../d3d12/d3d12_test.hpp"
#include <d3d10.h>
#include <dxgi1_6.h>

int
main(int argc, char **argv) {
  // a second argument sets the vendor through the configuration before anything reads it
  UINT vendor = 0x1002;
  if (argc > 2) {
    vendor = strtoul(argv[2], nullptr, 16);
    SetEnvironmentVariableA("DXMT_CONFIG", (std::string("dxgi.customVendorId=") + argv[2]).c_str());
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
  expect(wcsstr(desc.Description, L"Apple"), "the adapter's name is not the GPU's");

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  LUID luid = device->GetAdapterLuid(), other = luid;
  other.LowPart = ~other.LowPart;
  ComPtr<IDXGIAdapter> found, missing;
  expect(SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&found))) && found && same(luid, desc.AdapterLuid),
         "the device's LUID does not find its adapter");
  expect(FAILED(factory->EnumAdapterByLuid(other, IID_PPV_ARGS(&missing))) && !missing, "a LUID of no adapter finds one");

  LARGE_INTEGER version;
  expect(adapter->CheckInterfaceSupport(__uuidof(ID3D10Device), &version) == S_OK, "no Direct3D 10 on the adapter");
  expect(adapter->CheckInterfaceSupport(__uuidof(IDXGIFactory), &version) == DXGI_ERROR_UNSUPPORTED, "the adapter supports a factory");

  // an output is its monitor, whichever object stands for it
  ComPtr<IDXGIOutput> one, twin;
  if (SUCCEEDED(adapter->EnumOutputs(0, &one)) && SUCCEEDED(adapter->EnumOutputs(0, &twin))) {
    DXGI_GAMMA_CONTROL_CAPABILITIES caps{};
    static DXGI_GAMMA_CONTROL set, got, identity;
    CHECK(one->GetGammaControlCapabilities(&caps));
    set.Scale = identity.Scale = {1, 1, 1};
    for (UINT i = 0; i < caps.NumGammaControlPoints; i++) {
      float at = caps.ControlPointPositions[i];
      set.GammaCurve[i] = {at * at, at, at * at * at};
      identity.GammaCurve[i] = {at, at, at};
    }
    bool same = SUCCEEDED(one->SetGammaControl(&set)) && SUCCEEDED(twin->GetGammaControl(&got));
    for (UINT i = 0; same && i < caps.NumGammaControlPoints; i++)
      same = !memcmp(&set.GammaCurve[i], &got.GammaCurve[i], sizeof(DXGI_RGB));
    expect(same && caps.NumGammaControlPoints > 1, "a gamma ramp set on a monitor's output is not what another output of it has");
    one->SetGammaControl(&identity);
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO local, non_local, after;
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local));
  expect(local.Budget && local.AvailableForReservation && local.AvailableForReservation <= local.Budget && !local.CurrentReservation,
         "the local group's budget and reservation");
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

  HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  DWORD cookie = 0, second = 0;
  expect(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(event, &cookie) == S_OK &&
             WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
         "a registered budget event is not set");
  expect(adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(event, &second) == S_OK && second != cookie,
         "two registrations have one cookie");
  adapter->UnregisterVideoMemoryBudgetChangeNotification(cookie);
  adapter->UnregisterVideoMemoryBudgetChangeNotification(second);
  CloseHandle(event);

  printf("%s: %u wrong of %u checks over %u adapters\n", failures ? "failed" : "passed", failures, checks, count);
  return failures != 0;
}
