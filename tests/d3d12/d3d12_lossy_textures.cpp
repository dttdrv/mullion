// contract: with MULLION_LOSSY_TEXTURES=1 (the memory saver, src/mullion/mullion_memory.hpp) a committed RGBA16F render
// target, as a game's HDR targets are, takes about half the memory it takes without, on a GPU that has lossy
// compression (Apple8 and later), and still reads back what was put in it: a clear exactly, and a smooth gradient
// copied in to within 1/32. (an RGBA8 texture stays lossless: D3D12's relaxed format casting gives it views of another
// layout, which Metal makes no lossy texture for.)
// the test sets the variable itself; with the argument "lossless" it leaves it unset, and expects the full size.
#include "d3d12_test.hpp"
#include "../../src/winemetal/Metal.hpp"
#include <dxgi1_4.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

static uint16_t
half(float value) { // for values in [0, 1], rounded to nearest
  if (value <= 0)
    return 0;
  int exponent;
  float fraction = frexpf(value, &exponent); // value = fraction * 2^exponent, fraction in [0.5, 1)
  if (exponent < -13)
    return uint16_t(lrintf(value * 16777216.0f)); // a denormal: units of 2^-24
  uint32_t mantissa = uint32_t(lrintf((fraction * 2 - 1) * 1024));
  if (mantissa == 1024) {
    mantissa = 0;
    exponent++;
  }
  return uint16_t(((exponent - 1 + 15) << 10) | mantissa);
}

static float
single(uint16_t bits) {
  int exponent = (bits >> 10) & 31, mantissa = bits & 1023;
  float value = exponent ? ldexpf(1 + mantissa / 1024.0f, exponent - 15) : ldexpf(mantissa / 1024.0f, -14);
  return bits & 0x8000 ? -value : value;
}

int
main(int argc, char **argv) {
  bool lossless = argc > 1 && !strcmp(argv[argc - 1], "lossless");
  if (!lossless)
    SetEnvironmentVariableA("MULLION_LOSSY_TEXTURES", "1");
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter1;
  ComPtr<IDXGIAdapter3> adapter;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapters1(0, &adapter1));
  CHECK(adapter1.As(&adapter));
  // the family the bridge answers as, which the memory saver asks (src/mullion/mullion_memory.cpp): a newer GPU run as
  // Apple7 (MULLION_GPU_FAMILY) has no lossy compression either. the lossless case expects the full size on any
  auto pool = WMT::MakeAutoreleasePool();
  auto devices = WMT::CopyAllDevices();
  CHECK(devices && devices.count() ? S_OK : E_FAIL);
  if (!lossless && !devices.object(0).supportsFamily(WMTGPUFamilyApple8)) {
    printf("skipped: an Apple7 GPU has no lossy compression\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  constexpr UINT kSize = 4096;
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, kSize, kSize, 1, 1,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  DXGI_QUERY_VIDEO_MEMORY_INFO before, after;
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &before));
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&texture)));
  CHECK(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after));
  UINT64 full = UINT64(kSize) * kSize * 8, took = after.CurrentUsage - before.CurrentUsage;
  printf("a %ux%u RGBA16F render target took %llu MB (%llu MB uncompressed)%s\n", kSize, kSize, took >> 20, full >> 20,
         lossless ? "" : " with MULLION_LOSSY_TEXTURES=1");
  if (lossless)
    expect(took >= full, "the texture took less than its size without the memory saver");
  else
    expect(took <= full * 5 / 8, "the memory saver left the texture its full size");

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  CHECK(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture.Get(), nullptr, rtv);

  const UINT64 row = kSize * 8, bytes = row * kSize;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!readback || !upload) {
    printf("failed: buffers\n");
    return 1;
  }
  D3D12_TEXTURE_COPY_LOCATION in_texture{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION in_readback{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  in_readback.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16B16A16_FLOAT, kSize, kSize, 1, UINT(row)};
  D3D12_TEXTURE_COPY_LOCATION in_upload = in_readback;
  in_upload.pResource = upload.Get();

  // a clear, read back
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  list->ClearRenderTargetView(rtv, color, 0, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&in_readback, 0, 0, 0, &in_texture, nullptr);
  CHECK(forget(readback.Get()));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint16_t *data;
  CHECK(readback->Map(0, nullptr, (void **)&data));
  size_t cleared_wrong = 0;
  for (UINT64 i = 0; i < bytes / 2; i += 4)
    cleared_wrong += data[i] != 0x3400 || data[i + 1] != 0x3800 || data[i + 2] != 0x3a00 || data[i + 3] != 0x3c00;
  readback->Unmap(0, nullptr);
  expect(!cleared_wrong, "%zu texels of the clear read back otherwise", cleared_wrong);

  // a smooth gradient copied in, read back
  uint16_t *gradient;
  CHECK(upload->Map(0, nullptr, (void **)&gradient));
  for (UINT y = 0; y < kSize; y++)
    for (UINT x = 0; x < kSize; x++) {
      uint16_t *texel = gradient + (y * row) / 2 + x * 4;
      texel[0] = half(float(x) / (kSize - 1));
      texel[1] = half(float(y) / (kSize - 1));
      texel[2] = half(float(x + y) / (2 * kSize - 2));
      texel[3] = half(1);
    }
  upload->Unmap(0, nullptr);
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyTextureRegion(&in_texture, 0, 0, 0, &in_upload, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&in_readback, 0, 0, 0, &in_texture, nullptr);
  CHECK(forget(readback.Get()));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(readback->Map(0, nullptr, (void **)&data));
  CHECK(upload->Map(0, nullptr, (void **)&gradient));
  double worst = 0, total = 0;
  for (UINT64 i = 0; i < bytes / 2; i++) {
    double error = fabs(double(single(data[i])) - double(single(gradient[i])));
    worst = error > worst ? error : worst;
    total += error;
  }
  readback->Unmap(0, nullptr);
  upload->Unmap(0, nullptr);
  printf("the gradient read back within %.5f, %.6f on average\n", worst, total / double(bytes / 2));
  expect(worst <= 1.0 / 32, "the gradient read back %.5f off", worst);
  if (lossless)
    expect(worst == 0, "a lossless texture changed the gradient");
  return verdict();
}
