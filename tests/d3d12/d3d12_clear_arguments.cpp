// contract: ClearDepthStencilView's depth "will be clamped between 0 and 1" (Microsoft Learn,
// ID3D12GraphicsCommandList::ClearDepthStencilView, Depth), for float depth formats too, whole views and rectangles
// alike; and a clear given no rectangles clears the whole view (pRects: "If NULL, ... clears the entire resource
// view"), as vkd3d-proton does for a count of 0 whatever the pointer is (libs/vkd3d/command.c,
// d3d12_command_list_clear_attachment: "full_clear = !rect_count"). D32_FLOAT and D32_FLOAT_S8X24_UINT views holding
// 0.5 are cleared to 2 and to -1, whole and through a rectangle over the view's left half, and read 1 and 0 where they
// were cleared; an R8G8B8A8_UNORM view and a depth view are cleared with a rectangle array but a count of 0, and read
// the clear everywhere.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-cleardepthstencilview
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearrendertargetview
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
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

  constexpr UINT size = 8, pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pitch * size, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "the readback buffer is refused"))
    return verdict();
  const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RECT left{0, 0, size / 2, size};

  // runs what `record` records on `resource`, then reads its first plane back as 32-bit texels
  auto run = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES state, DXGI_FORMAT read_as, auto record,
                 auto want) {
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    record();
    transition(list.Get(), resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {0, {read_as, size, size, 1, pitch}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        UINT32 texel, wanted = want(x);
        memcpy(&texel, got + y * pitch + x * 4, 4);
        if (texel != wanted && !wrong++)
          printf("texel %u,%u is %08x, want %08x\n", x, y, texel, wanted);
      }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u wrong texels", wrong);
    return 0;
  };
  auto bits = [](float f) {
    UINT32 u;
    memcpy(&u, &f, 4);
    return u;
  };

  for (auto format : {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    for (float depth : {2.0f, -1.0f, 0.75f})
      for (UINT rects : {0u, 1u, 2u}) {
        // rects: 0, none; 1, the left half; 2, an array with a count of 0
        step("format %u, depth %g, %s", format, depth,
             rects == 0   ? "whole view"
             : rects == 1 ? "left half"
                          : "a rectangle array with a count of 0");
        ComPtr<ID3D12Resource> resource;
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                              nullptr, IID_PPV_ARGS(&resource)));
        device->CreateDepthStencilView(resource.Get(), nullptr, dsv);
        const float clamped = std::min(std::max(depth, 0.0f), 1.0f);
        if (run(
                resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, DXGI_FORMAT_R32_FLOAT,
                [&] {
                  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
                  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, rects == 1, rects ? &left : nullptr);
                },
                [&](UINT x) { return bits(rects == 1 && x >= size / 2 ? 0.5f : clamped); }
            ))
          return 1;
      }
  }

  step("a render target view, with a rectangle array and a count of 0");
  const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
  if (run(
          target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_R8G8B8A8_UNORM,
          [&] {
            list->ClearRenderTargetView(rtv, blue, 0, nullptr);
            list->ClearRenderTargetView(rtv, red, 0, &left);
          },
          [](UINT) { return 0xff0000ffu; }
      ))
    return 1;
  return verdict();
}
