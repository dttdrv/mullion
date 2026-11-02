// contract: MetalFX upscaling recorded in a command list (IMTLD3D12CommandListExt, what Mullion's NGX bridge records
// DLSS with) runs after the list's earlier work and before its later work, reads the inputs DLSS is given as DLSS
// reads them, and writes an image whose content is its input's. a uniform image is that image at any scale, so the
// output must be the input's color in every texel, within one unit of the 8-bit output. the inputs are cleared by the
// same list right before the upscale, and the output is copied right after it, so a pass out of order reads or leaves
// the bytes of the frame before. depth and exposure are R32_TYPELESS and motion vectors R16G16_TYPELESS, as games
// create them; DLSS takes motion vectors as floats only, so normalized ones are refused rather than misread
#include "d3d12_test.hpp"
#include "../../src/d3d12/d3d12_interfaces.hpp"
#include "../../src/nvngx/nvngx.hpp"
#include <array>
#include <cmath>
#include <functional>
#include <vector>

struct Texture {
  ComPtr<ID3D12Resource> resource;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
};

using init_t = NVNGX_RESULT (*)(unsigned long long, const wchar_t *, ID3D12Device *, const void *, unsigned int);
using parameters_t = NVNGX_RESULT (*)(NVNGXParameter **);
using destroy_t = NVNGX_RESULT (*)(NVNGXParameter *);
using create_t = NVNGX_RESULT (*)(ID3D12GraphicsCommandList *, unsigned int, NVNGXParameter *, unsigned int **);
using evaluate_t = NVNGX_RESULT (*)(ID3D12GraphicsCommandList *, const unsigned int *, NVNGXParameter *, void *);
using release_t = NVNGX_RESULT (*)(unsigned int *);
using shutdown_t = NVNGX_RESULT (*)(ID3D12Device *);

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<IMTLD3D12CommandListExt> ext;
  step("the list's MetalFX extension");
  CHECK(list.As(&ext));
  if (!ext->SupportsTemporalUpscale()) {
    printf("skipped: no MetalFX temporal scaler on this device\n");
    return 77;
  }

  const UINT width = 256, height = 144, scale = 2, out_width = width * scale, out_height = height * scale;
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  UINT rtv_count = 0;
  // a texture in `state`, with a render target view of `view` where it is cleared before each upscale
  auto texture = [&](DXGI_FORMAT format, DXGI_FORMAT view, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES state) {
    Texture t;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&t.resource));
    if (t.resource && view != DXGI_FORMAT_UNKNOWN) {
      t.rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      t.rtv.ptr += rtv_count++ * rtv_size;
      D3D12_RENDER_TARGET_VIEW_DESC rtv{view, D3D12_RTV_DIMENSION_TEXTURE2D};
      device->CreateRenderTargetView(t.resource.Get(), &rtv, t.rtv);
    }
    return t;
  };
  const auto input = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  const auto target = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, width, height, target, input);
  auto depth = texture(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, width, height, target, input);
  auto motion = texture(DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, width, height, target, input);
  auto exposure = texture(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, 1, 1, target, input);
  auto normalized = texture(DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, width, height, target, input);
  auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, out_width, out_height,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  CHECK(color.resource && depth.resource && motion.resource && exposure.resource && normalized.resource &&
                output.resource
            ? S_OK
            : E_FAIL);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  auto out_desc = output.resource->GetDesc();
  device->GetCopyableFootprints(&out_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto read_back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // one frame upscaled by what `record` records into the list: the color is `a` in the squares of a checkerboard of
  // `square` texels and `b` between them (one color when `square` is 0); the output's texels, tightly
  auto upscaled = [&](const BYTE *a, const BYTE *b, UINT square, const std::function<bool()> &record) {
    std::vector<BYTE> texels;
    if (FAILED(forget(read_back.Get())) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
      return texels;
    auto rgba = [](const BYTE *c) { return std::array<float, 4>{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f}; };
    const auto fa = rgba(a), fb = rgba(b);
    std::vector<D3D12_RECT> squares;
    for (UINT y = 0; square && y < height; y += square)
      for (UINT x = (y / square % 2) * square; x < width; x += 2 * square)
        squares.push_back({LONG(x), LONG(y), LONG(x + square), LONG(y + square)});
    const float half[] = {0.5f, 0, 0, 0}, zero[] = {0, 0, 0, 0}, one[] = {1, 0, 0, 0};
    std::pair<Texture *, const float *> clears[] = {{&color, fb.data()}, {&depth, half}, {&motion, zero}, {&exposure, one}};
    for (auto [t, value] : clears) {
      transition(list.Get(), t->resource.Get(), input, D3D12_RESOURCE_STATE_RENDER_TARGET);
      list->ClearRenderTargetView(t->rtv, value, 0, nullptr);
      if (t == &color && !squares.empty())
        list->ClearRenderTargetView(t->rtv, fa.data(), squares.size(), squares.data());
      transition(list.Get(), t->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, input);
    }
    if (!expect(record(), "the upscale was refused")) {
      list->Close();
      return texels;
    }
    transition(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{read_back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{output.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
    const BYTE *read;
    if (!expect(ran == S_OK, "the device did not live through it: %08lx", ran) ||
        FAILED(read_back->Map(0, nullptr, (void **)&read)))
      return texels;
    for (UINT y = 0; y < out_height; y++)
      texels.insert(texels.end(), read + footprint.Footprint.RowPitch * y, read + footprint.Footprint.RowPitch * y + out_width * 4);
    read_back->Unmap(0, nullptr);
    return texels;
  };
  // through the extension, as the NGX bridge records it
  auto frame = [&](const BYTE *a, const BYTE *b, UINT square, BOOL reset, IMTLD3D12Upscaler *&scaler) {
    return upscaled(a, b, square, [&] {
      MTL_TEMPORAL_UPSCALE_D3D12_DESC desc{};
      desc.InReset = reset;
      desc.Color = color.resource.Get();
      desc.Depth = depth.resource.Get();
      desc.MotionVector = motion.resource.Get();
      desc.ExposureTexture = exposure.resource.Get();
      desc.Output = output.resource.Get();
      desc.MotionVectorScaleX = desc.MotionVectorScaleY = 1;
      desc.PreExposure = 1;
      return SUCCEEDED(ext->TemporalUpscale(&desc, &scaler));
    });
  };
  // how many color channels of two images are more than one unit apart
  auto apart = [](const std::vector<BYTE> &x, const std::vector<BYTE> &y) {
    UINT count = 0;
    for (size_t i = 0; i < x.size() && i < y.size(); i++)
      count += i % 4 != 3 && std::abs(int(x[i]) - int(y[i])) > 1;
    return count;
  };

  // whether an output is `want` in every texel; false when there is none
  auto uniform = [&](const std::vector<BYTE> &read, const BYTE *want) {
    if (!expect(read.size() == out_width * out_height * 4, "no image"))
      return false;
    std::vector<BYTE> image;
    for (UINT i = 0; i < out_width * out_height; i++)
      image.insert(image.end(), want, want + 4);
    UINT off = apart(read, image);
    expect(!off, "%u of %u channels are not the input's color (first texel %u,%u,%u)", off, out_width * out_height * 3,
           read[0], read[1], read[2]);
    return true;
  };

  IMTLD3D12Upscaler *upscaler = nullptr;
  // the colors of two frames, each in reset: the second must be its own, not the first's
  const BYTE frames[][4] = {{64, 128, 192, 255}, {200, 20, 100, 255}};
  for (auto &want : frames) {
    step("a %ux%u frame of color %u,%u,%u upscaled %ux", width, height, want[0], want[1], want[2], scale);
    if (!uniform(frame(want, want, 0, TRUE, upscaler), want))
      return verdict();
  }

  // a frame in reset shows none of the frames before: it is the image a scaler with no frames before makes of it. a
  // checkerboard and its inverse make the frame before show where a frame is not in reset (the test's own check that
  // it can tell the two apart)
  const UINT square = 8;
  const BYTE *dark = frames[0], *light = frames[1];
  IMTLD3D12Upscaler *fresh = nullptr, *continued = nullptr;
  step("a checkerboard upscaled by a scaler with no frames before");
  auto first = frame(light, dark, square, TRUE, fresh);
  step("the same checkerboard in reset, after its inverse");
  frame(dark, light, square, TRUE, upscaler);
  auto reset = frame(light, dark, square, TRUE, upscaler);
  step("the same checkerboard not in reset, after its inverse");
  frame(dark, light, square, TRUE, continued);
  auto kept = frame(light, dark, square, FALSE, continued);
  if (expect(!first.empty() && !reset.empty() && !kept.empty(), "no image")) {
    expect(!apart(first, reset), "%u channels of the frame in reset show the frame before", apart(first, reset));
    expect(apart(first, kept), "a frame not in reset shows nothing of the frame before: reset cannot be told apart");
  }
  for (auto scaler : {fresh, continued})
    if (scaler)
      scaler->Destroy();

  step("normalized motion vectors, which hold no float to read");
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  MTL_TEMPORAL_UPSCALE_D3D12_DESC refused{};
  refused.Color = color.resource.Get();
  refused.Depth = depth.resource.Get();
  refused.MotionVector = normalized.resource.Get();
  refused.Output = output.resource.Get();
  refused.PreExposure = 1;
  expect(ext->TemporalUpscale(&refused, &upscaler) == E_INVALIDARG, "normalized motion vectors were taken");
  CHECK(list->Close());
  if (upscaler)
    upscaler->Destroy();

  // DLSS through Mullion's NGX core (src/nvngx/nvngx_d3d12.cpp), called as a game's NGX library calls it, where the
  // build has it: resources set as resources or, as Streamline sets them, as plain pointers, and DLSS's defaults for
  // what a game leaves out (no exposure texture; a pre-exposure of 0, or none, for no pre-exposure)
  if (argc < 2 || strcmp(argv[1], "ngx"))
    return verdict();
  step("NGX's core");
  auto core = LoadLibraryA("nvngx.dll");
  if (!expect(core, "nvngx.dll is not there: %lu", GetLastError()))
    return verdict();
  auto init = (init_t)GetProcAddress(core, "NVSDK_NGX_D3D12_Init");
  auto capabilities = (parameters_t)GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
  auto allocate = (parameters_t)GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters");
  auto destroy = (destroy_t)GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters");
  auto create = (create_t)GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature");
  auto evaluate = (evaluate_t)GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature");
  auto release = (release_t)GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature");
  auto shutdown = (shutdown_t)GetProcAddress(core, "NVSDK_NGX_D3D12_Shutdown1");
  if (!expect(init && capabilities && allocate && destroy && create && evaluate && release && shutdown,
              "the core lacks an entry point") ||
      !expect(init(0, L".", device.Get(), nullptr, 0) == NVNGX_RESULT_OK, "NGX did not start"))
    return verdict();
  NVNGXParameter *caps = nullptr, *params = nullptr;
  unsigned int available = 0, *feature = nullptr;
  expect(capabilities(&caps) == NVNGX_RESULT_OK && caps->Get("SuperSampling.Available", &available) == NVNGX_RESULT_OK &&
             available == 1,
         "DLSS is not offered");
  // Ray Reconstruction where MetalFX has its denoiser, and nowhere else
  unsigned int denoising = 2;
  expect(caps && caps->Get("SuperSamplingDenoising.Available", &denoising) == NVNGX_RESULT_OK &&
             denoising == unsigned(ext->SupportsDenoisedUpscale()),
         "Ray Reconstruction is offered as %u where the denoiser is %s", denoising,
         ext->SupportsDenoisedUpscale() ? "there" : "not");
  if (caps)
    destroy(caps);
  step("a DLSS feature for %ux%u to %ux%u", width, height, out_width, out_height);
  if (!expect(allocate(&params) == NVNGX_RESULT_OK, "no parameters"))
    return verdict();
  params->Set("Width", width);
  params->Set("Height", height);
  params->Set("OutWidth", out_width);
  params->Set("OutHeight", out_height);
  params->Set("DLSS.Feature.Create.Flags", int(NVNGX_DLSS_FLAG_MV_LOWRES));
  if (expect(create(list.Get(), NVNGX_FEATURE_SUPERSAMPLING, params, &feature) == NVNGX_RESULT_OK && feature,
             "DLSS was not created")) {
    params->Set("Color", color.resource.Get());
    params->Set("Output", (void *)output.resource.Get());
    params->Set("Depth", depth.resource.Get());
    params->Set("MotionVectors", (void *)motion.resource.Get());
    params->Set("Reset", 1);
    for (auto &want : frames) {
      step("DLSS of a frame of color %u,%u,%u, pre-exposure %s", want[0], want[1], want[2], &want == frames ? "none" : "0");
      if (&want != frames)
        params->Set("DLSS.Pre.Exposure", 0.0f);
      uniform(upscaled(want, want, 0, [&] { return evaluate(list.Get(), feature, params, nullptr) == NVNGX_RESULT_OK; }),
              want);
    }
    step("DLSS without depth");
    params->Set("Depth", (ID3D12Resource *)nullptr);
    expect(evaluate(list.Get(), feature, params, nullptr) == NVNGX_RESULT_INVALID_PARAMETER, "DLSS ran without depth");
    expect(release(feature) == NVNGX_RESULT_OK, "DLSS was not released");
  }
  step("a Ray Reconstruction feature, where the denoiser is %s", ext->SupportsDenoisedUpscale() ? "there" : "not");
  unsigned int *denoiser = nullptr;
  auto made = create(list.Get(), NVNGX_FEATURE_RAYRECONSTRUCTION, params, &denoiser);
  expect(made == (ext->SupportsDenoisedUpscale() ? NVNGX_RESULT_OK : NVNGX_RESULT_FEATURE_NOT_SUPPORTED),
         "Ray Reconstruction's creation answered %#x", made);
  if (denoiser)
    release(denoiser);
  destroy(params);
  expect(shutdown(device.Get()) == NVNGX_RESULT_OK, "NGX did not shut down");
  return verdict();
}
