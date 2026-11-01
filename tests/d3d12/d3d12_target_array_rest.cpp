// contract: ID3D12Device::CreateRenderTargetView and CreateDepthStencilView, an array view whose ArraySize is -1 and
// a 3D view whose WSize is -1. -1 takes the slices from the view's first one to the end of the resource: "Number of
// depth levels to use in the render-target view, starting from FirstWSlice. A value of -1 indicates all of the
// slices along the w axis, starting from FirstWSlice" (D3D12_TEX3D_RTV), and an array view's ArraySize of -1 does
// the same from FirstArraySlice, as for shader resource and unordered access views (vkd3d-proton clamps a render
// target's layers to DepthOrArraySize - FirstArraySlice the same way; CD3DX12 and CD3D11 view descriptions default
// ArraySize to -1, so engines pass it).
// for each kind of view, a resource of SLICES slices is first cleared whole through a view of all of it, then
// cleared again through a view that starts at slice FIRST with -1 for its size, once whole and once by a rectangle
// that covers it. the slices before FIRST must keep the first clear and those from FIRST on must have the second.
// a multisampled array is read through a resolve of each slice.
// a view that runs past the end of the resource is not one Direct3D defines (FirstArraySlice is "The index of the
// first texture to use in an array of textures", of those there are); Mullion refuses it, and Metal must never be
// asked for its slices. a view from one past the last slice with -1 slices, and one from FIRST of as many slices as
// the resource has, are each made in a slot of their own and cleared through after the second clear: the process
// must live and every slice keep what it had.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_tex3d_rtv
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_tex2d_array_rtv
#include "d3d12_test.hpp"
#include <vector>

static const UINT SLICES = 4, FIRST = 2, SIDE = 4;

enum class Kind { rtv_1d_array, rtv_2d_array, rtv_2dms_array, rtv_3d, dsv_1d_array, dsv_2d_array };

struct Case {
  const char *name;
  Kind kind;
};

static const Case cases[] = {
    {"render target view of a 1D array", Kind::rtv_1d_array},
    {"render target view of a 2D array", Kind::rtv_2d_array},
    {"render target view of a multisampled 2D array", Kind::rtv_2dms_array},
    {"render target view of a 3D texture", Kind::rtv_3d},
    {"depth stencil view of a 1D array", Kind::dsv_1d_array},
    {"depth stencil view of a 2D array", Kind::dsv_2d_array},
};

static bool
is_depth(Kind kind) {
  return kind == Kind::dsv_1d_array || kind == Kind::dsv_2d_array;
}

// the colour and depth of the two clears, and the words they leave in R8G8B8A8_UNORM and D32_FLOAT
static const float before_color[4] = {0, 0, 1, 1}, after_color[4] = {1, 0, 0, 1};
static const float before_depth = 0.25f, after_depth = 0.75f;
// what a clear through a view past the end would write
static const float past_color[4] = {0, 1, 0, 1}, past_depth = 0.5f;
static UINT
word(Kind kind, bool after) {
  if (is_depth(kind)) {
    float depth = after ? after_depth : before_depth;
    UINT bits;
    memcpy(&bits, &depth, 4);
    return bits;
  }
  return after ? 0xff0000ffu : 0xffff0000u;
}

// the resource of a kind of view
static D3D12_RESOURCE_DESC
resource_desc(Kind kind) {
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, SIDE, SIDE, SLICES, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  switch (kind) {
  case Kind::rtv_1d_array:
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D, desc.Height = 1;
    break;
  case Kind::rtv_2dms_array:
    desc.SampleDesc.Count = 4;
    break;
  case Kind::rtv_3d:
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D, desc.DepthOrArraySize = 2 * SLICES;
    break;
  case Kind::dsv_1d_array:
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D, desc.Height = 1;
    [[fallthrough]];
  case Kind::dsv_2d_array:
    desc.Format = DXGI_FORMAT_D32_FLOAT, desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    break;
  default:
    break;
  }
  return desc;
}

// the slices of a resource and where the views start: a 3D texture has twice as many W slices, and its view starts
// halfway
static UINT
slices(Kind kind) {
  return kind == Kind::rtv_3d ? 2 * SLICES : SLICES;
}
static UINT
first(Kind kind) {
  return kind == Kind::rtv_3d ? SLICES : FIRST;
}

// a view of the resource from slice `from`, of `count` slices
static void
create_view(ID3D12Device *device, ID3D12Resource *resource, Kind kind, UINT from, D3D12_CPU_DESCRIPTOR_HANDLE handle,
            UINT count = ~0u) {
  if (is_depth(kind)) {
    D3D12_DEPTH_STENCIL_VIEW_DESC desc{DXGI_FORMAT_D32_FLOAT};
    if (kind == Kind::dsv_1d_array)
      desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY, desc.Texture1DArray = {0, from, count};
    else
      desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY, desc.Texture2DArray = {0, from, count};
    device->CreateDepthStencilView(resource, &desc, handle);
    return;
  }
  D3D12_RENDER_TARGET_VIEW_DESC desc{DXGI_FORMAT_R8G8B8A8_UNORM};
  switch (kind) {
  case Kind::rtv_1d_array:
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY, desc.Texture1DArray = {0, from, count};
    break;
  case Kind::rtv_2d_array:
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY, desc.Texture2DArray = {0, from, count, 0};
    break;
  case Kind::rtv_2dms_array:
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY, desc.Texture2DMSArray = {from, count};
    break;
  default:
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D, desc.Texture3D = {0, from, count};
    break;
  }
  device->CreateRenderTargetView(resource, &desc, handle);
}

enum class Clear { before, after, past };

static void
clear(ID3D12GraphicsCommandList *list, Kind kind, D3D12_CPU_DESCRIPTOR_HANDLE handle, Clear which, bool by_rect) {
  const D3D12_RECT rect{0, 0, (LONG)SIDE, kind == Kind::rtv_1d_array || kind == Kind::dsv_1d_array ? 1 : (LONG)SIDE};
  if (is_depth(kind))
    list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH,
                                which == Clear::before  ? before_depth
                                : which == Clear::after ? after_depth
                                                        : past_depth,
                                0, by_rect, by_rect ? &rect : nullptr);
  else
    list->ClearRenderTargetView(handle,
                                which == Clear::before  ? before_color
                                : which == Clear::after ? after_color
                                                        : past_color,
                                by_rect, by_rect ? &rect : nullptr);
}

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));

  for (auto &c : cases)
    for (bool by_rect : {false, true}) {
      step("%s from slice %u of %u with -1 slices, cleared %s", c.name, first(c.kind), slices(c.kind),
           by_rect ? "by a rectangle" : "whole");
      auto desc = resource_desc(c.kind);
      const bool depth = is_depth(c.kind), multisampled = desc.SampleDesc.Count > 1;
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_CLEAR_VALUE clear_value{desc.Format};
      if (depth)
        clear_value.DepthStencil = {before_depth, 0};
      else
        memcpy(clear_value.Color, before_color, sizeof(before_color));
      const auto state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
      ComPtr<ID3D12Resource> target, resolved;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, &clear_value,
                                            IID_PPV_ARGS(&target)));
      // what a multisampled array resolves into, slice by slice, to be read
      auto read_desc = desc;
      read_desc.SampleDesc.Count = 1;
      read_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      if (multisampled)
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &read_desc,
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)));
      ID3D12Resource *read = multisampled ? resolved.Get() : target.Get();
      const UINT subresources = c.kind == Kind::rtv_3d ? 1 : SLICES;
      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
      std::vector<UINT> rows(subresources);
      UINT64 total;
      device->GetCopyableFootprints(&read_desc, 0, subresources, 0, footprints.data(), rows.data(), nullptr, &total);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(forget(readback.Get()));

      auto heap = depth ? dsvs : rtvs;
      // the slots: a view of all of it, the view from FIRST, and the two views past the end, which are refused
      const UINT increment = device->GetDescriptorHandleIncrementSize(depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV
                                                                            : D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      D3D12_CPU_DESCRIPTOR_HANDLE whole = heap->GetCPUDescriptorHandleForHeapStart(), rest = whole, past_last = whole,
                                  past_end = whole;
      rest.ptr += increment, past_last.ptr += 2 * increment, past_end.ptr += 3 * increment;
      create_view(device.Get(), target.Get(), c.kind, 0, whole);
      create_view(device.Get(), target.Get(), c.kind, first(c.kind), rest);
      create_view(device.Get(), target.Get(), c.kind, slices(c.kind), past_last);
      create_view(device.Get(), target.Get(), c.kind, first(c.kind), past_end, slices(c.kind));

      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      clear(list.Get(), c.kind, whole, Clear::before, by_rect);
      clear(list.Get(), c.kind, rest, Clear::after, by_rect);
      clear(list.Get(), c.kind, past_last, Clear::past, by_rect);
      clear(list.Get(), c.kind, past_end, Clear::past, by_rect);
      if (multisampled) {
        transition(list.Get(), target.Get(), state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        for (UINT slice = 0; slice < SLICES; slice++)
          list->ResolveSubresource(resolved.Get(), slice, target.Get(), slice, desc.Format);
        transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      } else {
        transition(list.Get(), target.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      }
      for (UINT subresource = 0; subresource < subresources; subresource++) {
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            src{read, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.PlacedFootprint = footprints[subresource];
        src.SubresourceIndex = subresource;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
      if (!expect(SUCCEEDED(hr), "the list ran with %08lx (the device was removed)", hr))
        return verdict();

      BYTE *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT slice = 0; slice < slices(c.kind); slice++) {
        // an array's slices are subresources; a 3D texture's W slices follow one another in its one subresource
        auto &footprint = footprints[c.kind == Kind::rtv_3d ? 0 : slice];
        UINT64 at = footprint.Offset;
        if (c.kind == Kind::rtv_3d)
          at += UINT64(slice) * footprint.Footprint.RowPitch * rows[0];
        const UINT want = word(c.kind, slice >= first(c.kind));
        UINT wrong = 0, seen = 0;
        for (UINT y = 0; y < footprint.Footprint.Height; y++)
          for (UINT x = 0; x < footprint.Footprint.Width; x++) {
            UINT texel;
            memcpy(&texel, got + at + y * footprint.Footprint.RowPitch + x * 4, 4);
            if (texel != want && !wrong++)
              seen = texel;
          }
        expect(!wrong, "slice %u: %u texels wrong, the first %08x, want %08x (%s)", slice, wrong, seen, want,
               slice >= first(c.kind) ? "the second clear" : "the first clear, which the view does not reach");
      }
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
