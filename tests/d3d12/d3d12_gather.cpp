// contract: gather4 returns texels "in counter clockwise order starting with the sample to the lower left"
// (D3D11.3 22.4.2), selects r/g/b/a, and uses only the most detailed view mip. gather4_po honors
// "The 6 least significant bits of each offset value" as signed (22.4.4). float32 data, including denorms and
// signed zeros, is unaltered; NaNs remain NaNs. the resource clamp is relative to resource mip zero (5.8.3).
// in view space, 5.8.5 removes mip zero at clamp >= 1, or > 0 for one-mip views.
// gather4_c "returns each comparison result, rather than filtering them" and an unbound resource "returns 0"
// (22.4.3). a clamp-removed mip instead compares the OOB zero texel against the reference (5.8.6.1).
// float32 comparisons flush denorms and leave normals, infinities and NaNs unchanged (22.4.3, 3.1.3.2), and so does a
// point sample_c_lz, of the texel under the coordinate (22.4.20).
// CPU addressing checks all four components, channels, array rounding, mips, nulls and all comparison functions.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <climits>
#include <limits>

struct Case {
  float u, v, slice;
  UINT kind;
  int x, y;
  UINT face;
  float reference;
};
using Texel = std::array<float, 4>;
static_assert(sizeof(Case) == 8 * sizeof(UINT));
static const char hlsl[] = R"hlsl(
#if COMPARE
#define VALUE float
#define REF , c.reference
#else
#define VALUE float4
#define REF
#endif
Texture2D<VALUE> flat : register(t0);
Texture2DArray<VALUE> layered : register(t1);
TextureCube<VALUE> cubed : register(t2);
TextureCubeArray<VALUE> cubes : register(t3);
struct C { float u, v, slice; uint kind; int x, y; uint face; float reference; };
StructuredBuffer<C> cases : register(t4);
RWStructuredBuffer<float4> output : register(u0);
#if COMPARE
SamplerComparisonState clamped : register(s0);
#else
SamplerState clamped : register(s0);
#endif
cbuffer Limits : register(b0) { uint count; }
#define FLAT(M) ((c.x == 0 && c.y == 0) ? flat.M(clamped, uv REF, int2(0, 0)) : flat.M(clamped, uv REF, int2(c.x, c.y)))
#define LAYER(M) layered.M(clamped, float3(uv, c.slice) REF, int2(c.x, c.y))
#define GATHER(M) (c.kind == 0 ? FLAT(M) : c.kind == 1 ? LAYER(M) : c.kind == 2 ? cubed.M(clamped, direction REF) : cubes.M(clamped, float4(direction, c.slice) REF))
[numthreads(GROUP, 1, 1)] void cs(uint i : SV_DispatchThreadID) {
  if (i >= count) return;
  C c = cases[i];
  float2 uv = float2(c.u, c.v);
  float3 axes[] = {float3(1,0,0), float3(-1,0,0), float3(0,1,0), float3(0,-1,0), float3(0,0,1), float3(0,0,-1)};
  float3 direction = axes[c.face];
#if COMPARE
  output[i * CHANNELS + 0] = GATHER(GatherCmp);
  output[i * CHANNELS + 1] = GATHER(GatherCmpRed);
  // a point sample compares the texel under uv in the view's first mip
  output[i * CHANNELS + 2] = c.kind == 0   ? flat.SampleCmpLevelZero(clamped, uv, c.reference)
                             : c.kind == 1 ? layered.SampleCmpLevelZero(clamped, float3(uv, c.slice), c.reference)
                                           : 0;
#else
  output[i * CHANNELS + 0] = GATHER(GatherRed);
  output[i * CHANNELS + 1] = GATHER(GatherGreen);
  output[i * CHANNELS + 2] = GATHER(GatherBlue);
  output[i * CHANNELS + 3] = GATHER(GatherAlpha);
#endif
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: shader compiler unavailable\n");
    return 77;
  }
  const UINT size = 8, mips = 2, faces = 6, cube_count = 2, channels = Texel{}.size();
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  std::array<std::string, 2> shaders;
  for (UINT comparison = 0; comparison < shaders.size(); comparison++) {
    shaders[comparison] = compiler.compile(
        hlsl, "cs", "cs",
        {"GROUP=" + std::to_string(group), "CHANNELS=" + std::to_string(comparison ? 3 : channels),
         "COMPARE=" + std::to_string(comparison)}
    );
    if (!expect(!shaders[comparison].empty(), "gather HLSL, comparison=%u", comparison))
      return verdict();
  }
  std::vector<Case> cases;
  const float slices[] = {-1, 0, 0.5f, 1, 1.5f, 2, 2.5f, 3, 3.5f};
  // all six-bit offset encodings, with higher bits both clear and set, and the signed integer ends
  std::vector<int> offsets;
  for (int i = -64; i < 64; i++)
    offsets.push_back(i);
  offsets.insert(offsets.end(), {INT_MIN, INT_MAX});
  for (UINT kind = 0; kind < 4; kind++) {
    if (kind < 2) {
      for (int y = -1; y <= int(size); y++)
        for (int x = -1; x <= int(size); x++)
          for (int offset : offsets)
            for (UINT s = 0; s < (kind ? std::size(slices) : 1); s++)
              cases.push_back({(x + 0.75f) / size, (y + 0.75f) / size, slices[s], kind, offset, offset ^ 1, 0, 0});
    } else {
      for (UINT face = 0; face < faces; face++)
        for (UINT s = 0; s < (kind == 3 ? std::size(slices) : 1); s++)
          cases.push_back({0.5f, 0.5f, slices[s], kind, 0, 0, face, 0});
    }
  }
  // literal-zero and programmable-zero offsets are separate from the axis sweep
  cases.push_back({0.5f, 0.5f, 0, 0, 0, 0, 0, 0});
  const UINT reference_bits[] = {0,           0x80000000u, 1,           0x80000001u, 0x3f000000u, 0x3f800000u,
                                 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u};
  for (UINT i = 0; i < cases.size(); i++)
    cases[i].reference = i % (std::size(reference_bits) + 1) == std::size(reference_bits)
                             ? 1000.0f
                             : std::bit_cast<float>(reference_bits[i % (std::size(reference_bits) + 1)]);
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const D3D12_COMPARISON_FUNC functions[] = {D3D12_COMPARISON_FUNC_NEVER,         D3D12_COMPARISON_FUNC_LESS,
                                             D3D12_COMPARISON_FUNC_EQUAL,         D3D12_COMPARISON_FUNC_LESS_EQUAL,
                                             D3D12_COMPARISON_FUNC_GREATER,       D3D12_COMPARISON_FUNC_NOT_EQUAL,
                                             D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
  for (UINT variant = 0; variant <= std::size(functions); variant++) {
    const bool comparison = variant != 0;
    const auto function = comparison ? functions[variant - 1] : D3D12_COMPARISON_FUNC_NEVER;
    const UINT results_per_case = comparison ? 3 : channels;
    const UINT pixel_bytes = comparison ? sizeof(float) : sizeof(Texel);
    auto texel = [&](UINT slice, UINT mip, UINT x, UINT y, bool cube) {
      Texel value;
      for (UINT c = 0; c < channels; c++) {
        UINT key = comparison ? (cube ? slice : y * (size >> mip) + x)
                   : cube     ? slice * channels + c
                              : (y * (size >> mip) + x) * channels + c;
        const UINT specials[] = {0, 0x80000000u, 1, 0x80000001u, 0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u};
        value[c] = key < std::size(specials) ? std::bit_cast<float>(specials[key])
                                             : float((slice + 1) * 1000 + mip * 100 + y * (size >> mip) + x + c * 10);
      }
      return value;
    };
    const DXGI_FORMAT format = comparison ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    const UINT required =
        D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
        (comparison ? D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON
                    : 0);
    if (!expect((support.Support1 & required) == required, "required gather format support, format %u", format))
      return verdict();
    D3D12_ROOT_PARAMETER params[] = {
        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
        {D3D12_ROOT_PARAMETER_TYPE_SRV},
        {D3D12_ROOT_PARAMETER_TYPE_UAV},
        {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
    };
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4};
    params[0].DescriptorTable = {1, &range};
    params[1].Descriptor.ShaderRegister = 4;
    params[3].Constants.Num32BitValues = 1;
    D3D12_STATIC_SAMPLER_DESC sampler{
        D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP
    };
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc = function;
    if (comparison)
      sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    auto rs = root_signature(device.Get(), {std::size(params), params, 1, &sampler});
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(shaders[comparison])};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    std::array<ComPtr<ID3D12Resource>, 3> textures;
    std::array<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>, 3> footprints;
    std::array<ComPtr<ID3D12Resource>, 3> uploads;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    for (UINT resource = 0; resource < textures.size(); resource++) {
      UINT layers = resource ? cube_count * faces : 1;
      D3D12_RESOURCE_DESC texture_desc{
          D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, UINT16(layers), UINT16(mips), format, {1, 0},
          D3D12_TEXTURE_LAYOUT_UNKNOWN
      };
      CHECK(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&textures[resource])
      ));
      footprints[resource].resize(layers * mips);
      UINT64 bytes;
      device->GetCopyableFootprints(
          &texture_desc, 0, layers * mips, 0, footprints[resource].data(), nullptr, nullptr, &bytes
      );
      uploads[resource] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!expect(bool(uploads[resource]), "texture upload %u", resource))
        return verdict();
      char *mapped;
      CHECK(uploads[resource]->Map(0, nullptr, (void **)&mapped));
      for (UINT slice = 0; slice < layers; slice++)
        for (UINT mip = 0; mip < mips; mip++) {
          auto &fp = footprints[resource][slice * mips + mip];
          for (UINT y = 0; y < fp.Footprint.Height; y++)
            for (UINT x = 0; x < fp.Footprint.Width; x++) {
              auto value = texel(slice, mip, x, y, resource == 2);
              memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch + x * pixel_bytes, value.data(), pixel_bytes);
            }
        }
      uploads[resource]->Unmap(0, nullptr);
    }
    const UINT64 bytes = cases.size() * results_per_case * sizeof(Texel);
    auto inputs =
        buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cases.size() * sizeof(Case), D3D12_RESOURCE_STATE_GENERIC_READ);
    auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(inputs && initial && output && readback, "gather buffers"))
      return verdict();
    void *mapped;
    CHECK(inputs->Map(0, nullptr, &mapped));
    memcpy(mapped, cases.data(), cases.size() * sizeof(Case));
    inputs->Unmap(0, nullptr);
    CHECK(initial->Map(0, nullptr, &mapped));
    memset(mapped, 0xcd, bytes);
    initial->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap)));
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
    ComPtr<ID3D12CommandSignature> signature;
    CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
    auto indirect = buffer(
        device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ
    );
    if (!expect(bool(indirect), "indirect gather dispatch"))
      return verdict();
    CHECK(indirect->Map(0, nullptr, &mapped));
    const D3D12_DISPATCH_ARGUMENTS dispatch{UINT((cases.size() + group - 1) / group), 1, 1};
    memcpy(mapped, &dispatch, sizeof(dispatch));
    indirect->Unmap(0, nullptr);
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    for (UINT r = 0; r < textures.size(); r++) {
      for (UINT sub = 0; sub < footprints[r].size(); sub++) {
        D3D12_TEXTURE_COPY_LOCATION from{
            uploads[r].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[r][sub]}
        },
            to{textures[r].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      transition(
          list.Get(), textures[r].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
      );
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    for (UINT view = 0; view < mips; view++)
      for (bool null : {false, true})
        for (float clamp : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f})
          for (UINT path = 0; path < 2; path++) {
            step(
                "gather comparison %u: mip %u, clamp %g, null %d, %s", comparison ? function : 0, view, clamp, null,
                path ? "indirect" : "direct"
            );
            for (UINT kind = 0; kind < 4; kind++) {
              D3D12_SHADER_RESOURCE_VIEW_DESC srv{
                  format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
              };
              if (kind == 0)
                srv.Texture2D = {view, mips - view, 0, clamp};
              if (kind == 1)
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                srv.Texture2DArray = {view, mips - view, 3, 3, 0, clamp};
              if (kind == 2)
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, srv.TextureCube = {view, mips - view, clamp};
              if (kind == 3)
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY,
                srv.TextureCubeArray = {view, mips - view, view * faces, cube_count - view, clamp};
              auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
              handle.ptr += kind * device->GetDescriptorHandleIncrementSize(heap_desc.Type);
              device->CreateShaderResourceView(null ? nullptr : textures[kind < 2 ? kind : 2].Get(), &srv, handle);
            }
            CHECK(forget(readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pso.Get()));
            list->CopyResource(output.Get(), initial.Get());
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(rs.Get());
            list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootShaderResourceView(1, inputs->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstant(3, cases.size(), 0);
            if (path)
              list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
            else
              list->Dispatch(dispatch.ThreadGroupCountX, dispatch.ThreadGroupCountY, dispatch.ThreadGroupCountZ);
            transition(
                list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            list->CopyResource(readback.Get(), output.Get());
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            Texel *got;
            CHECK(readback->Map(0, nullptr, (void **)&got));
            for (UINT i = 0; i < cases.size(); i++) {
              auto c = cases[i];
              auto rounded = [](float v) {
                int lo = int(std::floor(v));
                return lo + (v - lo > 0.5f || (v - lo == 0.5f && (lo & 1)));
              };
              UINT slice = c.kind == 1   ? 3 + std::clamp(rounded(c.slice), 0, 2)
                           : c.kind == 2 ? c.face
                           : c.kind == 3
                               ? (view + std::clamp(rounded(c.slice), 0, int(cube_count - view - 1))) * faces + c.face
                               : 0;
              int n = size >> view;
              auto offset = [](int v) {
                UINT bits = UINT(v) & 63;
                return bits & 32 ? int(bits) - 64 : int(bits);
              };
              int x = c.kind < 2 ? int(std::floor(c.u * n - 0.5f)) + offset(c.x) : n / 2 - 1;
              int y = c.kind < 2 ? int(std::floor(c.v * n - 0.5f)) + offset(c.y) : n / 2 - 1;
              const int dx[] = {0, 1, 1, 0}, dy[] = {1, 1, 0, 0};
              for (UINT channel = 0; channel < results_per_case; channel++)
                for (UINT k = 0; k < channels; k++) {
                  float relative_clamp = std::max(0.0f, clamp - view);
                  bool removed = null || relative_clamp >= 1 || (mips - view == 1 && relative_clamp > 0);
                  // a clamped sample reads a later mip, which d3d12_sampling checks; this one has no cube texel
                  const bool point = comparison && channel == 2;
                  if (point && !null && (relative_clamp > 0 || c.kind >= 2))
                    continue;
                  int px = point ? int(std::floor(c.u * n)) : x + dx[k], py = point ? int(std::floor(c.v * n)) : y + dy[k];
                  float want = removed ? 0
                                       : texel(
                                             slice, view, std::clamp(px, 0, n - 1), std::clamp(py, 0, n - 1),
                                             c.kind >= 2
                                         )[comparison ? 0 : channel];
                  if (comparison && !null) {
                    auto flush = [](float v) {
                      return std::fpclassify(v) == FP_SUBNORMAL ? std::copysign(0.0f, v) : v;
                    };
                    const float reference = flush(c.reference), fetched = flush(want);
                    switch (function) {
                    case D3D12_COMPARISON_FUNC_NEVER:
                      want = 0;
                      break;
                    case D3D12_COMPARISON_FUNC_LESS:
                      want = reference < fetched;
                      break;
                    case D3D12_COMPARISON_FUNC_EQUAL:
                      want = reference == fetched;
                      break;
                    case D3D12_COMPARISON_FUNC_LESS_EQUAL:
                      want = reference <= fetched;
                      break;
                    case D3D12_COMPARISON_FUNC_GREATER:
                      want = reference > fetched;
                      break;
                    case D3D12_COMPARISON_FUNC_NOT_EQUAL:
                      want = reference != fetched;
                      break;
                    case D3D12_COMPARISON_FUNC_GREATER_EQUAL:
                      want = reference >= fetched;
                      break;
                    case D3D12_COMPARISON_FUNC_ALWAYS:
                      want = 1;
                      break;
                    }
                  }
                  float value = got[i * results_per_case + channel][k];
                  if (!expect(
                          std::isnan(want) ? std::isnan(value)
                                           : std::bit_cast<UINT>(value) == std::bit_cast<UINT>(want),
                          "case %u kind %u channel %u component %u: %#x, want %#x", i, c.kind, channel, k,
                          std::bit_cast<UINT>(value), std::bit_cast<UINT>(want)
                      )) {
                    readback->Unmap(0, nullptr);
                    return verdict();
                  }
                }
            }
            readback->Unmap(0, nullptr);
          }
  }
  return verdict();
}
