// contract: see ../assembly.hpp, here through Direct3D 11.
#include "d3d11_test.hpp"
#include "../assembly.hpp"

using namespace assembly;

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  // by whether the vertex shader has cull distances
  ComPtr<ID3D11VertexShader> vertex[2];
  ComPtr<ID3D11GeometryShader> stream_out[2];
  ComPtr<ID3D11PixelShader> pixel_shader;
  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 1, 0}};
  const UINT stride = sizeof(uint32_t), sentinel = 0xa5a5a5a5u;
  for (bool cull : {false, true}) {
    auto vs = compile(hlsl, "vs", "vs", defines(cull)), ps = compile(hlsl, "ps", "ps", defines(cull));
    if (!vs || !ps) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex[cull]));
    CHECK(device->CreateGeometryShaderWithStreamOutput(
        vs->GetBufferPointer(), vs->GetBufferSize(), entries, 1, &stride, 1, 0, nullptr, &stream_out[cull]
    ));
    if (!cull)
      CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_shader));
  }
  // both passes with stream output append
  std::vector<uint32_t> streamed;
  for (int pass = 0; pass < 2; pass++)
    stream(streamed);
  auto stream_buffer = buffer(device.Get(), stride * (streamed.size() + 1), D3D11_BIND_STREAM_OUTPUT, sentinel);
  D3D11_BUFFER_DESC constants_desc{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constant_buffer;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constant_buffer));

  D3D11_TEXTURE2D_DESC texture_desc{width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, texture_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  const float clear[4] = {};
  context->ClearRenderTargetView(rtv.Get(), clear);
  context->RSSetState(raster.Get());
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->VSSetConstantBuffers(0, 1, constant_buffer.GetAddressOf());
  context->PSSetShader(pixel_shader.Get(), nullptr, 0);

  for (UINT pass = 0; pass < passes; pass++) {
    bool cull = pass & 1, with_stream = pass & 2;
    context->VSSetShader(vertex[cull].Get(), nullptr, 0);
    context->GSSetShader(with_stream ? stream_out[cull].Get() : nullptr, nullptr, 0);
    ID3D11Buffer *target = with_stream ? stream_buffer.Get() : nullptr;
    // the first pass that streams starts the buffer, the next appends
    UINT offset = pass == 2 ? 0 : ~0u;
    context->SOSetTargets(1, &target, &offset);
    for (UINT t = 0; t < case_count; t++) {
      auto values = constants(cases[t], pass_rows * pass + rows * t);
      context->UpdateSubresource(constant_buffer.Get(), 0, nullptr, &values, 0, 0);
      context->IASetPrimitiveTopology(cases[t].topology);
      context->Draw(cases[t].vertices, 0);
    }
    auto values = constants(turned, pass_rows * pass + turn_row);
    context->UpdateSubresource(constant_buffer.Get(), 0, nullptr, &values, 0, 0);
    context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
    context->Draw(turn_vertices, 0);
    context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ);
    context->Draw(turned_vertices, 0);
  }
  ID3D11Buffer *none = nullptr;
  UINT offset = 0;
  context->SOSetTargets(1, &none, &offset);

  unsigned failures = 0;
  auto expect = [&](const char *what, UINT a, UINT b, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 16)
      printf("%s %u,%u: %llu, want %llu\n", what, a, b, (unsigned long long)value, (unsigned long long)want);
  };
  context->CopyResource(texture_read.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  for (UINT y = 0; y < height; y++) {
    UINT pass = y / pass_rows, t = y % pass_rows / rows;
    bool turn = t == case_count;
    auto assembled = assemble(cases[turn ? 0 : t], pass & 1);
    for (UINT x = 0; x < width; x++)
      expect(
          "pixel", x, y, reinterpret_cast<const uint32_t *>((const uint8_t *)mapped.pData + mapped.RowPitch * y)[x],
          turn ? turned_pixel(pass & 1, x, y % rows) : pixel(cases[t], assembled, x, y % rows)
      );
  }
  context->Unmap(texture_read.Get(), 0);
  auto got = read(device.Get(), context.Get(), stream_buffer.Get());
  for (size_t i = 0; i < streamed.size(); i++)
    expect("streamed vertex", (UINT)i, 0, got[i], streamed[i]);
  expect("past the stream", (UINT)streamed.size(), 0, got[streamed.size()], sentinel);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %u topologies, %zu vertices streamed\n", case_count, streamed.size());
  return 0;
}
