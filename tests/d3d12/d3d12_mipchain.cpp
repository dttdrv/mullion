// contract: a compute pass that reads one mip of a texture and writes the next mip of the same texture builds the
// whole chain, one dispatch a mip, in one command list. this is how an engine makes a depth pyramid for occlusion
// culling (Unreal's HZB, Unity's depth pyramid) and its bloom and exposure chains. Direct3D 12 keeps a state for
// each subresource ("Subresources can be in different states", Using Resource Barriers to Synchronize Resource
// States), so mip n is a shader resource while mip n + 1 is an unordered access target, and the next dispatch has
// them the other way round after a barrier of its own.
// the pyramid is of maxima: a texel of mip n + 1 is the largest of the four texels of mip n under it, those past the
// edge of an odd-sized mip being the edge's (the texture is 300 by 170, so most mips are odd one way or the other).
// each way a shader has of reading a mip gives the same pyramid, and it is the one the CPU makes from the same
// first mip:
// - Load with the mip in the location, through a view of all mips;
// - SampleLevel with a point sampler that clamps, at the texels' centres, through a view of all mips;
// - GatherRed at the corner the four texels share, through a view of the one mip (MostDetailedMip, MipLevels 1),
//   which returns exactly those four ("Gather ... returns the four texel values that would be used in a bi-linear
//   filtering operation", HLSL).
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
Texture2D<float> from : register(t0);
RWTexture2D<float> to : register(u0);
SamplerState point_clamp : register(s0);
cbuffer Mip : register(b0) { uint mip; uint2 size; };
[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  // the four texels of the source under the destination's texel, held to the source's edge
  uint2 first = min(id.xy * 2, size - 1), last = min(id.xy * 2 + 1, size - 1);
#if WAY == 0
  float4 four = float4(from.Load(int3(first, mip)), from.Load(int3(last.x, first.y, mip)), from.Load(int3(first.x, last.y, mip)),
                       from.Load(int3(last, mip)));
#elif WAY == 1
  float2 a = (first + 0.5) / size, b = (last + 0.5) / size;
  float4 four = float4(from.SampleLevel(point_clamp, a, mip), from.SampleLevel(point_clamp, float2(b.x, a.y), mip),
                       from.SampleLevel(point_clamp, float2(a.x, b.y), mip), from.SampleLevel(point_clamp, b, mip));
#else
  // the view has the one mip; past the edge the sampler's clamp gives the edge's texels
  float4 four = from.GatherRed(point_clamp, (id.xy * 2 + 1.0) / size);
#endif
  to[id.xy] = max(max(four.x, four.y), max(four.z, four.w));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT width = 300, height = 170, ways = 3;
  const DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  // every mip down to one texel
  UINT mips = 1;
  while ((std::max(width, height) >> mips) > 0)
    mips++;

  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1}};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[0].DescriptorTable = {1, &ranges[0]}, params[1].DescriptorTable = {1, &ranges[1]};
  params[2].Constants = {0, 0, 3};
  D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params, 1, &sampler});

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the first mip: values that have no order along a row or a column, so a maximum of the wrong four shows
  std::vector<std::vector<float>> reference(mips);
  reference[0].resize(width * height);
  for (UINT i = 0, seed = 1; i < width * height; i++)
    reference[0][i] = float((seed = seed * 1664525 + 1013904223) >> 8);
  for (UINT mip = 1; mip < mips; mip++) {
    const UINT w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u), sw = std::max(width >> (mip - 1), 1u),
               sh = std::max(height >> (mip - 1), 1u);
    reference[mip].resize(w * h);
    for (UINT y = 0; y < h; y++)
      for (UINT x = 0; x < w; x++) {
        auto at = [&](UINT sx, UINT sy) { return reference[mip - 1][std::min(sy, sh - 1) * sw + std::min(sx, sw - 1)]; };
        reference[mip][y * w + x] = std::max({at(2 * x, 2 * y), at(2 * x + 1, 2 * y), at(2 * x, 2 * y + 1), at(2 * x + 1, 2 * y + 1)});
      }
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, (UINT16)mips, format, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips);
  UINT64 bytes;
  device->GetCopyableFootprints(&desc, 0, mips, 0, footprints.data(), nullptr, nullptr, &bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  char *staged;
  CHECK(upload->Map(0, nullptr, (void **)&staged));
  for (UINT y = 0; y < height; y++)
    memcpy(staged + footprints[0].Offset + y * footprints[0].Footprint.RowPitch, &reference[0][y * width], width * sizeof(float));

  // a view of all mips, and of each mip alone as a source and as a target
  enum { All, Source, Target, Kinds };
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Kinds * mips, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = [&](UINT kind, UINT mip) {
    auto handle = views->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (kind * mips + mip) * increment;
    return handle;
  };
  auto gpu = [&](UINT kind, UINT mip) {
    auto handle = views->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (kind * mips + mip) * increment;
    return handle;
  };

  for (UINT way = 0; way < ways; way++) {
    step("the chain of %u mips of a %u by %u texture, the source read by %s", mips, width, height,
         way == 0 ? "Load" : way == 1 ? "SampleLevel" : "GatherRed through a view of one mip");
    auto cs = compiler.compile(hlsl, "cs", "cs", {"WAY=" + std::to_string(way)});
    if (cs.empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
    CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
    // a texture of its own each way: nothing of the way before is left in it
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
    for (UINT mip = 0; mip < mips; mip++) {
      D3D12_SHADER_RESOURCE_VIEW_DESC all{format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
      all.Texture2D = {0, mips};
      auto one = all;
      one.Texture2D = {mip, 1};
      D3D12_UNORDERED_ACCESS_VIEW_DESC target{format, D3D12_UAV_DIMENSION_TEXTURE2D};
      target.Texture2D = {mip};
      device->CreateShaderResourceView(texture.Get(), &all, cpu(All, mip));
      device->CreateShaderResourceView(texture.Get(), &one, cpu(Source, mip));
      device->CreateUnorderedAccessView(texture.Get(), nullptr, &target, cpu(Target, mip));
    }

    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    D3D12_TEXTURE_COPY_LOCATION first{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        staging{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[0]}};
    list->CopyTextureRegion(&first, 0, 0, 0, &staging, nullptr);
    // each mip on its own: the first a source, the others targets
    auto moves = [&](UINT mip, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
      D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
      barrier.Transition = {texture.Get(), mip, before, after};
      list->ResourceBarrier(1, &barrier);
    };
    moves(0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (UINT mip = 1; mip < mips; mip++)
      moves(mip, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    for (UINT mip = 1; mip < mips; mip++) {
      const UINT w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
      // what the shader calls the source's mip: its number in a view of all, 0 in a view of the one
      const UINT constants[3] = {way == 2 ? 0 : mip - 1, std::max(width >> (mip - 1), 1u), std::max(height >> (mip - 1), 1u)};
      list->SetComputeRootDescriptorTable(0, gpu(way == 2 ? Source : All, mip - 1));
      list->SetComputeRootDescriptorTable(1, gpu(Target, mip));
      list->SetComputeRoot32BitConstants(2, 3, constants, 0);
      list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
      moves(mip, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (UINT mip = 0; mip < mips; mip++) {
      moves(mip, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[mip]}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    const char *out;
    CHECK(readback->Map(0, nullptr, (void **)&out));
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
      unsigned wrong = 0;
      for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
          float got = ((const float *)(out + footprints[mip].Offset + y * footprints[mip].Footprint.RowPitch))[x];
          if (got != reference[mip][y * w + x] && wrong++ < 3)
            expect(false, "mip %u, texel %u,%u is %g, want %g", mip, x, y, got, reference[mip][y * w + x]);
        }
      expect(wrong <= 3, "mip %u: and %u more texels", mip, wrong - 3);
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
