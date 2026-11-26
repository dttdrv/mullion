// contract: UAV clear rectangles are relative to the view and cannot change elements outside it.
// "rectangles in the resource view to clear" (Microsoft Learn, ClearUnorderedAccessViewUint and Float, pRects).
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewuint
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewfloat
// seeded rectangles test membership of each element, without reproducing clipping arithmetic. copies read all
// elements, including other mips and slices. there are no shaders; argv[2] selects the seed.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <random>

int
main(int argc, char **argv) {
  const UINT seed = argc > 2 ? UINT(strtoul(argv[2], nullptr, 0)) : std::mt19937::default_seed;
  std::mt19937 random(seed);
  auto watchdog = CreateThread(
      nullptr, 0,
      [](void *) -> DWORD {
        Sleep(550000);
        printf("failed: clear rectangle test exceeded its deadline\n");
        ExitProcess(1);
      },
      nullptr, 0, nullptr
  );
  if (!expect(watchdog != nullptr, "watchdog was created"))
    return verdict();
  CloseHandle(watchdog);
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  ComPtr<ID3D12DescriptorHeap> cpu, gpu;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu)));
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&gpu)));
  const auto cpu_handle = cpu->GetCPUDescriptorHandleForHeapStart();
  const auto gpu_handle = gpu->GetGPUDescriptorHandleForHeapStart();
  auto created_handle = cpu_handle;
  created_handle.ptr += device->GetDescriptorHandleIncrementSize(heap_desc.Type);
  const UINT sentinel = 0x45abcdef;
  UINT64 completed = 0;
  UINT variant = 0;
  auto run = [&](D3D12_RESOURCE_DESC desc, D3D12_UNORDERED_ACCESS_VIEW_DESC view, UINT stride, UINT first, UINT count,
                 UINT mip, UINT first_slice, UINT slices, bool floating) -> int {
    const bool buffered = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const UINT subresources = buffered ? 1 : desc.MipLevels * (volume ? 1 : desc.DepthOrArraySize);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    UINT64 bytes = desc.Width;
    if (!buffered)
      device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> resource;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)
    ));
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && readback, "copy buffers were created"))
      return 1;
    void *mapped;
    CHECK(upload->Map(0, nullptr, &mapped));
    for (UINT64 at = 0; at + sizeof(UINT) <= bytes; at += sizeof(UINT))
      memcpy(static_cast<char *>(mapped) + at, &sentinel, sizeof(sentinel));
    upload->Unmap(0, nullptr);
    device->CreateUnorderedAccessView(resource.Get(), nullptr, &view, created_handle);
    device->CopyDescriptorsSimple(1, cpu_handle, created_handle, heap_desc.Type);
    device->CopyDescriptorsSimple(1, gpu->GetCPUDescriptorHandleForHeapStart(), cpu_handle, heap_desc.Type);
    const LONG width = buffered ? count : std::max<UINT64>(1, desc.Width >> mip);
    const LONG height = buffered ? 1 : std::max(1u, desc.Height >> mip);
    for (UINT mode = 0; mode < 8; mode++) {
      std::vector<D3D12_RECT> rects;
      switch (mode) {
      case 0:
        break;
      case 1:
        rects = {{1, 0, width - 1, height}};
        break;
      case 2:
        rects = {{-2, -1, width + 2, height + 1}};
        break;
      case 3:
        rects = {{0, 0, 0, height}, {0, 0, width, 0}, {width, 0, 0, height}, {0, height, width, 0}};
        break;
      case 4:
        rects = {{0, 0, width / 2 + 1, height}, {width / 3, 0, width, height}};
        break;
      case 5:
        rects = {{-3, 0, -1, height}, {width, 0, width + 3, height}, {0, -3, width, 0}, {0, height, width, height + 3}};
        break;
      case 6:
        rects = {
            {std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::max(),
             std::numeric_limits<LONG>::max()}
        };
        break;
      default:
        for (UINT i = 0; i < 8; i++)
          rects.push_back(
              {LONG(random() % (width + 4)) - 2, LONG(random() % (height + 4)) - 2, LONG(random() % (width + 4)) - 2,
               LONG(random() % (height + 4)) - 2}
          );
        break;
      }
      step(
          "seed %u variant %u dimension %u stride %u first %u count %u mip %u slices %u+%u float %u mode %u", seed,
          variant, desc.Dimension, stride, first, count, mip, first_slice, slices, floating, mode
      );
      for (auto &r : rects)
        printf("rect: %ld,%ld-%ld,%ld\n", r.left, r.top, r.right, r.bottom);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      CHECK(forget(readback.Get()));
      if (buffered)
        list->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, bytes);
      else
        for (UINT sub = 0; sub < subresources; sub++) {
          D3D12_TEXTURE_COPY_LOCATION dst{resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          dst.SubresourceIndex = sub;
          D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
          src.PlacedFootprint = footprints[sub];
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ID3D12DescriptorHeap *heaps[] = {gpu.Get()};
      list->SetDescriptorHeaps(1, heaps);
      const UINT uints[4] = {17, 29, 43, 59};
      const float floats[4] = {0.25f, 0.5f, 0.75f, 1.0f};
      UINT value = uints[0];
      if (floating) {
        memcpy(&value, &floats[0], sizeof(value));
        list->ClearUnorderedAccessViewFloat(
            gpu_handle, cpu_handle, resource.Get(), floats, UINT(rects.size()), rects.empty() ? nullptr : rects.data()
        );
      } else {
        list->ClearUnorderedAccessViewUint(
            gpu_handle, cpu_handle, resource.Get(), uints, UINT(rects.size()), rects.empty() ? nullptr : rects.data()
        );
      }
      transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (buffered)
        list->CopyBufferRegion(readback.Get(), 0, resource.Get(), 0, bytes);
      else
        for (UINT sub = 0; sub < subresources; sub++) {
          D3D12_TEXTURE_COPY_LOCATION src{resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          src.SubresourceIndex = sub;
          D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
          dst.PlacedFootprint = footprints[sub];
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      transition(list.Get(), resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(list->Close());
      ID3D12CommandList *commands[] = {list.Get()};
      queue->ExecuteCommandLists(1, commands);
      CHECK(queue->Signal(fence.Get(), ++completed));
      const auto deadline = GetTickCount64() + 20000;
      while (fence->GetCompletedValue() < completed && GetTickCount64() < deadline)
        Sleep(1);
      if (!expect(fence->GetCompletedValue() == completed, "fence did not complete"))
        return 1;
      CHECK(device->GetDeviceRemovedReason());
      CHECK(readback->Map(0, nullptr, &mapped));
      bool ok = true;
      for (UINT sub = 0; sub < subresources && ok; sub++) {
        const auto &f = footprints[sub];
        const UINT w = buffered ? UINT(bytes / sizeof(UINT)) : f.Footprint.Width;
        const UINT h = buffered ? 1 : f.Footprint.Height;
        const UINT d = buffered ? 1 : f.Footprint.Depth;
        for (UINT z = 0; z < d && ok; z++)
          for (UINT y = 0; y < h && ok; y++)
            for (UINT x = 0; x < w && ok; x++) {
              const UINT element = buffered ? x / stride : x;
              const LONG local = LONG(element) - (buffered ? LONG(first) : 0);
              const UINT slice = volume ? z : sub / desc.MipLevels;
              const bool in_view =
                  buffered ? element >= first && element < first + count
                           : sub % desc.MipLevels == mip && slice >= first_slice && slice < first_slice + slices;
              bool covered = mode == 0;
              for (auto &r : rects)
                covered |= local >= r.left && local < r.right && LONG(y) >= r.top && LONG(y) < r.bottom;
              const UINT want = in_view && covered ? value : sentinel;
              const UINT64 at = buffered ? UINT64(x) * sizeof(UINT)
                                         : f.Offset + (UINT64(z) * h + y) * f.Footprint.RowPitch + x * sizeof(UINT);
              UINT got;
              memcpy(&got, static_cast<char *>(mapped) + at, sizeof(got));
              ok = expect(got == want, "sub %u (%u,%u,%u): %#x, want %#x", sub, x, y, z, got, want);
            }
      }
      readback->Unmap(0, nullptr);
      if (!ok)
        return 1;
    }
    return 0;
  };
  for (UINT boundary = 2; boundary <= MULLION_PAGE_SIZE / sizeof(UINT); boundary *= 2)
    for (UINT count : {boundary - 1, boundary, boundary + 1})
      for (UINT kind = 0; kind < 3; kind++)
        for (UINT first : {0u, 3u})
          for (bool floating : {false, true}) {
            const UINT stride = kind == 1 ? 3 : 1;
            D3D12_RESOURCE_DESC desc{
                D3D12_RESOURCE_DIMENSION_BUFFER,
                0,
                (first + count + 3) * stride * sizeof(UINT),
                1,
                1,
                1,
                DXGI_FORMAT_UNKNOWN,
                {1, 0},
                D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
            };
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Format = kind == 0   ? DXGI_FORMAT_R32_TYPELESS
                          : kind == 1 ? DXGI_FORMAT_UNKNOWN
                          : floating  ? DXGI_FORMAT_R32_FLOAT
                                      : DXGI_FORMAT_R32_UINT;
            view.Buffer = {
                first, count, kind == 1 ? stride * UINT(sizeof(UINT)) : 0, 0,
                kind == 0 ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE
            };
            variant++;
            if (run(desc, view, stride, first, count, 0, 0, 1, floating))
              return 1;
          }
  for (UINT boundary = 4; boundary <= MULLION_PAGE_SIZE / sizeof(UINT); boundary *= 2)
    for (UINT dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; dimension <= D3D12_RESOURCE_DIMENSION_TEXTURE3D;
         dimension++)
      for (UINT width : {boundary - 1, boundary, boundary + 1})
        for (UINT layers : {1u, 8u})
          for (UINT mip : {0u, 1u})
            for (bool floating : {false, true})
              for (bool ranged : {false, true}) {
                if (layers == 1 && ranged)
                  continue;
                const bool volume = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                const UINT first_slice = ranged ? 1 : 0;
                const UINT depth = volume ? std::max(1u, layers >> mip) : layers;
                const UINT slices = ranged ? depth - 2 : depth;
                D3D12_RESOURCE_DESC desc{
                    D3D12_RESOURCE_DIMENSION(dimension),
                    0,
                    width,
                    dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : 5u,
                    UINT16(layers),
                    2,
                    floating ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT,
                    {1, 0},
                    D3D12_TEXTURE_LAYOUT_UNKNOWN,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                };
                D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = desc.Format;
                if (volume) {
                  view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                  view.Texture3D = {mip, first_slice, slices};
                } else if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
                  view.ViewDimension = layers > 1 ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE1D;
                  view.Texture1DArray = {mip, first_slice, slices};
                } else {
                  view.ViewDimension = layers > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
                  view.Texture2DArray = {mip, first_slice, slices, 0};
                }
                variant++;
                if (run(desc, view, 1, 0, 0, mip, first_slice, slices, floating))
                  return 1;
              }
  return verdict();
}
