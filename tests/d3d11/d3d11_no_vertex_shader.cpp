// contract: a draw with no vertex shader bound runs nothing, and the context draws again once one is bound.
// "The vertex shader stage must always be active for the pipeline to execute" (Graphics pipeline, Microsoft Learn,
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-graphics-pipeline); the debug layer
// names the draw ("A Vertex Shader is always required when drawing, but none is currently bound",
// DEVICE_DRAW_VERTEX_SHADER_NOT_SET) and Direct3D goes on. an application gets here by binding the shader a failed
// CreateVertexShader left NULL.
// every kind of draw is made without one: alone, with a pixel shader, with a geometry shader, with a geometry shader
// that streams out, and with hull and domain shaders, each of which Mullion draws through another pipeline. the
// target must keep its clear colour and the stream output buffer its bytes. then the vertex shader is bound and one
// draw must cover the target.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { float4 position : SV_Position; };
V vs(uint id : SV_VertexID) {
  V o;
  o.position = float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps() : SV_Target { return float4(1, 1, 1, 1); }
[maxvertexcount(3)] void gs(triangle V input[3], inout TriangleStream<V> stream) {
  for (uint i = 0; i < 3; i++)
    stream.Append(input[i]);
}
struct Patch { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Patch constants() {
  Patch p;
  p.edges[0] = p.edges[1] = p.edges[2] = p.inside = 1;
  return p;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("constants")]
V hs(InputPatch<V, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }
[domain("tri")] V ds(Patch p, float3 at : SV_DomainLocation, const OutputPatch<V, 3> patch) {
  V o;
  o.position = patch[0].position * at.x + patch[1].position * at.y + patch[2].position * at.z;
  return o;
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps"), gs_code = compile(hlsl, "gs", "gs");
  auto hs_code = compile(hlsl, "hs", "hs"), ds_code = compile(hlsl, "ds", "ds");
  if (!vs_code || !ps_code || !gs_code || !hs_code || !ds_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11GeometryShader> gs, stream;
  ComPtr<ID3D11HullShader> hs;
  ComPtr<ID3D11DomainShader> ds;
  const D3D11_SO_DECLARATION_ENTRY entry{0, "SV_Position", 0, 0, 4, 0};
  const UINT stride = 4 * sizeof(float);
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateGeometryShader(gs_code->GetBufferPointer(), gs_code->GetBufferSize(), nullptr, &gs));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &entry, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream
  ));
  CHECK(device->CreateHullShader(hs_code->GetBufferPointer(), hs_code->GetBufferSize(), nullptr, &hs));
  CHECK(device->CreateDomainShader(ds_code->GetBufferPointer(), ds_code->GetBufferSize(), nullptr, &ds));

  const UINT size = 8;
  D3D11_TEXTURE2D_DESC target_desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> view;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &view));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &staging));
  const UINT16 indices[] = {0, 1, 2};
  D3D11_BUFFER_DESC ib_desc{sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA ib_data{indices};
  ComPtr<ID3D11Buffer> ib, arguments, so, so_staging;
  CHECK(device->CreateBuffer(&ib_desc, &ib_data, &ib));
  // the arguments of both indirect draws: three vertices or indices, one instance, from the start
  const UINT words[] = {3, 1, 0, 0, 0};
  D3D11_BUFFER_DESC arguments_desc{sizeof(words), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  D3D11_SUBRESOURCE_DATA arguments_data{words};
  CHECK(device->CreateBuffer(&arguments_desc, &arguments_data, &arguments));
  const UINT so_bytes = 16 * stride;
  std::vector<uint8_t> marked(so_bytes, 0xa5);
  D3D11_BUFFER_DESC so_desc{so_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
  D3D11_SUBRESOURCE_DATA so_data{marked.data()};
  CHECK(device->CreateBuffer(&so_desc, &so_data, &so));
  so_desc.Usage = D3D11_USAGE_STAGING;
  so_desc.BindFlags = 0;
  so_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateBuffer(&so_desc, nullptr, &so_staging));

  const float seed[4] = {0.25f, 0.5f, 0.75f, 1};
  const D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
  context->RSSetViewports(1, &viewport);
  context->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
  context->ClearRenderTargetView(view.Get(), seed);

  // how many of the target's pixels are `want`
  auto pixels = [&](uint32_t want) {
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    UINT count = 0;
    if (!expect(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map) == S_OK, "the target's copy does not map"))
      return count;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++)
        count += static_cast<const uint32_t *>(static_cast<const void *>(static_cast<const uint8_t *>(map.pData) + y * map.RowPitch))[x] == want;
    context->Unmap(staging.Get(), 0);
    return count;
  };
  const uint32_t seeded = 0xffbf8040, white = 0xffffffff;
  expect(pixels(seeded) == size * size, "the target is not its clear colour");

  struct Stages {
    const char *name;
    bool pixel, geometry, streaming, tessellated;
  };
  for (auto stages : {Stages{"no shader at all"}, Stages{"a pixel shader", true}, Stages{"a geometry shader", true, true},
                      Stages{"a geometry shader that streams out", false, false, true},
                      Stages{"hull and domain shaders", true, false, false, true}}) {
    context->VSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(stages.pixel ? ps.Get() : nullptr, nullptr, 0);
    context->GSSetShader(stages.geometry ? gs.Get() : stages.streaming ? stream.Get() : nullptr, nullptr, 0);
    context->HSSetShader(stages.tessellated ? hs.Get() : nullptr, nullptr, 0);
    context->DSSetShader(stages.tessellated ? ds.Get() : nullptr, nullptr, 0);
    context->IASetPrimitiveTopology(stages.tessellated ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                                       : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT offset = 0;
    context->SOSetTargets(stages.streaming ? 1 : 0, stages.streaming ? so.GetAddressOf() : nullptr, &offset);
    step("draws without a vertex shader, with %s", stages.name);
    context->Draw(3, 0);
    context->DrawIndexed(3, 0, 0);
    context->DrawInstanced(3, 2, 0, 0);
    context->DrawIndexedInstanced(3, 2, 0, 0, 0);
    context->DrawInstancedIndirect(arguments.Get(), 0);
    context->DrawIndexedInstancedIndirect(arguments.Get(), 0);
    context->DrawAuto();
    context->SOSetTargets(0, nullptr, nullptr);
    expect(pixels(seeded) == size * size, "a draw without a vertex shader, with %s, wrote to the target", stages.name);
    context->CopyResource(so_staging.Get(), so.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK(context->Map(so_staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    expect(!memcmp(map.pData, marked.data(), so_bytes), "a draw without a vertex shader, with %s, streamed out", stages.name);
    context->Unmap(so_staging.Get(), 0);
  }

  step("the same context draws with a vertex shader");
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->Draw(3, 0);
  expect(pixels(white) == size * size, "the draw with a vertex shader did not cover the target");
  return verdict();
}
