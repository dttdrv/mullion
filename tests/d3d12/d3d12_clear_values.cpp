// contract: ClearRenderTargetView stores the color in each channel the view's format has, with the conversions
// clear_values.hpp quotes; "NULL to clear the entire resource view" and rectangles that tile the view clear all of
// it (Microsoft Learn, ID3D12GraphicsCommandList::ClearRenderTargetView, pRects). every float and normalized
// format a render target can have, its channels in the order its SDK name stores them, every texel checked.
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearrendertargetview
#include "d3d12_test.hpp"
#include "../copy_formats.hpp"
#include "../clear_values.hpp"

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  constexpr UINT width = 3, height = 2;
  const D3D12_RECT halves[] = {{0, 0, LONG(width / 2), LONG(height)}, {LONG(width / 2), 0, LONG(width), LONG(height)}};
  UINT exercised = 0;
  for (const auto &format : copy_formats) {
    const auto kind = clear_values::kind(format);
    if (format.depth || format.unit != 1 || strstr(format.name, "TYPELESS") || strchr(format.family, 'X') ||
        !(kind.snorm || kind.unorm || kind.floating))
      continue;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format.id};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
      continue;
    const auto channels = clear_values::channels(format);
    if (channels.empty())
      return verdict();
    const auto values = clear_values::values(kind, channels);
    const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format.id, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&target)));
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 slot;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &slot);
    slot = (slot + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
           D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * values.size(), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(!!readback, "readback of %s", format.name))
      return verdict();
    for (bool rectangles : {false, true}) {
      step("clear %s %s", format.name, rectangles ? "through two rectangles" : "whole");
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      auto color = [&](UINT index) {
        std::array<float, 4> color;
        for (UINT c = 0; c < color.size(); c++)
          color[c] = values[(index + c) % values.size()];
        return color;
      };
      for (UINT index = 0; index < values.size(); index++) {
        const auto rgba = color(index);
        list->ClearRenderTargetView(rtv, rgba.data(), rectangles ? UINT(std::size(halves)) : 0,
                                    rectangles ? halves : nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = footprint;
        to.PlacedFootprint.Offset = slot * index;
        D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      void *mapped;
      CHECK(readback->Map(0, nullptr, &mapped));
      for (UINT index = 0; index < values.size(); index++) {
        bool ok = true;
        for (UINT y = 0; y < height && ok; y++)
          for (UINT x = 0; x < width && ok; x++)
            ok &= clear_values::check(static_cast<unsigned char *>(mapped) + slot * index + y * footprint.Footprint.RowPitch +
                                          x * format.bytes,
                                      kind, channels, color(index), x, y);
        exercised++;
      }
      readback->Unmap(0, nullptr);
    }
  }
  expect(exercised != 0, "no clear conversion was exercised");
  return verdict();
}
