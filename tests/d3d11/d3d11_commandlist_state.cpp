// contract: lists start from defaults, and Finish/Execute restore TRUE preserves the caller's complete state;
// FALSE and ClearState restore defaults without undoing resource writes. "No Immediate Context state (such as
// bound render targets nor shaders) can be inherited by the Command List" (D3D11.3 6.3.3). "Command Lists can call
// Command Lists, i.e. Execute can be called on a Deferred Context" (6.3.7). Source: D3D11_3_FunctionalSpec.htm.
// Microsoft Learn, ID3D11DeviceContext::FinishCommandList: "this parameter affects the command list of the next
// call to FinishCommandList on the same deferred context"; ExecuteCommandList: "Use TRUE to indicate that the
// runtime needs to save and restore the state." D3D11.3 6.3.4: "The state of the Context will revert to the default
// Context state"; ClearState: "The primitive topology is set to UNDEFINED."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-finishcommandlist
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-clearstate
// NULL objects and empty viewport/scissor arrays follow ClearState; numeric defaults use D3D11_DEFAULT_* (32.1).
// VSGetConstantBuffers1: "If no buffer is bound at a slot, pFirstConstant and pNumConstants are NULL for that slot."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-vsgetconstantbuffers1
// that text is ambiguous for UINT arrays: Wine's dlls/d3d11/tests/d3d11.c, test_constant_buffer_offset, records
// Windows returning first=0/count=4096 for unbound slots and buffers bound without a range. the count is
// D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT (D3D11.3 5.3.4.3.2), not the buffer's allocation size.
// ID3D11CommandList::GetContextFlags: "The context flag is reserved for future use and is always 0."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11commandlist-getcontextflags
// CreateDeferredContext: "Reserved for future use. Pass 0.", "CreateDeferredContext returns DXGI_ERROR_INVALID_CALL"
// for D3D11_CREATE_DEVICE_SINGLETHREADED, and "Returns E_INVALIDARG if the ContextFlags parameter is invalid."
// all versioned contexts must represent that context.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createdeferredcontext
// CreateDeferredContext3 specifies the same errors for invalid ContextFlags and a single-threaded device.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11device3-createdeferredcontext3
// every legal binding slot is checked, including SRVs on both sides of the implementation's 64-bit masks.
// draws add the VB word, VS/GS/PS constants, sampled texels and shader tags; dispatches add the CS equivalents.
// "Fetching from a ConstantBuffer slot with no Buffer present always returns 0" (D3D11.3 7.5).
// "Sampling from a slot with no texture bound returns 0 in all components." (7.18.17). each operation copies both
// destinations, so a missing, inherited or misdirected write is visible. at u=1.25, a two-texel texture samples
// its first texel with WRAP and last with CLAMP: "t = floor(scaledU)" (7.18.7), "scaledU = scaledU % numTexelsU;"
// and "scaledU = max( 0, min( scaledU, numTexelsU - 1 ) );" (7.18.9).
// readback copies disable predication after its snapshot: "CopyResource" honors predication (D3D11.3 20.2).
#include "d3d11_test.hpp"
#include <d3d11_3.h>
#include <algorithm>
#include <array>
#include <climits>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer Params : register(CB_REGISTER) { uint4 constants; };
Texture2D<float> source : register(SRV_REGISTER);
SamplerState nearest : register(SAMPLER_REGISTER);
RWBuffer<uint> result : register(u0);
struct Vertex { float4 position : SV_Position; nointerpolation uint value : VALUE; };
Vertex vs(uint word : WORD, uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  Vertex v;
  v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  v.value = word + constants.x + uint(source.SampleLevel(nearest, float2(1.25, 0.5), 0)) + TAG;
  return v;
}
uint ps(Vertex v) : SV_Target {
  return v.value + constants.x + uint(source.SampleLevel(nearest, float2(1.25, 0.5), 0)) + TAG;
}
[maxvertexcount(3)] void gs(triangle Vertex vertices[3], inout TriangleStream<Vertex> stream) {
  for (uint i = 0; i < 3; i++) {
    Vertex v = vertices[i];
    v.value += constants.x + uint(source.SampleLevel(nearest, float2(1.25, 0.5), 0)) + TAG;
    stream.Append(v);
  }
}
struct Control { float4 position : POSITION; };
struct Factors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<Control, 3> vertices) {
  Factors f; f.edges[0] = f.edges[1] = f.edges[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("factors")]
Control hs(InputPatch<Control, 3> vertices, uint id : SV_OutputControlPointID) { return vertices[id]; }
[domain("tri")]
float4 ds(Factors f, float3 uv : SV_DomainLocation, const OutputPatch<Control, 3> vertices) : SV_Position {
  return vertices[0].position * uv.x + vertices[1].position * uv.y + vertices[2].position * uv.z;
}
[numthreads(1, 1, 1)] void cs() {
  result[0] = constants.x + uint(source.SampleLevel(nearest, float2(1.25, 0.5), 0)) + TAG;
}
)hlsl";

int
main() {
  step("compile all six stages before creating the device");
  const char *stages[] = {"vs", "ps", "gs", "hs", "ds", "cs"};
  const UINT cb_slot = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1;
  const UINT srv_slot = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1;
  const UINT sampler_slot = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1;
  const std::vector<std::string> registers{"CB_REGISTER=b" + std::to_string(cb_slot),
      "SRV_REGISTER=t" + std::to_string(srv_slot), "SAMPLER_REGISTER=s" + std::to_string(sampler_slot)};
  ComPtr<ID3DBlob> code[2][std::size(stages)];
  for (UINT p = 0; p < std::size(code); p++)
    for (UINT s = 0; s < std::size(stages); s++) {
      auto defines = registers;
      defines.push_back("TAG=" + std::to_string(p + 1));
      code[p][s] = compile(hlsl, stages[s], stages[s], defines);
      if (!expect(!!code[p][s], "%s variant %u did not compile", stages[s], p))
        return verdict();
    }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                        &device, nullptr, &immediate));
  ComPtr<ID3D11Device3> device3;
  ComPtr<ID3D11DeviceContext1> context;
  CHECK(device.As(&device3));
  CHECK(immediate.As(&context));
  ComPtr<ID3D11DeviceContext1> v1;
  ComPtr<ID3D11DeviceContext2> v2;
  ComPtr<ID3D11DeviceContext3> v3;
  device3->GetImmediateContext1(&v1);
  device3->GetImmediateContext2(&v2);
  device3->GetImmediateContext3(&v3);
  step("versioned immediate contexts have the same COM identity");
  ComPtr<IUnknown> identity;
  CHECK(immediate.As(&identity));
  for (ID3D11DeviceContext *c : {static_cast<ID3D11DeviceContext *>(v1.Get()),
                               static_cast<ID3D11DeviceContext *>(v2.Get()),
                               static_cast<ID3D11DeviceContext *>(v3.Get())}) {
    ComPtr<IUnknown> got;
    CHECK(c->QueryInterface(IID_PPV_ARGS(&got)));
    expect(got == identity && c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE && c->GetContextFlags() == 0,
           "versioned immediate identity, type or flags differ");
  }
  ComPtr<ID3D11DeviceContext> base;
  CHECK(device3->CreateDeferredContext(0, &base));
  CHECK(device3->CreateDeferredContext1(0, v1.ReleaseAndGetAddressOf()));
  CHECK(device3->CreateDeferredContext2(0, v2.ReleaseAndGetAddressOf()));
  CHECK(device3->CreateDeferredContext3(0, v3.ReleaseAndGetAddressOf()));
  std::array<ComPtr<ID3D11DeviceContext1>, 4> deferred;
  CHECK(base.As(&deferred[0]));
  deferred[1] = v1;
  CHECK(v2.As(&deferred[2]));
  CHECK(v3.As(&deferred[3]));
  for (UINT i = 0; i < deferred.size(); i++) {
    step("deferred context version %u identity, type and flags", i);
    ComPtr<IUnknown> got;
    CHECK(deferred[i].As(&got));
    expect(got != identity && deferred[i]->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED &&
               deferred[i]->GetContextFlags() == 0, "deferred identity, type or flags differ");
    for (UINT j = 0; j < i; j++) {
      ComPtr<IUnknown> other;
      CHECK(deferred[j].As(&other));
      expect(got != other, "two CreateDeferredContext calls returned the same context");
    }
  }

  step("all four factories reject nonzero reserved flags and a single-threaded device");
  ComPtr<ID3D11Device> single;
  ComPtr<ID3D11Device3> single3;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
                        &level, 1, D3D11_SDK_VERSION, &single, nullptr, nullptr));
  CHECK(single.As(&single3));
  for (bool single_threaded : {false, true}) {
    ID3D11Device3 *d = single_threaded ? single3.Get() : device3.Get();
    const UINT flags = single_threaded ? 0 : 1;
    const HRESULT want = single_threaded ? DXGI_ERROR_INVALID_CALL : E_INVALIDARG;
    ComPtr<ID3D11DeviceContext> c0;
    ComPtr<ID3D11DeviceContext1> c1;
    ComPtr<ID3D11DeviceContext2> c2;
    ComPtr<ID3D11DeviceContext3> c3;
    const HRESULT results[] = {d->CreateDeferredContext(flags, &c0), d->CreateDeferredContext1(flags, &c1),
                               d->CreateDeferredContext2(flags, &c2), d->CreateDeferredContext3(flags, &c3)};
    for (UINT version = 0; version < std::size(results); version++)
      expect(results[version] == want, "version %u single-threaded=%d HRESULT %#lx, want %#lx",
             version, single_threaded, results[version], want);
  }

  struct State {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb, ib, cb, so[D3D11_SO_BUFFER_SLOT_COUNT], output;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RenderTargetView> rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> cuav[D3D11_1_UAV_SLOT_COUNT], ouav[D3D11_1_UAV_SLOT_COUNT];
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11Predicate> predicate;
    UINT stride, offset, index_offset, first, count, stencil, mask, rectangles;
    DXGI_FORMAT index_format;
    FLOAT factors[4];
    D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT rect[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  } state[2]{};
  const UINT width = std::size(state) + 1;
  const UINT mask_bits = sizeof(uint64_t) * CHAR_BIT;
  const UINT srv_slots[] = {0, mask_bits - 1, mask_bits, srv_slot};
  auto srv_bound = [&](UINT slot) {
    return std::find(std::begin(srv_slots), std::end(srv_slots), slot) != std::end(srv_slots);
  };
  auto uav_bound = [&](UINT slot) {
    return slot == 0 || slot == D3D11_PS_CS_UAV_REGISTER_COUNT - 1 ||
           slot == D3D11_PS_CS_UAV_REGISTER_COUNT || slot == D3D11_1_UAV_SLOT_COUNT - 1;
  };
  // D3D11.3 5.3.4.3: "FirstConstant must be a multiple of 16 constants."
  const UINT range = 16, rows = (std::size(state) + 1) * range;
  for (UINT p = 0; p < std::size(state); p++) {
    auto &s = state[p];
    auto shader = [&](UINT stage) { return code[p][stage]->GetBufferPointer(); };
    auto bytes = [&](UINT stage) { return code[p][stage]->GetBufferSize(); };
    CHECK(device->CreateVertexShader(shader(0), bytes(0), nullptr, &s.vs));
    CHECK(device->CreatePixelShader(shader(1), bytes(1), nullptr, &s.ps));
    CHECK(device->CreateGeometryShader(shader(2), bytes(2), nullptr, &s.gs));
    CHECK(device->CreateHullShader(shader(3), bytes(3), nullptr, &s.hs));
    CHECK(device->CreateDomainShader(shader(4), bytes(4), nullptr, &s.ds));
    CHECK(device->CreateComputeShader(shader(5), bytes(5), nullptr, &s.cs));
    D3D11_INPUT_ELEMENT_DESC element{"WORD", 0, DXGI_FORMAT_R32_UINT,
                                    D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1, p * UINT(sizeof(UINT)),
                                    D3D11_INPUT_PER_VERTEX_DATA, 0};
    CHECK(device->CreateInputLayout(&element, 1, shader(0), bytes(0), &s.layout));
    s.stride = (p + 1) * sizeof(UINT);
    s.offset = (p + 2) * sizeof(UINT);
    s.index_offset = (p + 3) * sizeof(UINT);
    const UINT indices[] = {0, 1, 2};
    const USHORT short_indices[] = {0, 1, 2};
    s.index_format = p ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    s.vb = buffer(device.Get(), s.offset + std::size(indices) * s.stride + element.AlignedByteOffset,
                  D3D11_BIND_VERTEX_BUFFER, p + 1);
    const UINT index_size = p ? sizeof(short_indices) : sizeof(indices);
    D3D11_BUFFER_DESC ib{index_size + s.index_offset, D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
    std::vector<BYTE> index_bytes(ib.ByteWidth);
    const void *index_source = p ? static_cast<const void *>(short_indices) : static_cast<const void *>(indices);
    memcpy(index_bytes.data() + s.index_offset, index_source, index_size);
    D3D11_SUBRESOURCE_DATA index_data{index_bytes.data()};
    CHECK(device->CreateBuffer(&ib, &index_data, &s.ib));
    std::vector<std::array<UINT, 4>> constants(rows);
    for (UINT row = 0; row < rows; row++)
      constants[row].fill(row + p + 1);
    D3D11_BUFFER_DESC cb{UINT(constants.size() * sizeof(constants[0])), D3D11_USAGE_DEFAULT,
                         D3D11_BIND_CONSTANT_BUFFER};
    D3D11_SUBRESOURCE_DATA cb_data{constants.data()};
    CHECK(device->CreateBuffer(&cb, &cb_data, &s.cb));
    s.first = (p + 1) * range;
    s.count = (p + 1) * range;
    if (!expect(!!s.vb, "vertex buffer creation failed"))
      return verdict();
    for (auto &so : s.so) {
      so = buffer(device.Get(), sizeof(indices), D3D11_BIND_STREAM_OUTPUT);
      if (!expect(!!so, "SO buffer creation failed"))
        return verdict();
    }
    const float texels[] = {float(p + 1), float(2 * (p + 1))};
    D3D11_TEXTURE2D_DESC tex{std::size(texels), 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA tex_data{texels, sizeof(texels)};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&tex, &tex_data, &texture));
    CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &s.srv));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        p ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(device->CreateSamplerState(&sampler, &s.sampler));
    tex.Width = width;
    tex.Format = DXGI_FORMAT_R32_UINT;
    tex.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (UINT slot = 0; slot < std::size(s.rtv); slot++) {
      CHECK(device->CreateTexture2D(&tex, nullptr, &texture));
      CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &s.rtv[slot]));
      if (!slot)
        s.target = texture;
    }
    tex.Format = DXGI_FORMAT_D32_FLOAT;
    tex.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(device->CreateTexture2D(&tex, nullptr, &texture));
    CHECK(device->CreateDepthStencilView(texture.Get(), nullptr, &s.dsv));
    for (UINT slot = 0; slot < D3D11_1_UAV_SLOT_COUNT; slot++) {
      if (!uav_bound(slot))
        continue;
      auto make = [&](ComPtr<ID3D11UnorderedAccessView> &view, bool compute) {
        auto b = buffer(device.Get(), sizeof(UINT), D3D11_BIND_UNORDERED_ACCESS);
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 1;
        if (!b)
          return E_FAIL;
        if (compute && !slot)
          s.output = b;
        return device->CreateUnorderedAccessView(b.Get(), &desc, &view);
      };
      CHECK(make(s.cuav[slot], true));
      if (slot >= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
        CHECK(make(s.ouav[slot], false));
    }
    D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE, BOOL(p)};
    raster.ScissorEnable = raster.DepthClipEnable = TRUE;
    CHECK(device->CreateRasterizerState(&raster, &s.raster));
    D3D11_BLEND_DESC blend{};
    blend.IndependentBlendEnable = p;
    for (auto &target : blend.RenderTarget) {
      target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
      target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
      target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
      target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    CHECK(device->CreateBlendState(&blend, &s.blend));
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = p;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    depth.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    depth.FrontFace = depth.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                      D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    CHECK(device->CreateDepthStencilState(&depth, &s.depth));
    D3D11_QUERY_DESC predicate{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    CHECK(device->CreatePredicate(&predicate, &s.predicate));
    immediate->Begin(s.predicate.Get());
    immediate->End(s.predicate.Get());
    s.rectangles = p ? 1 : std::size(s.viewport);
    for (UINT i = 0; i < s.rectangles; i++) {
      s.viewport[i] = {0, 0, float(width), 1, float(p) / 2, 1};
      s.rect[i] = {LONG(p), 0, LONG(p + 1), 1};
    }
    s.stencil = p + 1;
    s.mask = D3D11_DEFAULT_SAMPLE_MASK ^ (1u << (p + 1));
    for (UINT i = 0; i < std::size(s.factors); i++)
      s.factors[i] = float(p + i + 1) / (std::size(s.factors) + std::size(state));
  }

  const auto set_cb = std::array{
      &ID3D11DeviceContext1::VSSetConstantBuffers1,
      &ID3D11DeviceContext1::PSSetConstantBuffers1,
      &ID3D11DeviceContext1::GSSetConstantBuffers1,
      &ID3D11DeviceContext1::HSSetConstantBuffers1,
      &ID3D11DeviceContext1::DSSetConstantBuffers1,
      &ID3D11DeviceContext1::CSSetConstantBuffers1
  };
  const auto get_cb = std::array{
      &ID3D11DeviceContext1::VSGetConstantBuffers1,
      &ID3D11DeviceContext1::PSGetConstantBuffers1,
      &ID3D11DeviceContext1::GSGetConstantBuffers1,
      &ID3D11DeviceContext1::HSGetConstantBuffers1,
      &ID3D11DeviceContext1::DSGetConstantBuffers1,
      &ID3D11DeviceContext1::CSGetConstantBuffers1
  };
  step("all stages return default ranges for unranged and unbound buffers, with optional outputs");
  for (auto c : {context.Get(), deferred[0].Get()}) {
    for (UINT stage = 0; stage < set_cb.size(); stage++) {
      (c->*set_cb[stage])(cb_slot, 1, state[0].cb.GetAddressOf(), &state[0].first, &state[0].count);
      for (ID3D11Buffer *buffer : {state[0].cb.Get(), static_cast<ID3D11Buffer *>(nullptr)}) {
        (c->*set_cb[stage])(cb_slot, 1, &buffer, nullptr, nullptr);
        for (bool get_first : {false, true})
          for (bool get_count : {false, true}) {
            ComPtr<ID3D11Buffer> got;
            UINT first = ~0u, count = ~0u;
            (c->*get_cb[stage])(cb_slot, 1, &got, get_first ? &first : nullptr, get_count ? &count : nullptr);
            expect(got.Get() == buffer && first == (get_first ? 0 : ~0u) &&
                       count == (get_count ? D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT : ~0u),
                   "%s default constant-buffer range differs", stages[stage]);
          }
      }
    }
    c->ClearState();
  }
  const auto set_srv = std::array{
      &ID3D11DeviceContext::VSSetShaderResources,
      &ID3D11DeviceContext::PSSetShaderResources,
      &ID3D11DeviceContext::GSSetShaderResources,
      &ID3D11DeviceContext::HSSetShaderResources,
      &ID3D11DeviceContext::DSSetShaderResources,
      &ID3D11DeviceContext::CSSetShaderResources
  };
  const auto get_srv = std::array{
      &ID3D11DeviceContext::VSGetShaderResources,
      &ID3D11DeviceContext::PSGetShaderResources,
      &ID3D11DeviceContext::GSGetShaderResources,
      &ID3D11DeviceContext::HSGetShaderResources,
      &ID3D11DeviceContext::DSGetShaderResources,
      &ID3D11DeviceContext::CSGetShaderResources
  };
  const auto set_sampler = std::array{
      &ID3D11DeviceContext::VSSetSamplers,
      &ID3D11DeviceContext::PSSetSamplers,
      &ID3D11DeviceContext::GSSetSamplers,
      &ID3D11DeviceContext::HSSetSamplers,
      &ID3D11DeviceContext::DSSetSamplers,
      &ID3D11DeviceContext::CSSetSamplers
  };
  const auto get_sampler = std::array{
      &ID3D11DeviceContext::VSGetSamplers,
      &ID3D11DeviceContext::PSGetSamplers,
      &ID3D11DeviceContext::GSGetSamplers,
      &ID3D11DeviceContext::HSGetSamplers,
      &ID3D11DeviceContext::DSGetSamplers,
      &ID3D11DeviceContext::CSGetSamplers
  };
  auto bind = [&](ID3D11DeviceContext1 *c, UINT p, bool inputs = true) {
    auto &s = state[p];
    c->VSSetShader(s.vs.Get(), nullptr, 0);
    c->PSSetShader(s.ps.Get(), nullptr, 0);
    c->GSSetShader(s.gs.Get(), nullptr, 0);
    c->HSSetShader(s.hs.Get(), nullptr, 0);
    c->DSSetShader(s.ds.Get(), nullptr, 0);
    c->CSSetShader(s.cs.Get(), nullptr, 0);
    for (UINT stage = 0; stage < set_cb.size(); stage++) {
      bool present = inputs || (stage != 1 && stage != 5);
      for (UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; slot++) {
        ID3D11Buffer *b = present && (slot == 0 || slot == cb_slot) ? s.cb.Get() : nullptr;
        (c->*set_cb[stage])(slot, 1, &b, &s.first, &s.count);
      }
      for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; slot++) {
        ID3D11ShaderResourceView *v = present && srv_bound(slot) ? s.srv.Get() : nullptr;
        (c->*set_srv[stage])(slot, 1, &v);
      }
      for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; slot++) {
        ID3D11SamplerState *v = present && (slot == 0 || slot == sampler_slot) ? s.sampler.Get() : nullptr;
        (c->*set_sampler[stage])(slot, 1, &v);
      }
    }
    c->IASetInputLayout(s.layout.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; slot++) {
      bool present = slot == 0 || slot == D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1;
      ID3D11Buffer *b = present ? s.vb.Get() : nullptr;
      const UINT stride = present ? s.stride : 0, offset = present ? s.offset : 0;
      c->IASetVertexBuffers(slot, 1, &b, &stride, &offset);
    }
    c->IASetIndexBuffer(s.ib.Get(), s.index_format, s.index_offset);
    ID3D11Buffer *so[D3D11_SO_BUFFER_SLOT_COUNT];
    UINT offsets[D3D11_SO_BUFFER_SLOT_COUNT];
    for (UINT slot = 0; slot < std::size(so); slot++) {
      so[slot] = s.so[slot].Get();
      offsets[slot] = p * sizeof(UINT);
    }
    c->SOSetTargets(std::size(so), so, offsets);
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11UnorderedAccessView *uav[D3D11_1_UAV_SLOT_COUNT];
    for (UINT slot = 0; slot < std::size(rtv); slot++) rtv[slot] = s.rtv[slot].Get();
    for (UINT slot = 0; slot < std::size(uav); slot++) uav[slot] = uav_bound(slot) ? s.ouav[slot].Get() : nullptr;
    c->OMSetRenderTargetsAndUnorderedAccessViews(std::size(rtv), rtv, s.dsv.Get(), std::size(rtv),
        std::size(uav) - std::size(rtv), uav + std::size(rtv), nullptr);
    for (UINT slot = 0; slot < std::size(uav); slot++) uav[slot] = uav_bound(slot) ? s.cuav[slot].Get() : nullptr;
    c->CSSetUnorderedAccessViews(0, std::size(uav), uav, nullptr);
    c->RSSetState(s.raster.Get());
    c->RSSetViewports(s.rectangles, s.viewport);
    c->RSSetScissorRects(s.rectangles, s.rect);
    c->OMSetBlendState(s.blend.Get(), s.factors, s.mask);
    c->OMSetDepthStencilState(s.depth.Get(), s.stencil);
    c->SetPredication(s.predicate.Get(), p != 0);
  };
  auto snapshot = [&](ID3D11DeviceContext1 *c, int p) {
    State *s = p < 0 ? nullptr : &state[p];
    auto shader = [&](auto getter, auto &wanted) {
      auto got = wanted;
      UINT classes = 1;
      (c->*getter)(got.ReleaseAndGetAddressOf(), nullptr, &classes);
      expect(got.Get() == (s ? wanted.Get() : nullptr) && classes == 0, "shader or class count differs");
    };
    auto &want = state[p < 0 ? 0 : p];
    shader(&ID3D11DeviceContext::VSGetShader, want.vs);
    shader(&ID3D11DeviceContext::PSGetShader, want.ps);
    shader(&ID3D11DeviceContext::GSGetShader, want.gs);
    shader(&ID3D11DeviceContext::HSGetShader, want.hs);
    shader(&ID3D11DeviceContext::DSGetShader, want.ds);
    shader(&ID3D11DeviceContext::CSGetShader, want.cs);
    for (UINT stage = 0; stage < get_cb.size(); stage++) {
      bool bindings = true, ranges = true;
      for (UINT slot = 0; slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; slot++) {
        ComPtr<ID3D11Buffer> b;
        UINT first = ~0u, count = ~0u;
        (c->*get_cb[stage])(slot, 1, &b, &first, &count);
        bool present = s && (slot == 0 || slot == cb_slot);
        bindings &= b.Get() == (present ? s->cb.Get() : nullptr);
        ranges &= first == (present ? s->first : 0) &&
                  count == (present ? s->count : D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT);
      }
      expect(bindings, "%s constant-buffer identities differ", stages[stage]);
      expect(ranges, "%s constant-buffer ranges differ", stages[stage]);
      for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; slot++) {
        ComPtr<ID3D11ShaderResourceView> v;
        (c->*get_srv[stage])(slot, 1, &v);
        expect(v.Get() == (s && srv_bound(slot) ? s->srv.Get() : nullptr),
               "%s SRV slot %u differs", stages[stage], slot);
      }
      for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; slot++) {
        ComPtr<ID3D11SamplerState> v;
        (c->*get_sampler[stage])(slot, 1, &v);
        expect(v.Get() == (s && (slot == 0 || slot == sampler_slot) ? s->sampler.Get() : nullptr),
               "%s sampler slot %u differs", stages[stage], slot);
      }
    }
    ComPtr<ID3D11InputLayout> layout;
    c->IAGetInputLayout(&layout);
    expect(layout.Get() == (s ? s->layout.Get() : nullptr), "input layout differs");
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    c->IAGetPrimitiveTopology(&topology);
    expect(topology == (s ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),
           "topology differs");
    for (UINT slot = 0; slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; slot++) {
      ComPtr<ID3D11Buffer> b;
      UINT stride = ~0u, offset = ~0u;
      c->IAGetVertexBuffers(slot, 1, &b, &stride, &offset);
      bool present = s && (slot == 0 || slot == D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1);
      expect(b.Get() == (present ? s->vb.Get() : nullptr) &&
                 stride == (present ? s->stride : 0) && offset == (present ? s->offset : 0),
             "VB slot %u differs", slot);
    }
    ComPtr<ID3D11Buffer> ib;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT offset = ~0u;
    c->IAGetIndexBuffer(&ib, &format, &offset);
    expect(ib.Get() == (s ? s->ib.Get() : nullptr) && format == (s ? s->index_format : DXGI_FORMAT_UNKNOWN) &&
               offset == (s ? s->index_offset : D3D11_IA_DEFAULT_INDEX_BUFFER_OFFSET_IN_BYTES), "index buffer differs");
    ID3D11Buffer *so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
    c->SOGetTargets(std::size(so), so);
    for (UINT slot = 0; slot < std::size(so); slot++) {
      expect(so[slot] == (s ? s->so[slot].Get() : nullptr), "SO slot %u differs", slot);
      if (so[slot])
        so[slot]->Release();
    }
    ID3D11RenderTargetView *views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    c->OMGetRenderTargets(std::size(views), views, nullptr);
    for (UINT i = 0; i < std::size(views); i++) {
      expect(views[i] == (s ? s->rtv[i].Get() : nullptr), "RTV slot %u differs", i);
      if (views[i])
        views[i]->Release();
    }
    ComPtr<ID3D11DepthStencilView> dsv;
    c->OMGetRenderTargets(0, nullptr, &dsv);
    expect(dsv.Get() == (s ? s->dsv.Get() : nullptr), "DSV differs");
    for (UINT slot = 0; slot < D3D11_1_UAV_SLOT_COUNT; slot++) {
      ComPtr<ID3D11UnorderedAccessView> cv, ov;
      c->CSGetUnorderedAccessViews(slot, 1, &cv);
      c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, slot, 1, &ov);
      expect(cv.Get() == (s && uav_bound(slot) ? s->cuav[slot].Get() : nullptr), "CS UAV slot %u differs", slot);
      expect(ov.Get() == (s && uav_bound(slot) && slot >= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
                               ? s->ouav[slot].Get() : nullptr),
             "OM UAV slot %u differs", slot);
    }
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    FLOAT factors[4] = {};
    UINT mask = 0, stencil = ~0u;
    c->RSGetState(&raster);
    c->OMGetBlendState(&blend, factors, &mask);
    c->OMGetDepthStencilState(&depth, &stencil);
    // D3D11.3 32.1 defines D3D11_DEFAULT_BLEND_FACTOR_{RED,GREEN,BLUE,ALPHA} as 1.0f
    const FLOAT default_blend_factor = 1.0f;
    const FLOAT defaults[] = {default_blend_factor, default_blend_factor, default_blend_factor, default_blend_factor};
    expect(raster.Get() == (s ? s->raster.Get() : nullptr) && blend.Get() == (s ? s->blend.Get() : nullptr) &&
               depth.Get() == (s ? s->depth.Get() : nullptr), "RS/blend/depth objects differ");
    expect(!memcmp(factors, s ? s->factors : defaults, sizeof(factors)) &&
               mask == (s ? s->mask : D3D11_DEFAULT_SAMPLE_MASK) &&
               stencil == (s ? s->stencil : D3D11_DEFAULT_STENCIL_REFERENCE),
           "blend factors, sample mask or stencil differ");
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT nv = std::size(viewports), nr = std::size(rects);
    c->RSGetViewports(&nv, viewports);
    c->RSGetScissorRects(&nr, rects);
    expect(nv == (s ? s->rectangles : 0) && nr == (s ? s->rectangles : 0), "viewport/scissor counts differ");
    for (UINT i = 0; i < std::size(viewports); i++) {
      const D3D11_VIEWPORT empty_viewport{};
      const D3D11_RECT empty_rect{};
      expect(!memcmp(&viewports[i], s ? &s->viewport[i] : &empty_viewport, sizeof(viewports[i])) &&
                 !memcmp(&rects[i], s ? &s->rect[i] : &empty_rect, sizeof(rects[i])), "viewport/scissor %u differs", i);
    }
    ComPtr<ID3D11Predicate> predicate;
    BOOL value = FALSE;
    c->GetPredication(&predicate, &value);
    expect(predicate.Get() == (s ? s->predicate.Get() : nullptr) && (!s || value == (p != 0)), "predication differs");
  };

  struct Capture {
    UINT p;
    bool inputs;
    std::string name;
    ComPtr<ID3D11Texture2D> pixels[2];
    ComPtr<ID3D11Buffer> words[2];
  };
  std::vector<Capture> captures;
  auto capture = [&](ID3D11DeviceContext1 *c, UINT p, bool inputs) -> HRESULT {
    captures.emplace_back();
    auto &out = captures.back();
    out.p = p;
    out.inputs = inputs;
    out.name = trace::doing;
    for (UINT i = 0; i < std::size(state); i++) {
      D3D11_TEXTURE2D_DESC td;
      state[i].target->GetDesc(&td);
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      HRESULT hr = device->CreateTexture2D(&td, nullptr, &out.pixels[i]);
      if (FAILED(hr))
        return hr;
      D3D11_BUFFER_DESC bd{sizeof(UINT), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
      hr = device->CreateBuffer(&bd, nullptr, &out.words[i]);
      if (FAILED(hr))
        return hr;
      c->CopyResource(out.pixels[i].Get(), state[i].target.Get());
      c->CopyResource(out.words[i].Get(), state[i].output.Get());
    }
    return S_OK;
  };
  auto output = [&](ID3D11DeviceContext1 *c, UINT p, bool inputs = true) -> HRESULT {
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->SetPredication(nullptr, FALSE);
    const FLOAT zero[4] = {};
    const UINT zeros[4] = {};
    for (UINT i = 0; i < std::size(state); i++) {
      c->ClearRenderTargetView(state[i].rtv[0].Get(), zero);
      c->ClearUnorderedAccessViewUint(state[i].cuav[0].Get(), zeros);
    }
    c->DrawIndexed(3, 0, 0);
    c->Dispatch(1, 1, 1);
    HRESULT hr = capture(c, p, inputs);
    c->HSSetShader(state[p].hs.Get(), nullptr, 0);
    c->DSSetShader(state[p].ds.Get(), nullptr, 0);
    c->SetPredication(state[p].predicate.Get(), p != 0);
    return hr;
  };
  for (UINT version = 0; version < deferred.size(); version++) {
    auto c = deferred[version].Get();
    step("version %u initial defaults", version);
    snapshot(c, -1);
    for (BOOL finish : {FALSE, TRUE})
      for (BOOL execute : {FALSE, TRUE}) {
        step("version %u Finish=%d Execute=%d record B", version, finish, execute);
        bind(c, 1);
        snapshot(c, 1);
        CHECK(output(c, 1));
        ComPtr<ID3D11CommandList> list, next;
        CHECK(c->FinishCommandList(finish, &list));
        expect(list->GetContextFlags() == 0, "command-list flags differ");
        snapshot(c, finish ? 1 : -1);
        step("version %u Finish=%d Execute=%d next list", version, finish, execute);
        if (!finish)
          bind(c, 1);
        CHECK(output(c, 1));
        CHECK(c->FinishCommandList(FALSE, &next));
        snapshot(c, -1);
        bind(context.Get(), 0);
        step("version %u Finish=%d Execute=%d control A", version, finish, execute);
        snapshot(context.Get(), 0);
        CHECK(output(context.Get(), 0));
        step("version %u Finish=%d Execute=%d execute B", version, finish, execute);
        context->ExecuteCommandList(list.Get(), execute);
        snapshot(context.Get(), execute ? 0 : -1);
        step("version %u Finish=%d Execute=%d after B", version, finish, execute);
        if (!execute)
          bind(context.Get(), 0);
        CHECK(output(context.Get(), 0));
        context->ExecuteCommandList(next.Get(), TRUE);
        snapshot(context.Get(), 0);
      }
  }
  auto inner_context = deferred[0].Get(), outer_context = deferred[1].Get();
  step("record inner B");
  bind(inner_context, 1);
  CHECK(output(inner_context, 1));
  ComPtr<ID3D11CommandList> inner;
  CHECK(inner_context->FinishCommandList(FALSE, &inner));
  for (BOOL finish : {FALSE, TRUE})
    for (BOOL nested : {FALSE, TRUE})
      for (BOOL execute : {FALSE, TRUE}) {
        step("nested Finish=%d inner Execute=%d outer Execute=%d", finish, nested, execute);
        bind(outer_context, 0);
        CHECK(output(outer_context, 0));
        outer_context->ExecuteCommandList(inner.Get(), nested);
        snapshot(outer_context, nested ? 0 : -1);
        outer_context->SetPredication(nullptr, FALSE);
        CHECK(capture(outer_context, 1, true));
        if (!nested)
          bind(outer_context, 0);
        CHECK(output(outer_context, 0));
        outer_context->ClearState();
        snapshot(outer_context, -1);
        bind(outer_context, 1);
        CHECK(output(outer_context, 1));
        ComPtr<ID3D11CommandList> outer;
        CHECK(outer_context->FinishCommandList(finish, &outer));
        snapshot(outer_context, finish ? 1 : -1);
        bind(context.Get(), 0);
        context->ExecuteCommandList(outer.Get(), execute);
        snapshot(context.Get(), execute ? 0 : -1);
        if (!execute)
          bind(context.Get(), 0);
        CHECK(output(context.Get(), 0));
        outer_context->ClearState();
        ComPtr<ID3D11CommandList> discarded;
        CHECK(outer_context->FinishCommandList(FALSE, &discarded));
      }
  for (BOOL restore : {FALSE, TRUE}) {
    step("immediate FinishCommandList restore=%d is invalid and preserves A", restore);
    bind(context.Get(), 0);
    ComPtr<ID3D11CommandList> invalid;
    HRESULT hr = context->FinishCommandList(restore, &invalid);
    expect(hr == DXGI_ERROR_INVALID_CALL, "immediate FinishCommandList returned %#lx", hr);
    snapshot(context.Get(), 0);
  }
  step("deferred ClearState keeps earlier writes and removes PS/CS resource inheritance");
  bind(inner_context, 0);
  CHECK(output(inner_context, 0));
  inner_context->ClearState();
  snapshot(inner_context, -1);
  bind(inner_context, 1, false);
  CHECK(output(inner_context, 1, false));
  ComPtr<ID3D11CommandList> cleared;
  CHECK(inner_context->FinishCommandList(FALSE, &cleared));
  bind(context.Get(), 0);
  context->ExecuteCommandList(cleared.Get(), TRUE);
  snapshot(context.Get(), 0);
  step("a list with unbound PS/CS inputs cannot inherit A's resources");
  bind(inner_context, 1, false);
  CHECK(output(inner_context, 1, false));
  ComPtr<ID3D11CommandList> no_inputs;
  CHECK(inner_context->FinishCommandList(FALSE, &no_inputs));
  bind(context.Get(), 0);
  CHECK(output(context.Get(), 0));
  context->ExecuteCommandList(no_inputs.Get(), TRUE);
  snapshot(context.Get(), 0);
  CHECK(output(context.Get(), 0));

  for (auto &out : captures) {
    step("read back %s", out.name.c_str());
    const auto &s = state[out.p];
    const UINT tag = out.p + 1, constant = s.first + tag, sampled = (out.p + 1) * tag, vertex = tag;
    const UINT pixel = vertex + (constant + sampled + tag) + (constant + sampled + tag) +
                       (out.inputs ? constant + sampled : 0) + tag;
    const UINT word = (out.inputs ? constant + sampled : 0) + tag;
    for (UINT i = 0; i < std::size(state); i++) {
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(context->Map(out.pixels[i].Get(), 0, D3D11_MAP_READ, 0, &mapped));
      auto pixels = static_cast<const UINT *>(mapped.pData);
      for (UINT x = 0; x < width; x++) {
        UINT want = i == out.p && x == out.p ? pixel : 0;
        expect(pixels[x] == want, "target %u pixel %u = %u, want %u", i, x, pixels[x], want);
      }
      context->Unmap(out.pixels[i].Get(), 0);
      CHECK(context->Map(out.words[i].Get(), 0, D3D11_MAP_READ, 0, &mapped));
      UINT got = *static_cast<const UINT *>(mapped.pData), want = i == out.p ? word : 0;
      expect(got == want, "UAV %u = %u, want %u", i, got, want);
      context->Unmap(out.words[i].Get(), 0);
    }
  }
  return verdict();
}
