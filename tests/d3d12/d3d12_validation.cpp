// contract: invalid resource and fence arguments are refused without changing outputs or events; recording errors
// are returned by Close, and executing an open list removes the device. Close returns "E_INVALIDARG if an invalid
// argument was passed to the command list API during recording" (Microsoft Learn, ID3D12GraphicsCommandList::Close).
// GetHeapProperties: "If the resource was created as reserved, E_INVALIDARG is returned". D3D12_RESOURCE_FLAGS,
// ALLOW_DEPTH_STENCIL: "Can't be used with D3D12_RESOURCE_DIMENSION_BUFFER". CreatePlacedResource: "The HeapOffset
// must be a multiple of the resource's alignment"; its allocation must fit and the heap must have that alignment
// (D3D12_HEAP_DESC::Alignment). buffers use ROW_MAJOR (D3D12_RESOURCE_DESC, Buffers), reserved textures use undefined
// swizzle (CreateReservedResource; equivalent to D3D11_RESOURCE_MISC_TILED). "Formats containing stencil are not
// supported with Tiled Resources" (D3D11.3 5.9.2.5.1); tier 2: "Tiled Resources with any mipmaps less than standard
// tile size in any dimension are not allowed to have an array size larger than 1" (5.9.7.2). immediate writes use
// the three modes in D3D12_WRITEBUFFERIMMEDIATE_MODE, and a color resolve format must be typed and compatible with
// its resources (ID3D12GraphicsCommandList::ResolveSubresource); depth/stencil plane formats keep their unused
// typeless components (vkd3d-proton's test_multisample_resolve_formats). Windows witnesses: vkd3d-proton's
// test_create_reserved_resource, test_fence_wait_multiple_inner, test_multisample_resolve_strongly_typed and
// test_device_removed_reason. exhaustive variants name their inputs; sizes straddle placement and tile boundaries.
// valid resolves copy red samples over cyan, and depth/stencil samples over different clears; every pixel is read
// back. MIN and MAX use "their minimum value" and "their maximum value", AVERAGE "their average value"
// (Microsoft Learn, D3D12_RESOLVE_MODE), so equal samples keep the cleared value in each mode. DXGI type groups
// keep "the same bit counts for each component" (D3D11.3 19.1.1), including component order and missing alpha.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>

int
main() {
  ComPtr<ID3D12Device1> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  const bool tiled = options.TiledResourcesTier != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
  const UINT64 alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_HEAP_DESC heap_desc{4 * alignment, props};
  ComPtr<ID3D12Heap> heap;
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  for (UINT64 width : {alignment - 1, alignment, alignment + 1})
    for (UINT flags = D3D12_RESOURCE_FLAG_NONE; flags < D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE; flags++)
      for (UINT kind = 0; kind < (tiled ? 3u : 2u); kind++) {
        step("buffer: width %llu, flags %u, creation %u", width, flags, kind);
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                 0,
                                 width,
                                 1,
                                 1,
                                 1,
                                 DXGI_FORMAT_UNKNOWN,
                                 {1, 0},
                                 D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                 (D3D12_RESOURCE_FLAGS)flags};
        ComPtr<ID3D12Resource> resource;
        HRESULT hr =
            kind == 0 ? device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))
            : kind == 1
                ? device->CreatePlacedResource(heap.Get(), alignment, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&resource))
                : device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
        HRESULT want = flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ? E_INVALIDARG : S_OK;
        expect(hr == want, "creation: %08lx, want %08lx", hr, want);
        if (!resource)
          continue;
        for (UINT outputs = 0; outputs < 4; outputs++) {
          D3D12_HEAP_PROPERTIES got{D3D12_HEAP_TYPE_UPLOAD};
          D3D12_HEAP_FLAGS got_flags = D3D12_HEAP_FLAG_DENY_BUFFERS;
          hr = resource->GetHeapProperties(outputs & 1 ? &got : nullptr, outputs & 2 ? &got_flags : nullptr);
          expect(hr == (kind == 2 ? E_INVALIDARG : S_OK), "heap query %u: %08lx", outputs, hr);
          expect(got.Type == (outputs & 1 && kind != 2 ? props.Type : D3D12_HEAP_TYPE_UPLOAD),
                 "heap query %u changed the wrong properties", outputs);
          expect(got_flags == (outputs & 2 && kind != 2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_DENY_BUFFERS),
                 "heap query %u changed the wrong flags", outputs);
        }
      }

  if (tiled) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, alignment, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    for (UINT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; layout <= D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE; layout++) {
      desc.Layout = (D3D12_TEXTURE_LAYOUT)layout;
      for (bool clear : {false, true}) {
        step("reserved buffer: layout %u, clear %u", layout, clear);
        D3D12_CLEAR_VALUE value{DXGI_FORMAT_R8G8B8A8_UNORM};
        ComPtr<ID3D12Resource> resource;
        HRESULT hr = device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, clear ? &value : nullptr,
                                                    IID_PPV_ARGS(&resource));
        expect(hr == (layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && !clear ? S_OK : E_INVALIDARG), "creation: %08lx", hr);
      }
    }
    desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D,         0, 1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
            D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE};
    ComPtr<ID3D12Resource> probe;
    CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&probe)));
    D3D12_TILE_SHAPE shape{};
    device->GetResourceTiling(probe.Get(), nullptr, nullptr, &shape, nullptr, 0, nullptr);
    for (UINT outputs = 0; outputs < 4; outputs++) {
      step("reserved texture: heap outputs %u", outputs);
      D3D12_HEAP_PROPERTIES got{D3D12_HEAP_TYPE_UPLOAD};
      D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_DENY_BUFFERS;
      expect(probe->GetHeapProperties(outputs & 1 ? &got : nullptr, outputs & 2 ? &flags : nullptr) == E_INVALIDARG &&
                 got.Type == D3D12_HEAP_TYPE_UPLOAD && flags == D3D12_HEAP_FLAG_DENY_BUFFERS,
             "reserved texture has heap properties");
    }
    for (UINT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; layout <= D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE; layout++) {
      step("reserved texture: layout %u", layout);
      desc.Layout = (D3D12_TEXTURE_LAYOUT)layout;
      ComPtr<ID3D12Resource> resource;
      HRESULT hr = device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
      expect(hr == (layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE ? S_OK : E_INVALIDARG), "creation: %08lx",
             hr);
    }
    desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    step("reserved 1D texture");
    ComPtr<ID3D12Resource> resource;
    expect(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)) ==
               E_INVALIDARG,
           "reserved 1D texture accepted");
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    for (DXGI_FORMAT format : {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
      step("reserved stencil: format %u", format);
      desc.Format = format;
      resource.Reset();
      expect(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)) ==
                 E_INVALIDARG,
             "reserved stencil texture accepted");
    }
    desc.Format = DXGI_FORMAT_R32_UINT;
    for (UINT width : {shape.WidthInTexels - 1, shape.WidthInTexels, shape.WidthInTexels + 1,
                       2 * shape.WidthInTexels - 1, 2 * shape.WidthInTexels, 2 * shape.WidthInTexels + 1})
      for (UINT height : {shape.HeightInTexels - 1, shape.HeightInTexels, shape.HeightInTexels + 1,
                          2 * shape.HeightInTexels - 1, 2 * shape.HeightInTexels, 2 * shape.HeightInTexels + 1})
        for (UINT layers : {1u, 2u})
          for (UINT mips : {0u, 1u, 2u, 3u}) {
            step("reserved array: width %u, height %u, layers %u, mips %u", width, height, layers, mips);
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = layers;
            desc.MipLevels = mips;
            resource.Reset();
            HRESULT hr =
                device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
            UINT levels = mips ? mips : std::bit_width(std::max(desc.Width, UINT64(desc.Height)));
            bool packed = layers > 1 &&
                          ((desc.Width >> (levels - 1)) < shape.WidthInTexels ||
                           (desc.Height >> (levels - 1)) < shape.HeightInTexels) &&
                          options.TiledResourcesTier <= D3D12_TILED_RESOURCES_TIER_3;
            expect(hr == (packed ? E_INVALIDARG : S_OK), "creation: %08lx", hr);
          }
  } else {
    printf("reserved resource cases unavailable: tiled resources are not supported\n");
  }

  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                   0,
                                   8,
                                   8,
                                   1,
                                   1,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,
                                   {4, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  for (UINT64 requested : {UINT64(0), alignment}) {
    texture_desc.Alignment = requested;
    auto info = device->GetResourceAllocationInfo(0, 1, &texture_desc);
    for (UINT64 size : {alignment, info.SizeInBytes, info.SizeInBytes + 2 * info.Alignment})
      for (UINT64 heap_alignment : {alignment, UINT64(D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)}) {
        heap_desc.SizeInBytes = size;
        heap_desc.Alignment = heap_alignment;
        heap.Reset();
        CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
        auto actual = heap->GetDesc();
        UINT64 last = actual.SizeInBytes >= info.SizeInBytes ? actual.SizeInBytes - info.SizeInBytes : 0;
        for (UINT64 offset : {UINT64(0), info.Alignment - 1, info.Alignment, info.Alignment + 1, last, last + 1,
                              std::numeric_limits<UINT64>::max()}) {
          step("placed MSAA: requested %llu, heap size %llu, alignment %llu, offset %llu", requested, size,
               heap_alignment, offset);
          ComPtr<ID3D12Resource> resource;
          HRESULT hr = device->CreatePlacedResource(heap.Get(), offset, &texture_desc, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(&resource));
          bool valid = actual.Alignment >= info.Alignment && offset % info.Alignment == 0 &&
                       offset <= actual.SizeInBytes && info.SizeInBytes <= actual.SizeInBytes - offset;
          expect(hr == (valid ? S_OK : E_INVALIDARG), "placement: %08lx", hr);
        }
      }
  }

  HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(event != nullptr, "fence wait event was not made"))
    return verdict();
  for (UINT shared = D3D12_FENCE_FLAG_NONE; shared <= D3D12_FENCE_FLAG_SHARED; shared++) {
    ComPtr<ID3D12Fence> fences[2];
    for (auto &f : fences)
      CHECK(device->CreateFence(0, (D3D12_FENCE_FLAGS)shared, IID_PPV_ARGS(&f)));
    ID3D12Fence *fence_ptrs[] = {fences[0].Get(), fences[1].Get()};
    UINT64 values[] = {0, 0};
    for (UINT count = 0; count <= std::size(fences); count++)
      for (UINT flags = D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL; flags <= D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY + 1; flags++)
        for (bool with_event : {false, true}) {
          step("fence wait: shared %u, count %u, flags %u, event %u", shared, count, flags, with_event);
          ResetEvent(event);
          HRESULT hr = device->SetEventOnMultipleFenceCompletion(count ? fence_ptrs : nullptr, count ? values : nullptr,
                                                                 count, (D3D12_MULTIPLE_FENCE_WAIT_FLAGS)flags,
                                                                 with_event ? event : nullptr);
          bool valid = count && flags <= D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY;
          expect(hr == (valid ? S_OK : E_INVALIDARG), "fence wait: %08lx", hr);
          expect(WaitForSingleObject(event, 0) == (valid && with_event ? WAIT_OBJECT_0 : WAIT_TIMEOUT),
                 "invalid wait changed the event");
        }
  }
  CloseHandle(event);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  const UINT mode_count = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT - D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT + 1;
  auto target =
      buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, mode_count * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, mode_count * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(target && readback, "immediate write buffers were not made"))
    return verdict();
  std::array<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER, mode_count> params{};
  std::array<D3D12_WRITEBUFFERIMMEDIATE_MODE, mode_count> modes{};
  for (UINT i = 0; i < mode_count; i++) {
    params[i] = {target->GetGPUVirtualAddress() + i * sizeof(UINT), i + 1};
    modes[i] = (D3D12_WRITEBUFFERIMMEDIATE_MODE)(D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT + i);
  }
  for (int invalid = -2; invalid < int(mode_count); invalid++) {
    step("immediate write: invalid mode index %d", invalid);
    CHECK(allocator->Reset());
    ComPtr<ID3D12GraphicsCommandList2> list;
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    auto given = modes;
    for (UINT i = 0; i < mode_count; i++)
      params[i].Value = (invalid + 3) * (i + 1);
    if (invalid >= 0)
      given[invalid] = (D3D12_WRITEBUFFERIMMEDIATE_MODE)(D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT + 1);
    list->WriteBufferImmediate(0, nullptr, nullptr);
    list->WriteBufferImmediate(mode_count, params.data(), invalid == -2 ? nullptr : given.data());
    if (invalid >= 0)
      list->WriteBufferImmediate(mode_count, params.data(), nullptr);
    if (invalid < 0) {
      CHECK(forget(readback.Get()));
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, mode_count * sizeof(UINT));
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    HRESULT hr = list->Close();
    expect(hr == (invalid < 0 ? S_OK : E_INVALIDARG), "Close: %08lx", hr);
    if (invalid >= 0) {
      expect(list->Reset(allocator.Get(), nullptr) == E_INVALIDARG, "failed list can be reset");
    } else if (SUCCEEDED(hr)) {
      CHECK(execute(device.Get(), queue.Get(), list.Get()));
      const UINT *words;
      CHECK(readback->Map(0, nullptr, (void **)&words));
      for (UINT i = 0; i < mode_count; i++)
        expect(words[i] == params[i].Value, "write %u: %u, want %u", i, words[i], params[i].Value);
      readback->Unmap(0, nullptr);
      CHECK(list->Reset(allocator.Get(), nullptr));
      CHECK(list->Close());
    }
  }

  const float red[] = {1, 0, 0, 1}, cyan[] = {0, 1, 1, 0};
  auto read_resolve = [&](ID3D12GraphicsCommandList *list, ID3D12Resource *destination, UINT plane,
                          bool scalar, float value, bool bgr) -> HRESULT {
    auto desc = destination->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, plane, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!pixels)
      return E_OUTOFMEMORY;
    HRESULT hr = forget(pixels.Get());
    if (FAILED(hr))
      return hr;
    transition(list, destination, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        into{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    from.SubresourceIndex = plane;
    list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    transition(list, destination, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    if (FAILED(hr = submit(device.Get(), queue.Get(), list)))
      return hr;
    const unsigned char *got;
    if (FAILED(hr = pixels->Map(0, nullptr, (void **)&got)))
      return hr;
    for (UINT y = 0; y < desc.Height; y++)
      for (UINT x = 0; x < desc.Width; x++) {
        auto at = got + footprint.Offset + y * footprint.Footprint.RowPitch +
                  x * (scalar ? sizeof(float) : plane ? sizeof(UINT8) : std::size(red) * sizeof(UINT8));
        if (scalar) {
          float actual;
          memcpy(&actual, at, sizeof(actual));
          expect(actual == value, "pixel %u,%u: %g, want %g", x, y, actual, value);
        } else if (plane) {
          expect(*at == UINT8(value), "stencil %u,%u: %u, want %u", x, y, *at, UINT8(value));
        } else {
          const UINT8 full = std::numeric_limits<UINT8>::max();
          bool missing_alpha = desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
                               desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
          expect(at[bgr ? 2 : 0] == full && at[1] == 0 && at[bgr ? 0 : 2] == 0 && (missing_alpha || at[3] == full),
                 "pixel %u,%u: %u,%u,%u,%u, want red with alpha one", x, y, at[0], at[1], at[2], at[3]);
        }
      }
    pixels->Unmap(0, nullptr);
    return S_OK;
  };
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto src_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart(), dst_rtv = src_rtv;
  dst_rtv.ptr += device->GetDescriptorHandleIncrementSize(rtv_desc.Type);
  auto family = [](DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default: return format;
    }
  };
  const DXGI_FORMAT colors[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                               DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                               DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_R32_FLOAT};
  texture_desc.Alignment = 0;
  for (DXGI_FORMAT src_format : colors)
    for (DXGI_FORMAT dst_format : colors) {
      ComPtr<ID3D12Resource> source, destination;
      texture_desc.SampleDesc.Count = 4;
      texture_desc.Format = src_format;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                            D3D12_RESOURCE_STATE_RESOLVE_SOURCE, nullptr, IID_PPV_ARGS(&source)));
      texture_desc.SampleDesc.Count = 1;
      texture_desc.Format = dst_format;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                            D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&destination)));
      device->CreateRenderTargetView(source.Get(), nullptr, src_rtv);
      device->CreateRenderTargetView(destination.Get(), nullptr, dst_rtv);
      for (DXGI_FORMAT format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS,
                                 DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                 DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM,
                                 DXGI_FORMAT_B8G8R8X8_UNORM_SRGB})
        for (UINT mode = D3D12_RESOLVE_MODE_MIN; mode <= D3D12_RESOLVE_MODE_AVERAGE; mode++) {
          step("resolve: source %u, destination %u, format %u, mode %u", src_format, dst_format, format, mode);
          CHECK(allocator->Reset());
          ComPtr<ID3D12GraphicsCommandList1> list;
          CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
          bool valid = std::find(std::begin(colors), std::end(colors), format) != std::end(colors) &&
                       family(src_format) == family(dst_format) && family(format) == family(src_format);
          if (valid) {
            transition(list.Get(), source.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(list.Get(), destination.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ClearRenderTargetView(src_rtv, red, 0, nullptr);
            list->ClearRenderTargetView(dst_rtv, cyan, 0, nullptr);
            transition(list.Get(), source.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(list.Get(), destination.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RESOLVE_DEST);
          }
          if (mode == D3D12_RESOLVE_MODE_AVERAGE)
            list->ResolveSubresource(destination.Get(), 0, source.Get(), 0, format);
          else
            list->ResolveSubresourceRegion(destination.Get(), 0, 0, 0, source.Get(), 0, nullptr, format,
                                          (D3D12_RESOLVE_MODE)mode);
          if (valid) {
            CHECK(read_resolve(list.Get(), destination.Get(), 0, src_format == DXGI_FORMAT_R32_FLOAT, red[0],
                               family(dst_format) != DXGI_FORMAT_R8G8B8A8_TYPELESS));
            continue;
          }
          HRESULT hr = list->Close();
          expect(hr == E_INVALIDARG, "Close: %08lx", hr);
          expect(list->Reset(allocator.Get(), nullptr) == E_INVALIDARG, "failed resolve list can be reset");
        }
    }

  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  texture_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  texture_desc.SampleDesc.Count = 4;
  ComPtr<ID3D12Resource> depth_source, depth_destination;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                        D3D12_RESOURCE_STATE_RESOLVE_SOURCE, nullptr, IID_PPV_ARGS(&depth_source)));
  texture_desc.SampleDesc.Count = 1;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                        nullptr, IID_PPV_ARGS(&depth_destination)));
  D3D12_FEATURE_DATA_FORMAT_INFO planes{texture_desc.Format};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &planes, sizeof(planes)));
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  auto src_dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart(), dst_dsv = src_dsv;
  dst_dsv.ptr += device->GetDescriptorHandleIncrementSize(dsv_desc.Type);
  device->CreateDepthStencilView(depth_source.Get(), nullptr, src_dsv);
  device->CreateDepthStencilView(depth_destination.Get(), nullptr, dst_dsv);
  for (UINT plane = 0; plane < planes.PlaneCount; plane++)
    for (UINT mode = D3D12_RESOLVE_MODE_MIN; mode <= D3D12_RESOLVE_MODE_AVERAGE; mode++) {
      if (plane && mode == D3D12_RESOLVE_MODE_AVERAGE)
        continue;
      step("resolve: depth/stencil plane %u, mode %u", plane, mode);
      CHECK(allocator->Reset());
      ComPtr<ID3D12GraphicsCommandList1> list;
      CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      transition(list.Get(), depth_source.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      transition(list.Get(), depth_destination.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
      const float depth = 0.25f;
      const UINT8 stencil = std::numeric_limits<UINT8>::max() / 3;
      list->ClearDepthStencilView(src_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, depth, stencil, 0,
                                 nullptr);
      list->ClearDepthStencilView(dst_dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1 - depth, 0, 0, nullptr);
      transition(list.Get(), depth_source.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      transition(list.Get(), depth_destination.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                 D3D12_RESOURCE_STATE_RESOLVE_DEST);
      list->ResolveSubresourceRegion(depth_destination.Get(), plane, 0, 0, depth_source.Get(), plane, nullptr,
                                     plane ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                                     (D3D12_RESOLVE_MODE)mode);
      CHECK(read_resolve(list.Get(), depth_destination.Get(), plane, !plane, plane ? stencil : depth, false));
    }

  step("submit an open list: the reason persists and queue creation fails");
  CHECK(allocator->Reset());
  ComPtr<ID3D12GraphicsCommandList> closed;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&closed)));
  CHECK(closed->Close());
  ComPtr<ID3D12GraphicsCommandList> open;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&open)));
  ID3D12CommandList *lists[] = {closed.Get(), open.Get()};
  queue->ExecuteCommandLists((UINT)std::size(lists), lists);
  expect(device->GetDeviceRemovedReason() == DXGI_ERROR_INVALID_CALL, "device retained after invalid submission");
  ComPtr<ID3D12CommandQueue> next_queue;
  expect(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&next_queue)) == DXGI_ERROR_DEVICE_REMOVED && !next_queue,
         "queue made after device removal");
  expect(device->GetDeviceRemovedReason() == DXGI_ERROR_INVALID_CALL, "device removal reason changed");
  return verdict();
}
