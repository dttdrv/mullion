// contract: a resource the device makes committed has a size and an alignment, and is placed in a heap of the same
// properties and that size at any multiple of that alignment. "If Alignment is set to 0, the runtime will use 4MB for
// MSAA textures and 64KB for everything else"; a small texture may ask for 4KB, and a small MSAA one for 64KB; a
// buffer's alignment is 64KB (Microsoft Learn, D3D12_RESOURCE_DESC, Alignment, Buffers). "The HeapOffset must be a
// multiple of the resource's alignment, and HeapOffset plus the resource size must be smaller than or equal to the
// heap size" (CreatePlacedResource). the device must make a texture its format's support names
// (D3D12_FEATURE_FORMAT_SUPPORT) in a default heap; typeless formats name no usage, and games make them render
// targets and depth buffers, so every usage is tried.
// every format, dimension, usage, sample count, array size, layout, and a small and a large size; the large size is
// past a 64KB tile in every format, and neither is a power of two. each texture goes also in one heap the size
// GetResourceAllocationInfo1 gives it after a texture of one block, at the offset it gives. buffers go in every heap
// type, below, at and past 64KB, and a page's buffer in heaps whose size is not a multiple of 64KB.
#include "d3d12_test.hpp"
#include <limits>

int main(int, char **) {
  ComPtr<ID3D12Device4> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  auto custom = [](D3D12_CPU_PAGE_PROPERTY page) {
    return D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_CUSTOM, page, D3D12_MEMORY_POOL_L0};
  };
  const D3D12_HEAP_PROPERTIES device_local{D3D12_HEAP_TYPE_DEFAULT},
      custom_heaps[]{custom(D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE), custom(D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE),
                     custom(D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)};
  auto heap_flags = [&](const D3D12_RESOURCE_DESC &desc) {
    return options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2 ? D3D12_HEAP_FLAG_NONE
           : desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER   ? D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
           : desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
               ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
               : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  };
  auto state = [](const D3D12_HEAP_PROPERTIES &props) {
    return props.Type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
           : props.Type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                    : D3D12_RESOURCE_STATE_COMMON;
  };
  // places the resources each at its offset in a heap of the given size and alignment
  auto place_in = [&](const D3D12_HEAP_PROPERTIES &props, UINT64 size, UINT64 alignment,
                      std::initializer_list<std::pair<const D3D12_RESOURCE_DESC *, UINT64>> resources) {
    D3D12_HEAP_DESC heap_desc{size, props, std::max<UINT64>(alignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT),
                              heap_flags(*resources.begin()->first)};
    ComPtr<ID3D12Heap> heap;
    if (!expect(SUCCEEDED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap))), "a heap of %llu bytes", size))
      return;
    for (auto [desc, offset] : resources) {
      ComPtr<ID3D12Resource> placed;
      HRESULT hr = device->CreatePlacedResource(heap.Get(), offset, desc, state(props), nullptr, IID_PPV_ARGS(&placed));
      expect(hr == S_OK && placed, "placed at %llu of %llu: %08lx", offset, size, hr);
    }
  };
  // whether the device makes the resource committed in a heap of these properties; when it does, it is placed in one
  // twice, back to back, the second one ending where the heap does. `required`: the device must make it
  auto place = [&](const D3D12_RESOURCE_DESC &desc, const D3D12_HEAP_PROPERTIES &props, bool required,
                   UINT64 wanted) {
    ComPtr<ID3D12Resource> committed;
    bool made = SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state(props), nullptr,
                                                          IID_PPV_ARGS(&committed)));
    if (!expect(made || !required, "the committed resource is refused") || !made)
      return false;
    auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    if (!expect(info.SizeInBytes && info.SizeInBytes != std::numeric_limits<UINT64>::max() && info.Alignment == wanted,
                "allocation size %llu, alignment %llu; want alignment %llu", info.SizeInBytes, info.Alignment, wanted))
      return true;
    UINT64 slot = (info.SizeInBytes + info.Alignment - 1) / info.Alignment * info.Alignment;
    place_in(props, slot + info.SizeInBytes, info.Alignment, {{&desc, 0}, {&desc, slot}});
    return true;
  };

  for (UINT f = DXGI_FORMAT_R32G32B32A32_TYPELESS; f <= DXGI_FORMAT_B4G4R4A4_UNORM; f++) {
    auto format = DXGI_FORMAT(f);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    D3D12_FEATURE_DATA_FORMAT_INFO planes{format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &planes, sizeof(planes))))
      continue;
    const bool planar = planes.PlaneCount > 1;
    for (auto [dimension, bit] : {std::pair{D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_FORMAT_SUPPORT1_TEXTURE1D},
                                  {D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_FORMAT_SUPPORT1_TEXTURE2D},
                                  {D3D12_RESOURCE_DIMENSION_TEXTURE3D, D3D12_FORMAT_SUPPORT1_TEXTURE3D}}) {
      if (!(support.Support1 & bit))
        continue;
      const bool three = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
      // each usage, and the support that makes it one the device must take
      for (auto [usage, needs] : {std::pair{D3D12_RESOURCE_FLAG_NONE, D3D12_FORMAT_SUPPORT1_NONE},
                                  {D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_FORMAT_SUPPORT1_RENDER_TARGET},
                                  {D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL},
                                  {D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW}})
        for (UINT samples : {1u, 4u}) {
          if (usage == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL && three)
            continue;
          const bool required = (support.Support1 & needs) == needs;
          bool attachment = usage & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
          if (samples > 1) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{format, samples};
            if (dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !attachment ||
                FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels))) ||
                !levels.NumQualityLevels)
              continue;
          }
          for (auto [small, layers, layout] : {std::tuple{true, 1, D3D12_TEXTURE_LAYOUT_UNKNOWN},
                                               {false, 1, D3D12_TEXTURE_LAYOUT_UNKNOWN},
                                               {false, 6, D3D12_TEXTURE_LAYOUT_UNKNOWN},
                                               {false, 1, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE}}) {
            // a 64KB layout is a tiled one: "Supported only for 2D and 3D textures ... no MSAA, no planar" when the
            // device has tiled resources (D3D12_TEXTURE_LAYOUT), 3D ones from tier 3
            if (layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE &&
                (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D || samples > 1 || planar ||
                 options.TiledResourcesTier < (three ? D3D12_TILED_RESOURCES_TIER_3 : D3D12_TILED_RESOURCES_TIER_1)))
              continue;
            if (layers > 1 && (three || planar))
              continue;
            D3D12_RESOURCE_DESC desc{dimension, 0, small ? 16u : 300u,
                                     dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : small ? 16u : 200u,
                                     UINT16(three ? small ? 4 : 20 : layers),
                                     UINT16(samples > 1 || planar || !(support.Support1 & D3D12_FORMAT_SUPPORT1_MIP)),
                                     format, {samples, 0}, layout, usage};
            // the smaller alignment a small texture may ask for: 4KB without MSAA or attachments, 64KB with MSAA
            std::vector<UINT64> alignments{0};
            if (small && samples > 1)
              alignments.push_back(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
            else if (small && !attachment)
              alignments.push_back(D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT);
            for (auto alignment : alignments) {
              desc.Alignment = alignment;
              UINT64 wanted = alignment ? alignment
                              : samples > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                            : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
              for (const auto &props : custom_heaps) {
                step("format %u, dimension %u, flags %#x, %u samples, %s, %u layers, layout %u, alignment %llu, "
                     "custom heap %u", f, dimension, usage, samples, small ? "small" : "large", layers, layout,
                     alignment, props.CPUPageProperty);
                place(desc, props, false, wanted);
              }
              step("format %u, dimension %u, flags %#x, %u samples, %s, %u layers, layout %u, alignment %llu", f,
                   dimension, usage, samples, small ? "small" : "large", layers, layout, alignment);
              if (!place(desc, device_local, required, wanted))
                continue;
              // after a texture of one block of every format, planar and block-compressed ones too
              D3D12_RESOURCE_DESC pair[2]{desc, desc};
              pair[0].Width = 4;
              pair[0].Height = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 4;
              pair[0].MipLevels = 1;
              pair[0].Alignment = 0;
              pair[0].Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
              D3D12_RESOURCE_ALLOCATION_INFO1 each[2];
              auto both = device->GetResourceAllocationInfo1(0, 2, pair, each);
              if (expect(both.SizeInBytes != std::numeric_limits<UINT64>::max(), "two take %llu bytes",
                         both.SizeInBytes))
                place_in(device_local, both.SizeInBytes, both.Alignment,
                         {{&pair[0], each[0].Offset}, {&pair[1], each[1].Offset}});
            }
          }
        }
    }
  }

  const D3D12_HEAP_PROPERTIES buffer_heaps[]{device_local, {D3D12_HEAP_TYPE_UPLOAD}, {D3D12_HEAP_TYPE_READBACK},
                                             custom_heaps[0], custom_heaps[1], custom_heaps[2]};
  for (const auto &props : buffer_heaps)
    for (UINT64 width : {UINT64(1), UINT64(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT),
                         UINT64(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) * 48 + 1})
      for (auto usage : {D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}) {
        step("buffer of %llu bytes, flags %#x, heap type %u, page %u", width, usage, props.Type,
             props.CPUPageProperty);
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, width, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_ROW_MAJOR, usage};
        // upload and readback heaps take no unordered access (D3D12_HEAP_TYPE)
        place(desc, props, props.Type == D3D12_HEAP_TYPE_DEFAULT || !usage,
              D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
      }
  // a heap's size need not be a multiple of its alignment: "non-aligned SizeInBytes is also supported, for
  // convenience" (D3D12_HEAP_DESC), so the heap is its size rounded up to it. "HeapOffset plus the resource size must
  // be smaller than or equal to the heap size" (ID3D12Device::CreatePlacedResource), and a buffer's size is its width:
  // a buffer of one page fills the start of a heap of one page, and the start of the second 64KB of a heap one page
  // past 64KB (dttdrv/mullion#32)
  // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource
  for (const auto &props : buffer_heaps) {
    step("buffer of a page in a heap of a page, heap type %u, page %u", props.Type, props.CPUPageProperty);
    D3D12_RESOURCE_DESC page{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    place_in(props, page.Width, 0, {{&page, 0}});
    step("buffers of a page in a heap of 64KB and a page, heap type %u, page %u", props.Type, props.CPUPageProperty);
    place_in(props, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT + page.Width, 0,
             {{&page, 0}, {&page, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT}});
    // a size that is no multiple of a page fills a heap of its own size
    step("buffer of a page and four bytes in a heap of that size, heap type %u, page %u", props.Type,
         props.CPUPageProperty);
    D3D12_RESOURCE_DESC odd = page;
    odd.Width += 4;
    place_in(props, odd.Width, 0, {{&odd, 0}});
  }
  // a texture whose most-detailed mip of one slice D3D12 estimates at 64KB or less may ask for 4KB alignment, whatever
  // its real size: "applications can create smaller aligned resources when the estimated size of the most-detailed
  // mip level is a total of the larger alignment restriction or less" (D3D12_RESOURCE_DESC, Alignment). the estimate
  // is width times height in elements (blocks for a compressed format) times their bytes, the depth or array size left
  // out: "Windows uses the slice size to determine small alignment eligibility. DepthOrArraySize is ignored"
  // (vkd3d-proton, libs/vkd3d/resource.c, d3d12_resource_supports_small_resource_alignment). each eligible case is
  // given its size and an alignment of 4KB or 64KB ("Alignment may be one of 0, 4KB, 64KB, or 4MB"), and is placed and
  // committed there; each case one element past it is refused. Starfield's 320x180 R8_TYPELESS with unordered access
  // asks for it (dttdrv/mullion#32)
  const struct {
    D3D12_RESOURCE_DIMENSION dimension;
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    UINT16 depth;
    D3D12_RESOURCE_FLAGS flags;
    bool eligible;
  } small_cases[] = {
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_TYPELESS, 320, 180, 1,
       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_UNORM, 256, 256, 1, D3D12_RESOURCE_FLAG_NONE, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_UNORM, 257, 256, 1, D3D12_RESOURCE_FLAG_NONE, false},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8_UNORM, 4, 4096, 1, D3D12_RESOURCE_FLAG_NONE, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_BC1_UNORM, 512, 256, 1, D3D12_RESOURCE_FLAG_NONE, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_BC1_UNORM, 516, 256, 1, D3D12_RESOURCE_FLAG_NONE, false},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64, 1, D3D12_RESOURCE_FLAG_NONE, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_FLOAT, 65, 64, 1, D3D12_RESOURCE_FLAG_NONE, false},
      {D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8_UNORM, 64, 64, 64, D3D12_RESOURCE_FLAG_NONE, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE1D, DXGI_FORMAT_R32_FLOAT, 16384, 1, 1, D3D12_RESOURCE_FLAG_NONE, true},
      {D3D12_RESOURCE_DIMENSION_TEXTURE1D, DXGI_FORMAT_R32_FLOAT, 16385, 1, 1, D3D12_RESOURCE_FLAG_NONE, false},
  };
  for (auto &c : small_cases) {
    step("4KB alignment: dimension %u, format %u, %llux%ux%u", c.dimension, c.format, c.width, c.height, c.depth);
    D3D12_RESOURCE_DESC small{c.dimension, D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT, c.width, c.height, c.depth, 1,
                              c.format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, c.flags};
    auto info = device->GetResourceAllocationInfo(0, 1, &small);
    ComPtr<ID3D12Resource> committed;
    HRESULT hr = device->CreateCommittedResource(&device_local, D3D12_HEAP_FLAG_NONE, &small, state(device_local),
                                                 nullptr, IID_PPV_ARGS(&committed));
    if (!c.eligible) {
      expect(info.SizeInBytes == std::numeric_limits<UINT64>::max() && hr == E_INVALIDARG,
             "an ineligible 4KB alignment gives %llu bytes and %08lx", info.SizeInBytes, hr);
      continue;
    }
    if (!expect(info.SizeInBytes != std::numeric_limits<UINT64>::max() &&
                    info.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT && hr == S_OK,
                "an eligible 4KB alignment gives %llu bytes at alignment %llu, and %08lx", info.SizeInBytes,
                info.Alignment, hr))
      continue;
    UINT64 slot = (info.SizeInBytes + info.Alignment - 1) / info.Alignment * info.Alignment;
    place_in(device_local, slot + info.SizeInBytes, info.Alignment, {{&small, 0}, {&small, slot}});
  }
  return verdict();
}
