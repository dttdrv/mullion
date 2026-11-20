// contract: every resource-creation entry point validates the buffer description before allocating a resource.
// "Height, DepthOrArraySize, and MipLevels must be 1"; "Format must be DXGI_FORMAT_UNKNOWN";
// SampleDesc.Count is 1 and Quality is 0 (Microsoft Learn, D3D12_RESOURCE_DESC, Buffers).
// each invalid field differs from one valid description; null-output validation must reject the same inputs.
// allocation queries report invalid descriptions: "If an error occurs, then
// D3D12_RESOURCE_ALLOCATION_INFO::SizeInBytes equals UINT64_MAX." (GetResourceAllocationInfo, Return value).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
#include "d3d12_test.hpp"
#include <limits>

int main(int, char **) {
  ComPtr<ID3D12Device10> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC hd{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, props, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
  CHECK(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
  D3D12_RESOURCE_DESC good{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  struct Case { const char *name; D3D12_RESOURCE_DESC desc; bool valid; };
  std::vector<Case> cases{{"valid one-byte buffer", good, true}};
  auto add = [&](const char *name, auto change) {
    auto desc = good;
    change(desc);
    cases.push_back({name, desc, false});
  };
  add("zero width", [](auto &d) { d.Width = 0; });
  for (UINT n : {0u, 2u, std::numeric_limits<UINT>::max()}) {
    add("height", [=](auto &d) { d.Height = n; });
    add("sample count", [=](auto &d) { d.SampleDesc.Count = n; });
  }
  for (UINT16 n : {UINT16(0), UINT16(2), std::numeric_limits<UINT16>::max()})
    add("depth/array size", [=](auto &d) { d.DepthOrArraySize = n; });
  for (UINT16 n : {UINT16(0), UINT16(2), std::numeric_limits<UINT16>::max()})
    add("mip count", [=](auto &d) { d.MipLevels = n; });
  for (UINT n : {1u, std::numeric_limits<UINT>::max()})
    add("sample quality", [=](auto &d) { d.SampleDesc.Quality = n; });
  for (auto format : {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_FORCE_UINT})
    add("format", [=](auto &d) { d.Format = format; });
  for (auto layout : {D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                       D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE})
    add("layout", [=](auto &d) { d.Layout = layout; });
  for (UINT64 alignment : {UINT64(D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT),
                           UINT64(D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)})
    add("alignment", [=](auto &d) { d.Alignment = alignment; });
  for (auto flags : {D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS})
    add("flags", [=](auto &d) { d.Flags = flags; });
  for (const auto &c : cases)
    for (UINT api = 0; api < 3; api++) {
      const auto &d = c.desc;
      step("allocation query %u, case %s", api, c.name);
      D3D12_RESOURCE_DESC1 d1{d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels,
                             d.Format, d.SampleDesc, d.Layout, d.Flags};
      D3D12_RESOURCE_ALLOCATION_INFO info;
      if (api == 0)
        info = device->GetResourceAllocationInfo(0, 1, &d);
      else if (api == 1)
        info = device->GetResourceAllocationInfo1(0, 1, &d, nullptr);
      else
        info = device->GetResourceAllocationInfo2(0, 1, &d1, nullptr);
      const UINT64 wanted = c.valid ? D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
                                    : std::numeric_limits<UINT64>::max();
      expect(info.SizeInBytes == wanted, "allocation size %llu, want %llu", info.SizeInBytes, wanted);
    }
  for (UINT api = 0; api < 7; api++)
    for (UINT i = 0; i < cases.size(); i++)
      for (bool validate_only : {false, true}) {
        const auto &c = cases[i];
        const auto &d = c.desc;
        step("creation API %u, case %u %s, %s", api, i, c.name, validate_only ? "validation" : "allocation");
        D3D12_RESOURCE_DESC1 d1{d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels,
                               d.Format, d.SampleDesc, d.Layout, d.Flags};
        ComPtr<ID3D12Resource> resource;
        void **out = validate_only ? nullptr : reinterpret_cast<void **>(resource.GetAddressOf());
        HRESULT hr = E_FAIL;
        switch (api) {
        case 0:
          hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, __uuidof(ID3D12Resource), out);
          break;
        case 1:
          hr = device->CreateCommittedResource1(&props, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, nullptr, __uuidof(ID3D12Resource), out);
          break;
        case 2:
          hr = device->CreateCommittedResource2(&props, D3D12_HEAP_FLAG_NONE, &d1, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, nullptr, __uuidof(ID3D12Resource), out);
          break;
        case 3:
          hr = device->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &d1, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                                 nullptr, nullptr, 0, nullptr, __uuidof(ID3D12Resource), out);
          break;
        case 4:
          hr = device->CreatePlacedResource(heap.Get(), 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             __uuidof(ID3D12Resource), out);
          break;
        case 5:
          hr = device->CreatePlacedResource1(heap.Get(), 0, &d1, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              __uuidof(ID3D12Resource), out);
          break;
        case 6:
          hr = device->CreatePlacedResource2(heap.Get(), 0, &d1, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr,
                                              __uuidof(ID3D12Resource), out);
          break;
        }
        HRESULT wanted = !c.valid ? E_INVALIDARG : validate_only ? S_FALSE : S_OK;
        expect(hr == wanted && bool(resource) == (c.valid && !validate_only),
               "creation %08lx, resource %p; want %08lx", hr, resource.Get(), wanted);
        if (c.valid && resource) {
          auto got = resource->GetDesc();
          expect(got.Dimension == d.Dimension && got.Width == d.Width && got.Height == d.Height &&
                     got.DepthOrArraySize == d.DepthOrArraySize && got.MipLevels == d.MipLevels &&
                     got.Format == d.Format && got.SampleDesc.Count == d.SampleDesc.Count &&
                     got.SampleDesc.Quality == d.SampleDesc.Quality && got.Layout == d.Layout && got.Flags == d.Flags &&
                     got.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                 "valid buffer GetDesc changed fields");
        }
      }
  return verdict();
}
