// contract: DLSS's statistics (NVSDK_NGX_DLSS_GetStats, nvsdk_ngx_helpers.h: the "DLSSGetStatsCallback" of the
// capability parameters, which writes "SizeInBytes", "VRAM allocated" by DLSS). on Mullion that is what MetalFX's
// temporal scaler of a feature's sizes takes, which the adapter's usage (IDXGIAdapter3::QueryVideoMemoryInfo,
// CurrentUsage, the device's currentAllocatedSize) shows: the scaler is made when the list records its first upscale
// (IMTLD3D12CommandListExt::TemporalUpscale) and runs, and the usage grows by its memory. the size NGX reports for the
// render and output sizes of a feature (Width, Height, OutWidth, OutHeight, as NGX_D3D12_CREATE_DLSS_EXT sets them on
// the parameters) is within a quarter of that, for each of several sizes, and 0 without them.
#include "d3d12_test.hpp"
#include "../../src/d3d12/d3d12_interfaces.hpp"
#include "../../src/nvngx/nvngx.hpp"
#include <dxgi1_4.h>

using init_t = NVNGX_RESULT (*)(unsigned long long, const wchar_t *, ID3D12Device *, const void *, unsigned int);
using parameters_t = NVNGX_RESULT (*)(NVNGXParameter **);
using destroy_t = NVNGX_RESULT (*)(NVNGXParameter *);
using stats_t = NVNGX_RESULT (*)(NVNGXParameter *);

int
main(int argc, char **argv) {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> first;
  ComPtr<IDXGIAdapter3> adapter;
  CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapters1(0, &first));
  CHECK(first.As(&adapter));
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  ComPtr<IMTLD3D12CommandListExt> ext;
  CHECK(list.As(&ext));
  if (!ext->SupportsTemporalUpscale()) {
    printf("skipped: no MetalFX temporal scaler on this device\n");
    return 77;
  }

  step("NGX's core");
  auto core = LoadLibraryA("nvngx.dll");
  if (!expect(core, "nvngx.dll is not there: %lu", GetLastError()))
    return verdict();
  auto init = (init_t)GetProcAddress(core, "NVSDK_NGX_D3D12_Init");
  auto capabilities = (parameters_t)GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
  auto destroy = (destroy_t)GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters");
  NVNGXParameter *caps = nullptr;
  void *callback = nullptr;
  if (!expect(
          init && capabilities && destroy && init(0, L".", device.Get(), nullptr, 0) == NVNGX_RESULT_OK &&
              capabilities(&caps) == NVNGX_RESULT_OK &&
              caps->Get("DLSSGetStatsCallback", &callback) == NVNGX_RESULT_OK && callback,
          "no statistics callback"
      ))
    return verdict();
  auto stats = (stats_t)callback;
  unsigned long long size = ~0ull;
  expect(
      stats(caps) == NVNGX_RESULT_OK && caps->Get("SizeInBytes", &size) == NVNGX_RESULT_OK && size == 0,
      "statistics without a feature's sizes: %llu", size
  );

  auto usage = [&] {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    return info.CurrentUsage;
  };
  auto texture = [&](DXGI_FORMAT format, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    ComPtr<ID3D12Resource> t;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,    w, h, 1, 1, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,       flags};
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&t));
    return t;
  };
  const struct {
    UINT width, height, out_width, out_height;
  } sizes[] = {
      {960, 540, 1920, 1080},
      {1280, 720, 2560, 1440},
      {1920, 1080, 3840, 2160},
      {2560, 1440, 3840, 2160},
      {1920, 1080, 1920, 1080}
  };
  std::vector<IMTLD3D12Upscaler *> scalers;
  for (auto &s : sizes) {
    step("an upscale of %ux%u to %ux%u", s.width, s.height, s.out_width, s.out_height);
    const auto input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, s.width, s.height, D3D12_RESOURCE_FLAG_NONE, input);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, s.width, s.height, D3D12_RESOURCE_FLAG_NONE, input);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, s.width, s.height, D3D12_RESOURCE_FLAG_NONE, input);
    auto output = texture(
        DXGI_FORMAT_R16G16B16A16_FLOAT, s.out_width, s.out_height, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    if (!expect(color && depth && motion && output, "the textures"))
      break;
    // what the textures take is not the scaler's: the usage is read once they are there and the GPU is idle
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    UINT64 before = usage();
    MTL_TEMPORAL_UPSCALE_D3D12_DESC desc{};
    desc.Color = color.Get();
    desc.Depth = depth.Get();
    desc.MotionVector = motion.Get();
    desc.Output = output.Get();
    desc.MotionVectorScaleX = desc.MotionVectorScaleY = desc.PreExposure = 1;
    desc.InReset = TRUE;
    IMTLD3D12Upscaler *scaler = nullptr;
    CHECK(ext->TemporalUpscale(&desc, &scaler));
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT64 measured = usage() - before;
    // kept to the end: a scaler destroyed here frees its memory while the next one is measured
    scalers.push_back(scaler);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));

    caps->Set(NVNGX_Parameter_Width, s.width);
    caps->Set(NVNGX_Parameter_Height, s.height);
    caps->Set(NVNGX_Parameter_OutWidth, s.out_width);
    caps->Set(NVNGX_Parameter_OutHeight, s.out_height);
    size = 0;
    expect(
        stats(caps) == NVNGX_RESULT_OK && caps->Get("SizeInBytes", &size) == NVNGX_RESULT_OK,
        "no statistics for %ux%u to %ux%u", s.width, s.height, s.out_width, s.out_height
    );
    printf(
        "%ux%u to %ux%u: the scaler takes %llu bytes, NGX reports %llu\n", s.width, s.height, s.out_width, s.out_height,
        (unsigned long long)measured, size
    );
    expect(
        measured && size >= measured - measured / 4 && size <= measured + measured / 4,
        "NGX reports %llu bytes for %ux%u to %ux%u, where the scaler takes %llu", size, s.width, s.height, s.out_width,
        s.out_height, (unsigned long long)measured
    );
  }
  for (auto scaler : scalers)
    if (scaler)
      scaler->Destroy();
  destroy(caps);
  return verdict();
}
