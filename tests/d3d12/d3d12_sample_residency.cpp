// contract: sampling reports whether the texels that contribute were mapped: "These 0 weight texels do not
// contribute to the filter result at all, so if they happen to fall on NULL tiles they don't count as an unmapped
// access" (D3D11.3 5.9.4.5.3). CheckAccessFullyMapped "returns true if all data being accessed was mapped in the
// resource; otherwise, CheckAccessFullyMapped returns false" (5.9.4.6); point sampling selects the nearest mip,
// "0.5 is added, and then the integer part of the LOD is taken as the mipmap level (clamped
// to available miplevels or any settings for clamping miplevels)" (7.18.10). SampleGrad and SampleLevel give the same
// texels and residency for isotropic footprints, including one larger than the texture with its last mip mapped.
// an anisotropic read may conservatively report unmapped. it must never report mapped for a NULL tile, even when
// its long axis suggests a mapped coarser mip (7.18.11: "LOD is based on area").
// the mips fill standard tiles: "Mipmaps that fill at least one standard tile in all dimensions are guaranteed
// to use the standard tiling" (D3D11.3 5.9.7.2).
// remapping precedes sampling: "tile mapping updates specified by this call must be completed before any
// subsequent D3D command can proceed" without NO_OVERWRITE (D3D11.3 5.9.3.1).
// the last phase leaves no tile mapped, and every read is of a NULL tile: "Reading from NULL-mapped tiles treat that
// sampled value as zero" (D3D12_TILED_RESOURCES_TIER_2).
// zero and nonzero texels share every tile; their values cannot stand in for its mapping status. R8G8B8A8_UNORM's
// n / (2^8 - 1) conversion is undone before comparing bytes (D3D11.3 3.2.3.5).
#include "d3d12_test.hpp"
#include <algorithm>
#include <d3d11.h>
#include <cmath>
#include <cstdint>

static const char hlsl[] = R"hlsl(
Texture2D<float4> tex : register(t0);
SamplerState s : register(s0);
SamplerComparisonState sc : register(s1);
#ifdef NO_STATUS
#define STATUS_ARGS
#define RESIDENCY 0
#else
#define STATUS_ARGS , status
#define RESIDENCY CheckAccessFullyMapped(status)
#endif
cbuffer Args : register(b0) { float2 extent; float lod; float least_lod; uint parallel; float shift; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint2 ps_grad(float4 pos : SV_Position) : SV_Target {
  float2 dx = float2(exp2(lod) / extent.x, 0), dy = float2(0, exp2(lod) / extent.y);
  if (parallel == 1) dx = dy = float2(1, 1);
  if (parallel == 2) dy /= MAX_ANISOTROPY;
  uint status;
  float2 uv = parallel == 2 ? float2(0.75, 0.25) : (pos.xy + shift) / extent;
  float4 v = tex.SampleGrad(s, uv, dx, dy, int2(0, 0), least_lod STATUS_ARGS);
  return uint2(round(v.x * TEXEL_MAX), RESIDENCY);
}
uint2 ps_level(float4 pos : SV_Position) : SV_Target {
  uint status;
  float4 v = tex.SampleLevel(s, (pos.xy + shift) / extent, max(lod, least_lod), int2(0, 0) STATUS_ARGS);
  return uint2(round(v.x * TEXEL_MAX), RESIDENCY);
}
uint2 ps_sample(float4 pos : SV_Position) : SV_Target {
  uint status;
  float4 v = tex.Sample(s, (pos.xy + shift) / extent, int2(0, 0), least_lod STATUS_ARGS);
  return uint2(round(v.x * TEXEL_MAX), RESIDENCY);
}
uint2 ps_bias(float4 pos : SV_Position) : SV_Target {
  uint status;
  float4 v = tex.SampleBias(s, (pos.xy + shift) / extent, lod, int2(0, 0), least_lod STATUS_ARGS);
  return uint2(round(v.x * TEXEL_MAX), RESIDENCY);
}
uint2 ps_cmp(float4 pos : SV_Position) : SV_Target {
  uint status;
  float v = tex.SampleCmp(sc, (pos.xy + shift) / extent, 0, int2(0, 0), least_lod STATUS_ARGS);
  return uint2(v, RESIDENCY);
}
uint2 ps_cmp_zero(float4 pos : SV_Position) : SV_Target {
  uint status;
  float v = tex.SampleCmpLevelZero(sc, (pos.xy + shift) / extent, 0, int2(0, 0) STATUS_ARGS);
  return uint2(v, RESIDENCY);
}
)hlsl";

static DWORD WINAPI
wait_for_main(void *thread) {
  if (WaitForSingleObject((HANDLE)thread, 5 * 60 * 1000) != WAIT_OBJECT_0) {
    printf("failed: process exceeded five minutes\n");
    ExitProcess(1);
  }
  CloseHandle((HANDLE)thread);
  return 0;
}

int
main(int argc, char **argv) {
  // a hung library call or destructor must end here, before the runner's ten-minute limit.
  HANDLE main_thread = OpenThread(SYNCHRONIZE, FALSE, GetCurrentThreadId());
  if (!main_thread)
    return 1;
  HANDLE watchdog = CreateThread(nullptr, 0, wait_for_main, main_thread, 0, nullptr);
  if (!watchdog) {
    CloseHandle(main_thread);
    return 1;
  }
  CloseHandle(watchdog);
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines{"TEXEL_MAX=" + std::to_string(UINT8_MAX),
                                         "MAX_ANISOTROPY=" + std::to_string(D3D11_REQ_MAXANISOTROPY)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines);
  const char *entries[] = {"ps_grad", "ps_level", "ps_sample", "ps_bias", "ps_cmp", "ps_cmp_zero"};
  std::string ps[2 * std::size(entries)];
  for (UINT way = 0; way < std::size(ps); way++) {
    auto options = defines;
    if (way >= std::size(entries))
      options.push_back("NO_STATUS=1");
    ps[way] = compiler.compile(hlsl, entries[way % std::size(entries)], "ps", options);
  }
  if (vs.empty() || std::any_of(std::begin(ps), std::end(ps), [](auto &p) { return p.empty(); })) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
    printf("skipped: tiled resources tier 2 unavailable\n");
    return 77;
  }

  auto finish = [&](ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list) {
    HRESULT hr = list->Close();
    ComPtr<ID3D12Fence> fence;
    if (FAILED(hr) || FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
      return hr;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      return HRESULT_FROM_WIN32(GetLastError());
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    if (SUCCEEDED(hr = queue->Signal(fence.Get(), 1)) && SUCCEEDED(hr = fence->SetEventOnCompletion(1, event))) {
      DWORD waited = WaitForSingleObject(event, 10000);
      hr = waited == WAIT_OBJECT_0 ? device->GetDeviceRemovedReason()
                                   : HRESULT_FROM_WIN32(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    CloseHandle(event);
    if (FAILED(hr)) {
      printf("failed: GPU submission %08lx (while: %s)\n", hr, trace::doing);
      ExitProcess(1);
    }
    return hr;
  };

  for (UINT mip_count = 1; mip_count <= 2; mip_count++)
    for (UINT usage = 0; usage < 4; usage++) {
      const bool comparison = usage == 2;
      const UINT tile_texels = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES / sizeof(uint8_t[4]);
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               0,
                               tile_texels,
                               tile_texels,
                               1,
                               1,
                               comparison ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM,
                               {1, 0},
                               D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                               comparison ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                               : usage    ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                          : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      ComPtr<ID3D12Resource> texture;
      CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
      D3D12_TILE_SHAPE shape{};
      device->GetResourceTiling(texture.Get(), nullptr, nullptr, &shape, nullptr, 0, nullptr);
      if (!expect(shape.WidthInTexels && shape.HeightInTexels, "empty tile shape"))
        return verdict();
      desc.Width = 2 * shape.WidthInTexels, desc.Height = 2 * shape.HeightInTexels, desc.MipLevels = mip_count;
      const bool sparse = usage != 3;
      if (sparse) {
        CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
      } else {
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
        CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&texture)));
      }
      const UINT width = desc.Width, height = desc.Height;
      UINT total = 0;
      D3D12_PACKED_MIP_INFO packed{};
      if (sparse)
        device->GetResourceTiling(texture.Get(), &total, &packed, nullptr, nullptr, 0, nullptr);
      const UINT tiles_wide = width / shape.WidthInTexels;
      const UINT first_mip_tiles = tiles_wide * (height / shape.HeightInTexels);
      if (sparse && !expect(packed.NumStandardMips == desc.MipLevels && total == first_mip_tiles + (mip_count > 1),
                            "the standard mips do not have their expected tiles"))
        return verdict();
      ComPtr<ID3D12Heap> heap;
      D3D12_HEAP_DESC heap_desc{UINT64(total) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, {D3D12_HEAP_TYPE_DEFAULT}};
      if (sparse) {
        CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
      }
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
      CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(
          device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      D3D12_TILED_RESOURCE_COORDINATE origin{};
      D3D12_TILE_REGION_SIZE all{total};
      if (sparse)
        queue->UpdateTileMappings(texture.Get(), 1, &origin, &all, heap.Get(), 1, nullptr, nullptr, nullptr,
                                  D3D12_TILE_MAPPING_FLAG_NONE);

      std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placed(mip_count);
      UINT64 bytes;
      device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, placed.data(), nullptr, nullptr, &bytes);
      auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      char *staged;
      CHECK(upload->Map(0, nullptr, (void **)&staged));
      for (UINT mip = 0; mip < desc.MipLevels; mip++) {
        for (UINT y = 0; y < height >> mip; y++)
          for (UINT x = 0; x < width >> mip; x++) {
            const uint8_t value[4] = {uint8_t(((x + y) & 1) ? mip + 1 : 0), 0, 0, 0};
            float depth = float(value[0]) / UINT8_MAX;
            memcpy(staged + placed[mip].Offset + y * placed[mip].Footprint.RowPitch + x * sizeof(value),
                   comparison ? (const void *)&depth : value, sizeof(value));
          }
        D3D12_TEXTURE_COPY_LOCATION to{
            texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}},
            from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed[mip]}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      upload->Unmap(0, nullptr);
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      CHECK(finish(queue.Get(), list.Get()));

      D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
          sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2};
      D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
      params[0].DescriptorTable = {1, &range};
      struct Args {
        float extent[2], lod, least_lod;
        UINT parallel;
        float shift = 0;
        float sampler_min = -D3D12_FLOAT32_MAX, sampler_max = D3D12_FLOAT32_MAX;
        D3D12_FILTER filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      };
      params[1].Constants = {0, 0, sizeof(Args) / sizeof(UINT)};
      params[2].DescriptorTable = {1, &sampler_range};
      D3D12_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
      sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS, sampler.MaxAnisotropy = 1;
      auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
      if (!expect(bool(rs), "root signature creation failed"))
        return verdict();
      ComPtr<ID3D12DescriptorHeap> views, rtvs, samplers;
      D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
          rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
      CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
      CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
      views_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, views_desc.NumDescriptors = sampler_range.NumDescriptors;
      CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&samplers)));
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = comparison ? DXGI_FORMAT_R32_FLOAT : desc.Format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = desc.MipLevels;
      device->CreateShaderResourceView(texture.Get(), &srv, views->GetCPUDescriptorHandleForHeapStart());
      desc.MipLevels = 1, desc.Format = DXGI_FORMAT_R32G32_UINT, desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> target;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            nullptr, IID_PPV_ARGS(&target)));
      auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT result;
      device->GetCopyableFootprints(&desc, 0, 1, 0, &result, nullptr, nullptr, &bytes);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      ComPtr<ID3D12PipelineState> pipelines[std::size(ps)];
      for (UINT way = 0; way < std::size(ps); way++) {
        if ((way % std::size(entries) >= 4) != comparison)
          continue;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{rs.Get(), bytecode(vs), bytecode(ps[way])};
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = ~0u;
        pipeline.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1, pipeline.RTVFormats[0] = desc.Format, pipeline.SampleDesc = {1, 0};
        CHECK(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipelines[way])));
      }
      const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
      const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
      const float large = std::log2(float(std::max(width, height)));
      constexpr unsigned lod_fraction_bits = 8; // D3D11_MIP_LOD_FRACTIONAL_BIT_COUNT, D3D11.3 7.18.16.1
      const Args cases[] = {{{float(width), float(height)}, 0, 0, 0},
                            {{float(width), float(height)}, -1, 0, 0},
                            {{float(width), float(height)}, 0.25f, 0, 0},
                            {{float(width), float(height)}, 0.75f, 0, 0},
                            {{float(width), float(height)}, 1, 0, 0},
                            {{float(width), float(height)}, large, 0, 0},
                            {{float(width), float(height)}, large, 0, 1},
                            {{float(width), float(height)}, 0, 1, 0},
                            {{float(width), float(height)}, 1.4f, 0, 0},
                            {{float(width), float(height)}, 1.6f, 0, 0},
                            {{float(width), float(height)}, 2, 0, 2},
                            {{float(width), float(height)}, large, 0, 2},
                            {{float(width), float(height)}, large, 0, 0, 0, 0, float(mip_count - 1)},
                            {{float(width), float(height)}, large, 0, 0, 0, 0,
                             float(mip_count - 1) - std::ldexp(1.0f, -int(lod_fraction_bits))},
                            {{float(width), float(height)}, large, 0, 0, 0, large, 0},
                            {{float(width), float(height)}, large, 0, 0, 0.25f, -D3D12_FLOAT32_MAX, D3D12_FLOAT32_MAX,
                             D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT},
                            {{float(width), float(height)}, large, 0, 0, 0.25f, -D3D12_FLOAT32_MAX, D3D12_FLOAT32_MAX,
                             D3D12_FILTER_MIN_MAG_MIP_LINEAR},
                            {{float(width), float(height)}, large, 0, 0, 0.25f, -D3D12_FLOAT32_MAX, D3D12_FLOAT32_MAX,
                             D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR},
                            {{float(width), float(height)}, large, 0, 0, 0.25f, -D3D12_FLOAT32_MAX, D3D12_FLOAT32_MAX,
                             D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT},
                            {{float(width), float(height)}, 1.4f, 0, 0, 0.25f, -D3D12_FLOAT32_MAX, D3D12_FLOAT32_MAX,
                             D3D12_FILTER_MIN_MAG_MIP_LINEAR},
                            {{float(width), float(height)}, 1.6f, 0, 0, 0.25f, -D3D12_FLOAT32_MAX, D3D12_FLOAT32_MAX,
                             D3D12_FILTER_MIN_MAG_MIP_LINEAR},
                            {{float(width), float(height)}, 2, 0, 0}};
      for (UINT phase = 0; phase < 3; phase++) {
        for (UINT tile = 0; tile < total; tile++) {
          const UINT mip = tile == first_mip_tiles, x = mip ? 0 : tile % tiles_wide, y = mip ? 0 : tile / tiles_wide;
          const bool mapped = phase < 2 && (mip ? !phase : ((x == y) != bool(phase)));
          D3D12_TILED_RESOURCE_COORDINATE coord{x, y, 0, mip};
          D3D12_TILE_RANGE_FLAGS flag = mapped ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
          queue->UpdateTileMappings(texture.Get(), 1, &coord, nullptr, heap.Get(), 1, &flag, &tile, nullptr,
                                    D3D12_TILE_MAPPING_FLAG_NONE);
        }
        for (UINT variant = 0; variant < std::size(cases); variant++)
          for (UINT way = 0; way < std::size(ps); way++) {
            const UINT form = way % std::size(entries);
            const bool feedback = way < std::size(entries);
            if ((form >= 4) != comparison || (cases[variant].parallel == 2 && form > 1) ||
                (mip_count == 1 && cases[variant].parallel == 2 && cases[variant].lod == large) ||
                (form == 5 && cases[variant].least_lod) || (mip_count == 1 && cases[variant].least_lod) ||
                (cases[variant].filter != D3D12_FILTER_MIN_MAG_MIP_POINT && comparison))
              continue;
            sampler.MipLODBias = form == 2 || form >= 4 ? cases[variant].lod : 0;
            sampler.MinLOD = cases[variant].sampler_min;
            sampler.MaxLOD = cases[variant].sampler_max;
            if (!phase) {
              if (sampler.MinLOD == -D3D12_FLOAT32_MAX)
                sampler.MinLOD = -2 * large;
              if (sampler.MaxLOD == D3D12_FLOAT32_MAX)
                sampler.MaxLOD = 2 * large;
            }
            sampler.MaxAnisotropy = cases[variant].parallel == 2 ? D3D11_REQ_MAXANISOTROPY : 1;
            sampler.Filter = comparison                     ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT
                             : cases[variant].parallel == 2 ? D3D12_FILTER_ANISOTROPIC
                                                            : cases[variant].filter;
            auto sampler_handle = samplers->GetCPUDescriptorHandleForHeapStart();
            device->CreateSampler(&sampler, sampler_handle);
            sampler_handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            device->CreateSampler(&sampler, sampler_handle);
            step("mips %u, usage %u, phase %u, variant %u, %s, status %u, lod %g, clamp %g, parallel %u", mip_count,
                 usage, phase, variant, entries[form], UINT(feedback), cases[variant].lod, cases[variant].least_lod,
                 cases[variant].parallel);
            CHECK(forget(readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pipelines[way].Get()));
            ID3D12DescriptorHeap *bound[] = {views.Get(), samplers.Get()};
            list->SetDescriptorHeaps(std::size(bound), bound);
            list->SetGraphicsRootSignature(rs.Get());
            list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
            list->SetGraphicsRootDescriptorTable(2, samplers->GetGPUDescriptorHandleForHeapStart());
            list->SetGraphicsRoot32BitConstants(1, sizeof(Args) / sizeof(UINT), &cases[variant], 0);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
            transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = result}};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            CHECK(finish(queue.Get(), list.Get()));
            const char *got;
            CHECK(readback->Map(0, nullptr, (void **)&got));
            const bool anisotropic = cases[variant].parallel == 2 && form != 1;
            const float lod = cases[variant].lod - (anisotropic ? std::log2(float(D3D11_REQ_MAXANISOTROPY)) : 0);
            const float clamped =
                std::max(cases[variant].least_lod, std::max(sampler.MinLOD, std::min(sampler.MaxLOD, lod)));
            const UINT mip = mip_count > 1 && clamped >= 0.5f;
            const bool linear = !anisotropic && D3D12_DECODE_MIN_FILTER(sampler.Filter) == D3D12_FILTER_TYPE_LINEAR;
            UINT wrong = 0;
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++) {
                bool mapped =
                    !sparse ||
                    (phase < 2 &&
                     (mip           ? !phase
                      : anisotropic ? bool(phase)
                                    : ((x / shape.WidthInTexels == y / shape.HeightInTexels) != bool(phase))));
                const UINT wanted = mapped && (((x >> mip) + (y >> mip)) & 1) ? comparison ? 1 : mip + 1 : 0;
                const UINT *value = (const UINT *)(got + result.Offset + y * result.Footprint.RowPitch) + 2 * x;
                // 7.18.16.3: filtering lies between the smallest and largest contributing texels. an estimate may
                // reject a mapped read, but may not accept an unmapped one, including this thin footprint in mip 0.
                // only a reserved texture a shader can write has its status from the library (Metal's is wrong
                // past the last mip). where the library cannot ask Metal again it says unmapped: an estimated
                // level near the last mip, a sampler that holds every read past the last mip, and one mip read
                // minified with a magnification filter that differs. these are gaps against 5.9.4.6
                const bool differs = D3D12_DECODE_MIN_FILTER(sampler.Filter) != D3D12_DECODE_MAG_FILTER(sampler.Filter);
                const bool conservative =
                    anisotropic ||
                    (usage == 1 &&
                     ((form != 1 &&
                       (mip_count == 1 || (clamped > float(mip_count - 1) && clamped < mip_count + 1) ||
                        sampler.MaxLOD <= float(mip_count - 1) || sampler.MinLOD >= float(mip_count - 1))) ||
                      sampler.MinLOD > float(mip_count - 1) || (mip_count == 1 && differs && clamped > 0)));
                float filtered = wanted;
                if (linear) {
                  mapped = true;
                  float u = (x + 0.5f + cases[variant].shift) / (1u << mip) - 0.5f;
                  float v = (y + 0.5f + cases[variant].shift) / (1u << mip) - 0.5f;
                  int left = std::floor(u), top = std::floor(v);
                  float fx = u - left, fy = v - top;
                  filtered = 0;
                  for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                      int tx = std::clamp(left + dx, 0, int((width >> mip) - 1));
                      int ty = std::clamp(top + dy, 0, int((height >> mip) - 1));
                      float weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                      bool resident = !sparse || (phase < 2 && (mip ? !phase :
                          ((tx / shape.WidthInTexels == ty / shape.HeightInTexels) != bool(phase))));
                      if (weight && !resident)
                        mapped = false;
                      filtered += (resident && ((tx + ty) & 1) ? mip + 1 : 0) * weight;
                    }
                }
                const bool texel = anisotropic ? value[0] <= (mapped ? mip + 1 : 0)
                                   : linear    ? value[0] >= std::floor(filtered) && value[0] <= std::ceil(filtered)
                                               : value[0] == wanted;
                const bool resident = !feedback      ? value[1] == 0
                                      : conservative ? value[1] <= UINT(mapped)
                                                     : value[1] == UINT(mapped);
                if ((!texel || !resident) && wrong++ < 3)
                  expect(false, "pixel %u,%u: value %u, residency %u; want %g, %u", x, y, value[0], value[1],
                         double(filtered), UINT(mapped));
              }
            expect(!wrong, "%u pixels differ", wrong);
            readback->Unmap(0, nullptr);
          }
      }
    }
  return verdict();
}
