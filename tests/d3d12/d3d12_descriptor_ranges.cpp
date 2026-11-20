// contract: descriptor copies flatten each range list in order, independently of how either side is partitioned.
// CopyDescriptors: "Each entry in the array specifies the number of descriptors to copy in that range"; a null
// size array means one per range (Microsoft Learn). source heaps are CPU-only. copied RTV/DSV descriptors select
// the original slices; copied sampler descriptors retain arbitrary border colors, including their metadata.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptors
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html#copying-descriptors
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>

enum Kind { SRV, CBV, UAV, Sampler, RTV, DSV, Kinds };
static const char hlsl[] = R"hlsl(
cbuffer index_data : register(b0) { uint slot; };
RWStructuredBuffer<uint4> results : register(u0);
#if KIND == 0
Buffer<uint> source_data : register(t0);
#elif KIND == 1
cbuffer source_data : register(b1) { uint value; };
#elif KIND == 2
RWBuffer<uint> source_data : register(u1);
#else
Texture2D<float4> source_data : register(t0);
SamplerState copied_sampler : register(s0);
#endif
[numthreads(1, 1, 1)] void cs() {
#if KIND == 3
  results[slot] = asuint(source_data.SampleLevel(copied_sampler, float2(-2, -2), 0));
#elif KIND == 1
  results[slot] = uint4(value, value, value, value);
#else
  uint v = source_data[0];
  results[slot] = uint4(v, v, v, v);
#endif
}
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::string shaders[Sampler + 1];
  for (UINT kind = SRV; kind <= Sampler; kind++) {
    shaders[kind] = compiler.compile(hlsl, "cs", "cs", {"KIND=" + std::to_string(kind)});
    if (!expect(!shaders[kind].empty(), "descriptor shader kind %u did not compile", kind))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  using Words = std::array<UINT, 4>;
  const UINT size = 33, slots = size + 2, stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  const UINT64 data_bytes = UINT64(slots) * stride, result_bytes = slots * sizeof(Words);
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, data_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto writable = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, data_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(source && writable && output && back, "descriptor buffers"))
    return verdict();
  auto value = [](UINT descriptor) { return descriptor ^ (descriptor << 16) ^ ~0u; };
  UINT8 *bytes;
  CHECK(source->Map(0, nullptr, (void **)&bytes));
  memset(bytes, 0, data_bytes);
  for (UINT i = 0; i < slots; i++) {
    UINT word = value(i);
    memcpy(bytes + i * stride, &word, sizeof(word));
  }
  source->Unmap(0, nullptr);
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  constexpr UINT width = 3, height = 2;
  D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, UINT16(slots), 1,
                         DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> color, depth, sampled;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&color)));
  auto dd = td;
  dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        nullptr, IID_PPV_ARGS(&depth)));
  auto sd = td;
  sd.DepthOrArraySize = 1;
  sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&sampled)));
  ComPtr<ID3D12DescriptorHeap> sample_views, sample_rtv;
  D3D12_DESCRIPTOR_HEAP_DESC sv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  D3D12_DESCRIPTOR_HEAP_DESC rv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&sv, IID_PPV_ARGS(&sample_views)));
  CHECK(device->CreateDescriptorHeap(&rv, IID_PPV_ARGS(&sample_rtv)));
  device->CreateShaderResourceView(sampled.Get(), nullptr, sample_views->GetCPUDescriptorHandleForHeapStart());
  device->CreateRenderTargetView(sampled.Get(), nullptr, sample_rtv->GetCPUDescriptorHandleForHeapStart());
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  list->CopyBufferRegion(writable.Get(), 0, source.Get(), 0, data_bytes);
  transition(list.Get(), writable.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const float interior[4] = {1, 1, 1, 1};
  list->ClearRenderTargetView(sample_rtv->GetCPUDescriptorHandleForHeapStart(), interior, 0, nullptr);
  transition(list.Get(), sampled.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  auto border = [](UINT descriptor) {
    return std::array<float, 4>{float(descriptor) / 64, float(descriptor + 1) / 64,
                                 float(descriptor + 2) / 64, float(descriptor + 3) / 64};
  };
  for (UINT kind = SRV; kind < Kinds; kind++) {
    const auto type = kind <= UAV ? D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV :
                      kind == Sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER :
                      kind == RTV ? D3D12_DESCRIPTOR_HEAP_TYPE_RTV : D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    const UINT increment = device->GetDescriptorHandleIncrementSize(type);
    ComPtr<ID3D12DescriptorHeap> made, stage, copied;
    D3D12_DESCRIPTOR_HEAP_DESC hd{type, slots};
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&made)));
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&stage)));
    if (kind <= Sampler)
      hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&copied)));
    auto cpu = [&](ID3D12DescriptorHeap *heap, UINT index) {
      return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * increment};
    };
    for (UINT i = 0; i < slots; i++) {
      auto at = cpu(made.Get(), i);
      if (kind == SRV) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER,
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        view.Buffer = {UINT64(i) * stride / sizeof(UINT), 1};
        device->CreateShaderResourceView(source.Get(), &view, at);
      } else if (kind == CBV) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC view{source->GetGPUVirtualAddress() + UINT64(i) * stride, stride};
        device->CreateConstantBufferView(&view, at);
      } else if (kind == UAV) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER};
        view.Buffer = {UINT64(i) * stride / sizeof(UINT), 1};
        device->CreateUnorderedAccessView(writable.Get(), nullptr, &view, at);
      } else if (kind == Sampler) {
        D3D12_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                   D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        auto color = border(i);
        std::copy(color.begin(), color.end(), sampler.BorderColor);
        device->CreateSampler(&sampler, at);
      } else if (kind == RTV) {
        D3D12_RENDER_TARGET_VIEW_DESC view{td.Format, D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
        view.Texture2DArray = {0, i, 1, 0};
        device->CreateRenderTargetView(color.Get(), &view, at);
      } else {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{dd.Format, D3D12_DSV_DIMENSION_TEXTURE2DARRAY};
        view.Texture2DArray = {0, i, 1};
        device->CreateDepthStencilView(depth.Get(), &view, at);
      }
    }
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    if (kind <= Sampler) {
      D3D12_DESCRIPTOR_RANGE ranges[] = {{kind == CBV ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV :
                                          kind == UAV ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                          1, kind == CBV || kind == UAV ? 1u : 0u},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1}};
      D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                       {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                       {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
      params[0].DescriptorTable = {1, &ranges[0]};
      params[1].DescriptorTable = {1, &ranges[1]};
      params[3].Constants.Num32BitValues = 1;
      root = root_signature(device.Get(), {UINT(std::size(params)), params});
      if (!expect(root != nullptr, "descriptor copy root signature kind %u", kind))
        return verdict();
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(shaders[kind])};
      CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    }
    const auto texture_desc = kind == DSV ? dd : td;
    const UINT subresources = slots * (kind == DSV ? 2 : 1);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    UINT64 texture_bytes;
    device->GetCopyableFootprints(&texture_desc, 0, subresources, 0, footprints.data(), nullptr, nullptr, &texture_bytes);
    auto texture_back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, texture_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(texture_back != nullptr, "texture copy result"))
      return verdict();
    for (UINT count : {1u, 2u, 3u, size - 2, size - 1, size})
      for (UINT mode = 0; mode < 7; mode++) {
        step("descriptor kind %u, count %u, range mode %u", kind, count, mode);
        // a CPU-to-CPU hop exercises both representations before the shader-visible copy.
        device->CopyDescriptorsSimple(slots, cpu(stage.Get(), 0), cpu(made.Get(), 0), type);
        for (UINT i = 0; i < slots; i++)
          device->CopyDescriptorsSimple(1, cpu(copied.Get(), i), cpu(made.Get(), 0), type);
        std::vector<UINT> expected(slots, 0);
        if (!mode) {
          device->CopyDescriptorsSimple(count, cpu(copied.Get(), 1), cpu(stage.Get(), 1), type);
        } else if (mode <= 4) {
          std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> from, into;
          std::vector<UINT> from_sizes, into_sizes;
          const bool source_null = mode == 2 || mode == 4, destination_null = mode == 3 || mode == 4;
          auto ranges = [&](bool singles, ID3D12DescriptorHeap *heap, auto &handles, auto &sizes, UINT split) {
            if (singles) {
              for (UINT i = 0; i < count; i++) {
                handles.push_back(cpu(heap, i + 1));
                sizes.push_back(1);
              }
            } else {
              handles = {cpu(heap, 1), cpu(heap, 1 + split), cpu(heap, 1 + split)};
              sizes = {split, 0, count - split};
            }
          };
          ranges(source_null, stage.Get(), from, from_sizes, count / 2);
          ranges(destination_null, copied.Get(), into, into_sizes, count / 2 + count % 2);
          device->CopyDescriptors(into.size(), into.data(), destination_null ? nullptr : into_sizes.data(),
                                  from.size(), from.data(), source_null ? nullptr : from_sizes.data(), type);
        } else {
          D3D12_CPU_DESCRIPTOR_HANDLE from = cpu(stage.Get(), 1), into = cpu(copied.Get(), 1);
          const UINT empty = 0;
          device->CopyDescriptors(mode == 5 ? 0 : 1, &into, &empty, mode == 6 ? 0 : 1, &from, &empty, type);
        }
        if (mode < 5)
          for (UINT i = 1; i <= count; i++)
            expected[i] = i;
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        if (kind <= Sampler) {
          ID3D12DescriptorHeap *heaps[] = {kind == Sampler ? sample_views.Get() : copied.Get(),
                                           kind == Sampler ? copied.Get() : nullptr};
          list->SetDescriptorHeaps(kind == Sampler ? 2 : 1, heaps);
          list->SetComputeRootSignature(root.Get());
          list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
          if (kind == Sampler)
            list->SetComputeRootDescriptorTable(0, sample_views->GetGPUDescriptorHandleForHeapStart());
          for (UINT i = 0; i < slots; i++) {
            list->SetComputeRootDescriptorTable(kind == Sampler ? 1 : 0,
                {copied->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * increment});
            list->SetComputeRoot32BitConstant(3, i, 0);
            list->Dispatch(1, 1, 1);
          }
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, result_bytes);
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else {
          for (UINT i = 0; i < slots; i++) {
            if (kind == RTV) {
              const float clear[4] = {-1};
              list->ClearRenderTargetView(cpu(made.Get(), i), clear, 0, nullptr);
            } else
              list->ClearDepthStencilView(cpu(made.Get(), i), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                           1, UINT8(~0u), 0, nullptr);
          }
          for (UINT i = 0; i < slots; i++) {
            float v = float(i + 1) / (slots + 1);
            if (kind == RTV) {
              const float clear[4] = {v};
              list->ClearRenderTargetView(cpu(copied.Get(), i), clear, 0, nullptr);
            } else
              list->ClearDepthStencilView(cpu(copied.Get(), i), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                           v, UINT8(i), 0, nullptr);
          }
          auto target = kind == RTV ? color.Get() : depth.Get();
          auto state = kind == RTV ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_DEPTH_WRITE;
          transition(list.Get(), target, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
          for (UINT i = 0; i < subresources; i++) {
            D3D12_TEXTURE_COPY_LOCATION src{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = i}};
            D3D12_TEXTURE_COPY_LOCATION dst{texture_back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                           {.PlacedFootprint = footprints[i]}};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
          }
          transition(list.Get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        }
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        if (kind <= Sampler) {
          Words *got;
          CHECK(back->Map(0, nullptr, (void **)&got));
          for (UINT i = 0; i < slots; i++) {
            Words want;
            if (kind == Sampler) {
              auto c = border(expected[i]);
              memcpy(want.data(), c.data(), sizeof(want));
            } else
              want.fill(value(expected[i]));
            expect(got[i] == want, "copied slot %u selected descriptor %u incorrectly", i, expected[i]);
          }
          back->Unmap(0, nullptr);
        } else {
          std::vector<INT> last(slots, -1);
          for (UINT i = 0; i < slots; i++)
            last[expected[i]] = i;
          UINT8 *got;
          CHECK(texture_back->Map(0, nullptr, (void **)&got));
          for (UINT sub = 0; sub < subresources; sub++)
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++) {
                const UINT slice = sub % slots;
                const auto &at = footprints[sub];
                if (kind == DSV && sub >= slots) {
                  UINT8 want = last[slice] < 0 ? UINT8(~0u) : UINT8(last[slice]);
                  expect(got[at.Offset + y * at.Footprint.RowPitch + x] == want,
                         "copied stencil subresource %u pixel %u,%u", sub, x, y);
                } else {
                  float have, want = last[slice] < 0 ? (kind == RTV ? -1.0f : 1.0f) :
                                                       float(last[slice] + 1) / (slots + 1);
                  memcpy(&have, got + at.Offset + y * at.Footprint.RowPitch + x * sizeof(have), sizeof(have));
                  expect(have == want, "copied subresource %u pixel %u,%u: %g, want %g", sub, x, y, have, want);
                }
              }
          texture_back->Unmap(0, nullptr);
        }
      }
  }
  return verdict();
}
