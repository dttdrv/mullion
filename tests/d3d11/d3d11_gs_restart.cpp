// contract: RestartStrip in a geometry shader ends the strip, so the next vertex starts a new one and no primitive
// joins the vertices on each side (D3D11.3 13.4, emitthencut and cut). a line strip of two lines along rows 0 and 2 of
// a 4x3 target, cut between them, covers those rows and leaves row 1 empty; without the cut a third line would cross
// it. one geometry shader cuts right after an emit, the other in a branch on its input, each compiled for gs_4_0
// (emit, emitthencut and cut) and gs_5_0 (emit_stream and cut_stream).
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = id; return v; }
struct P { float4 pos : SV_Position; };
// a point at pixel coordinates of the 4x3 viewport
P at(float x, float y) { P p; p.pos = float4(x / 2 - 1, 1 - y / 1.5, 0, 1); return p; }
[maxvertexcount(4)]
void gs_emit_then_cut(point V v[1], inout LineStream<P> s) {
  s.Append(at(-0.5, 0.5));
  s.Append(at(4.5, 0.5));
  s.RestartStrip();
  s.Append(at(-0.5, 2.5));
  s.Append(at(4.5, 2.5));
}
[maxvertexcount(4)]
void gs_cut(point V v[1], inout LineStream<P> s) {
  s.Append(at(-0.5, 0.5));
  s.Append(at(4.5, 0.5));
  if (v[0].id == 0)
    s.RestartStrip();
  s.Append(at(-0.5, 2.5));
  s.Append(at(4.5, 2.5));
}
uint ps() : SV_Target { return 1; }
)hlsl";

int
main() {
  const char *entries[] = {"gs_emit_then_cut", "gs_cut", "gs_emit_then_cut", "gs_cut"};
  const char *profiles[] = {"gs_4_0", "gs_4_0", "gs_5_0", "gs_5_0"};
  const UINT variants = 4;
  auto vs = compile(hlsl, "vs", "vs"), ps = compile(hlsl, "ps", "ps");
  ComPtr<ID3DBlob> gs[variants];
  for (UINT k = 0; k < variants; k++)
    gs[k] = compile(hlsl, entries[k], profiles[k]);
  if (!vs || !ps || !gs[0] || !gs[1] || !gs[2] || !gs[3]) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  ComPtr<ID3D11GeometryShader> geometry[variants];
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
  for (UINT k = 0; k < variants; k++)
    CHECK(device->CreateGeometryShader(gs[k]->GetBufferPointer(), gs[k]->GetBufferSize(), nullptr, &geometry[k]));

  const UINT width = 4, height = 3;
  D3D11_TEXTURE2D_DESC target_desc{width, height * variants, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target, target_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target_read));
  const float clear[4] = {};
  context->ClearRenderTargetView(rtv.Get(), clear);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  // three rows for each variant
  for (UINT k = 0; k < variants; k++) {
    D3D11_VIEWPORT viewport{0, float(height * k), (float)width, (float)height, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->GSSetShader(geometry[k].Get(), nullptr, 0);
    context->Draw(1, 0);
  }

  context->CopyResource(target_read.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  for (UINT k = 0; k < variants; k++) {
    step("a line strip cut between its two lines (%s, %s)", entries[k], profiles[k]);
    for (UINT y = 0; y < height; y++) {
      auto pixels = reinterpret_cast<const uint32_t *>((const uint8_t *)mapped.pData + mapped.RowPitch * (height * k + y));
      for (UINT x = 0; x < width; x++) {
        uint32_t want = y == 1 ? 0 : 1;
        expect(pixels[x] == want, "pixel %u,%u: %u, want %u", x, y, pixels[x], want);
      }
    }
  }
  context->Unmap(target_read.Get(), 0);
  return verdict();
}
