// contract: changing one field of a binding to the same object changes what the next draw/dispatch reads, and
// graphics/compute/copy transitions preserve the bindings and their getters.
// D3D11.3 5.3.4.3.2: "FirstConstant must be a multiple of 16 constants." "NumConstants must be a multiple of 16
// constants, in the range [0..4096]." The bound buffer "will appear as if it starts at the specified
// "FirstConstant" offset (where 1 means 16 bytes) and has a size defined by NumConstants"; addresses beyond the
// resource "count as out of bounds reads from the shader, which is defined to return 0 for all components."
// D3D11.3 7.5: "Fetching from a ConstantBuffer slot with no Buffer present always returns 0".
// D3D11.3 3.4.6: "a point is represented as a square of width 1 oriented to the RenderTarget"; 15.7:
// "Any pixel outside these extents is discarded." D3D11.3 8.4.1/8.6.1 define the vertex address as the bound
// offset plus stride times the vertex index. D3D11.3 5.3.10.2: "Specifying -1 means maintain the current counter
// value already in the Buffer. Any other value sets the counter value." A COUNTER UAV preserves record order.
// Microsoft Learn, IASetVertexBuffers: "Each stride is the size (in bytes) of the elements".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-iasetvertexbuffers
// Microsoft Learn, VSGetConstantBuffers1: "Array of constant buffer interface pointers to be returned by the method."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-vsgetconstantbuffers1
// only explicitly set ranges have numeric getter expectations: the documentation describes default/unbound
// UINT outputs as "NULL", which does not define their numeric values. ordinary setters show the whole buffer
// (Microsoft Learn, VSSetConstantBuffers1: "binding the entire buffer into view").
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-vssetconstantbuffers1
// Microsoft Learn, OMSetBlendState: "A sample mask is always applied" and the runtime stores blend factors for
// OMGetBlendState even when the blend object does not use them.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetblendstate
// D3D11.3 8.16: "For Draw() and DrawInstanced(), VertexID starts at 0". a fixed binding of the vertex words
// places three points at the fetched vertices' pixel centres; changing the IB offset can select earlier tiles.
// their components are the fetched vertex word, VS constant, PS constant and sampled texel. constant reads are
// below, at and above the range end. compute records use the
// same probes and the returned counter index; every unwritten pixel/word is checked too.
// texels follow D3D11.3 7.18.7 ("t = floor(scaledU)") and 7.18.9: wrapping U=1.25 selects the first of two
// texels; clamping selects the last. sampling an unbound resource returns zero (7.18.17).
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <array>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer Rows : register(BREG) { uint4 rows[DECLARED]; };
Texture2D<float> tex : register(TREG);
SamplerState filtering : register(SREG);
RWStructuredBuffer<uint4> records : register(UREG);
struct V { float4 pos : SV_Position; nointerpolation uint3 ids : IDS; };
V vs(uint value : VALUE, uint place : PLACE) {
  V v;
  uint id = place - 1;
  uint x = id % WIDTH;
  v.pos = float4(2 * (float(x) + 0.5) / WIDTH - 1, 1 - 2 * (float(id / WIDTH) + 0.5) / HEIGHT, 0, 1);
  v.ids = uint3(value, rows[RANGE - 1 + x].x, x);
  return v;
}
uint4 ps(V v) : SV_Target {
  return uint4(v.ids.xy, rows[RANGE - 1 + v.ids.z].x, uint(tex.SampleLevel(filtering, float2(1.25, 0.5), 0)));
}
[numthreads(1, 1, 1)] void cs() {
  uint index = records.IncrementCounter();
  records[index] = uint4(rows[RANGE - 1].x, rows[RANGE].x, rows[RANGE + 1].x,
                        uint(tex.SampleLevel(filtering, float2(1.25, 0.5), 0)));
}
)hlsl";

int
main() {
  constexpr UINT range = 16; // D3D11.3 5.3.4.3.2, in constants, not bytes
  constexpr UINT width = 3, constant_rows = 3 * range;
  constexpr UINT cb_slot = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1;
  constexpr UINT vb_slot = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1;
  constexpr UINT srv_slot = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1;
  constexpr UINT sampler_slot = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1;
  constexpr UINT uav_slot = D3D11_PS_CS_UAV_REGISTER_COUNT - 1;
  using Word = std::array<UINT, 4>;
  enum Draw { Direct, Indexed, Instanced, IndexedInstanced, Indirect, IndexedIndirect };
  enum Boundary { None, Compute, Blit, ComputeBlit };
  struct Case {
    const char *name = "control";
    UINT stride = sizeof(UINT), offset = 0;
    UINT vs_first = 0, vs_count = 2 * range, ps_first = range, ps_count = 2 * range;
    bool vertex_bound = true, vs_bound = true, ps_bound = true;
    bool vs_ordinary = false, ps_ordinary = false;
    UINT resource = 0, sampler = 0, left = 0, mask = ~0u;
    DXGI_FORMAT index_format = DXGI_FORMAT_R32_UINT;
    bool index_at_start = false;
    Draw draw = Direct;
    Boundary boundary = None;
  };
  std::vector<Case> cases(1);
  auto add = [&](Case changed) {
    cases.push_back(changed);
    cases.emplace_back();
  };
  Case c;
  c.name = "same VB, stride only"; c.stride *= 2; add(c);
  c = {}; c.name = "same VB, zero stride"; c.stride = 0; add(c);
  c = {}; c.name = "same VB, offset only"; c.offset = sizeof(UINT); add(c);
  c = {}; c.name = "null VB and rebind"; c.vertex_bound = false; add(c);
  c = {}; c.name = "same VS CB, first only"; c.vs_first = range; add(c);
  c = {}; c.name = "same VS CB, count only"; c.vs_count = range; add(c);
  c = {}; c.name = "same PS CB, first only"; c.ps_first = 2 * range; add(c);
  c = {}; c.name = "same PS CB, count only"; c.ps_count = range; add(c);
  c = {}; c.name = "same PS CB, empty range"; c.ps_count = 0; add(c);
  c = {}; c.name = "null VS CB and rebind"; c.vs_bound = false; add(c);
  c = {}; c.name = "null PS CB and rebind"; c.ps_bound = false; add(c);
  c = {}; c.name = "VS range before ordinary reset"; c.vs_count = range; cases.push_back(c);
  c.name = "same VS CB, ordinary reset"; c.vs_ordinary = true; c.vs_count = constant_rows; add(c);
  c = {}; c.name = "PS range before ordinary reset"; c.ps_count = range; cases.push_back(c);
  c.name = "same PS CB, ordinary reset"; c.ps_ordinary = true;
  c.ps_first = 0; c.ps_count = constant_rows; add(c);
  c = {}; c.name = "high SRV replacement"; c.resource = 1; add(c);
  c = {}; c.name = "null SRV and rebind"; c.resource = 2; add(c);
  c = {}; c.name = "high sampler replacement"; c.sampler = 1; add(c);
  c = {}; c.name = "scissor field only"; c.left = 1; add(c);
  c = {}; c.name = "same blend object, sample mask only"; c.mask = 0; add(c);
  for (Draw form : {Indexed, Instanced, IndexedInstanced, Indirect, IndexedIndirect}) {
    c = {}; c.name = "draw form"; c.draw = form; add(c);
  }
  c = {}; c.name = "indexed control"; c.draw = Indexed; cases.push_back(c);
  c.name = "same IB, offset only"; c.index_at_start = true; cases.push_back(c);
  c.name = "same IB, format only"; c.index_format = DXGI_FORMAT_R16_UINT; cases.push_back(c);
  c.name = "same IB, format restored"; c.index_format = DXGI_FORMAT_R32_UINT; cases.push_back(c);
  c.name = "same IB, offset restored"; c.index_at_start = false; add(c);
  for (Boundary boundary : {Compute, Blit, ComputeBlit}) {
    c = {}; c.name = "encoder transition without rebinding"; c.boundary = boundary; add(c);
  }
  const UINT height = UINT(cases.size());
  const std::vector<std::string> defines{
      "WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height), "RANGE=" + std::to_string(range),
      "DECLARED=" + std::to_string(2 * range + 1), "BREG=b" + std::to_string(cb_slot),
      "TREG=t" + std::to_string(srv_slot), "SREG=s" + std::to_string(sampler_slot),
      "UREG=u" + std::to_string(uav_slot)};
  step("compile all shaders before device creation");
  auto vs_code = compile(hlsl, "vs", "vs", defines), ps_code = compile(hlsl, "ps", "ps", defines);
  auto cs_code = compile(hlsl, "cs", "cs", defines);
  if (!expect(vs_code && ps_code && cs_code, "HLSL compilation"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11DeviceContext1> context1;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &context));
  CHECK(context.As(&context1));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"VALUE", 0, DXGI_FORMAT_R32_UINT, vb_slot, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"PLACE", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ComPtr<ID3D11InputLayout> layout;
  CHECK(device->CreateInputLayout(elements, std::size(elements), vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                                  &layout));
  std::vector<UINT> vertex_words(2 * width * height + 1);
  for (UINT i = 0; i < vertex_words.size(); i++)
    vertex_words[i] = i + 1;
  D3D11_BUFFER_DESC desc{UINT(vertex_words.size() * sizeof(UINT)), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA data{vertex_words.data()};
  ComPtr<ID3D11Buffer> vertices, constants, indices, arguments;
  CHECK(device->CreateBuffer(&desc, &data, &vertices));
  std::array<Word, constant_rows> rows;
  for (UINT i = 0; i < rows.size(); i++)
    rows[i].fill(i + 1);
  desc = {sizeof(rows), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  data.pSysMem = rows.data();
  CHECK(device->CreateBuffer(&desc, &data, &constants));
  const UINT index16_bytes = width * height * sizeof(uint16_t);
  const UINT index32_offset = (index16_bytes + sizeof(UINT) - 1) / sizeof(UINT) * sizeof(UINT);
  std::vector<UINT> index_words(index32_offset / sizeof(UINT) + width * height);
  for (UINT i = 0; i < width * height; i++) {
    const uint16_t index = uint16_t(i);
    memcpy(reinterpret_cast<char *>(index_words.data()) + i * sizeof(index), &index, sizeof(index));
    index_words[index32_offset / sizeof(UINT) + i] = i;
  }
  desc = {UINT(index_words.size() * sizeof(UINT)), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
  data.pSysMem = index_words.data();
  CHECK(device->CreateBuffer(&desc, &data, &indices));
  std::vector<std::array<UINT, 5>> draw_args(height);
  for (UINT y = 0; y < height; y++)
    draw_args[y] = {width, 1, y * width, 0, 0};
  desc = {UINT(draw_args.size() * sizeof(draw_args[0])), D3D11_USAGE_DEFAULT, 0, 0,
          D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  data.pSysMem = draw_args.data();
  CHECK(device->CreateBuffer(&desc, &data, &arguments));
  D3D11_TEXTURE2D_DESC td{width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                         D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&td, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&td, nullptr, &staging));
  ComPtr<ID3D11ShaderResourceView> srvs[2];
  const float texels[2][2] = {{7, 11}, {17, 19}};
  for (UINT i = 0; i < std::size(srvs); i++) {
    td = {2, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    data = {texels[i], sizeof(texels[i])};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&td, &data, &texture));
    CHECK(device->CreateShaderResourceView(texture.Get(), nullptr, &srvs[i]));
  }
  ComPtr<ID3D11SamplerState> samplers[2];
  for (UINT i = 0; i < std::size(samplers); i++) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = i ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = D3D11_DEFAULT_MAX_ANISOTROPY;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(device->CreateSamplerState(&sd, &samplers[i]));
  }
  ComPtr<ID3D11RasterizerState> raster;
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.ScissorEnable = rd.DepthClipEnable = TRUE;
  CHECK(device->CreateRasterizerState(&rd, &raster));
  ComPtr<ID3D11BlendState> blend;
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  CHECK(device->CreateBlendState(&bd, &blend));
  const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  const float zero[4] = {}, factor[4] = {0.25f, 0.5f, 0.75f, 1};
  context->ClearRenderTargetView(rtv.Get(), zero);
  context->IASetInputLayout(layout.Get());
  const UINT placement_stride = sizeof(UINT), placement_offset = 0;
  context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &placement_stride, &placement_offset);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->RSSetState(raster.Get());
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  const Word sentinel{~0u, ~0u, ~0u, ~0u};
  std::vector<Word> wanted_records(height, sentinel);
  ComPtr<ID3D11Buffer> output, copied, count_buffer, dispatch_args;
  ComPtr<ID3D11UnorderedAccessView> uav;
  desc = {UINT(wanted_records.size() * sizeof(Word)), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
          D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Word)};
  data.pSysMem = wanted_records.data();
  CHECK(device->CreateBuffer(&desc, &data, &output));
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
  ud.Buffer = {0, UINT(wanted_records.size()), D3D11_BUFFER_UAV_FLAG_COUNTER};
  CHECK(device->CreateUnorderedAccessView(output.Get(), &ud, &uav));
  count_buffer = buffer(device.Get(), sizeof(Word), 0, sentinel[0]);
  copied = buffer(device.Get(), sizeof(rows), 0);
  if (!expect(count_buffer && copied, "copy buffers created"))
    return verdict();
  const UINT dispatch[3] = {1, 1, 1};
  desc = {sizeof(dispatch), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  data.pSysMem = dispatch;
  CHECK(device->CreateBuffer(&desc, &data, &dispatch_args));
  UINT counter = 0, cs_first = 0, cs_count = 2 * range;
  ID3D11Buffer *cb = constants.Get();
  context->CSSetShader(cs.Get(), nullptr, 0);
  context1->CSSetConstantBuffers1(cb_slot, 1, &cb, &cs_first, &cs_count);
  context->CSSetShaderResources(srv_slot, 1, srvs[0].GetAddressOf());
  context->CSSetSamplers(sampler_slot, 1, samplers[0].GetAddressOf());
  context->CSSetUnorderedAccessViews(uav_slot, 1, uav.GetAddressOf(), &counter);
  auto constant = [&](bool bound, UINT first, UINT count, UINT index) {
    return bound && index < count && first + index < rows.size() ? first + index + 1 : 0;
  };
  auto compute = [&](bool indirect = false) {
    if (indirect)
      context->DispatchIndirect(dispatch_args.Get(), 0);
    else
      context->Dispatch(1, 1, 1);
    wanted_records[counter++] = {constant(true, cs_first, cs_count, range - 1),
                                 constant(true, cs_first, cs_count, range),
                                 constant(true, cs_first, cs_count, range + 1), UINT(texels[0][1])};
  };
  auto get_range = [&](auto getter, ID3D11Buffer *want, UINT first, UINT count) {
    ComPtr<ID3D11Buffer> got;
    UINT got_first = ~0u, got_count = ~0u;
    (context1.Get()->*getter)(cb_slot, 1, &got, &got_first, &got_count);
    expect(got.Get() == want, "CB identity");
    if (want)
      expect(got_first == first && got_count == count, "CB range %u/%u, want %u/%u",
             got_first, got_count, first, count);
  };
  std::vector<Word> wanted_pixels(width * height);
  for (UINT y = 0; y < height; y++) {
    const auto &of = cases[y];
    step("tile row %u: %s (draw %u, boundary %u)", y, of.name, of.draw, of.boundary);
    if (of.boundary == Compute || of.boundary == ComputeBlit)
      compute();
    if (of.boundary == Blit || of.boundary == ComputeBlit)
      context->CopyResource(copied.Get(), constants.Get());
    ID3D11Buffer *vb = of.vertex_bound ? vertices.Get() : nullptr;
    ID3D11Buffer *vs_cb = of.vs_bound ? constants.Get() : nullptr;
    ID3D11Buffer *ps_cb = of.ps_bound ? constants.Get() : nullptr;
    ID3D11ShaderResourceView *srv = of.resource < std::size(srvs) ? srvs[of.resource].Get() : nullptr;
    context->IASetVertexBuffers(vb_slot, 1, &vb, &of.stride, &of.offset);
    const UINT index_offset = of.index_at_start ? 0 : index32_offset;
    context->IASetIndexBuffer(indices.Get(), of.index_format, index_offset);
    if (of.vs_ordinary)
      context->VSSetConstantBuffers(cb_slot, 1, &vs_cb);
    else
      context1->VSSetConstantBuffers1(cb_slot, 1, &vs_cb, &of.vs_first, &of.vs_count);
    if (of.ps_ordinary)
      context->PSSetConstantBuffers(cb_slot, 1, &ps_cb);
    else
      context1->PSSetConstantBuffers1(cb_slot, 1, &ps_cb, &of.ps_first, &of.ps_count);
    context->PSSetShaderResources(srv_slot, 1, &srv);
    context->PSSetSamplers(sampler_slot, 1, samplers[of.sampler].GetAddressOf());
    const D3D11_RECT rect{LONG(of.left), 0, LONG(width), LONG(height)};
    context->RSSetScissorRects(1, &rect);
    context->OMSetBlendState(blend.Get(), factor, of.mask);
    ComPtr<ID3D11Buffer> got_vb, got_ib;
    UINT stride = ~0u, offset = ~0u, ib_offset = ~0u;
    DXGI_FORMAT format;
    context->IAGetVertexBuffers(vb_slot, 1, &got_vb, &stride, &offset);
    context->IAGetIndexBuffer(&got_ib, &format, &ib_offset);
    expect(got_vb.Get() == vb && stride == (vb ? of.stride : 0) && offset == (vb ? of.offset : 0),
           "VB identity/stride/offset: %u/%u", stride, offset);
    expect(got_ib == indices && format == of.index_format && ib_offset == index_offset, "IB fields");
    ComPtr<ID3D11InputLayout> got_layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    context->IAGetInputLayout(&got_layout);
    context->IAGetPrimitiveTopology(&topology);
    expect(got_layout == layout && topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, "IA layout/topology");
    ComPtr<ID3D11VertexShader> got_vs;
    ComPtr<ID3D11PixelShader> got_ps;
    ComPtr<ID3D11ComputeShader> got_cs;
    context->VSGetShader(&got_vs, nullptr, nullptr);
    context->PSGetShader(&got_ps, nullptr, nullptr);
    context->CSGetShader(&got_cs, nullptr, nullptr);
    expect(got_vs == vs && got_ps == ps && got_cs == cs, "shader identities across encoders");
    ComPtr<ID3D11Buffer> got_vs_cb, got_ps_cb;
    context->VSGetConstantBuffers(cb_slot, 1, &got_vs_cb);
    context->PSGetConstantBuffers(cb_slot, 1, &got_ps_cb);
    expect(got_vs_cb.Get() == vs_cb && got_ps_cb.Get() == ps_cb, "VS/PS CB identities");
    if (!of.vs_ordinary)
      get_range(&ID3D11DeviceContext1::VSGetConstantBuffers1, vs_cb, of.vs_first, of.vs_count);
    if (!of.ps_ordinary)
      get_range(&ID3D11DeviceContext1::PSGetConstantBuffers1, ps_cb, of.ps_first, of.ps_count);
    ComPtr<ID3D11ShaderResourceView> got_srv;
    ComPtr<ID3D11SamplerState> got_sampler;
    context->PSGetShaderResources(srv_slot, 1, &got_srv);
    context->PSGetSamplers(sampler_slot, 1, &got_sampler);
    expect(got_srv.Get() == srv && got_sampler == samplers[of.sampler], "PS SRV/sampler identities");
    ComPtr<ID3D11BlendState> got_blend;
    float got_factor[4];
    UINT mask;
    context->OMGetBlendState(&got_blend, got_factor, &mask);
    expect(got_blend == blend && mask == of.mask && !memcmp(got_factor, factor, sizeof(factor)), "blend fields");
    D3D11_RECT got_rect;
    UINT num = 1;
    context->RSGetScissorRects(&num, &got_rect);
    expect(num == 1 && !memcmp(&got_rect, &rect, sizeof(rect)), "scissor fields");
    ComPtr<ID3D11RasterizerState> got_raster;
    ComPtr<ID3D11RenderTargetView> got_rtv;
    context->RSGetState(&got_raster);
    context->OMGetRenderTargets(1, &got_rtv, nullptr);
    D3D11_VIEWPORT got_viewport;
    num = 1;
    context->RSGetViewports(&num, &got_viewport);
    expect(got_raster == raster && got_rtv == rtv && num == 1 &&
               !memcmp(&got_viewport, &viewport, sizeof(viewport)), "RS/OM state across encoders");
    switch (of.draw) {
    case Direct: context->Draw(width, y * width); break;
    case Indexed: context->DrawIndexed(width, y * width, 0); break;
    case Instanced: context->DrawInstanced(width, 1, y * width, 0); break;
    case IndexedInstanced: context->DrawIndexedInstanced(width, 1, y * width, 0, 0); break;
    case Indirect: context->DrawInstancedIndirect(arguments.Get(), y * sizeof(draw_args[0])); break;
    case IndexedIndirect: context->DrawIndexedInstancedIndirect(arguments.Get(), y * sizeof(draw_args[0])); break;
    }
    for (UINT n = 0; n < width; n++) {
      UINT id = y * width + n;
      if (of.draw == Indexed || of.draw == IndexedInstanced || of.draw == IndexedIndirect) {
        const UINT bytes = of.index_format == DXGI_FORMAT_R32_UINT ? sizeof(UINT) : sizeof(uint16_t);
        id = 0;
        memcpy(&id, reinterpret_cast<const char *>(index_words.data()) + index_offset + (y * width + n) * bytes,
               bytes);
      }
      if (id >= wanted_pixels.size())
        continue;
      const UINT x = id % width;
      if (x < of.left || !of.mask)
        continue;
      wanted_pixels[id] = {
          vb ? vertex_words[(of.offset + id * of.stride) / sizeof(UINT)] : 0,
          constant(of.vs_bound, of.vs_first, of.vs_count, range - 1 + x),
          constant(of.ps_bound, of.ps_first, of.ps_count, range - 1 + x),
          srv ? UINT(texels[of.resource][of.sampler ? 0 : 1]) : 0};
    }
  }
  for (auto [first, count] : {std::pair{range, 2 * range}, std::pair{range, range},
                             std::pair{range, 0u}, std::pair{0u, 2 * range}}) {
    step("same CS CB: range %u/%u", first, count);
    cs_first = first; cs_count = count;
    context1->CSSetConstantBuffers1(cb_slot, 1, &cb, &cs_first, &cs_count);
    get_range(&ID3D11DeviceContext1::CSGetConstantBuffers1, cb, cs_first, cs_count);
    compute();
  }
  step("same UAV: reset counter past one unwritten record, then preserve it");
  counter++;
  context->CSSetUnorderedAccessViews(uav_slot, 1, uav.GetAddressOf(), &counter);
  compute();
  const UINT preserve = ~0u;
  context->CSSetUnorderedAccessViews(uav_slot, 1, uav.GetAddressOf(), &preserve);
  compute();
  ID3D11UnorderedAccessView *null_uav = nullptr;
  context->CSSetUnorderedAccessViews(uav_slot, 1, &null_uav, nullptr);
  ComPtr<ID3D11UnorderedAccessView> got_uav;
  context->CSGetUnorderedAccessViews(uav_slot, 1, &got_uav);
  expect(!got_uav, "null CS UAV getter");
  context->CSSetUnorderedAccessViews(uav_slot, 1, uav.GetAddressOf(), nullptr);
  context->CSGetUnorderedAccessViews(uav_slot, 1, &got_uav);
  expect(got_uav == uav, "rebound CS UAV getter");
  compute(true);
  context->CopyStructureCount(count_buffer.Get(), sizeof(UINT), uav.Get());
  step("six stages: same-pointer range getters, ordinary reset and null/rebind");
  const decltype(&ID3D11DeviceContext1::VSSetConstantBuffers1) setters[] = {
      &ID3D11DeviceContext1::VSSetConstantBuffers1, &ID3D11DeviceContext1::PSSetConstantBuffers1,
      &ID3D11DeviceContext1::GSSetConstantBuffers1, &ID3D11DeviceContext1::HSSetConstantBuffers1,
      &ID3D11DeviceContext1::DSSetConstantBuffers1, &ID3D11DeviceContext1::CSSetConstantBuffers1};
  const decltype(&ID3D11DeviceContext1::VSGetConstantBuffers1) getters[] = {
      &ID3D11DeviceContext1::VSGetConstantBuffers1, &ID3D11DeviceContext1::PSGetConstantBuffers1,
      &ID3D11DeviceContext1::GSGetConstantBuffers1, &ID3D11DeviceContext1::HSGetConstantBuffers1,
      &ID3D11DeviceContext1::DSGetConstantBuffers1, &ID3D11DeviceContext1::CSGetConstantBuffers1};
  const decltype(&ID3D11DeviceContext::VSSetConstantBuffers) ordinary[] = {
      &ID3D11DeviceContext::VSSetConstantBuffers, &ID3D11DeviceContext::PSSetConstantBuffers,
      &ID3D11DeviceContext::GSSetConstantBuffers, &ID3D11DeviceContext::HSSetConstantBuffers,
      &ID3D11DeviceContext::DSSetConstantBuffers, &ID3D11DeviceContext::CSSetConstantBuffers};
  for (UINT stage = 0; stage < std::size(setters); stage++) {
    for (auto [first, count] : {std::pair{0u, 2 * range}, std::pair{range, 2 * range}, std::pair{range, range}}) {
      step("stage %u range %u/%u", stage, first, count);
      (context1.Get()->*setters[stage])(cb_slot, 1, &cb, &first, &count);
      get_range(getters[stage], cb, first, count);
    }
    (context.Get()->*ordinary[stage])(cb_slot, 1, &cb);
    ComPtr<ID3D11Buffer> got;
    (context1.Get()->*getters[stage])(cb_slot, 1, &got, nullptr, nullptr);
    expect(got.Get() == cb, "ordinary CB identity");
    ID3D11Buffer *null_cb = nullptr;
    (context.Get()->*ordinary[stage])(cb_slot, 1, &null_cb);
    get_range(getters[stage], nullptr, 0, 0);
    (context.Get()->*ordinary[stage])(cb_slot, 1, &cb);
    got.Reset();
    (context1.Get()->*getters[stage])(cb_slot, 1, &got, nullptr, nullptr);
    expect(got.Get() == cb, "rebound CB identity");
  }
  step("read every tile and UAV word");
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  for (UINT y = 0; y < height; y++) {
    const auto *pixels = reinterpret_cast<const Word *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
    for (UINT x = 0; x < width; x++)
      for (UINT component = 0; component < std::tuple_size<Word>::value; component++)
        expect(pixels[x][component] == wanted_pixels[y * width + x][component],
               "row %u (%s), tile %u component %u: %u, want %u", y, cases[y].name, x, component,
               pixels[x][component], wanted_pixels[y * width + x][component]);
  }
  context->Unmap(staging.Get(), 0);
  const auto got_records = read(device.Get(), context.Get(), output.Get());
  if (expect(got_records.size() * sizeof(UINT) == wanted_records.size() * sizeof(Word), "UAV readback size"))
    for (UINT i = 0; i < wanted_records.size(); i++)
      for (UINT component = 0; component < std::tuple_size<Word>::value; component++)
        expect(got_records[i * std::tuple_size<Word>::value + component] == wanted_records[i][component],
               "UAV record %u component %u", i, component);
  auto got_count = read(device.Get(), context.Get(), count_buffer.Get());
  expect(got_count == std::vector<UINT>({sentinel[0], counter, sentinel[0], sentinel[0]}),
         "counter and untouched words");
  auto got_copy = read(device.Get(), context.Get(), copied.Get());
  if (expect(got_copy.size() * sizeof(UINT) == sizeof(rows), "blit readback size"))
    expect(!memcmp(got_copy.data(), rows.data(), sizeof(rows)), "blit bytes");
  return verdict();
}
