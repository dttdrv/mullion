// contract: non-precise mad may round once or twice; precise DXIL FMad rounds the multiply and add separately.
// "Floating point multiply & add. This operation is not fused for "precise" operations." (DXIL.rst, FMad,
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst; Precise qualifier also covers /Gis).
// DXBC mad is "Component-wise multiply & add." (D3D11.3 22.10.9). "a multiply followed by an add, where either or
// both are flagged as PRECISE, cannot be merged into a fused mad." (22.19.2). precise DXBC mad may use either
// implementation, consistently across shaders (https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/mad).
// Metal fma: "Rounding of intermediate products shall not occur." (MSL 4.1, 6.6 table 6.4); contraction is allowed
// by default and -ffp-contract=off disables it (1.6.3,
// https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).
// p is binary32's precision, e = 2^(1-p), q = 2^(-p/2). (1+q)^2 = 1+2q+e/2: its product rounds down (even),
// then adding e/2 rounds down again; a fused operation gives 1+2q+e, one ULP higher. both signs, exact products,
// every vector width, local precise and /Gis run in pixel and compute shaders. a non-precise result accepts exactly
// those two rounding results, bit for bit. this contract also permits the unfused baseline;
// airconv -S exposes FMA emission.
#include "d3d12_test.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>

static const char hlsl[] = R"hlsl(
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
uint4 evaluate(uint id) {
  uint at = id * 3 * 4 * WORD;
  TYPE a = asfloat(src.Load4(at).CHANNELS);
  TYPE b = asfloat(src.Load4(at + 4 * WORD).CHANNELS);
  TYPE c = asfloat(src.Load4(at + 8 * WORD).CHANNELS);
#if PRECISE
  precise TYPE value;
#else
  TYPE value;
#endif
#if SEPARATE
  precise TYPE product = a * b;
  value = product + c;
#else
  value = mad(a, b, c);
#endif
  uint4 bits = 0;
  bits.CHANNELS = asuint(value);
  return bits;
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 ps(float4 pos : SV_Position) : SV_Target { return evaluate((uint)pos.x); }
[numthreads(GROUP, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  if (id.x < COUNT) dst.Store4(id.x * 4 * WORD, evaluate(id.x));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, count = 2 * group + 1;
  const char *channels[] = {"x", "xy", "xyz", "xyzw"};
  enum Mode { Fast, Precise, Global, Separate, Modes };
  const char *names[] = {"non-precise mad", "precise mad", "/Gis mad", "precise mul/add"};
  struct Shader {
    UINT width, mode;
    bool kernel;
    std::string code;
  };
  std::vector<Shader> shaders;
  std::string vertex;
  for (UINT width = 1; width <= std::size(channels); width++)
    for (UINT mode = 0; mode < Modes; mode++)
      for (bool kernel : {false, true}) {
        const char *entry = kernel ? "cs" : "ps";
        std::vector<std::string> defines = {
            "TYPE=float" + (width == 1 ? std::string() : std::to_string(width)),
            std::string("CHANNELS=") + channels[width - 1],
            "PRECISE=" + std::to_string(mode == Precise || mode == Separate),
            "SEPARATE=" + std::to_string(mode == Separate), "WORD=" + std::to_string(sizeof(UINT)),
            "GROUP=" + std::to_string(group), "COUNT=" + std::to_string(count)};
        step("compile %s width=%u %s", names[mode], width, entry);
        std::string code;
        if (mode == Global && !compiler.dxc) {
          std::vector<std::string> keys, values;
          for (auto &d : defines) {
            keys.push_back(d.substr(0, d.find('=')));
            values.push_back(d.substr(d.find('=') + 1));
          }
          std::vector<D3D_SHADER_MACRO> macros;
          for (UINT i = 0; i < keys.size(); i++) macros.push_back({keys[i].c_str(), values[i].c_str()});
          macros.push_back({});
          ComPtr<ID3DBlob> blob, errors;
          HRESULT hr = D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, macros.data(), nullptr, entry,
                                  kernel ? "cs_5_1" : "ps_5_1", D3DCOMPILE_IEEE_STRICTNESS, 0, &blob, &errors);
          if (FAILED(hr) && errors)
            printf("%s: %.*s\n", entry, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
          CHECK(hr);
          code = kept(std::string((const char *)blob->GetBufferPointer(), blob->GetBufferSize()), entry);
        } else {
          code = compiler.compile(hlsl, entry, entry, defines, mode == Global ? std::vector<std::wstring>{L"-Gis"}
                                                                             : std::vector<std::wstring>{});
        }
        if (!expect(!code.empty(), "shader compiled")) return verdict();
        shaders.push_back({width, mode, kernel, std::move(code)});
        if (vertex.empty()) {
          vertex = compiler.compile(hlsl, "vs", "vs", defines);
          if (!expect(!vertex.empty(), "vertex shader compiled")) return verdict();
        }
      }

  const int p = std::numeric_limits<float>::digits;
  const float e = std::ldexp(1.0f, 1 - p), q = std::ldexp(1.0f, -p / 2);
  const std::array triples = {
      std::array{1 + q, 1 + q, e / 2}, std::array{-(1 + q), 1 + q, -e / 2},
      std::array{1.0f, 2.0f, 0.5f}, std::array{-2.0f, 0.5f, 2.0f},
      std::array{1 + q, 1 - q, 0.0f}, std::array{0.0f, 1.0f, 1.0f}};
  std::array<UINT, std::size(triples)> fused{}, serial{}, precise_reference{};
  std::array<bool, std::size(triples)> seen{};
  for (UINT i = 0; i < triples.size(); i++) {
    auto [a, b, c] = triples[i];
    volatile float product = a * b;
    serial[i] = std::bit_cast<UINT>(float(product + c));
    fused[i] = std::bit_cast<UINT>(std::fma(a, b, c));
  }
  if (!expect(fused[0] == serial[0] + 1 && fused[1] == serial[1] + 1 && fused[2] == serial[2],
              "rounding witnesses differ by one ULP and agree")) return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto rs = root_signature(device.Get(), {std::size(params), params});
  if (!expect(bool(rs), "root signature")) return verdict();
  using Input = std::array<std::array<float, 4>, 3>;
  std::vector<Input> input(count);
  for (UINT i = 0; i < count; i++)
    for (UINT lane = 0; lane < std::size(channels); lane++)
      for (UINT operand = 0; operand < std::tuple_size_v<Input>; operand++)
        input[i][operand][lane] = triples[(i + lane) % triples.size()][operand];
  const UINT64 input_bytes = input.size() * sizeof(Input);
  const UINT64 output_bytes = UINT64(count) * std::size(channels) * sizeof(UINT);
  auto src = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input_bytes + output_bytes,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
  auto dst = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!expect(src && dst, "buffers")) return verdict();
  void *mapped;
  CHECK(src->Map(0, nullptr, &mapped));
  memcpy(mapped, input.data(), input_bytes);
  memset((char *)mapped + input_bytes, 0, output_bytes);
  src->Unmap(0, nullptr);
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, count, 1, 1, 1,
                                 DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "readback")) return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  for (auto &shader : shaders) {
    step("%s width=%u %s", names[shader.mode], shader.width, shader.kernel ? "kernel" : "pixel");
    ComPtr<ID3D12PipelineState> pso;
    if (shader.kernel) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(shader.code)};
      CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    } else {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vertex), bytecode(shader.code)};
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = target_desc.Format;
      desc.SampleDesc = {1, 0};
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                    IID_PPV_ARGS(&list)));
    CHECK(forget(readback.Get()));
    if (shader.kernel) {
      transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(dst.Get(), 0, src.Get(), input_bytes, output_bytes);
      transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootShaderResourceView(0, src->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
      list->Dispatch((count + group - 1) / group, 1, 1);
      transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, dst.Get(), 0, output_bytes);
      transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootShaderResourceView(0, src->GetGPUVirtualAddress());
      list->SetGraphicsRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
      D3D12_VIEWPORT viewport{0, 0, float(count), 1, 0, 1};
      D3D12_RECT scissor{0, 0, LONG(count), 1};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      const float clear[] = {0, 0, 0, 0};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, 1, 0, 0);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      to.PlacedFootprint = footprint;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(readback->Map(0, nullptr, &mapped));
    const UINT *got = (const UINT *)((const char *)mapped + (shader.kernel ? 0 : footprint.Offset));
    for (UINT i = 0; i < count; i++)
      for (UINT lane = 0; lane < std::size(channels); lane++) {
        UINT value = got[i * std::size(channels) + lane], at = (i + lane) % triples.size();
        bool separate = shader.mode == Separate || (compiler.dxc && shader.mode != Fast);
        bool valid = lane >= shader.width ? value == 0 : value == serial[at] || (!separate && value == fused[at]);
        expect(valid, "i=%u lane=%u got=%08x serial=%08x fused=%08x", i, lane, value, serial[at], fused[at]);
        if (!compiler.dxc && lane < shader.width && (shader.mode == Precise || shader.mode == Global)) {
          if (seen[at]) expect(value == precise_reference[at], "DXBC precise mad is consistent, triple=%u", at);
          precise_reference[at] = value;
          seen[at] = true;
        }
      }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
