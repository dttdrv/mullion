// contract: a draw the device cannot run does not take the draws around it with it. a geometry shader of 32 instances
// over 96000 points is 3000 object threadgroups of 1024 mesh threadgroups, more than an Apple7 GPU runs in one draw
// (2^21), which fails its whole render pass there; Mullion leaves that draw out on such a GPU. the small draws before
// and after it, in the same pass, still draw their points. the big draw's points are all clipped, so a GPU without the
// limit leaves the same pixels.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint base; };
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = base + id; return v; }
struct P { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
// point p comes out once, from geometry instance p % 32, on pixel p of the first 64 and clipped after them
[maxvertexcount(1)] [instance(32)]
void gs(point V v[1], uint i : SV_GSInstanceID, inout PointStream<P> s) {
  if (i != v[0].id % 32)
    return;
  P o;
  o.pos = float4((v[0].id % 64 + 0.5) / 32 - 1, 0, v[0].id < 64 ? 0.5 : 2, 1);
  o.value = v[0].id + 1;
  s.Append(o);
}
uint ps(P p) : SV_Target { return p.value; }
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), gs = compile(hlsl, "gs", "gs"), ps = compile(hlsl, "ps", "ps");
  if (!vs || !gs || !ps) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11GeometryShader> geometry;
  ComPtr<ID3D11PixelShader> pixel;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &geometry));
  CHECK(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));

  const UINT width = 64, big = 96000;
  D3D11_TEXTURE2D_DESC texture_desc{width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture, texture_read;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture_read));
  D3D11_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  const float clear[4] = {};
  context->ClearRenderTargetView(rtv.Get(), clear);
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->GSSetShader(geometry.Get(), nullptr, 0);
  context->PSSetShader(pixel.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  // SV_VertexID starts at 0 in every draw (D3D11.3 8.16), so each draw's first point comes from the constant buffer
  D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, nullptr, &constants));
  context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
  auto draw = [&](UINT first, UINT points) {
    const UINT words[4] = {first};
    context->UpdateSubresource(constants.Get(), 0, nullptr, words, 0, 0);
    context->Draw(points, 0);
  };
  // pixels 0 to 31, the clipped points 64 on, then pixels 32 to 63
  draw(0, 32);
  draw(64, big);
  draw(32, 32);

  unsigned failures = 0;
  context->CopyResource(texture_read.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(texture_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  auto pixels = reinterpret_cast<const uint32_t *>(mapped.pData);
  for (UINT x = 0; x < width; x++)
    if (pixels[x] != x + 1 && failures++ < 12)
      printf("pixel %u, drawn %s the oversized draw: %u, want %u\n", x, x < 32 ? "before" : "after", pixels[x], x + 1);
  context->Unmap(texture_read.Get(), 0);
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %u pixels around a draw of %u points\n", width, big);
  return 0;
}
