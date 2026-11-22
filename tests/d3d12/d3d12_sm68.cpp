// contract: shader model 6.8 (https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_8.html).
// - wave size ranges: [WaveSize(min, max[, preferred])], "the shader is compiled for and will only run with a wave
//   size in this range"; a pipeline of a range without the device's size is refused, as a single [WaveSize] outside
//   WaveLaneCountMin..Max is (HLSL_SM_6_6_WaveSize, HLSL_SM_6_8_WaveSizeRange), and one with it runs at a size in it.
// - expanded comparison sampling (HLSL_SM_6_8_SampleCmpGradBias): SampleCmpBias and SampleCmpGrad compare at the LOD
//   SampleBias and SampleGrad choose; D3D12_FEATURE_DATA_D3D12_OPTIONS21::SampleCmpGradientAndBiasSupported.
// - extended command information (HLSL_SM_6_8_Extended_Command_Info): SV_StartVertexLocation is DrawInstanced's
//   StartVertexLocation or DrawIndexedInstanced's BaseVertexLocation, SV_StartInstanceLocation the
//   StartInstanceLocation, directly and through ExecuteIndirect; ExtendedCommandInfoSupported.
// - the barrier of separate memory and semantics, Barrier(MemoryTypeFlags, SemanticFlags) (Work Graphs, "Barrier"),
//   in a compute shader: group-shared memory and UAVs written before it are seen after it by the group.
// the device reports 6.8 (D3D12_FEATURE_SHADER_MODEL) when, and only when, every one of these works, and an OPTIONS21
// flag with its feature; what it does not report is still run, to say what is missing.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>

namespace sdk {
constexpr auto D3D_SHADER_MODEL_6_8 = D3D_SHADER_MODEL(0x68);
constexpr auto D3D12_FEATURE_D3D12_OPTIONS21 = D3D12_FEATURE(53);
struct D3D12_FEATURE_DATA_D3D12_OPTIONS21 {
  UINT WorkGraphsTier, ExecuteIndirectTier;
  BOOL SampleCmpGradientAndBiasSupported, ExtendedCommandInfoSupported;
};
} // namespace sdk

static const char wave_hlsl[] = R"hlsl(
RWStructuredBuffer<uint> lanes : register(u0);
[WaveSize(RANGE)] [numthreads(128, 1, 1)]
void cs(uint t : SV_GroupIndex) { lanes[t] = WaveGetLaneCount(); }
)hlsl";

static const char compare_hlsl[] = R"hlsl(
Texture2D<float> depth : register(t0);
SamplerComparisonState less : register(s0);
cbuffer C : register(b0) { float bias; float lod; float reference_low; float reference_high; };
struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Out vs(uint id : SV_VertexID) {
  Out o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
// the comparisons of SampleCmpBias, and of SampleCmpGrad with gradients of the LOD asked for, at two references
float4 ps(Out i) : SV_Target {
  float2 g = float2(exp2(lod) / SIDE, 0);
  return float4(depth.SampleCmpBias(less, i.uv, reference_low, bias), depth.SampleCmpBias(less, i.uv, reference_high, bias),
                depth.SampleCmpGrad(less, i.uv, reference_low, g, g.yx), depth.SampleCmpGrad(less, i.uv, reference_high, g, g.yx));
}
)hlsl";

static const char start_hlsl[] = R"hlsl(
struct Out { float4 position : SV_Position; nointerpolation uint2 start : START; };
Out vs(uint id : SV_VertexID, uint instance : SV_InstanceID, uint start_vertex : SV_StartVertexLocation,
       uint start_instance : SV_StartInstanceLocation) {
  Out o;
  uint k = id - start_vertex;
  float2 uv = float2((k << 1) & 2, k & 2);
  o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.start = uint2(start_vertex, start_instance);
  return o;
}
uint2 ps(Out i) : SV_Target { return i.start; }
)hlsl";

static const char barrier_hlsl[] = R"hlsl(
RWStructuredBuffer<uint> data : register(u0);
groupshared uint shared_values[64];
[numthreads(64, 1, 1)]
void cs(uint t : SV_GroupIndex, uint3 g : SV_GroupID) {
  shared_values[t] = t * 3 + 1;
  data[g.x * 64 + t] = t * 5 + 2;
  Barrier(GROUP_SHARED_MEMORY | UAV_MEMORY, GROUP_SYNC | GROUP_SCOPE);
  uint other = (t + 17) % 64;
  uint a = shared_values[other], b = data[g.x * 64 + other];
  Barrier(UAV_MEMORY, GROUP_SYNC | DEVICE_SCOPE);
  data[g.x * 64 + t] = a + (b << 16);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: shader model 6.8 needs DXC\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_SHADER_MODEL model{sdk::D3D_SHADER_MODEL_6_8};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
  sdk::D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
  CHECK(device->CheckFeatureSupport(sdk::D3D12_FEATURE_D3D12_OPTIONS21, &options21, sizeof(options21)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  printf("reported: shader model %x, SampleCmpGradientAndBias %d, ExtendedCommandInfo %d, wave lanes %u..%u\n",
         model.HighestShaderModel, options21.SampleCmpGradientAndBiasSupported, options21.ExtendedCommandInfoSupported,
         options1.WaveLaneCountMin, options1.WaveLaneCountMax);
  bool reports_68 = model.HighestShaderModel >= sdk::D3D_SHADER_MODEL_6_8;
  // what works, feature by feature
  bool wave_ok = true, compare_ok = true, start_ok = true, barrier_ok = true;

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  auto run = [&]() -> HRESULT {
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (SUCCEEDED(hr) && SUCCEEDED(hr = allocator->Reset()))
      hr = list->Reset(allocator.Get(), nullptr);
    return hr;
  };
  auto readback_of = [&](ID3D12Resource *from, UINT64 bytes) {
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    forget(readback.Get());
    list->CopyBufferRegion(readback.Get(), 0, from, 0, bytes);
    return readback;
  };

  // wave size ranges
  {
    D3D12_ROOT_PARAMETER uav{D3D12_ROOT_PARAMETER_TYPE_UAV};
    auto rs = root_signature(device.Get(), {1, &uav});
    struct {
      UINT min, max, preferred;
    } ranges[] = {{4, 128, 0}, {16, 64, 32}, {32, 32, 0}, {64, 128, 0}, {4, 16, 0}, {8, 128, 64}, {64, 64, 0}};
    for (auto &r : ranges) {
      std::string range = std::to_string(r.min) + "," + std::to_string(r.max) + (r.preferred ? "," + std::to_string(r.preferred) : "");
      if (r.min == r.max)
        range = std::to_string(r.min);
      step("a compute shader of [WaveSize(%s)]", range.c_str());
      auto cs = compiler.compile(wave_hlsl, "cs", "cs_6_8", {"RANGE=" + range});
      if (!expect(!cs.empty(), "[WaveSize(%s)] does not compile", range.c_str())) {
        wave_ok = false;
        continue;
      }
      bool fits = false;
      for (UINT size = options1.WaveLaneCountMin; size && size <= options1.WaveLaneCountMax; size *= 2)
        fits |= size >= r.min && size <= r.max;
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
      ComPtr<ID3D12PipelineState> pso;
      HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
      if (!expect(SUCCEEDED(hr) == fits, "[WaveSize(%s)] on waves of %u..%u: %08lx", range.c_str(),
                  options1.WaveLaneCountMin, options1.WaveLaneCountMax, hr))
        wave_ok = false;
      if (FAILED(hr))
        continue;
      auto lanes = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 128 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      list->SetComputeRootSignature(rs.Get());
      list->SetPipelineState(pso.Get());
      list->SetComputeRootUnorderedAccessView(0, lanes->GetGPUVirtualAddress());
      list->Dispatch(1, 1, 1);
      transition(list.Get(), lanes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      auto readback = readback_of(lanes.Get(), 128 * 4);
      CHECK(run());
      const UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      if (!expect(got[0] >= r.min && got[0] <= r.max && std::all_of(got, got + 128, [&](UINT v) { return v == got[0]; }),
                  "[WaveSize(%s)] runs on waves of %u", range.c_str(), got[0]))
        wave_ok = false;
      readback->Unmap(0, nullptr);
    }
  }

  // expanded comparison sampling: a depth texture whose mip m holds m / 16, sampled over 8x8 pixels at LOD 5
  const UINT side = 256, mips = 9, target_side = 8;
  {
    step("SampleCmpBias and SampleCmpGrad");
    auto vs = compiler.compile(compare_hlsl, "vs", "vs_6_8", {"SIDE=" + std::to_string(side)}),
         ps = compiler.compile(compare_hlsl, "ps", "ps_6_8", {"SIDE=" + std::to_string(side)});
    if (!expect(!vs.empty() && !ps.empty(), "the comparison shaders do not compile"))
      return verdict();
    D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].DescriptorTable = {1, &srv};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 4};
    D3D12_STATIC_SAMPLER_DESC less{D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1,
                                   D3D12_COMPARISON_FUNC_LESS, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0, D3D12_FLOAT32_MAX,
                                   0, 0, D3D12_SHADER_VISIBILITY_PIXEL};
    auto rs = root_signature(device.Get(), {2, params, 1, &less});
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
    graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.SampleMask = ~0u;
    graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets = 1;
    graphics.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    graphics.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = rs ? device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pso)) : E_FAIL;
    if (!expect(SUCCEEDED(hr), "the comparison pipeline: %08lx", hr))
      compare_ok = false;
    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, mips, DXGI_FORMAT_R32_TYPELESS,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, target_side, target_side, 1, 1,
                                    DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> depth, target;
    CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
    CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    ComPtr<ID3D12DescriptorHeap> dsvs, rtvs, views;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, mips}, rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
        view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
    const UINT dsv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT m = 0; m < mips; m++) {
      D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
      dsv.Texture2D.MipSlice = m;
      auto at = dsvs->GetCPUDescriptorHandleForHeapStart();
      at.ptr += m * dsv_step;
      device->CreateDepthStencilView(depth.Get(), &dsv, at);
      list->ClearDepthStencilView(at, D3D12_CLEAR_FLAG_DEPTH, m / 16.0f, 0, 0, nullptr);
    }
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv_desc.Texture2D.MipLevels = mips;
    device->CreateShaderResourceView(depth.Get(), &srv_desc, views->GetCPUDescriptorHandleForHeapStart());
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 readback_size;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
    // the bias SampleCmpBias adds to LOD 5, and the LOD SampleCmpGrad's gradients have: the mip read is the one
    // between the references a mip apart
    const struct {
      float bias, lod;
    } cases[] = {{1, 2}, {-2, 6}, {2.4f, 0}};
    for (auto &c : cases) {
      if (!pso)
        break;
      float wanted_bias_mip = std::round(5 + c.bias), wanted_grad_mip = std::round(c.lod);
      for (bool grad : {false, true}) {
        float mip = grad ? wanted_grad_mip : wanted_bias_mip;
        float constants[4] = {c.bias, c.lod, (mip - 0.5f) / 16, (mip + 0.5f) / 16};
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST);
        forget(readback.Get());
        ID3D12DescriptorHeap *heaps[] = {views.Get()};
        const D3D12_VIEWPORT viewport{0, 0, float(target_side), float(target_side), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(target_side), LONG(target_side)};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetPipelineState(pso.Get());
        list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        hr = run();
        if (!expect(SUCCEEDED(hr), "the comparison draw: %08lx", hr)) {
          compare_ok = false;
          break;
        }
        const char *out;
        CHECK(readback->Map(0, nullptr, (void **)&out));
        auto got = reinterpret_cast<const float *>(out + footprint.Offset) + (grad ? 2 : 0);
        // LESS passes where the reference is below the texel: the low reference passes at mip `mip`, the high one not
        if (!expect(got[0] == 1 && got[1] == 0, "%s with bias %g, gradients of LOD %g: %g %g, want mip %g read", grad ? "SampleCmpGrad" : "SampleCmpBias",
                    c.bias, c.lod, got[0], got[1], mip))
          compare_ok = false;
        readback->Unmap(0, nullptr);
      }
    }
  }

  // extended command information
  {
    step("SV_StartVertexLocation and SV_StartInstanceLocation");
    auto vs = compiler.compile(start_hlsl, "vs", "vs_6_8"), ps = compiler.compile(start_hlsl, "ps", "ps_6_8");
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12RootSignature> rs = root_signature(device.Get(), {0, nullptr});
    if (!expect(!vs.empty() && !ps.empty(), "the start location shaders do not compile")) {
      start_ok = false;
    } else {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
      graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      graphics.SampleMask = ~0u;
      graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      graphics.NumRenderTargets = 1;
      graphics.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
      graphics.SampleDesc = {1, 0};
      HRESULT hr = device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pso));
      if (!expect(SUCCEEDED(hr), "the start location pipeline: %08lx", hr))
        start_ok = false;
    }
    if (pso) {
      D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, target_side, target_side, 1, 1,
                                      DXGI_FORMAT_R32G32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      ComPtr<ID3D12Resource> target;
      CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
      ComPtr<ID3D12DescriptorHeap> rtvs;
      D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
      CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
      auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);
      const uint16_t indices[3] = {0, 1, 2};
      auto index_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(indices), D3D12_RESOURCE_STATE_GENERIC_READ);
      void *mapped;
      CHECK(index_buffer->Map(0, nullptr, &mapped));
      memcpy(mapped, indices, sizeof(indices));
      index_buffer->Unmap(0, nullptr);
      D3D12_INDEX_BUFFER_VIEW ibv{index_buffer->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
      // the indirect draws' arguments
      D3D12_DRAW_ARGUMENTS draw_args{3, 1, 21, 23};
      D3D12_DRAW_INDEXED_ARGUMENTS indexed_args{3, 1, 0, 25, 27};
      auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 64, D3D12_RESOURCE_STATE_GENERIC_READ);
      CHECK(arguments->Map(0, nullptr, &mapped));
      memcpy(mapped, &draw_args, sizeof(draw_args));
      memcpy((char *)mapped + 32, &indexed_args, sizeof(indexed_args));
      arguments->Unmap(0, nullptr);
      D3D12_INDIRECT_ARGUMENT_DESC draw_kind{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
          indexed_kind{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
      D3D12_COMMAND_SIGNATURE_DESC draw_signature{sizeof(draw_args), 1, &draw_kind},
          indexed_signature{sizeof(indexed_args), 1, &indexed_kind};
      ComPtr<ID3D12CommandSignature> draws, indexed;
      CHECK(device->CreateCommandSignature(&draw_signature, nullptr, IID_PPV_ARGS(&draws)));
      CHECK(device->CreateCommandSignature(&indexed_signature, nullptr, IID_PPV_ARGS(&indexed)));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 readback_size;
      device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
      const struct {
        const char *what;
        UINT start_vertex, start_instance;
      } draws_made[] = {{"DrawInstanced", 5, 7}, {"DrawIndexedInstanced", 9, 11}, {"an indirect draw", 21, 23},
                        {"an indirect indexed draw", 25, 27}};
      for (UINT d = 0; d < std::size(draws_made); d++) {
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST);
        forget(readback.Get());
        const D3D12_VIEWPORT viewport{0, 0, float(target_side), float(target_side), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(target_side), LONG(target_side)};
        list->SetGraphicsRootSignature(rs.Get());
        list->SetPipelineState(pso.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetIndexBuffer(&ibv);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        if (d == 0)
          list->DrawInstanced(3, 1, 5, 7);
        else if (d == 1)
          list->DrawIndexedInstanced(3, 1, 0, 9, 11);
        else if (d == 2)
          list->ExecuteIndirect(draws.Get(), 1, arguments.Get(), 0, nullptr, 0);
        else
          list->ExecuteIndirect(indexed.Get(), 1, arguments.Get(), 32, nullptr, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        HRESULT hr = run();
        if (!expect(SUCCEEDED(hr), "%s: %08lx", draws_made[d].what, hr)) {
          start_ok = false;
          break;
        }
        const char *out;
        CHECK(readback->Map(0, nullptr, (void **)&out));
        auto got = reinterpret_cast<const UINT *>(out + footprint.Offset);
        if (!expect(got[0] == draws_made[d].start_vertex && got[1] == draws_made[d].start_instance,
                    "%s: SV_StartVertexLocation %u, SV_StartInstanceLocation %u, want %u and %u", draws_made[d].what,
                    got[0], got[1], draws_made[d].start_vertex, draws_made[d].start_instance))
          start_ok = false;
        readback->Unmap(0, nullptr);
      }
    }
  }

  // the barrier of memory and semantics
  {
    step("Barrier(MemoryTypeFlags, SemanticFlags)");
    auto cs = compiler.compile(barrier_hlsl, "cs", "cs_6_8");
    D3D12_ROOT_PARAMETER uav{D3D12_ROOT_PARAMETER_TYPE_UAV};
    auto rs = root_signature(device.Get(), {1, &uav});
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
    HRESULT hr = cs.empty() ? E_FAIL : device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (!expect(SUCCEEDED(hr), "the barrier pipeline: %08lx%s", hr, cs.empty() ? " (did not compile)" : "")) {
      barrier_ok = false;
    } else {
      const UINT groups = 16;
      auto data = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, groups * 64 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      list->SetComputeRootSignature(rs.Get());
      list->SetPipelineState(pso.Get());
      list->SetComputeRootUnorderedAccessView(0, data->GetGPUVirtualAddress());
      list->Dispatch(groups, 1, 1);
      transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      auto readback = readback_of(data.Get(), groups * 64 * 4);
      CHECK(run());
      const UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < groups * 64; i++) {
        UINT t = i % 64, other = (t + 17) % 64, want = (other * 3 + 1) + ((other * 5 + 2) << 16);
        if (!expect(got[i] == want, "thread %u read %#x, want %#x", i, got[i], want)) {
          barrier_ok = false;
          break;
        }
      }
      readback->Unmap(0, nullptr);
    }
  }

  printf("works: wave size ranges %d, comparison sampling %d, start locations %d, barriers %d\n", wave_ok, compare_ok,
         start_ok, barrier_ok);
  expect(!reports_68 || (wave_ok && compare_ok && start_ok && barrier_ok), "shader model 6.8 reported without all of it");
  expect(!options21.SampleCmpGradientAndBiasSupported || (reports_68 && compare_ok),
         "SampleCmpGradientAndBiasSupported reported without it");
  expect(!options21.ExtendedCommandInfoSupported || (reports_68 && start_ok), "ExtendedCommandInfoSupported reported without it");
  // and what works is reported
  expect(reports_68 || !(wave_ok && compare_ok && start_ok && barrier_ok), "shader model 6.8 works but is not reported");
  expect(options21.SampleCmpGradientAndBiasSupported || !compare_ok, "SampleCmpGradientAndBiasSupported is not reported");
  expect(options21.ExtendedCommandInfoSupported || !start_ok, "ExtendedCommandInfoSupported is not reported");
  return verdict();
}
