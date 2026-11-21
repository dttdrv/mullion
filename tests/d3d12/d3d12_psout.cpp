// alpha-to-coverage endpoints: "alpha of 0 (or less) must map to no coverage and alpha of 1 (or greater) must
// map to full coverage"; "An alpha value of NaN results in a no coverage (zero) mask." and "If the Pixel Shader
// outputs oMask (output coverage), Alpha-to-Coverage is disabled." (D3D11.3 17.18).
// contract: the view interprets the pixel shader's register bits, then converts to its storage format.
// "There is no data type associated with registers; Shader instructions determine how the data stored in
// registers is interpreted." (D3D11.3 7.8.1). "The application is given control over the data type interpretation for
// Shader outputs (i.e. writing raw integer values vs. writing normalized float values) by simply choosing an
// appropriate format to interpret the output resource's contents as." (7.6).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm Wine's d3d10core
// test_layered_rtv_mismatch records float bits in R32G32B32A32_UINT. the other pairs keep the same register bits
// under 7.6; 19.1.3.2 and 3.2.3 give the view's conversion. float, UNORM, SNORM and sRGB interpret them as floats, SINT
// as signed integers, UINT as unsigned integers, independently for each target (17.14). "Failure to provide sufficient
// data to the Output Merger for all of the RenderTarget(s)/component(s) enabled with the write masks results in
// undefined values being written out." (17.15): undeclared components are not compared; disabled channels keep the
// clear. BLEND_FACTOR scales the source by one half (17.1, 17.5), dual source multiplies o0 by o1 (17.6); 16.5
// broadcasts at pixel frequency and computes each sample at sample frequency. D3D11 leaves blending off for integer
// views, which have no blend capability. D3D12 refuses that PSO when its output is declared: vkd3d-proton
// tests/d3d12_pso.c:test_integer_blending_pipeline_state records E_INVALIDARG. depth without a DSV is ignored (17.9); a
// bound D32_FLOAT is tested too. all shaders compile before device creation.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cfloat>
#include <cmath>
#include <limits>

static constexpr UINT TARGETS = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
static constexpr UINT SIDE = 4, SAMPLES = 4;
enum Mode {
  Full,
  Fewer,
  Narrow,
  Mask,
  ZeroMask,
  Blend,
  Dual,
  DepthAbsent,
  DepthPresent,
  PixelFrequency,
  SampleFrequency,
  Geometry,
  Tessellation,
  AlphaToCoverage,
  CoverageOverridesAlpha,
  MODES
};

static const float alphas[] = {-INFINITY, -1, -0.0f, 0, 1, 2, INFINITY, std::numeric_limits<float>::quiet_NaN()};

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct Vertex { float4 pos : SV_Position; };
[maxvertexcount(3)]
void gs(triangle Vertex vertices[3], inout TriangleStream<Vertex> stream) {
  for (uint i = 0; i < 3; i++) {
    Vertex v = vertices[i];
    v.pos.x = -v.pos.x;
    stream.Append(v);
  }
}
struct Factors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<Vertex, 3> vertices) {
  Factors f; f.edge[0] = f.edge[1] = f.edge[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("factors")]
Vertex hs(InputPatch<Vertex, 3> vertices, uint id : SV_OutputControlPointID) { return vertices[id]; }
[domain("tri")]
float4 ds(Factors f, const OutputPatch<Vertex, 3> vertices, float3 bary : SV_DomainLocation) : SV_Position {
  return vertices[0].pos * bary.x + vertices[1].pos * bary.y + vertices[2].pos * bary.z;
}
float4 out_float(float4 v) { return v; }
uint4 out_uint(float4 v) { return asuint(v); }
int4 out_int(float4 v) { return asint(v); }
struct Outputs {
  TYPE0 colour : TARGET;
  TYPE1 other : OTHER_TARGET;
#if COVERAGE_OUTPUT
  uint coverage : SV_Coverage;
#endif
#if DEPTH
  float z : SV_Depth;
#endif
};
Outputs ps(float4 pos : SV_Position
#if RATE
, uint sid : SV_SampleIndex
#endif
) {
#if !RATE
  uint sid = 0;
#endif
  float4 v = float4((1 + ((uint)pos.x + sid) % 3) * 0.25, -0.5, 0.5, 1);
#if ALPHA_COVERAGE
  static const uint bits[] = {ALPHA_VALUES};
  v.a = asfloat(bits[((uint)pos.y * SIDE + (uint)pos.x) % ALPHA_COUNT]);
#endif
  Outputs o;
#if COVERAGE_OUTPUT
  o.coverage = ~0u;
#endif
  o.colour = CONVERT0(v).CHANNELS;
  o.other = CONVERT1(float4(0.5, 0.25, 1, 0.75));
#if DEPTH
  o.z = 0.75;
#endif
  return o;
}
Texture2DMS<READ_TYPE> image : register(t0);
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1, 1, 1)] void cs(uint2 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLES; s++)
    result[(p.y * SIDE + p.x) * SAMPLES + s] = asuint(image.Load(p, s));
}
)hlsl";

enum Kind { Float, Unorm, Snorm, Srgb, Sint, Uint };
struct Format {
  DXGI_FORMAT format;
  Kind kind;
  UINT components, bits;
};
static Format
format(Kind kind, UINT components) {
  const DXGI_FORMAT floats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
  const DXGI_FORMAT sints[] = {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32A32_SINT};
  const DXGI_FORMAT uints[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
  const DXGI_FORMAT unorms[] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
  const DXGI_FORMAT snorms[] = {DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM};
  UINT at = components == 1 ? 0 : components == 2 ? 1 : 2;
  if (kind == Srgb)
    return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kind, 4, CHAR_BIT};
  return {kind == Float   ? floats[at]
          : kind == Sint  ? sints[at]
          : kind == Uint  ? uints[at]
          : kind == Unorm ? unorms[at]
                          : snorms[at],
          kind, components,
          kind == Float || kind == Sint || kind == Uint ? UINT(sizeof(UINT) * CHAR_BIT) : UINT(CHAR_BIT)};
}

// ideal output conversion and its tolerances, D3D11.3 3.2.3.4, 3.2.3.6 and 3.2.3.8
static double
scaled(Format f, float value, UINT channel) {
  if (std::isnan(value)) value = 0;
  if (f.kind == Snorm)
    return std::clamp(double(value), -1.0, 1.0) * INT8_MAX;
  double v = std::clamp(double(value), 0.0, 1.0);
  if (f.kind == Srgb && channel != 3)
    v = v <= D3D12_FLOAT_TO_SRGB_THRESHOLD
            ? D3D12_FLOAT_TO_SRGB_SCALE_1 * v
            : D3D12_FLOAT_TO_SRGB_SCALE_2 *
                      std::pow(v, D3D12_FLOAT_TO_SRGB_EXPONENT_NUMERATOR / D3D12_FLOAT_TO_SRGB_EXPONENT_DENOMINATOR) -
                  D3D12_FLOAT_TO_SRGB_OFFSET;
  return v * UINT8_MAX;
}

static UINT
stored(Format f, float value, UINT channel) {
  return f.bits == sizeof(UINT) * CHAR_BIT ? std::bit_cast<UINT>(value)
                                           : UINT(int(std::round(scaled(f, value, channel)))) & UINT8_MAX;
}

static bool
matches(Format f, UINT got, float value, UINT channel, bool sampled) {
  if (f.bits == sizeof(UINT) * CHAR_BIT)
    return f.kind == Float && std::isnan(value) ? std::isnan(std::bit_cast<float>(got)) : got == stored(f, value, channel);
  double actual = f.kind == Snorm ? int(int8_t(got)) : int(got);
  double tolerance = D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP;
  if (sampled) {
    auto number = std::bit_cast<float>(got);
    if (!std::isfinite(number))
      return false;
    actual = scaled(f, number, channel);
    tolerance += f.kind == Srgb && channel != 3 ? D3D12_SRGB_TO_FLOAT_TOLERANCE_IN_ULP : FLT_EPSILON * UINT8_MAX;
  }
  return std::abs(actual - scaled(f, value, channel)) <= tolerance;
}

static std::vector<std::string>
defines(UINT kind, UINT slot, UINT mode) {
  const char *types[] = {"float", "int", "uint"}, *conversions[] = {"out_float", "out_int", "out_uint"};
  UINT components = mode == Fewer ? 1 : mode == Mask ? 2 : 4;
  std::string suffix = components == 1 ? "" : std::to_string(components);
  std::string alpha_values;
  for (auto alpha : alphas) {
    if (!alpha_values.empty()) alpha_values += ",";
    alpha_values += std::to_string(std::bit_cast<UINT>(alpha)) + "u";
  }
  return {"ALPHA_COVERAGE=" + std::to_string(mode == AlphaToCoverage || mode == CoverageOverridesAlpha),
          "COVERAGE_OUTPUT=" + std::to_string(mode == CoverageOverridesAlpha),
          "ALPHA_COUNT=" + std::to_string(std::size(alphas)), "ALPHA_VALUES=" + alpha_values,
          std::string("TYPE0=") + types[kind] + suffix,
          std::string("TYPE1=") + types[(kind + 1) % std::size(types)] + "4",
          std::string("CONVERT0=") + conversions[kind],
          std::string("CONVERT1=") + conversions[(kind + 1) % std::size(types)],
          std::string("CHANNELS=") + (components == 1   ? "x"
                                      : components == 2 ? "xy"
                                                        : "xyzw"),
          "TARGET=SV_Target" + std::to_string(slot),
          "OTHER_TARGET=SV_Target" + std::to_string((slot + 1) % TARGETS),
          "DEPTH=" + std::to_string(mode == DepthAbsent || mode == DepthPresent),
          "RATE=" + std::to_string(mode == SampleFrequency),
          "READ_TYPE=float4",
          "SIDE=" + std::to_string(SIDE),
          "SAMPLES=" + std::to_string(SAMPLES)};
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::array<std::array<std::array<std::string, TARGETS>, MODES>, 3> codes;
  auto defaults = defines(0, 0, 0);
  auto vertex = compiler.compile(hlsl, "vs", "vs", defaults);
  auto geometry = compiler.compile(hlsl, "gs", "gs", defaults);
  auto hull = compiler.compile(hlsl, "hs", "hs", defaults);
  auto domain = compiler.compile(hlsl, "ds", "ds", defaults);
  if (!expect(!vertex.empty() && !geometry.empty() && !hull.empty() && !domain.empty(),
              "rasterizing HLSL did not compile"))
    return verdict();
  for (UINT kind = 0; kind < codes.size(); kind++)
    for (UINT mode = 0; mode < MODES; mode++)
      for (UINT slot = 0; slot < (mode ? 1 : TARGETS); slot++) {
        step("compile output %u, mode %u, slot %u", kind, mode, slot);
        codes[kind][mode][slot] = compiler.compile(hlsl, "ps", "ps", defines(kind, slot, mode));
        if (!expect(!codes[kind][mode][slot].empty(), "pixel HLSL did not compile"))
          return verdict();
      }
  std::string reads[3];
  const char *read_types[] = {"float4", "int4", "uint4"};
  for (UINT i = 0; i < std::size(reads); i++) {
    auto d = defaults;
    for (auto &define : d)
      if (define.rfind("READ_TYPE=", 0) == 0)
        define = std::string("READ_TYPE=") + read_types[i];
    reads[i] = compiler.compile(hlsl, "cs", "cs", d);
    if (!expect(!reads[i].empty(), "readback HLSL did not compile"))
      return verdict();
  }

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto draw_rs = root_signature(device.Get(), {});
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].DescriptorTable = {1, &range};
  auto read_rs = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  if (!expect(draw_rs && read_rs, "root signatures were not created"))
    return verdict();
  ComPtr<ID3D12PipelineState> cs[3];
  for (UINT i = 0; i < std::size(cs); i++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{read_rs.Get(), bytecode(reads[i])};
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&cs[i])));
  }
  const UINT result_bytes = SIDE * SIDE * SAMPLES * sizeof(UINT[4]);
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto sampled = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(result && sampled, "sample buffers were not created"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> views, depth_views, srvs;
  D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, TARGETS},
      depth_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1},
      srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&depth_desc, IID_PPV_ARGS(&depth_views)));
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs)));
  auto dsv = depth_views->GetCPUDescriptorHandleForHeapStart();
  auto srv = srvs->GetCPUDescriptorHandleForHeapStart();
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

  for (UINT cycle = 0; cycle < 2; cycle++)
    for (UINT kind = 0; kind < codes.size(); kind++)
      for (UINT mode = 0; mode < MODES; mode++)
        for (UINT slot = 0; slot < (mode ? 1 : TARGETS); slot++)
          for (UINT view = 0; view <= Uint; view++) {
            auto target_kind = Kind(cycle ? Uint - view : view);
            bool dual = mode == Dual, multisample = mode == PixelFrequency || mode == SampleFrequency ||
                mode == AlphaToCoverage || mode == CoverageOverridesAlpha;
            if (dual && (target_kind == Sint || target_kind == Uint))
              continue;
            step("cycle %u, output %u, view %u, slot %u, mode %u", cycle, kind, target_kind, slot, mode);
            UINT other_slot = (slot + 1) % TARGETS, samples = multisample ? SAMPLES : 1;
            UINT components = mode == Fewer ? 1 : mode == Mask ? 2 : 4;
            Format formats[] = {format(target_kind, mode == Narrow ? 1 : 4),
                                format(Kind((kind + 1) % codes.size() == 0   ? Float
                                            : (kind + 1) % codes.size() == 1 ? Sint
                                                                             : Uint),
                                       4)};
            // the widest target of the kind that the device multisamples: Apple GPUs have no multisampled 128-bit
            // integer format (Metal feature set tables)
            if (multisample)
              for (auto &wide : formats)
                for (UINT fewer = wide.components; fewer; fewer /= 2) {
                  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format(wide.kind, fewer).format, samples};
                  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality,
                                                            sizeof(quality))) &&
                      quality.NumQualityLevels) {
                    wide = format(wide.kind, fewer);
                    break;
                  }
                }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = draw_rs.Get();
            desc.VS = bytecode(vertex);
            desc.PS = bytecode(codes[kind][mode][slot]);
            if (mode == Geometry)
              desc.GS = bytecode(geometry);
            if (mode == Tessellation) {
              desc.HS = bytecode(hull);
              desc.DS = bytecode(domain);
            }
            desc.NumRenderTargets = dual ? 1 : std::max(slot, other_slot) + 1;
            desc.RTVFormats[slot] = formats[0].format;
            if (!dual)
              desc.RTVFormats[other_slot] = formats[1].format;
            desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
            desc.DepthStencilState.DepthEnable = mode == DepthPresent;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            desc.DSVFormat = mode == DepthPresent ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
            desc.PrimitiveTopologyType =
                mode == Tessellation ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.SampleDesc = {samples, 0};
            desc.SampleMask = ~0u;
            if (mode == PixelFrequency || mode == AlphaToCoverage || mode == CoverageOverridesAlpha) {
              desc.SampleMask = 0;
              for (UINT s = 0; s < samples; s += 2)
                desc.SampleMask |= 1u << s;
            }
            desc.BlendState.IndependentBlendEnable = !dual;
            desc.BlendState.AlphaToCoverageEnable = mode == AlphaToCoverage || mode == CoverageOverridesAlpha;
            for (auto &rt : desc.BlendState.RenderTarget) {
              rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
              rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
              rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
              rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            auto &blend = desc.BlendState.RenderTarget[slot];
            blend.RenderTargetWriteMask = mode == ZeroMask ? 0
                                          : mode == Mask ? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_ALPHA
                                                         : D3D12_COLOR_WRITE_ENABLE_ALL;
            blend.BlendEnable = mode == Blend || dual;
            blend.SrcBlend = dual ? D3D12_BLEND_SRC1_COLOR : D3D12_BLEND_BLEND_FACTOR;
            blend.SrcBlendAlpha = dual ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_BLEND_FACTOR;
            ComPtr<ID3D12PipelineState> pso;
            HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
            if (mode == Blend && (target_kind == Sint || target_kind == Uint)) {
              expect(hr == E_INVALIDARG && !pso, "integer blending returned %08lx", hr);
              continue;
            }
            if (!expect(hr == S_OK && pso, "pipeline returned %08lx", hr))
              continue;
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pso.Get()));
            ComPtr<ID3D12Resource> textures[2], depth;
            D3D12_CPU_DESCRIPTOR_HANDLE bound[TARGETS];
            for (UINT i = 0; i < TARGETS; i++) {
              bound[i] = views->GetCPUDescriptorHandleForHeapStart();
              bound[i].ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
              D3D12_RENDER_TARGET_VIEW_DESC null_view{
                  DXGI_FORMAT_R32_FLOAT, multisample ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D};
              device->CreateRenderTargetView(nullptr, &null_view, bound[i]);
            }
            for (UINT i = 0; i < (dual ? 1u : UINT(std::size(textures))); i++) {
              if (multisample) {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{formats[i].format, samples};
                CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
                if (!expect(quality.NumQualityLevels != 0, "format %u has no %u-sample target", formats[i].format,
                            samples))
                  return verdict();
              }
              D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                               0,
                                               SIDE,
                                               SIDE,
                                               1,
                                               1,
                                               formats[i].format,
                                               {samples, 0},
                                               D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
              CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                    IID_PPV_ARGS(&textures[i])));
              device->CreateRenderTargetView(textures[i].Get(), nullptr, bound[i ? other_slot : slot]);
              const float clear[4] = {};
              list->ClearRenderTargetView(bound[i ? other_slot : slot], clear, 0, nullptr);
            }
            if (mode == DepthPresent) {
              D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                               0,
                                               SIDE,
                                               SIDE,
                                               1,
                                               1,
                                               DXGI_FORMAT_D32_FLOAT,
                                               {samples, 0},
                                               D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
              CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                    D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
              device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
              list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            }
            list->SetGraphicsRootSignature(draw_rs.Get());
            list->OMSetRenderTargets(desc.NumRenderTargets, bound, FALSE, mode == DepthPresent ? &dsv : nullptr);
            D3D12_VIEWPORT viewport{0, 0, float(SIDE), float(SIDE), 0, 1};
            D3D12_RECT scissor{0, 0, LONG(SIDE), LONG(SIDE)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(mode == Tessellation ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                              : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const float factor[4] = {0.5, 0.5, 0.5, 0.5};
            list->OMSetBlendFactor(factor);
            list->DrawInstanced(3, 1, 0, 0);
            ComPtr<ID3D12Resource> copied[2];
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
            for (UINT i = 0; i < (dual ? 1u : UINT(std::size(textures))); i++) {
              if (multisample) {
                transition(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                D3D12_SHADER_RESOURCE_VIEW_DESC view_desc{formats[i].format, D3D12_SRV_DIMENSION_TEXTURE2DMS,
                                                          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
                device->CreateShaderResourceView(textures[i].Get(), &view_desc, srv);
                ID3D12DescriptorHeap *heaps[] = {srvs.Get()};
                list->SetDescriptorHeaps(1, heaps);
                list->SetComputeRootSignature(read_rs.Get());
                list->SetComputeRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
                list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
                list->SetPipelineState(cs[formats[i].kind == Sint ? 1 : formats[i].kind == Uint ? 2 : 0].Get());
                list->Dispatch(SIDE, SIDE, 1);
                transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyResource(sampled.Get(), result.Get());
                transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                CHECK(submit(device.Get(), queue.Get(), list.Get()));
                CHECK(allocator->Reset());
                CHECK(list->Reset(allocator.Get(), nullptr));
                copied[i] = sampled;
              } else {
                auto texture_desc = textures[i]->GetDesc();
                UINT64 bytes;
                device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprints[i], nullptr, nullptr, &bytes);
                copied[i] = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
                if (!expect(bool(copied[i]), "readback buffer was not created"))
                  return verdict();
                CHECK(forget(copied[i].Get()));
                transition(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION from{textures[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                    to{copied[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                to.PlacedFootprint = footprints[i];
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                CHECK(submit(device.Get(), queue.Get(), list.Get()));
                CHECK(allocator->Reset());
                CHECK(list->Reset(allocator.Get(), nullptr));
              }
              const char *bytes;
              CHECK(copied[i]->Map(0, nullptr, (void **)&bytes));
              UINT pitch = multisample ? SIDE * samples * sizeof(UINT[4]) : footprints[i].Footprint.RowPitch;
              bytes += footprints[i].Offset;
              for (UINT y = 0; y < SIDE; y++)
                for (UINT x = 0; x < SIDE; x++)
                  for (UINT s = 0; s < samples; s++) {
                    float colour[] = {(1 + (x + (mode == SampleFrequency ? s : 0)) % 3) * 0.25f, -0.5f, 0.5f, 1};
                    if (mode == AlphaToCoverage || mode == CoverageOverridesAlpha)
                      colour[3] = alphas[(y * SIDE + x) % std::size(alphas)];
                    const float secondary[] = {0.5, 0.25, 1, 0.75};
                    for (UINT c = 0; c < formats[i].components; c++) {
                      bool written = (i || (blend.RenderTargetWriteMask >> c & 1)) && (desc.SampleMask >> s & 1);
                      if (mode == AlphaToCoverage && !(colour[3] > 0)) written = false;
                      if (!i && c >= components && written)
                        continue;
                      float value = i ? secondary[c] : colour[c];
                      if (!i && dual)
                        value *= secondary[c];
                      else if (!i && mode == Blend)
                        value *= factor[c];
                      UINT got = 0;
                      UINT stride = multisample ? sizeof(UINT[4]) : formats[i].components * formats[i].bits / CHAR_BIT;
                      const char *at = bytes + y * pitch + (x * samples + s) * stride;
                      memcpy(&got, at + c * (multisample ? sizeof(UINT) : formats[i].bits / CHAR_BIT),
                             multisample ? sizeof(UINT) : formats[i].bits / CHAR_BIT);
                      expect(matches(formats[i], got, written ? value : 0, c, multisample),
                             "target %u pixel %u,%u sample %u channel %u has %#x, expected %#x", i, x, y, s, c, got,
                             stored(formats[i], written ? value : 0, c));
                    }
                  }
              copied[i]->Unmap(0, nullptr);
            }
            if (depth) {
              auto texture_desc = depth->GetDesc();
              D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
              UINT64 bytes;
              device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
              auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
              if (!expect(bool(readback), "depth readback buffer was not created"))
                return verdict();
              CHECK(forget(readback.Get()));
              transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
              D3D12_TEXTURE_COPY_LOCATION from{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                  to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
              to.PlacedFootprint = footprint;
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
              CHECK(submit(device.Get(), queue.Get(), list.Get()));
              const char *data;
              CHECK(readback->Map(0, nullptr, (void **)&data));
              for (UINT y = 0; y < SIDE; y++)
                for (UINT x = 0; x < SIDE; x++)
                  expect(((const float *)(data + footprint.Offset + y * footprint.Footprint.RowPitch))[x] == 0.75f,
                         "depth at %u,%u is not the shader's 0.75", x, y);
              readback->Unmap(0, nullptr);
              CHECK(allocator->Reset());
              CHECK(list->Reset(allocator.Get(), nullptr));
            }
            CHECK(list->Close());
          }
  return verdict();
}
