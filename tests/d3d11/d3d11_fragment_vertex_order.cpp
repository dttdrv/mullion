// contract: a draw sees what the draws before it wrote through UAVs (D3D11.3 5.3.5.1), in every stage: a vertex
// shader reads what an earlier draw's pixel shader wrote, through a shader resource view and as a vertex buffer. the
// two draws share render targets, and only the UAVs are rebound between them, so they could share a render pass,
// where a tile-based GPU runs every draw's vertices before any fragment. both cases run on the immediate context
// and recorded in a deferred one.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d11_test.hpp"

static const char hlsl[] = R"(
RWByteAddressBuffer written : register(u1);
ByteAddressBuffer seen : register(t0);

float4 vs_cover(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

uint ps_write(float4 position : SV_Position) : SV_Target {
  written.Store(0, 7);
  return 1;
}

struct Seen {
  float4 position : SV_Position;
  nointerpolation uint value : VALUE;
};

Seen vs_srv(uint id : SV_VertexID) {
  Seen out_;
  out_.position = vs_cover(id);
  out_.value = seen.Load(0);
  return out_;
}

Seen vs_vertex(uint id : SV_VertexID, uint value : VALUE) {
  Seen out_;
  out_.position = vs_cover(id);
  out_.value = value;
  return out_;
}

uint ps_seen(Seen in_) : SV_Target {
  return in_.value;
}
)";

int
main() {
  auto cover_code = compile(hlsl, "vs_cover", "vs"), write_code = compile(hlsl, "ps_write", "ps");
  auto srv_code = compile(hlsl, "vs_srv", "vs"), vertex_code = compile(hlsl, "vs_vertex", "vs");
  auto seen_code = compile(hlsl, "ps_seen", "ps");
  if (!expect(cover_code && write_code && srv_code && vertex_code && seen_code, "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> cover, srv_vs, vertex_vs;
  ComPtr<ID3D11PixelShader> write, seen;
  CHECK(device->CreateVertexShader(cover_code->GetBufferPointer(), cover_code->GetBufferSize(), nullptr, &cover));
  CHECK(device->CreateVertexShader(srv_code->GetBufferPointer(), srv_code->GetBufferSize(), nullptr, &srv_vs));
  CHECK(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr,
                                   &vertex_vs));
  CHECK(device->CreatePixelShader(write_code->GetBufferPointer(), write_code->GetBufferSize(), nullptr, &write));
  CHECK(device->CreatePixelShader(seen_code->GetBufferPointer(), seen_code->GetBufferSize(), nullptr, &seen));
  D3D11_INPUT_ELEMENT_DESC element{"VALUE", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  ComPtr<ID3D11InputLayout> layout;
  CHECK(device->CreateInputLayout(&element, 1, vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(),
                                  &layout));

  for (bool as_vertex : {false, true})
    for (bool recorded : {false, true}) {
      step("%s, deferred %u", as_vertex ? "a vertex buffer" : "a shader resource view", recorded);
      immediate->ClearState();
      deferred->ClearState();
      auto context = recorded ? deferred.Get() : immediate.Get();
      // the buffer the first draw's pixel shader writes, read by the second draw's vertex shader
      D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT,
                           UINT(D3D11_BIND_UNORDERED_ACCESS | (as_vertex ? D3D11_BIND_VERTEX_BUFFER
                                                                         : D3D11_BIND_SHADER_RESOURCE)),
                           0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
      const uint32_t zero[4]{};
      D3D11_SUBRESOURCE_DATA initial{zero};
      ComPtr<ID3D11Buffer> shared;
      CHECK(device->CreateBuffer(&bd, &initial, &shared));
      D3D11_UNORDERED_ACCESS_VIEW_DESC ud{DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER};
      ud.Buffer.NumElements = 4;
      ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      ComPtr<ID3D11UnorderedAccessView> uav;
      CHECK(device->CreateUnorderedAccessView(shared.Get(), &ud, &uav));
      ComPtr<ID3D11ShaderResourceView> srv;
      if (!as_vertex) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX};
        sd.BufferEx = {0, 4, D3D11_BUFFEREX_SRV_FLAG_RAW};
        CHECK(device->CreateShaderResourceView(shared.Get(), &sd, &srv));
      }
      // two pixels: the first draw covers the first, the second draw the second
      D3D11_TEXTURE2D_DESC td{2, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> target, target_read;
      CHECK(device->CreateTexture2D(&td, nullptr, &target));
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&td, nullptr, &target_read));
      ComPtr<ID3D11RenderTargetView> rtv;
      CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
      const float black[4]{};
      context->ClearRenderTargetView(rtv.Get(), black);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      context->OMSetRenderTargetsAndUnorderedAccessViews(1, rtv.GetAddressOf(), nullptr, 1, 1, uav.GetAddressOf(),
                                                         nullptr);
      D3D11_VIEWPORT first{0, 0, 1, 1, 0, 1}, second{1, 0, 1, 1, 0, 1};
      context->RSSetViewports(1, &first);
      context->VSSetShader(cover.Get(), nullptr, 0);
      context->PSSetShader(write.Get(), nullptr, 0);
      context->Draw(3, 0);

      // the UAV is unbound and the render target kept, so the buffer can be read as the second draw's input
      ID3D11UnorderedAccessView *none = nullptr;
      context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr,
                                                         nullptr, 1, 1, &none, nullptr);
      context->RSSetViewports(1, &second);
      if (as_vertex) {
        UINT stride = 0, offset = 0;
        context->IASetInputLayout(layout.Get());
        context->IASetVertexBuffers(0, 1, shared.GetAddressOf(), &stride, &offset);
        context->VSSetShader(vertex_vs.Get(), nullptr, 0);
      } else {
        context->VSSetShaderResources(0, 1, srv.GetAddressOf());
        context->VSSetShader(srv_vs.Get(), nullptr, 0);
      }
      context->PSSetShader(seen.Get(), nullptr, 0);
      context->Draw(3, 0);
      if (recorded) {
        ComPtr<ID3D11CommandList> commands;
        CHECK(deferred->FinishCommandList(FALSE, &commands));
        immediate->ExecuteCommandList(commands.Get(), FALSE);
      }
      immediate->CopyResource(target_read.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      CHECK(immediate->Map(target_read.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      auto texels = static_cast<const uint32_t *>(mapped.pData);
      expect(texels[0] == 1, "the first draw wrote %u, not 1", texels[0]);
      expect(texels[1] == 7, "the second draw's vertex shader read %u, not the 7 the first draw's pixels wrote",
             texels[1]);
      immediate->Unmap(target_read.Get(), 0);
    }
  return verdict();
}
