// contract: sharing samplers preserves the mip selected by anisotropic gradients. D3D11.3 7.18.11 says
// "lengthMinor = determinant/lengthMajor" when the ratio is not clamped, and "output.LOD = log2(lengthMinor);".
// mip 0 is zero and later mips are one: footprints (n, 1) and (4n, 4), with MaxAnisotropy n, select LOD 0 and 2.
// both axes, descriptor and static samplers, and every supported integer n exercise each power-of-two boundary.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

static const char hlsl[] = R"hlsl(
Texture2D<float> t : register(t0);
SamplerState s : register(s0);
RWStructuredBuffer<float4> o : register(u0);
cbuffer C : register(b0) { uint anisotropy; };
[numthreads(1, 1, 1)] void cs() {
  uint width, height;
  t.GetDimensions(width, height);
  float2 dx = float2(float(anisotropy) / width, 0), dy = float2(0, 1.0 / height);
  float2 vx = float2(1.0 / width, 0), vy = float2(0, float(anisotropy) / height);
  o[0] = float4(t.SampleGrad(s, float2(0.5, 0.5), dx, dy),
                t.SampleGrad(s, float2(0.5, 0.5), vx, vy),
                t.SampleGrad(s, float2(0.5, 0.5), 4 * dx, 4 * dy),
                t.SampleGrad(s, float2(0.5, 0.5), 4 * vx, 4 * vy));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile anisotropic sampling shader");
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!cs.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  constexpr UINT maximum = D3D12_MAX_MAXANISOTROPY, size = 4 * maximum, mips = std::bit_width(size);
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, mips, DXGI_FORMAT_R32_FLOAT,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[mips];
  UINT64 bytes;
  device->GetCopyableFootprints(&desc, 0, mips, 0, footprints, nullptr, nullptr, &bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  constexpr UINT cases = 2 * maximum;
  constexpr UINT64 result_bytes = cases * 4 * sizeof(float);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && output && readback, "buffers were not created"))
    return verdict();
  CHECK(forget(readback.Get()));
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT mip = 0; mip < mips; mip++)
    for (UINT y = 0; y < footprints[mip].Footprint.Height; y++)
      std::fill_n((float *)(mapped + footprints[mip].Offset + y * footprints[mip].Footprint.RowPitch),
                  footprints[mip].Footprint.Width, float(mip != 0));
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12DescriptorHeap> views, samplers;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  D3D12_DESCRIPTOR_HEAP_DESC samplers_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, maximum,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&samplers_desc, IID_PPV_ARGS(&samplers)));
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{desc.Format, D3D12_SRV_DIMENSION_TEXTURE2D,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv.Texture2D.MipLevels = mips;
  device->CreateShaderResourceView(texture.Get(), &srv, views->GetCPUDescriptorHandleForHeapStart());
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1}};
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                   {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                   {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].DescriptorTable = {1, &ranges[0]};
  params[2].Constants = {0, 0, 1};
  params[3].DescriptorTable = {1, &ranges[1]};
  auto dynamic_root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(dynamic_root), "descriptor root signature was not created"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{dynamic_root.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> dynamic_pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&dynamic_pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT mip = 0; mip < mips; mip++) {
    D3D12_TEXTURE_COPY_LOCATION dst{
        texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[mip]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  for (bool fixed : {false, true})
    for (UINT n = 1; n <= maximum; n++) {
      step("%s sampler, MaxAnisotropy %u", fixed ? "static" : "descriptor", n);
      D3D12_SAMPLER_DESC sampler{D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                  D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, n,
                                  D3D12_COMPARISON_FUNC_NEVER};
      sampler.MaxLOD = D3D12_FLOAT32_MAX;
      ComPtr<ID3D12RootSignature> root = dynamic_root;
      ComPtr<ID3D12PipelineState> pso = dynamic_pso;
      if (fixed) {
        D3D12_STATIC_SAMPLER_DESC stat{sampler.Filter, sampler.AddressU, sampler.AddressV, sampler.AddressW,
                                       sampler.MipLODBias, sampler.MaxAnisotropy, sampler.ComparisonFunc,
                                       D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, sampler.MinLOD, sampler.MaxLOD};
        root = root_signature(device.Get(), {UINT(std::size(params) - 1), params, 1, &stat});
        if (!expect(bool(root), "static root signature was not created"))
          return verdict();
        pso_desc.pRootSignature = root.Get();
        pso.Reset();
        CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
      } else {
        device->CreateSampler(
            &sampler, {samplers->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(n - 1) * increment}
        );
      }
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
      list->SetDescriptorHeaps(UINT(std::size(heaps)), heaps);
      list->SetComputeRootSignature(root.Get());
      list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
      const UINT index = UINT(fixed) * maximum + n - 1;
      list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + index * 4 * sizeof(float));
      list->SetComputeRoot32BitConstant(2, n, 0);
      if (!fixed)
        list->SetComputeRootDescriptorTable(
            3, {samplers->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(n - 1) * increment}
        );
      list->Dispatch(1, 1, 1);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
    }

  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), output.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  float *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  const float tolerance = std::ldexp(1.0f, -D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT);
  for (UINT index = 0; index < cases; index++) {
    step("read %s sampler, MaxAnisotropy %u", index < maximum ? "descriptor" : "static", index % maximum + 1);
    for (UINT c = 0; c < 4; c++) {
      const float want = float(c / 2);
      expect(std::fabs(got[index * 4 + c] - want) <= tolerance,
             "gradient %u returned %g, expected %g", c, got[index * 4 + c], want);
    }
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
