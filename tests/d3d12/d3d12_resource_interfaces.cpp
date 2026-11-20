// contract: every resource creation path and swap-chain back buffer exposes Resource, Resource1 and Resource2
// with one COM identity and count, and GetDesc1 preserves GetDesc's members and the requested feedback mip region.
// "The ID3D12Resource1 interface inherits from the ID3D12Resource interface." and
// "The ID3D12Resource2 interface inherits from the ID3D12Resource1 interface." (Microsoft Learn,
// ID3D12Resource1 and ID3D12Resource2, Inheritance; also declared in DirectX-Headers' d3d12.idl).
// QueryInterface "must always return the same pointer value" for IUnknown and returns an interface
// "after calling IUnknown::AddRef on it" (Microsoft Learn, IUnknown::QueryInterface, Remarks and introduction:
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-queryinterface%28refiid_void%29).
// the Sampler Feedback spec's API declares "D3D12_RESOURCE_DESC1 STDMETHODCALLTYPE GetDesc1( void)";
// its revised description adds SamplerFeedbackMipRegion to the same members as RESOURCE_DESC, with defaults
// of zero in CD3DX12_RESOURCE_DESC1 (https://microsoft.github.io/DirectX-Specs/d3d/SamplerFeedback.html).
// MipLevels: "When 0 is used, the API will automatically calculate the maximum mip levels supported and use that."
// (Microsoft Learn, D3D12_RESOURCE_DESC). the expected full chain is the bit width of the largest dimension.
// Protected Resources says "Gets the protected session used to create the resource." and "May be nullptr."
// (https://microsoft.github.io/DirectX-Specs/d3d/ProtectedResources.html, ID3D12Resource1). no session is supplied
// here. neither that spec nor the inspected Wine d3d12 tests or vkd3d-proton tests specifies the no-session HRESULT;
// only the null session output is asserted, and the HRESULT is printed. vkd3d-proton's Windows witness
// test_sampler_feedback_resource_creation supplies valid committed/placed map descriptions, including depth 0
// and 1. Wine's test_swapchain_refcount witnesses one public count for each buffer, excluding private ownership.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <bit>

// the toolchain's dxgiformat.h lacks these; Microsoft DirectX-Headers, include/directx/dxgiformat.h declares
// "DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = 189" and "DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = 190".
constexpr auto DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = DXGI_FORMAT(189);
constexpr auto DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = DXGI_FORMAT(190);

int
main() {
  const IID *interfaces[] = {&__uuidof(ID3D12Resource), &__uuidof(ID3D12Resource1), &__uuidof(ID3D12Resource2)};
  auto check = [&](IUnknown *created, const D3D12_RESOURCE_DESC1 &asked) {
    created->AddRef();
    const auto initial = created->Release();
    expect(initial == 1, "creation must return one public reference");
    std::array<ComPtr<IUnknown>, std::size(interfaces)> aliases;
    for (size_t i = 0; i < aliases.size(); i++) {
      auto hr = created->QueryInterface(*interfaces[i], reinterpret_cast<void **>(aliases[i].GetAddressOf()));
      expect(hr == S_OK && aliases[i], "QueryInterface Resource%zu returned %#lx", i, hr);
      if (!aliases[i])
        return;
      created->AddRef();
      expect(created->Release() == initial + i + 1, "QueryInterface did not add to the shared reference count");
    }
    ComPtr<IUnknown> identity;
    if (!expect(created->QueryInterface(IID_PPV_ARGS(&identity)) == S_OK, "could not query IUnknown"))
      return;
    for (auto &alias : aliases) {
      ComPtr<IUnknown> unknown;
      if (!expect(alias->QueryInterface(IID_PPV_ARGS(&unknown)) == S_OK, "could not query alias identity"))
        return;
      expect(unknown.Get() == identity.Get(), "resource interfaces have different IUnknown identities");
      for (auto iid : interfaces) {
        ComPtr<IUnknown> other;
        if (!expect(alias->QueryInterface(*iid, reinterpret_cast<void **>(other.GetAddressOf())) == S_OK,
                    "could not query between resource interfaces"))
          return;
        identity->AddRef();
        const auto count = identity->Release();
        other.Reset();
        identity->AddRef();
        expect(identity->Release() + 1 == count, "interface release did not use the shared count");
      }
    }
    auto resource = static_cast<ID3D12Resource *>(aliases.front().Get());
    auto resource1 = static_cast<ID3D12Resource1 *>(aliases[1].Get());
    auto resource2 = static_cast<ID3D12Resource2 *>(aliases.back().Get());
    const auto desc = resource->GetDesc();
    const auto desc1 = resource2->GetDesc1();
    expect(desc1.Dimension == desc.Dimension && desc1.Alignment == desc.Alignment && desc1.Width == desc.Width &&
               desc1.Height == desc.Height && desc1.DepthOrArraySize == desc.DepthOrArraySize &&
               desc1.MipLevels == desc.MipLevels && desc1.Format == desc.Format &&
               desc1.SampleDesc.Count == desc.SampleDesc.Count && desc1.SampleDesc.Quality == desc.SampleDesc.Quality &&
               desc1.Layout == desc.Layout && desc1.Flags == desc.Flags,
           "GetDesc1 disagrees with GetDesc");
    auto largest = std::max(asked.Width, UINT64(asked.Height));
    if (asked.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      largest = std::max(largest, UINT64(asked.DepthOrArraySize));
    const auto mips = asked.MipLevels ? asked.MipLevels : std::bit_width(largest);
    expect(desc.Dimension == asked.Dimension && desc.Width == asked.Width && desc.Height == asked.Height &&
               desc.DepthOrArraySize == asked.DepthOrArraySize && desc.MipLevels == mips &&
               desc.Format == asked.Format && desc.SampleDesc.Count == asked.SampleDesc.Count &&
               desc.SampleDesc.Quality == asked.SampleDesc.Quality && desc.Layout == asked.Layout &&
               desc.Flags == asked.Flags,
           "resource lost its creation description");
    expect(desc1.SamplerFeedbackMipRegion.Width == asked.SamplerFeedbackMipRegion.Width &&
               desc1.SamplerFeedbackMipRegion.Height == asked.SamplerFeedbackMipRegion.Height &&
               desc1.SamplerFeedbackMipRegion.Depth == asked.SamplerFeedbackMipRegion.Depth,
           "GetDesc1 lost or invented a sampler feedback mip region");
    for (ID3D12Resource1 *with_session : {resource1, static_cast<ID3D12Resource1 *>(resource2)}) {
      for (bool output : {false, true}) {
        void *session = created;
        auto hr = with_session->GetProtectedResourceSession(__uuidof(ID3D12ProtectedResourceSession),
                                                            output ? &session : nullptr);
        printf("GetProtectedResourceSession output=%u returned %#lx\n", output, hr);
        if (output)
          expect(session == nullptr, "resource with no protected session returned one");
      }
    }
    identity.Reset();
    for (auto &alias : aliases)
      alias.Reset();
    created->AddRef();
    expect(created->Release() == initial, "queries leaked resource references");
  };

  ComPtr<ID3D12Device10> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)));
  if (!expect(options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2, "reserved coverage requires tiled tier 2") ||
      !expect(options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9, "feedback coverage is unavailable"))
    return verdict();
  auto window = CreateWindowA("static", "d3d12_resource_interfaces", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!expect(window != nullptr, "could not create window"))
    return verdict();
  RECT client{};
  if (!expect(GetClientRect(window, &client) && client.right > 0 && client.bottom > 0, "could not read client size")) {
    DestroyWindow(window);
    return verdict();
  }
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  const char *methods[] = {"committed", "committed1", "committed2", "committed3", "placed",
                           "placed1",   "placed2",    "reserved",   "reserved1",  "reserved2"};
  auto create = [&](size_t method, const D3D12_RESOURCE_DESC1 &desc1, ID3D12Heap *heap, REFIID iid, void **out) {
    D3D12_RESOURCE_DESC desc{desc1.Dimension, desc1.Alignment, desc1.Width,      desc1.Height, desc1.DepthOrArraySize,
                             desc1.MipLevels, desc1.Format,    desc1.SampleDesc, desc1.Layout, desc1.Flags};
    switch (method) {
    case 0:
      return device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, iid, out);
    case 1:
      return device->CreateCommittedResource1(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, nullptr, iid, out);
    case 2:
      return device->CreateCommittedResource2(&properties, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, nullptr, iid, out);
    case 3:
      return device->CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_COMMON,
                                              nullptr, nullptr, 0, nullptr, iid, out);
    case 4:
      return device->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, iid, out);
    case 5:
      return device->CreatePlacedResource1(heap, 0, &desc1, D3D12_RESOURCE_STATE_COMMON, nullptr, iid, out);
    case 6:
      return device->CreatePlacedResource2(heap, 0, &desc1, D3D12_BARRIER_LAYOUT_COMMON, nullptr, 0, nullptr, iid, out);
    case 7:
      return device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, iid, out);
    case 8:
      return device->CreateReservedResource1(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, nullptr, iid, out);
    default:
      return device->CreateReservedResource2(&desc, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, iid,
                                             out);
    }
  };
  auto exercise = [&](size_t method, const D3D12_RESOURCE_DESC1 &desc) {
    ComPtr<ID3D12Heap> heap;
    if (method >= 4 && method <= 6) {
      const auto info = device->GetResourceAllocationInfo2(0, 1, &desc, nullptr);
      if (!expect(info.SizeInBytes != UINT64_MAX, "allocation query failed"))
        return;
      D3D12_HEAP_DESC heap_desc{info.SizeInBytes, properties, info.Alignment,
                                desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                    ? D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
                                    : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
      if (!expect(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)) == S_OK, "could not create placed heap"))
        return;
    }
    for (size_t i = 0; i < std::size(interfaces); i++) {
      step("%s dimension=%u format=%u size=%llux%ux%u mips=%u region=%ux%ux%u Resource%zu", methods[method],
           desc.Dimension, desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels,
           desc.SamplerFeedbackMipRegion.Width, desc.SamplerFeedbackMipRegion.Height,
           desc.SamplerFeedbackMipRegion.Depth, i);
      ComPtr<IUnknown> made;
      auto hr = create(method, desc, heap.Get(), *interfaces[i], reinterpret_cast<void **>(made.GetAddressOf()));
      if (expect(hr == S_OK && made, "creation returned %#lx", hr))
        check(made.Get(), desc);
    }
  };
  for (size_t method = 0; method < std::size(methods); method++) {
    for (auto width : {D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES - 1ull, UINT64(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES),
                       D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES + 1ull}) {
      D3D12_RESOURCE_DESC1 desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, width, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
      exercise(method, desc);
    }
    for (auto dimension :
         {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
      if (method >= 7 && dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        continue; // tiled tier 2 covers 2D textures, not 1D or tier 3's 3D textures
      for (UINT16 mips : {UINT16(1), UINT16(0)}) {
        D3D12_RESOURCE_DESC1 desc{dimension,
                                  0,
                                  UINT64(client.right),
                                  dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : UINT(client.bottom),
                                  // D3D11.3 5.9.7.2: a tiled resource with a mip smaller than a tile is no array
                                  UINT16(method >= 7 && !mips ? 1 : 2),
                                  mips,
                                  DXGI_FORMAT_R8G8B8A8_UNORM,
                                  {1, 0},
                                  method >= 7 ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE
                                              : D3D12_TEXTURE_LAYOUT_UNKNOWN};
        exercise(method, desc);
      }
    }
  }
  // Sampler Feedback, "Mip region constraints": each 2D axis is a power of two at least 4 and at most half the texture.
  const UINT minimum_mip_region = 4;
  for (auto format : {DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE, DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE})
    for (auto region : {D3D12_MIP_REGION{minimum_mip_region, minimum_mip_region * 2, 0},
                        D3D12_MIP_REGION{minimum_mip_region * 2, minimum_mip_region, 1}})
      for (UINT16 mips : {UINT16(1), UINT16(0)}) {
        D3D12_RESOURCE_DESC1 desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  region.Width * 2ull + 1,
                                  region.Height * 2 + 1,
                                  2,
                                  mips,
                                  format,
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                  region};
        for (size_t method : {2u, 3u, 5u, 6u})
          exercise(method, desc);
      }
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  // Direct3D 12 Swap Chains, "Swap effects": flip-model swap chains have at least two buffers.
  DXGI_SWAP_CHAIN_DESC1 swap_desc{UINT(client.right),
                                  UINT(client.bottom),
                                  DXGI_FORMAT_R8G8B8A8_UNORM,
                                  FALSE,
                                  {1, 0},
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                  2,
                                  DXGI_SCALING_STRETCH,
                                  DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<IDXGISwapChain1> swapchain;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &swap_desc, nullptr, nullptr, &swapchain));
  for (UINT index = 0; index < swap_desc.BufferCount; index++) {
    IUnknown *identity = nullptr;
    for (size_t i = 0; i < std::size(interfaces); i++) {
      step("GetBuffer index=%u Resource%zu", index, i);
      ComPtr<IUnknown> made;
      auto hr = swapchain->GetBuffer(index, *interfaces[i], reinterpret_cast<void **>(made.GetAddressOf()));
      if (!expect(hr == S_OK && made, "GetBuffer returned %#lx", hr))
        continue;
      ComPtr<IUnknown> unknown;
      CHECK(made->QueryInterface(IID_PPV_ARGS(&unknown)));
      if (!identity)
        identity = unknown.Get();
      expect(unknown.Get() == identity, "GetBuffer's IID changed the buffer identity");
      unknown.Reset();
      D3D12_RESOURCE_DESC1 asked{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                 0,
                                 swap_desc.Width,
                                 swap_desc.Height,
                                 1,
                                 1,
                                 swap_desc.Format,
                                 swap_desc.SampleDesc,
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      check(made.Get(), asked);
    }
  }
  swapchain.Reset();
  DestroyWindow(window);
  return verdict();
}
