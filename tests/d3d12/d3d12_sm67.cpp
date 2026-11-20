// contract: SM 6.7 requires quad votes and helper-inclusive wave operations; optional texture capabilities must
// agree with pipeline acceptance and the values read back. "Shader Model 6.7 adds the WaveOpsIncludeHelperLanes
// entry function attribute." (Microsoft HLSL_ShaderModel6_7, Helper Lanes in Wave Ops Mode; also QuadAny and QuadAll
// and Advanced Texture Operations).
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_7.html
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <map>

// the toolchain lacks this enum member; Microsoft DirectX-Headers, include/directx/d3d12.idl,
// D3D12_UAV_DIMENSION declares D3D12_UAV_DIMENSION_TEXTURE2DMS = 6.
constexpr auto msaa_uav_dimension = D3D12_UAV_DIMENSION(6);

// contract: quad votes include helpers. "The routines always compare values from all four lanes of the current
// quad." (HLSL_SM_6_7_QuadAny_QuadAll, introduction, QuadAny, QuadAll).
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_QuadAny_QuadAll.html
// compute quads use 2x2 thread IDs: "X position in quad from left = SV_GroupThreadID.x & 1" (HLSL_SM_6_6_Derivatives,
// 2D Quads; Y uses the same rule). every quad visits all sixteen input masks, including triangle-edge helpers.
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Derivatives.html
static const char quad_hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint pattern; uint width; uint count; uint unused; };
RWStructuredBuffer<uint4> result : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2(1.25, 1.25);
  if (id == 1) p.x = SIZE - 1.75;
  if (id == 2) p.y = SIZE - 1.75;
  return float4(p / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint input_mask(uint2 p) { return (pattern + p.x / 2 + p.y / 2 * (width / 2)) % PATTERNS; }
uint4 ps(float4 pos : SV_Position) : SV_Target {
  uint2 p = uint2(pos.xy);
  bool v = (input_mask(p) & (1u << ((p.x & 1) + 2 * (p.y & 1)))) != 0;
  uint a = QuadAny(v), b = QuadAll(v), h = IsHelperLane();
  uint4 r = 0;
  [unroll] for (uint q = 0; q < 4; q++) {
    r.x |= QuadReadLaneAt(a, q) << q;
    r.y |= QuadReadLaneAt(b, q) << q;
    r.z |= QuadReadLaneAt(h, q) << q;
  }
  r.w = input_mask(p);
  result[p.y * width + p.x] = r;
  return r;
}
[numthreads(X, Y, 1)]
void cs(uint3 t : SV_DispatchThreadID) {
  uint m = input_mask(t.xy), q = (t.x & 1) + 2 * (t.y & 1);
  bool v = (m & (1u << q)) != 0;
  uint4 r = uint4(QuadAny(v), QuadAll(v), m, q);
#if DERIVATIVES
  r.w = asuint(ddx_fine(float(t.x * t.x)) + ddy_fine(float(3 * t.y * t.y)));
#endif
  result[t.y * width + t.x] = r;
}
)hlsl";

// contract: "By default and when this attribute is not present only non-helper lanes participate in wave
// operations." (HLSL_SM_6_7_Wave_Ops_Include_Helper_Lanes, WaveOpsIncludeHelperLanes Attribute and Device Behavior).
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Wave_Ops_Include_Helper_Lanes.html
// helpers cannot write UAVs, so covered lanes relay all four lanes through quad reads after the wave operations.
// the CPU groups those records by the wave's first pixel and counts distinct active and helper lane indices.
static const char helper_hlsl[] = R"hlsl(
RWStructuredBuffer<uint4> result : register(u0);
#if INCLUDE_HELPERS
[WaveOpsIncludeHelperLanes]
#endif
uint4 ps(float4 pos : SV_Position) : SV_Target {
  uint pixel = uint(pos.y) * SIZE + uint(pos.x);
  uint helper = IsHelperLane(), lane = WaveGetLaneIndex(), size = WaveGetLaneCount();
  uint first = WaveReadLaneFirst(pixel), n = WaveActiveCountBits(true);
  [unroll] for (uint q = 0; q < 4; q++) {
    uint reported = n;
#if INCLUDE_HELPERS
    reported = QuadReadLaneAt(n, q);
#endif
    result[(pixel * 4 + q) * 2] = QuadReadLaneAt(uint4(pixel, helper, lane, size), q);
    result[(pixel * 4 + q) * 2 + 1] = uint4(first, reported, 0, 0);
  }
  return n;
}
)hlsl";

// contract: SampleCmpLevel uses an "explicitly specified MIP level of detail(LOD)"; Sample and Load offsets honor
// "the 4 least significant bits" (HLSL_SM_6_7_Advanced_Texture_Ops, SampleCmpLevel and Programmable Offsets).
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Advanced_Texture_Ops.html
// Gather already has programmable offsets: "The 6 least significant bits of each offset value is honored as a
// signed value" (D3D11.3 functional specification 22.4.4, gather4_po). it does not require the optional SM 6.7 cap.
static const char texture_hlsl[] = R"hlsl(
struct Case { int x; int y; uint mip; float reference; };
StructuredBuffer<Case> cases : register(t2);
Texture2D<float> color : register(t0);
Texture2D<float> depth : register(t1);
SamplerState smp : register(s0);
SamplerComparisonState cmp : register(s1);
RWStructuredBuffer<uint4> result : register(u0);
cbuffer C : register(b0) { uint pattern; uint width; uint count; uint unused; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 ps(float4 pos : SV_Position) : SV_Target {
  uint i = uint(pos.y) * width + uint(pos.x);
  Case c = cases[i % count];
  uint dim = TEX_SIZE;
#if OP != 1 && OP != 4
  dim >>= c.mip;
#endif
  int2 base = int2(dim / 2, dim / 2), offset = int2(c.x, c.y);
  float2 uv = (float2(base) + 0.5) / dim;
#if OP == 0
  uint4 r = asuint(depth.SampleCmpLevel(cmp, uv, c.reference, c.mip)).xxxx;
#elif OP == 1
  uint4 r = asuint(color.Sample(smp, uv, offset)).xxxx;
#elif OP == 2
  uint4 r = asuint(color.SampleLevel(smp, uv, c.mip, offset)).xxxx;
#elif OP == 3
  uint4 r = asuint(depth.SampleCmpLevel(cmp, uv, c.reference, c.mip, offset)).xxxx;
#elif OP == 4
  uint4 r = asuint(color.Gather(smp, uv, offset));
#else
  uint4 r = asuint(color.Load(int3(base, c.mip), offset)).xxxx;
#endif
  result[i] = r;
  return r;
}
)hlsl";

// contract: GatherRaw returns texel bits "without any conversion of texture contents"
// (HLSL_SM_6_7_Advanced_Texture_Ops, Raw Gather Methods and Resource Integer Aliasing).
// each backing format is created castable to its UINT view.
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Advanced_Texture_Ops.html#raw-gather
static const char raw_hlsl[] = R"hlsl(
Texture2D<T> tex : register(t0);
SamplerState smp : register(s0);
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1, 1, 1)] void cs(uint3 p : SV_DispatchThreadID) {
  vector<T, 4> r = tex.GatherRaw(smp, (float2(p.xy) + 1) / RAW_SIZE);
  uint i = (p.y * RAW_SIZE + p.x) * 2;
  result[i] = uint4(r);
#if BITS == 64
  result[i + 1] = uint4(r >> 32);
#else
  result[i + 1] = 0;
#endif
}
)hlsl";

// contract: writable MSAA subscript writes sample zero; .sample[][] writes the "provided sample index"
// (HLSL_SM_6_7_Advanced_Texture_Ops, Writable MSAA Textures and Capability Queries).
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Advanced_Texture_Ops.html#writable-msaa-textures
// Texture2DMS.Load copies each sample into a buffer without resolving: CopyTextureRegion requires matching sample
// counts (Microsoft Learn, ID3D12GraphicsCommandList::CopyTextureRegion, Remarks), so cannot copy MSAA to a buffer.
static const char msaa_hlsl[] = R"hlsl(
RWTexture2DMS<uint, SAMPLES> tex : register(u1);
Texture2DMS<uint, SAMPLES> source : register(t0);
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1, 1, 1)] void write(uint3 p : SV_DispatchThreadID) {
  uint pixel = p.y * SIZE + p.x;
  for (uint s = 0; s < SAMPLES; s++) tex.sample[s][p.xy] = pixel * SAMPLES + s + 1;
  tex[p.xy] = ~pixel;
}
[numthreads(1, 1, 1)] void read(uint3 p : SV_DispatchThreadID) {
  uint pixel = p.y * SIZE + p.x;
  for (uint s = 0; s < SAMPLES; s++) result[pixel * SAMPLES + s] = uint4(source.Load(p.xy, s), 0, 0, 0);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: shader model 6.7 requires dxcompiler.dll and DXIL\n");
    return 77;
  }
  constexpr UINT quad_lanes = 4, patterns = 1u << quad_lanes, side = patterns * 4, pixels = side * side;
  constexpr UINT offset_bits = 4, gather_offset_bits = 6, tex_size = 1u << gather_offset_bits;
  constexpr UINT mips = std::bit_width(tex_size), raw_size = quad_lanes;
  struct Layout {
    UINT x, y;
    bool derivatives;
  };
  const Layout layouts[] = {{6, 4, false}, {16, 16, false}, {6, 4, true}, {16, 16, true}};
  std::vector<std::string> defines = {"SIZE=" + std::to_string(side), "PATTERNS=" + std::to_string(patterns),
                                     "X=2", "Y=2", "TEX_SIZE=" + std::to_string(tex_size)};
  auto vs = compiler.compile(quad_hlsl, "vs", "vs_6_7", defines);
  auto full_vs = compiler.compile(texture_hlsl, "vs", "vs_6_7", defines);
  auto quad_ps = compiler.compile(quad_hlsl, "ps", "ps_6_7", defines);
  std::array<std::string, std::size(layouts)> quad_cs;
  for (size_t i = 0; i < quad_cs.size(); i++) {
    auto d = defines;
    d[2] = "X=" + std::to_string(layouts[i].x);
    d[3] = "Y=" + std::to_string(layouts[i].y);
    d.push_back("DERIVATIVES=" + std::to_string(layouts[i].derivatives));
    quad_cs[i] = compiler.compile(quad_hlsl, "cs", "cs_6_7", d);
  }
  std::array<std::string, 2> helper_ps;
  for (size_t i = 0; i < helper_ps.size(); i++)
    helper_ps[i] = compiler.compile(helper_hlsl, "ps", "ps_6_7", {"SIZE=" + std::to_string(side),
                                                               "INCLUDE_HELPERS=" + std::to_string(i)});
  const char *operations[] = {"SampleCmpLevel", "Sample", "SampleLevel", "SampleCmpLevel offset", "Gather", "Load"};
  std::array<std::string, std::size(operations)> texture_ps;
  for (size_t i = 0; i < texture_ps.size(); i++) {
    auto d = defines;
    d.push_back("OP=" + std::to_string(i));
    texture_ps[i] = compiler.compile(texture_hlsl, "ps", "ps_6_7", d);
  }
  struct Raw {
    UINT bits;
    DXGI_FORMAT format, view;
    const char *type;
  };
  const Raw raw_formats[] = {{16, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UINT, "uint16_t"},
                              {32, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_UINT, "uint"},
                              {64, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32_UINT, "uint64_t"}};
  std::array<std::string, std::size(raw_formats)> raw_cs;
  for (size_t i = 0; i < raw_cs.size(); i++)
    raw_cs[i] = compiler.compile(raw_hlsl, "cs", "cs_6_7",
                                {"T=" + std::string(raw_formats[i].type), "BITS=" + std::to_string(raw_formats[i].bits),
                                 "RAW_SIZE=" + std::to_string(raw_size)}, {L"-enable-16bit-types"});
  std::vector<std::array<std::string, 2>> msaa_cs;
  for (UINT samples = 2; samples <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2) {
    auto d = defines;
    d.push_back("SAMPLES=" + std::to_string(samples));
    msaa_cs.push_back({compiler.compile(msaa_hlsl, "write", "cs_6_7", d),
                       compiler.compile(msaa_hlsl, "read", "cs_6_7", d)});
  }
  bool compiled = !vs.empty() && !full_vs.empty() && !quad_ps.empty();
  for (auto &code : quad_cs) compiled &= !code.empty();
  for (auto &code : helper_ps) compiled &= !code.empty();
  for (auto &code : texture_ps) compiled &= !code.empty();
  for (auto &code : raw_cs) compiled &= !code.empty();
  for (auto &pair : msaa_cs)
    for (auto &code : pair) compiled &= !code.empty();
  if (!expect(compiled, "SM 6.7 HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  step("shader model 6.7 feature query");
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_7};
  auto hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
  expect(hr == S_OK && model.HighestShaderModel == D3D_SHADER_MODEL_6_7,
         "shader model query returned %#lx and %#x, want 6.7", hr, model.HighestShaderModel);
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14{TRUE, TRUE};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options14, sizeof(options14)));
  expect(options1.WaveOps && options1.WaveLaneCountMin >= quad_lanes &&
             options1.WaveLaneCountMax >= options1.WaveLaneCountMin, "invalid wave capabilities");
  expect(!options14.AdvancedTextureOpsSupported || options12.RelaxedFormatCastingSupported,
         "advanced texture operations require relaxed format casting");

  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1}};
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                  {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                  {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].DescriptorTable = {1, &ranges[0]};
  params[2].Descriptor.ShaderRegister = 2;
  params[3].Constants = {0, 0, 4};
  params[4].DescriptorTable = {1, &ranges[1]};
  D3D12_STATIC_SAMPLER_DESC samplers[] = {
      {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
       D3D12_TEXTURE_ADDRESS_MODE_CLAMP},
      {D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
       D3D12_TEXTURE_ADDRESS_MODE_CLAMP}};
  for (UINT i = 0; i < std::size(samplers); i++) {
    samplers[i].ShaderRegister = i;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  }
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params, UINT(std::size(samplers)), samplers});
  if (!expect(bool(rs), "root signature creation failed"))
    return verdict();
  auto graphics = [&](const std::string &vertex, const std::string &pixel, ComPtr<ID3D12PipelineState> &pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vertex), bytecode(pixel)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
    desc.SampleDesc = {1, 0};
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  };
  ComPtr<ID3D12DescriptorHeap> views, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC view_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  const UINT view_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu_view = [&](UINT i) {
    auto h = views->GetCPUDescriptorHandleForHeapStart();
    h.ptr += i * view_stride;
    return h;
  };
  auto gpu_view = [&](UINT i) {
    auto h = views->GetGPUDescriptorHandleForHeapStart();
    h.ptr += i * view_stride;
    return h;
  };
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1,
                                  DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  const UINT64 bytes = UINT64(pixels) * std::max(quad_lanes * 2, UINT(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT)) *
                       sizeof(std::array<UINT, 4>);
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto init = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(out && init && readback, "readback buffers were not created"))
    return verdict();
  void *mapped;
  CHECK(init->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  init->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  auto begin = [&](ID3D12PipelineState *pso) {
    HRESULT status = allocator->Reset();
    if (FAILED(status)) return status;
    if (FAILED(status = list->Reset(allocator.Get(), pso))) return status;
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(out.Get(), 0, init.Get(), 0, bytes);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetGraphicsRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, gpu_view(0));
    list->SetGraphicsRootDescriptorTable(1, gpu_view(0));
    list->SetComputeRootDescriptorTable(4, gpu_view(2));
    D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(side), LONG(side)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    return S_OK;
  };
  UINT *o;
  auto finish = [&]() {
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, bytes);
    HRESULT status = submit(device.Get(), queue.Get(), list.Get());
    return FAILED(status) ? status : readback->Map(0, nullptr, reinterpret_cast<void **>(&o));
  };
  auto covered = [&](UINT x, UINT y) { return x >= 1 && y >= 1 && x + y < side - 1; };

  for (size_t i = 0; i < quad_cs.size(); i++) {
    step("QuadAny/QuadAll compute %ux%u groups, 3x2 dispatch, derivatives %u", layouts[i].x, layouts[i].y,
         layouts[i].derivatives);
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(quad_cs[i])};
    hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (!expect(hr == S_OK && pso, "compute quad pipeline returned %#lx", hr)) continue;
    const UINT w = layouts[i].x * 3, h = layouts[i].y * 2;
    for (UINT mask = 0; mask < patterns; mask++) {
      CHECK(begin(pso.Get()));
      const UINT constants[] = {mask, w, 0, 0};
      list->SetComputeRoot32BitConstants(3, std::size(constants), constants, 0);
      list->Dispatch(3, 2, 1);
      CHECK(finish());
      for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
          UINT m = (mask + x / 2 + y / 2 * (w / 2)) % patterns;
          bool any = false, all = true;
          for (UINT q = 0; q < quad_lanes; q++) {
            bool v = (m & (1u << q)) != 0;
            any |= v; all &= v;
          }
          auto r = o + (y * w + x) * 4;
          auto f = [](UINT x, UINT y) { return float(x * x + 3 * y * y); };
          float derivative = f(x / 2 * 2 + 1, y) - f(x / 2 * 2, y) +
                             f(x, y / 2 * 2 + 1) - f(x, y / 2 * 2);
          UINT last = layouts[i].derivatives ? std::bit_cast<UINT>(derivative) : x % 2 + 2 * (y % 2);
          expect(r[0] == any && r[1] == all && r[2] == m && r[3] == last,
                 "compute (%u,%u) mask %u returned %u,%u,%u,%u", x, y, m, r[0], r[1], r[2], r[3]);
        }
      readback->Unmap(0, nullptr);
    }
  }
  step("QuadAny/QuadAll pixel quads including triangle-edge helpers");
  ComPtr<ID3D12PipelineState> quad_pso;
  hr = graphics(vs, quad_ps, quad_pso);
  if (expect(hr == S_OK && quad_pso, "pixel quad pipeline returned %#lx", hr))
    for (UINT mask = 0; mask < patterns; mask++) {
      CHECK(begin(quad_pso.Get()));
      const UINT constants[] = {mask, side, 0, 0};
      list->SetGraphicsRoot32BitConstants(3, std::size(constants), constants, 0);
      list->DrawInstanced(3, 1, 0, 0);
      CHECK(finish());
      for (UINT y = 0; y < side; y++)
        for (UINT x = 0; x < side; x++) {
          auto r = o + (y * side + x) * 4;
          if (!covered(x, y)) {
            expect(r[0] == ~0u && r[1] == ~0u && r[2] == ~0u && r[3] == ~0u,
                   "uncovered pixel (%u,%u) wrote a UAV", x, y);
            continue;
          }
          UINT m = (mask + x / 2 + y / 2 * (side / 2)) % patterns, helpers = 0;
          bool any = false, all = true;
          for (UINT q = 0; q < quad_lanes; q++) {
            bool v = (m & (1u << q)) != 0;
            any |= v; all &= v;
            helpers |= UINT(!covered(x / 2 * 2 + q % 2, y / 2 * 2 + q / 2)) << q;
          }
          expect(r[0] == (any ? patterns - 1 : 0) && r[1] == (all ? patterns - 1 : 0) && r[2] == helpers && r[3] == m,
                 "pixel (%u,%u) mask %u returned %#x,%#x,%#x,%#x, helpers %#x", x, y, m,
                 r[0], r[1], r[2], r[3], helpers);
        }
      readback->Unmap(0, nullptr);
    }
  for (UINT include = 0; include < helper_ps.size(); include++) {
    step("WaveActiveCountBits with helper participation %u", include);
    ComPtr<ID3D12PipelineState> pso;
    hr = graphics(vs, helper_ps[include], pso);
    if (!expect(hr == S_OK && pso, "helper pipeline returned %#lx", hr)) continue;
    CHECK(begin(pso.Get()));
    list->DrawInstanced(3, 1, 0, 0);
    CHECK(finish());
    std::map<UINT, std::map<UINT, std::array<UINT, 4>>> waves;
    bool saw_helpers = false;
    for (UINT pixel = 0; pixel < pixels; pixel++) {
      UINT x = pixel % side, y = pixel / side;
      for (UINT q = 0; q < quad_lanes; q++) {
        auto r = o + (pixel * quad_lanes + q) * 8;
        if (!covered(x, y)) {
          expect(std::all_of(r, r + 8, [](UINT v) { return v == ~0u; }),
                 "helper/uncovered pixel %u wrote a UAV", pixel);
          continue;
        }
        UINT px = x / 2 * 2 + q % 2, py = y / 2 * 2 + q / 2;
        if (!expect(r[0] == py * side + px && r[1] == UINT(!covered(px, py)) && r[2] < r[3] && r[2] % 4 == q &&
                        r[3] >= options1.WaveLaneCountMin && r[3] <= options1.WaveLaneCountMax && r[4] < pixels,
                    "pixel %u quad lane %u has invalid identity/helper/wave data", pixel, q)) continue;
        // wave results on helpers without the attribute are undefined; only covered lanes supply the count there
        std::array<UINT, 4> record{r[0], r[1], r[5], r[3]};
        auto [at, inserted] = waves[r[4]].emplace(r[2], record);
        expect(inserted || at->second == record, "wave %u lane %u disagrees across quad reporters", r[4], r[2]);
      }
    }
    for (auto &[key, lanes] : waves) {
      UINT active = 0, helpers = 0;
      for (auto &[lane, r] : lanes) r[1] ? helpers++ : active++;
      // WaveReadLaneFirst reads the lowest-indexed participating lane, the first lane here or the first non-helper
      auto first = std::find_if(lanes.begin(), lanes.end(), [&](auto &lane) { return include || !lane.second[1]; });
      expect(first != lanes.end() && key == first->second[0], "wave %u reads its first lane as pixel %u, want %u",
             lanes.begin()->first, key, first == lanes.end() ? ~0u : first->second[0]);
      saw_helpers |= helpers != 0;
      UINT want = active + (include ? helpers : 0);
      for (auto &[lane, r] : lanes)
        expect(r[2] == want && lanes.size() <= r[3] && r[3] == lanes.begin()->second[3],
               "wave %u lane %u counts %u, want %u (%u active, %u helpers), wave size %u",
               key, lane, r[2], want, active, helpers, r[3]);
    }
    expect(!waves.empty() && saw_helpers, "triangle produced no observable helper-containing waves");
    readback->Unmap(0, nullptr);
  }

  // mip bands do not overlap, and each texel has its own value within its band
  auto depth_value = [&](UINT m, UINT x, UINT y) {
    return float(m * tex_size * tex_size + y * (tex_size >> m) + x + 1) / (mips * tex_size * tex_size);
  };
  auto color_value = [&](UINT m, UINT x, UINT y) {
    return float(m * tex_size * tex_size + y * (tex_size >> m) + x + 1);
  };
  auto signed_offset = [](int v, UINT bits) {
    UINT range = 1u << bits, low = UINT(v) % range;
    return low < range / 2 ? int(low) : int(low) - int(range);
  };
  struct Case {
    int x, y;
    UINT mip;
    float reference;
  };
  std::vector<Case> cases;
  const int offsets[] = {-8, -1, 0, 7, -9, 8, 15, 16, -16, 23, -17, 32, -33, 63, 64};
  for (UINT m : {0u, 1u, mips - 1})
    for (int y : offsets)
      for (int x : offsets) {
        int dim = tex_size >> m;
        UINT tx = std::clamp(dim / 2 + signed_offset(x, offset_bits), 0, dim - 1);
        UINT ty = std::clamp(dim / 2 + signed_offset(y, offset_bits), 0, dim - 1);
        float d = depth_value(m, tx, ty), half_step = 0.5f / (mips * tex_size * tex_size);
        for (float reference : {d - half_step, d, d + half_step}) cases.push_back({x, y, m, reference});
      }
  if (!expect(cases.size() <= pixels, "texture cases exceed the draw")) return verdict();
  auto inputs = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cases.size() * sizeof(Case),
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(inputs), "texture inputs were not created")) return verdict();
  CHECK(inputs->Map(0, nullptr, &mapped));
  memcpy(mapped, cases.data(), cases.size() * sizeof(Case));
  inputs->Unmap(0, nullptr);
  ComPtr<ID3D12Resource> color, depth;
  D3D12_RESOURCE_DESC tex_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, tex_size, tex_size, 1, UINT16(mips),
                               DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN};
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&color)));
  auto depth_desc = tex_desc;
  depth_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&depth)));
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, mips> footprints;
  UINT64 texture_bytes;
  device->GetCopyableFootprints(&tex_desc, 0, mips, 0, footprints.data(), nullptr, nullptr, &texture_bytes);
  const UINT64 texture_stride = (texture_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                               D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, texture_stride * 2, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(upload), "texture upload was not created")) return verdict();
  CHECK(upload->Map(0, nullptr, &mapped));
  for (UINT m = 0; m < mips; m++)
    for (UINT y = 0; y < (tex_size >> m); y++)
      for (UINT x = 0; x < (tex_size >> m); x++) {
        auto at = footprints[m].Offset + y * footprints[m].Footprint.RowPitch + x * sizeof(float);
        float c = color_value(m, x, y), d = depth_value(m, x, y);
        memcpy(static_cast<char *>(mapped) + at, &c, sizeof(c));
        memcpy(static_cast<char *>(mapped) + texture_stride + at, &d, sizeof(d));
      }
  upload->Unmap(0, nullptr);
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  for (UINT i = 0; i < 2; i++)
    for (UINT m = 0; m < mips; m++) {
      D3D12_TEXTURE_COPY_LOCATION dst{i ? depth.Get() : color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.SubresourceIndex = m;
      D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.PlacedFootprint = footprints[m];
      src.PlacedFootprint.Offset += i * texture_stride;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
  transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv.Texture2D.MipLevels = mips;
  device->CreateShaderResourceView(color.Get(), &srv, cpu_view(0));
  device->CreateShaderResourceView(depth.Get(), &srv, cpu_view(1));
  for (UINT op = 0; op < texture_ps.size(); op++) {
    step("%s, AdvancedTextureOpsSupported=%u", operations[op], options14.AdvancedTextureOpsSupported);
    ComPtr<ID3D12PipelineState> pso;
    hr = graphics(full_vs, texture_ps[op], pso);
    bool supported = op == 4 || options14.AdvancedTextureOpsSupported;
    if (!supported) {
      expect(FAILED(hr) && !pso, "unsupported %s pipeline returned %#lx", operations[op], hr);
      continue;
    }
    if (!expect(hr == S_OK && pso, "supported %s pipeline returned %#lx", operations[op], hr)) continue;
    CHECK(begin(pso.Get()));
    const UINT constants[] = {0, side, UINT(cases.size()), 0};
    list->SetGraphicsRoot32BitConstants(3, std::size(constants), constants, 0);
    list->SetGraphicsRootShaderResourceView(2, inputs->GetGPUVirtualAddress());
    list->DrawInstanced(3, 1, 0, 0);
    CHECK(finish());
    for (UINT i = 0; i < pixels; i++) {
      auto c = cases[i % cases.size()];
      UINT m = op == 1 || op == 4 ? 0 : c.mip;
      int dim = tex_size >> m;
      int base = dim / 2;
      int x = base + (op == 0 ? 0 : signed_offset(c.x, op == 4 ? gather_offset_bits : offset_bits));
      int y = base + (op == 0 ? 0 : signed_offset(c.y, op == 4 ? gather_offset_bits : offset_bits));
      for (UINT channel = 0; channel < 4; channel++) {
        int tx = x, ty = y;
        if (op == 4) {
          // gather component order: bottom left, bottom right, top right, top left (D3D11.3 22.4.1)
          tx += channel == 1 || channel == 2;
          ty += channel < 2;
        }
        float want;
        if (op == 5 && (tx < 0 || tx >= dim || ty < 0 || ty >= dim)) want = 0;
        else {
          tx = std::clamp(tx, 0, dim - 1); ty = std::clamp(ty, 0, dim - 1);
          want = op == 0 || op == 3 ? float(c.reference <= depth_value(m, tx, ty)) : color_value(m, tx, ty);
        }
        expect(o[i * 4 + channel] == std::bit_cast<UINT>(want),
               "%s case %u mip %u offset (%d,%d) channel %u: %#x, want %g", operations[op], i, c.mip, c.x, c.y,
               channel, o[i * 4 + channel], want);
      }
    }
    readback->Unmap(0, nullptr);
  }

  for (UINT f = 0; f < raw_cs.size(); f++) {
    auto format = raw_formats[f];
    step("GatherRaw %u-bit, castable format %u to %u", format.bits, format.format, format.view);
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(raw_cs[f])};
    hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    bool supported = options14.AdvancedTextureOpsSupported &&
                     (format.bits != 16 || options4.Native16BitShaderOpsSupported) &&
                     (format.bits != 64 || options1.Int64ShaderOps);
    if (!supported) {
      expect(FAILED(hr) && !pso, "unsupported GatherRaw %u pipeline returned %#lx", format.bits, hr);
      continue;
    }
    if (!expect(hr == S_OK && pso, "supported GatherRaw pipeline returned %#lx", hr)) continue;
    ComPtr<ID3D12Device10> device10;
    if (!expect(device.As(&device10) == S_OK, "castable resources require ID3D12Device10")) continue;
    D3D12_RESOURCE_DESC1 desc1{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, raw_size, raw_size, 1, 1, format.format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN};
    ComPtr<ID3D12Resource> tex;
    hr = device10->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                           nullptr, 1, &format.view, IID_PPV_ARGS(&tex));
    if (!expect(hr == S_OK && tex, "castable resource returned %#lx", hr)) continue;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 upload_bytes;
    auto desc0 = tex->GetDesc();
    device->GetCopyableFootprints(&desc0, 0, 1, 0, &footprint, nullptr, nullptr, &upload_bytes);
    auto raw_upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(bool(raw_upload), "raw upload was not created")) continue;
    std::array<uint64_t, raw_size * raw_size> bits;
    CHECK(raw_upload->Map(0, nullptr, &mapped));
    for (UINT i = 0; i < bits.size(); i++) {
      bits[i] = (~uint64_t(i) ^ (uint64_t(i + 1) << (format.bits / 2))) & (~uint64_t(0) >> (64 - format.bits));
      memcpy(static_cast<char *>(mapped) + footprint.Offset + i / raw_size * footprint.Footprint.RowPitch +
                 i % raw_size * (format.bits / 8), &bits[i], format.bits / 8);
    }
    raw_upload->Unmap(0, nullptr);
    auto raw_srv = srv;
    raw_srv.Format = format.view;
    raw_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(tex.Get(), &raw_srv, cpu_view(0));
    CHECK(begin(pso.Get()));
    transition(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{raw_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->Dispatch(raw_size, raw_size, 1);
    CHECK(finish());
    for (UINT i = 0; i < bits.size(); i++)
      for (UINT channel = 0; channel < 4; channel++) {
        UINT x = std::min(i % raw_size + UINT(channel == 1 || channel == 2), raw_size - 1);
        UINT y = std::min(i / raw_size + UINT(channel < 2), raw_size - 1);
        uint64_t got = o[i * 8 + channel] | uint64_t(o[i * 8 + 4 + channel]) << 32;
        expect(got == bits[y * raw_size + x], "GatherRaw %u texel %u channel %u: %#llx, want %#llx", format.bits,
               i, channel, (unsigned long long)got, (unsigned long long)bits[y * raw_size + x]);
      }
    readback->Unmap(0, nullptr);
  }
  UINT ran_msaa = 0;
  for (UINT k = 0, samples = 2; k < msaa_cs.size(); k++, samples *= 2) {
    step("RWTexture2DMS %u samples, WriteableMSAATexturesSupported=%u", samples,
         options14.WriteableMSAATexturesSupported);
    ComPtr<ID3D12PipelineState> write_pso, read_pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(msaa_cs[k][0])};
    hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&write_pso));
    if (!options14.WriteableMSAATexturesSupported) {
      expect(FAILED(hr) && !write_pso, "unsupported writable MSAA pipeline returned %#lx", hr);
      continue;
    }
    if (!expect(hr == S_OK && write_pso, "supported writable MSAA pipeline returned %#lx", hr)) continue;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{DXGI_FORMAT_R32_UINT, samples};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
    if (!quality.NumQualityLevels) {
      printf("not exercised: R32_UINT with %u samples is unavailable\n", samples);
      continue;
    }
    auto ms_desc = target_desc;
    ms_desc.Format = DXGI_FORMAT_R32_UINT;
    ms_desc.SampleDesc = {samples, 0};
    ms_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> tex;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ms_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&tex)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_UINT, msaa_uav_dimension};
    device->CreateUnorderedAccessView(tex.Get(), nullptr, &uav, cpu_view(2));
    D3D12_SHADER_RESOURCE_VIEW_DESC ms_srv{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2DMS,
                                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    device->CreateShaderResourceView(tex.Get(), &ms_srv, cpu_view(0));
    desc.CS = bytecode(msaa_cs[k][1]);
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&read_pso)));
    CHECK(begin(write_pso.Get()));
    list->Dispatch(side, side, 1);
    transition(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(read_pso.Get());
    list->Dispatch(side, side, 1);
    CHECK(finish());
    for (UINT pixel = 0; pixel < pixels; pixel++)
      for (UINT s = 0; s < samples; s++) {
        UINT want = s ? pixel * samples + s + 1 : ~pixel;
        auto r = o + (pixel * samples + s) * 4;
        expect(r[0] == want && !r[1] && !r[2] && !r[3], "MSAA pixel %u sample %u: %#x, want %#x", pixel, s, r[0], want);
      }
    readback->Unmap(0, nullptr);
    ran_msaa++;
  }
  expect(!options14.WriteableMSAATexturesSupported || ran_msaa,
         "writable MSAA advertised but no sample count was exercised");
  return verdict();
}
