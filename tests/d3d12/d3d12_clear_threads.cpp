// contract: ClearRenderTargetView with rectangles clears those rectangles and leaves the rest of the view
// (Microsoft Learn, ID3D12GraphicsCommandList::ClearRenderTargetView, pRects), whichever thread records it. a command
// list and its allocator are used by one thread at a time, and threads record lists of their own at once
// (Microsoft Learn, Recording command lists and bundles).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearrendertargetview
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles
// eight threads, each with an allocator, a list and a target of its own, record at once on a device that has cleared
// nothing yet: a full clear to zero, then a clear of the left half to a color only that thread uses. the targets
// alternate between two formats, so the threads ask for the same two clears at the same time. every channel is 0 or
// 1, which both formats hold exactly.
#include "d3d12_test.hpp"
#include <atomic>
#include <thread>

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  constexpr UINT threads = 8, size = 8;
  const struct {
    DXGI_FORMAT format;
    UINT bytes;
    UINT64 one;
  } formats[] = {{DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0xff}, {DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 0x3c00}};
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, threads};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  const UINT view_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  struct Work {
    ComPtr<ID3D12Resource> target, readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  } work[threads];
  for (UINT t = 0; t < threads; t++) {
    auto &w = work[t];
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        size,
        size,
        1,
        1,
        formats[t % std::size(formats)].format,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&w.target)
    ));
    w.rtv = views->GetCPUDescriptorHandleForHeapStart();
    w.rtv.ptr += t * view_size;
    device->CreateRenderTargetView(w.target.Get(), nullptr, w.rtv);
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &w.footprint, nullptr, nullptr, &bytes);
    w.readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(bool(w.readback), "readback buffer %u was not created", t))
      return verdict();
    CHECK(forget(w.readback.Get()));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&w.allocator)));
    CHECK(
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, w.allocator.Get(), nullptr, IID_PPV_ARGS(&w.list))
    );
  }

  step("%u threads record a clear of a rectangle at once", threads);
  std::atomic<UINT> ready{0};
  std::atomic<bool> go{false};
  HRESULT closed[threads];
  std::vector<std::thread> recorders;
  for (UINT t = 0; t < threads; t++)
    recorders.emplace_back([&, t] {
      auto &w = work[t];
      ready++;
      while (!go)
        std::this_thread::yield();
      const float zero[4] = {}, color[4] = {float(t & 1), float(t >> 1 & 1), float(t >> 2 & 1), 1};
      const D3D12_RECT left{0, 0, LONG(size / 2), LONG(size)};
      w.list->ClearRenderTargetView(w.rtv, zero, 0, nullptr);
      w.list->ClearRenderTargetView(w.rtv, color, 1, &left);
      transition(w.list.Get(), w.target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{w.target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          into{w.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      into.PlacedFootprint = w.footprint;
      w.list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
      closed[t] = w.list->Close();
    });
  while (ready != threads)
    std::this_thread::yield();
  go = true;
  for (auto &recorder : recorders)
    recorder.join();

  for (UINT t = 0; t < threads; t++) {
    auto &w = work[t];
    auto &format = formats[t % std::size(formats)];
    step("thread %u's target, format %u", t, format.format);
    if (!expect(closed[t] == S_OK, "Close returned %08lx", closed[t]))
      continue;
    CHECK(execute(device.Get(), queue.Get(), w.list.Get()));
    const unsigned char *got;
    CHECK(w.readback->Map(0, nullptr, (void **)&got));
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++)
        for (UINT channel = 0; channel < 4; channel++) {
          bool set = x < size / 2 && (channel == 3 || (t >> channel & 1));
          UINT64 want = set ? format.one : 0, value = 0;
          memcpy(
              &value, got + w.footprint.Offset + y * w.footprint.Footprint.RowPitch + (x * 4 + channel) * format.bytes,
              format.bytes
          );
          expect(value == want, "pixel %u,%u channel %u is %llx, expected %llx", x, y, channel, value, want);
        }
    w.readback->Unmap(0, nullptr);
  }
  return verdict();
}
