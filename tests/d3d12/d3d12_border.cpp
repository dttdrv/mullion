// contract: a sampler's custom border color (D3D11.3 7.18.9.1). only the format's own channels take it, the rest keep
// their defaults; it is clamped to the format's range, then filtered with the texels like one; a comparison compares
// against its red (sample_c); a gather returns it for each texel off the edge. descriptors copied from a CPU heap keep
// it, and Metal's own border colors stay native. the range is the view format's: D24 is unorm on a float32 backing,
// and a swizzled half view keeps half's
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float4> f : register(t0);
Texture2D<float4> u : register(t1);
Texture2D<float4> sn : register(t2);
Texture2D<uint4> i : register(t3);
Texture2D<float> d : register(t4);
Texture2D<float> d24 : register(t5);
Texture2D<float4> h : register(t6);
SamplerState s[6] : register(s0);
SamplerComparisonState c : register(s6);
SamplerComparisonState c24 : register(s7);
cbuffer C : register(b0) { float ref, high, one; };
RWStructuredBuffer<float4> o : register(u0);
[numthreads(1, 1, 1)] void cs() {
  // on the left edge half of a linear footprint is border, further left all of it
  float2 edge = float2(0, 0.5), off = float2(-1, 0.5);
  o[0] = f.SampleLevel(s[0], edge, 0);
  o[1] = f.SampleLevel(s[0], off, 0);
  o[2] = u.SampleLevel(s[0], off, 0);
  o[3] = u.SampleLevel(s[1], off, 0);
  o[4] = sn.SampleLevel(s[2], off, 0);
  o[5] = u.SampleLevel(s[3], off, 0);
  o[6] = f.SampleLevel(s[4], off, 0);
  o[7] = asfloat(i.GatherRed(s[0], edge));
  o[8] = float4(d.SampleCmpLevelZero(c, edge, ref), d.SampleCmpLevelZero(c, off, ref), d.SampleCmpLevelZero(c, off, high), 0);
  o[9] = d.GatherCmp(c, edge, ref);
  o[10] = float4(d24.SampleCmpLevelZero(c24, off, one), 0, 0, 0);
  o[11] = h.SampleLevel(s[5], off, 0);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  // four uniform 4x4 textures of 32-bit texels, two depth textures, and a half texture only read off its edge
  const float value = 0.25f;
  const UINT uint_value = 5, size = 4, pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_FLOAT,     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM,
                                 DXGI_FORMAT_R32_UINT,      DXGI_FORMAT_R32_TYPELESS,   DXGI_FORMAT_R24G8_TYPELESS,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT};
  const DXGI_FORMAT views_formats[] = {formats[0],          formats[1], formats[2], formats[3], DXGI_FORMAT_R32_FLOAT,
                                       DXGI_FORMAT_R24_UNORM_X8_TYPELESS, formats[6]};
  const DXGI_FORMAT dsv_formats[] = {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT};
  const UINT texel[] = {std::bit_cast<UINT>(value), 0x40404040, 0x20202020, uint_value};
  const UINT count = std::size(formats);
  ComPtr<ID3D12Resource> textures[count];
  D3D12_HEAP_PROPERTIES heap_props{D3D12_HEAP_TYPE_DEFAULT};
  for (UINT t = 0; t < count; t++) {
    bool depth = t == 4 || t == 5;
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, formats[t], {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE};
    CHECK(device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
              : t == 6 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&textures[t])
    ));
  }
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 4 * size * pitch + 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  BYTE *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT t = 0; t < 4; t++)
    for (UINT y = 0; y < size; y++)
      std::fill_n((UINT *)(mapped + (t * size + y) * pitch), size, texel[t]);
  // the comparison references: 2 passes LESS against the border's 32 only, 64 against neither, 1 against D24's border
  // of 2 only unclamped
  const float ref = 2, high = 64, one = 1, cb[3] = {ref, high, one};
  const UINT64 cb_offset = 4 * size * pitch;
  memcpy(mapped + cb_offset, cb, sizeof(cb));

  // samplers 0-2 and 5 with custom colors, 3 with Metal's opaque black, 4 a copy of 0 from a CPU heap, 6 and 7
  // comparisons with custom colors
  const float colors[][4] = {{32, 32, 32, 32},     {0.25f, 0.5f, 0.75f, 0.3f}, {-3, -3, -3, -3}, {0, 0, 0, 1},
                             {1e5, 1e5, 1e5, 1e5}, {2, 2, 2, 2}};
  D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      cpu_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1},
      view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> samplers, cpu_samplers, views;
  CHECK(device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&samplers)));
  CHECK(device->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu_samplers)));
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  auto slot = [&](ID3D12DescriptorHeap *heap, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += i * device->GetDescriptorHandleIncrementSize(type);
    return h;
  };
  auto sampler = [&](const float *color, D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR) {
    D3D12_SAMPLER_DESC desc{filter, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                            D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0, 1, D3D12_COMPARISON_FUNC_LESS};
    std::copy_n(color, 4, desc.BorderColor);
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    return desc;
  };
  for (UINT i = 0; i < 4; i++) {
    auto desc = sampler(colors[i]);
    device->CreateSampler(&desc, slot(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, i));
  }
  auto copied = sampler(colors[0]), compare = sampler(colors[0], D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR);
  device->CreateSampler(&copied, cpu_samplers->GetCPUDescriptorHandleForHeapStart());
  device->CopyDescriptorsSimple(
      1, slot(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4), cpu_samplers->GetCPUDescriptorHandleForHeapStart(),
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
  );
  device->CreateSampler(&compare, slot(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 6));
  auto half = sampler(colors[4]), compare24 = sampler(colors[5], D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR);
  device->CreateSampler(&half, slot(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 5));
  device->CreateSampler(&compare24, slot(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 7));
  for (UINT t = 0; t < count; t++) {
    // the half view swizzles its channels
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{views_formats[t], D3D12_SRV_DIMENSION_TEXTURE2D,
                                        UINT(t == 6 ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, 3)
                                                    : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)};
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(textures[t].Get(), &srv, slot(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, t));
  }
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
  ComPtr<ID3D12DescriptorHeap> dsv;
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv)));
  for (UINT i = 0; i < 2; i++) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view{dsv_formats[i], D3D12_DSV_DIMENSION_TEXTURE2D};
    device->CreateDepthStencilView(textures[4 + i].Get(), &dsv_view, slot(dsv.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, i));
  }

  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, count}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 8}};
  D3D12_ROOT_PARAMETER params[4] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_CBV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].DescriptorTable = {1, &ranges[0]};
  params[1].DescriptorTable = {1, &ranges[1]};
  auto rs = root_signature(device.Get(), {4, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  const UINT results = 12;
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, results * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, results * 16, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  for (UINT t = 0; t < 4; t++) {
    D3D12_TEXTURE_COPY_LOCATION dst{textures[t].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = {t * size * pitch, {formats[t], size, size, 1, pitch}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), textures[t].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  for (UINT i = 0; i < 2; i++) {
    list->ClearDepthStencilView(slot(dsv.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, i), D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
    transition(
        list.Get(), textures[4 + i].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
  }
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootDescriptorTable(1, samplers->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootConstantBufferView(2, upload->GetGPUVirtualAddress() + cb_offset);
  list->SetComputeRootUnorderedAccessView(3, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), out.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  // expectations: a clamped color per channel, half of it at the edge, compared with LESS
  auto clamp = [](const float *c, float lo, float hi) {
    std::array<float, 4> r;
    for (int i = 0; i < 4; i++)
      r[i] = std::clamp(c[i], lo, hi);
    return r;
  };
  auto less = [](float r, float texel) { return float(r < texel); };
  const float c = colors[0][0], w = 0.5f, half_max = std::ldexp(2 - std::ldexp(1.0f, -10), 15);
  const std::array<float, 4> expected[results] = {
      {w * value + w * c, 0, 0, 1},
      {c, 0, 0, 1},
      clamp(colors[0], 0, 1),
      clamp(colors[1], 0, 1),
      clamp(colors[2], -1, 1),
      clamp(colors[3], 0, 1),
      {c, 0, 0, 1},
      {std::bit_cast<float>((UINT)c), std::bit_cast<float>(uint_value), std::bit_cast<float>(uint_value),
       std::bit_cast<float>((UINT)c)},
      {w * less(ref, value) + w * less(ref, c), less(ref, c), less(high, c), 0},
      {less(ref, c), less(ref, value), less(ref, value), less(ref, c)},
      {less(one, clamp(colors[5], 0, 1)[0]), 0, 0, 0},
      clamp(colors[4], -half_max, half_max),
  };
  const char *names[results] = {"R32F edge",   "R32F outside",       "unorm clamped", "unorm color",
                                "snorm clamped", "native black",     "copied",        "uint gather",
                                "compare",     "gather compare",   "D24 compare",   "swizzled half"};
  float *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  unsigned failures = 0;
  for (UINT r = 0; r < results; r++) {
    bool ok = true;
    for (UINT i = 0; i < 4; i++)
      ok &= r == 7 ? std::bit_cast<UINT>(got[r * 4 + i]) == std::bit_cast<UINT>(expected[r][i])
                   : std::fabs(got[r * 4 + i] - expected[r][i]) <= 1.0f / 255;
    if (!ok && ++failures)
      printf(
          "%s: %g %g %g %g, want %g %g %g %g\n", names[r], got[r * 4], got[r * 4 + 1], got[r * 4 + 2], got[r * 4 + 3],
          expected[r][0], expected[r][1], expected[r][2], expected[r][3]
      );
  }
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
