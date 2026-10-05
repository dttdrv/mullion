// contract: DrawAuto draws the vertices that stream output filled a buffer with, from the offset of vertex buffer slot
// 0 to the buffer's filled size, as one instance of the current topology, without the count reaching the CPU (D3D11.3
// functional spec, 8.9). a vertex shader streams points out in two draws, the second appending. DrawAuto feeds them,
// from the second vertex on, through a geometry shader that streams each out twice into a second buffer; DrawAuto
// feeds that one through a vertex shader streaming out alone into a third; and DrawAuto draws the third as points, one
// pixel per vertex, its vertex shader streaming out what it draws too: stream output does not replace rasterization
// (14.7). the draws span several object threadgroups, each stage changes the vertices, and the expectations
// are the same stages run on the CPU.
#include "d3d11_test.hpp"
#include <array>

static const char hlsl[] = R"hlsl(
struct D { uint4 value : VALUE; };
D vs_source(uint id : SV_VertexID) { D d; d.value = uint4(id, id * 7, 1, 2); return d; }
D vs_pass(D d) { return d; }
[maxvertexcount(2)]
void gs_twice(point D v[1], inout PointStream<D> s) {
  D d = v[0];
  s.Append(d);
  d.value.z += 1;
  s.Append(d);
}
D vs_step(D d) { d.value.w += 10; return d; }
struct R { float4 pos : SV_Position; nointerpolation uint value : SHOWN; };
// vertex n covers pixel n of the one row
R vs_draw(D d, uint id : SV_VertexID) {
  R r;
  r.pos = float4((id + 0.5) / WIDTH * 2 - 1, 0, 0, 1);
  r.value = d.value.x + d.value.z + d.value.w;
  return r;
}
uint ps(R r) : SV_Target { return r.value; }
)hlsl";

int
main() {
  using Vertex = std::array<uint32_t, 4>;
  const UINT first = 70, second = 5, skipped = 1, stride = sizeof(Vertex);
  // the CPU's stages
  std::vector<Vertex> a, b, c;
  for (UINT count : {first, second})
    for (UINT id = 0; id < count; id++)
      a.push_back({id, id * 7, 1, 2});
  for (size_t i = skipped; i < a.size(); i++) {
    b.push_back(a[i]);
    b.push_back({a[i][0], a[i][1], a[i][2] + 1, a[i][3]});
  }
  for (auto v : b)
    c.push_back({v[0], v[1], v[2], v[3] + 10});
  // a target wider than the vertices drawn, so drawing too many shows
  const UINT width = c.size() + 8;

  std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto vs_source = compile(hlsl, "vs_source", "vs", defines), vs_pass = compile(hlsl, "vs_pass", "vs", defines),
       gs_twice = compile(hlsl, "gs_twice", "gs", defines), vs_step = compile(hlsl, "vs_step", "vs", defines),
       vs_draw = compile(hlsl, "vs_draw", "vs", defines), ps = compile(hlsl, "ps", "ps", defines);
  if (!vs_source || !vs_pass || !gs_twice || !vs_step || !vs_draw || !ps) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));

  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 4, 0}};
  const D3D11_INPUT_ELEMENT_DESC elements[] = {{"VALUE", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  ComPtr<ID3D11VertexShader> source, pass, step, draw;
  ComPtr<ID3D11GeometryShader> source_out, twice_out, step_out;
  ComPtr<ID3D11PixelShader> pixel;
  ComPtr<ID3D11InputLayout> layout;
  auto stream_out = [&](ID3DBlob *code, ID3D11GeometryShader **shader) {
    return device->CreateGeometryShaderWithStreamOutput(
        code->GetBufferPointer(), code->GetBufferSize(), entries, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, shader
    );
  };
  CHECK(device->CreateVertexShader(vs_source->GetBufferPointer(), vs_source->GetBufferSize(), nullptr, &source));
  CHECK(device->CreateVertexShader(vs_pass->GetBufferPointer(), vs_pass->GetBufferSize(), nullptr, &pass));
  CHECK(device->CreateVertexShader(vs_step->GetBufferPointer(), vs_step->GetBufferSize(), nullptr, &step));
  CHECK(device->CreateVertexShader(vs_draw->GetBufferPointer(), vs_draw->GetBufferSize(), nullptr, &draw));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  CHECK(device->CreateInputLayout(elements, 1, vs_pass->GetBufferPointer(), vs_pass->GetBufferSize(), &layout));
  CHECK(stream_out(vs_source.Get(), &source_out));
  CHECK(stream_out(gs_twice.Get(), &twice_out));
  CHECK(stream_out(vs_step.Get(), &step_out));
  // the drawing vertex shader's stream, which rasterizes
  const D3D11_SO_DECLARATION_ENTRY shown_entries[] = {{0, "SHOWN", 0, 0, 1, 0}};
  const UINT shown_stride = sizeof(uint32_t);
  ComPtr<ID3D11GeometryShader> draw_out;
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_draw->GetBufferPointer(), vs_draw->GetBufferSize(), shown_entries, 1, &shown_stride, 1, 0, nullptr, &draw_out
  ));

  const UINT sentinel = 0xa5a5a5a5u, append = ~0u, none = 0;
  auto filled = [&](size_t vertices) {
    return buffer(device.Get(), stride * (vertices + 1), D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER, sentinel);
  };
  auto buffer_a = filled(a.size()), buffer_b = filled(b.size()), buffer_c = filled(c.size());
  auto buffer_d = buffer(device.Get(), shown_stride * (c.size() + 1), D3D11_BIND_STREAM_OUTPUT, sentinel);
  auto target = [&](ID3D11Buffer *to, UINT offset) { context->SOSetTargets(1, &to, &offset); };
  auto vertices = [&](ID3D11Buffer *from, UINT offset) {
    context->SOSetTargets(0, nullptr, nullptr);
    context->IASetVertexBuffers(0, 1, &from, &stride, &offset);
  };
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  context->VSSetShader(source.Get(), nullptr, 0);
  context->GSSetShader(source_out.Get(), nullptr, 0);
  target(buffer_a.Get(), 0);
  context->Draw(first, 0);
  target(buffer_a.Get(), append);
  context->Draw(second, 0);

  ComPtr<ID3D11Query> statistics;
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_SO_STATISTICS};
  CHECK(device->CreateQuery(&query_desc, &statistics));
  context->IASetInputLayout(layout.Get());
  vertices(buffer_a.Get(), skipped * stride);
  context->VSSetShader(pass.Get(), nullptr, 0);
  context->GSSetShader(twice_out.Get(), nullptr, 0);
  target(buffer_b.Get(), 0);
  context->Begin(statistics.Get());
  context->DrawAuto();
  context->End(statistics.Get());

  vertices(buffer_b.Get(), 0);
  context->VSSetShader(step.Get(), nullptr, 0);
  context->GSSetShader(step_out.Get(), nullptr, 0);
  target(buffer_c.Get(), 0);
  context->DrawAuto();

  D3D11_TEXTURE2D_DESC texture_desc{width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, texture_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
  const float clear[4] = {};
  context->ClearRenderTargetView(rtv.Get(), clear);
  D3D11_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  vertices(buffer_c.Get(), 0);
  target(buffer_d.Get(), 0);
  context->VSSetShader(draw.Get(), nullptr, 0);
  context->GSSetShader(draw_out.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->DrawAuto();

  unsigned failures = 0;
  auto expect = [&](const char *what, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, (unsigned long long)value, (unsigned long long)want);
  };
  auto check = [&](const char *what, ID3D11Buffer *from, const std::vector<Vertex> &want) {
    auto got = read(device.Get(), context.Get(), from);
    for (size_t v = 0; v < want.size(); v++)
      for (int k = 0; k < 4; k++)
        expect(what, got[4 * v + k], want[v][k]);
    expect(what, got[4 * want.size()], sentinel);
  };
  check("the source", buffer_a.Get(), a);
  check("each vertex twice", buffer_b.Get(), b);
  check("the vertex shader alone", buffer_c.Get(), c);
  auto shown = read(device.Get(), context.Get(), buffer_d.Get());
  for (size_t v = 0; v < c.size(); v++)
    expect("the drawn value streamed", shown[v], c[v][0] + c[v][2] + c[v][3]);
  expect("the drawn value streamed", shown[c.size()], sentinel);
  D3D11_QUERY_DATA_SO_STATISTICS counted{};
  HRESULT hr;
  for (int tries = 0; (hr = context->GetData(statistics.Get(), &counted, sizeof(counted), 0)) == S_FALSE && tries < 1000; tries++)
    Sleep(1);
  CHECK(hr);
  expect("primitives written", counted.NumPrimitivesWritten, b.size());
  expect("primitives needed", counted.PrimitivesStorageNeeded, b.size());

  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
  context->CopyResource(texture_read.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  for (UINT x = 0; x < width; x++)
    expect("drawn pixel", static_cast<const uint32_t *>(mapped.pData)[x], x < c.size() ? c[x][0] + c[x][2] + c[x][3] : none);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %zu vertices, twice %zu, drawn %zu\n", a.size(), b.size(), c.size());
  return 0;
}
