// D3D11 performance probe: one small scene per CPU cost of a Direct3D 11 frame as games make them, offscreen at 1080p.
// Not part of the functional tests. argv: scene [frames] [warmup]; "list" prints the scenes.
//
// Each frame records the scene on the immediate context, flushes and keeps two frames in flight (an event query of
// each). Printed: the median and 95th percentile of the scene's recording time on the CPU (api), of the whole frame
// (frame, including the wait for the GPU two frames back), and the GPU time of the frame (timestamp queries).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (FAILED(x)) {                                                                                                   \
      printf("failed: %s (line %d)\n", #x, __LINE__);                                                                  \
      exit(1);                                                                                                         \
    }                                                                                                                  \
  } while (0)

static const char hlsl[] = R"hlsl(
cbuffer Draw : register(b0) { float4 offset; float4 color; };
cbuffer Frame : register(b1) { float4 tint; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V v;
  float2 corner = float2(id & 1, id >> 1);
  v.uv = corner;
  v.position = float4(offset.xy + corner * offset.zw, 0.5, 1);
  return v;
}
struct In { float2 position : POSITION; float2 uv : TEXCOORD0; };
V vs_input(In i) {
  V v;
  v.uv = i.uv;
  v.position = float4(i.position * offset.zw + offset.xy, 0.5, 1);
  return v;
}
V vs_instanced(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  V v;
  float2 corner = float2(id & 1, id >> 1);
  v.uv = corner;
  float2 at = float2(instance % 64, instance / 64) / 32.0 - 1;
  v.position = float4(at + offset.xy * 0.01 + corner * offset.zw, 0.5, 1);
  return v;
}
float4 ps(V v) : SV_Target { return color * tint + float4(v.uv, 0, 0) * VARIANT; }
float4 ps_tex(V v) : SV_Target { return tex.Sample(smp, v.uv) * color; }
)hlsl";

struct Probe {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> target, depth;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11DepthStencilView> dsv;
  ComPtr<ID3D11VertexShader> vs, vs_input, vs_instanced;
  ComPtr<ID3D11PixelShader> ps[8], ps_tex;
  ComPtr<ID3D11VertexShader> vs_variants[8];
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> dynamic_cb, default_cb, frame_cb, cbs[64], vbs[8], ibs[8], ring_vb;
  ComPtr<ID3D11ShaderResourceView> srvs[16];
  ComPtr<ID3D11Texture2D> textures[16], atlas, staging;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11BlendState> blends[4];
  ComPtr<ID3D11DepthStencilState> depths[4];
  ComPtr<ID3D11RasterizerState> rasters[4];
  ComPtr<ID3D11Texture2D> targets[4];
  ComPtr<ID3D11RenderTargetView> rtvs[4];
  ComPtr<ID3D11Query> occlusion[500];
  UINT ring_at = 0;
} p;

static ComPtr<ID3DBlob>
compile(const char *entry, const char *profile, const char *variant = "0") {
  D3D_SHADER_MACRO macros[] = {{"VARIANT", variant}, {}};
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, macros, nullptr, entry, profile, 0, 0, &code, &errors))) {
    printf("failed: %s: %.*s\n", entry, errors ? (int)errors->GetBufferSize() : 0,
           errors ? (const char *)errors->GetBufferPointer() : "");
    exit(1);
  }
  return code;
}

static ComPtr<ID3D11Buffer>
make_buffer(UINT bytes, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0, const void *data = nullptr) {
  D3D11_BUFFER_DESC desc{bytes, usage, bind, cpu};
  D3D11_SUBRESOURCE_DATA init{data};
  ComPtr<ID3D11Buffer> out;
  CHECK(p.device->CreateBuffer(&desc, data ? &init : nullptr, &out));
  return out;
}

static ComPtr<ID3D11Texture2D>
make_texture(UINT width, UINT height, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0) {
  D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, usage, bind, cpu};
  std::vector<uint32_t> pixels(width * height, 0xff808080);
  D3D11_SUBRESOURCE_DATA init{pixels.data(), width * 4};
  ComPtr<ID3D11Texture2D> out;
  CHECK(p.device->CreateTexture2D(&desc, usage == D3D11_USAGE_STAGING ? nullptr : &init, &out));
  return out;
}

static void
setup() {
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &p.device,
                          nullptr, &p.context));
  p.target = make_texture(1920, 1080, D3D11_BIND_RENDER_TARGET);
  CHECK(p.device->CreateRenderTargetView(p.target.Get(), nullptr, &p.rtv));
  D3D11_TEXTURE2D_DESC depth{1920, 1080, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_DEPTH_STENCIL};
  CHECK(p.device->CreateTexture2D(&depth, nullptr, &p.depth));
  CHECK(p.device->CreateDepthStencilView(p.depth.Get(), nullptr, &p.dsv));
  auto vs = compile("vs", "vs_5_0"), vs_input = compile("vs_input", "vs_5_0"),
       vs_instanced = compile("vs_instanced", "vs_5_0");
  CHECK(p.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &p.vs));
  CHECK(p.device->CreateVertexShader(vs_input->GetBufferPointer(), vs_input->GetBufferSize(), nullptr, &p.vs_input));
  CHECK(p.device->CreateVertexShader(vs_instanced->GetBufferPointer(), vs_instanced->GetBufferSize(), nullptr,
                                     &p.vs_instanced));
  for (int i = 0; i < 8; i++) {
    auto variant = std::to_string(i);
    auto ps = compile("ps", "ps_5_0", variant.c_str());
    CHECK(p.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p.ps[i]));
    p.vs_variants[i] = p.vs;
  }
  auto ps_tex = compile("ps_tex", "ps_5_0");
  CHECK(p.device->CreatePixelShader(ps_tex->GetBufferPointer(), ps_tex->GetBufferSize(), nullptr, &p.ps_tex));
  D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  CHECK(p.device->CreateInputLayout(elements, 2, vs_input->GetBufferPointer(), vs_input->GetBufferSize(), &p.layout));
  p.dynamic_cb = make_buffer(256, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  p.default_cb = make_buffer(256, D3D11_BIND_CONSTANT_BUFFER);
  float tint[64] = {1, 1, 1, 1};
  p.frame_cb = make_buffer(256, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, tint);
  for (int i = 0; i < 64; i++) {
    float values[64] = {-1.0f + (i % 8) * 0.25f, -1.0f + (i / 8) * 0.25f, 0.02f, 0.02f, 1, float(i) / 64, 0, 1};
    p.cbs[i] = make_buffer(256, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_IMMUTABLE, 0, values);
  }
  float quad[16] = {0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1};
  uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
  for (int i = 0; i < 8; i++) {
    p.vbs[i] = make_buffer(sizeof(quad), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_IMMUTABLE, 0, quad);
    p.ibs[i] = make_buffer(sizeof(indices), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE, 0, indices);
  }
  p.ring_vb = make_buffer(4 << 20, D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  for (int i = 0; i < 16; i++) {
    p.textures[i] = make_texture(256, 256, D3D11_BIND_SHADER_RESOURCE);
    CHECK(p.device->CreateShaderResourceView(p.textures[i].Get(), nullptr, &p.srvs[i]));
  }
  p.atlas = make_texture(1024, 1024, D3D11_BIND_SHADER_RESOURCE);
  p.staging = make_texture(256, 256, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
  D3D11_SAMPLER_DESC sampler{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                             D3D11_TEXTURE_ADDRESS_WRAP};
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  CHECK(p.device->CreateSamplerState(&sampler, &p.sampler));
  for (int i = 0; i < 4; i++) {
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = i & 1;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = i & 2 ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK(p.device->CreateBlendState(&blend, &p.blends[i]));
    D3D11_DEPTH_STENCIL_DESC depth_state{TRUE, i & 1 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO,
                                         i & 2 ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_ALWAYS};
    CHECK(p.device->CreateDepthStencilState(&depth_state, &p.depths[i]));
    D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, i & 1 ? D3D11_CULL_NONE : D3D11_CULL_BACK, FALSE, i * 10};
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = i & 2;
    CHECK(p.device->CreateRasterizerState(&raster, &p.rasters[i]));
    p.targets[i] = make_texture(1280, 720, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    CHECK(p.device->CreateRenderTargetView(p.targets[i].Get(), nullptr, &p.rtvs[i]));
  }
  for (auto &query : p.occlusion) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION};
    CHECK(p.device->CreateQuery(&desc, &query));
  }
}

// the state every scene starts from
static void
base_state() {
  auto c = p.context.Get();
  c->ClearState();
  c->OMSetRenderTargets(1, p.rtv.GetAddressOf(), p.dsv.Get());
  D3D11_VIEWPORT viewport{0, 0, 1920, 1080, 0, 1};
  c->RSSetViewports(1, &viewport);
  D3D11_RECT scissor{0, 0, 1920, 1080};
  c->RSSetScissorRects(1, &scissor);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->VSSetShader(p.vs.Get(), nullptr, 0);
  c->PSSetShader(p.ps[0].Get(), nullptr, 0);
  c->VSSetConstantBuffers(0, 1, p.cbs[0].GetAddressOf());
  c->PSSetConstantBuffers(0, 1, p.cbs[0].GetAddressOf());
  c->PSSetConstantBuffers(1, 1, p.frame_cb.GetAddressOf());
  c->PSSetSamplers(0, 1, p.sampler.GetAddressOf());
  c->PSSetShaderResources(0, 1, p.srvs[0].GetAddressOf());
  float clear[4] = {0, 0, 0, 1};
  c->ClearRenderTargetView(p.rtv.Get(), clear);
  c->ClearDepthStencilView(p.dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
}

static void
draw_constants(float *values, UINT i) {
  values[0] = -1.0f + (i % 64) / 32.0f;
  values[1] = -1.0f + (i / 64 % 64) / 32.0f;
  values[2] = values[3] = 0.02f;
  values[4] = 1;
  values[5] = (i % 256) / 256.0f;
  values[6] = 0;
  values[7] = 1;
}

struct Scene {
  const char *name, *description;
  std::function<void()> init;
  std::function<void(UINT)> frame;
};

static std::vector<Scene>
scenes() {
  auto c = [] { return p.context.Get(); };
  return {
      {"draws", "10000 draws of a small quad, nothing changed between them", [] {}, [c](UINT) {
         for (UINT i = 0; i < 10000; i++)
           c()->Draw(4, 0);
       }},
      {"draws_map", "10000 draws, each mapping a dynamic constant buffer with WRITE_DISCARD (the common pattern)", [] {},
       [c](UINT) {
         c()->VSSetConstantBuffers(0, 1, p.dynamic_cb.GetAddressOf());
         c()->PSSetConstantBuffers(0, 1, p.dynamic_cb.GetAddressOf());
         for (UINT i = 0; i < 10000; i++) {
           D3D11_MAPPED_SUBRESOURCE mapped;
           c()->Map(p.dynamic_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
           draw_constants(static_cast<float *>(mapped.pData), i);
           c()->Unmap(p.dynamic_cb.Get(), 0);
           c()->Draw(4, 0);
         }
       }},
      {"draws_map_ps", "10000 draws, each mapping a dynamic constant buffer with WRITE_DISCARD that only the pixel shader "
                       "binds, the vertex shader reading a constant buffer of its own",
       [] {}, [c](UINT) {
         c()->PSSetShader(p.ps[0].Get(), nullptr, 0);
         c()->VSSetConstantBuffers(0, 1, p.cbs[0].GetAddressOf());
         c()->PSSetConstantBuffers(0, 1, p.cbs[1].GetAddressOf());
         c()->PSSetConstantBuffers(1, 1, p.dynamic_cb.GetAddressOf());
         for (UINT i = 0; i < 10000; i++) {
           D3D11_MAPPED_SUBRESOURCE mapped;
           c()->Map(p.dynamic_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
           draw_constants(static_cast<float *>(mapped.pData), i);
           c()->Unmap(p.dynamic_cb.Get(), 0);
           c()->Draw(4, 0);
         }
       }},
      {"draws_update", "10000 draws, each UpdateSubresource of a default constant buffer", [] {}, [c](UINT) {
         c()->VSSetConstantBuffers(0, 1, p.default_cb.GetAddressOf());
         c()->PSSetConstantBuffers(0, 1, p.default_cb.GetAddressOf());
         for (UINT i = 0; i < 10000; i++) {
           float values[64];
           draw_constants(values, i);
           c()->UpdateSubresource(p.default_cb.Get(), 0, nullptr, values, 0, 0);
           c()->Draw(4, 0);
         }
       }},
      {"draws_cbs", "10000 draws, each binding one of 64 immutable constant buffers to VS and PS", [] {}, [c](UINT) {
         for (UINT i = 0; i < 10000; i++) {
           c()->VSSetConstantBuffers(0, 1, p.cbs[i % 64].GetAddressOf());
           c()->PSSetConstantBuffers(0, 1, p.cbs[i % 64].GetAddressOf());
           c()->Draw(4, 0);
         }
       }},
      {"draws_srvs", "10000 textured draws, each binding one of 16 textures", [] {}, [c](UINT) {
         c()->PSSetShader(p.ps_tex.Get(), nullptr, 0);
         for (UINT i = 0; i < 10000; i++) {
           c()->PSSetShaderResources(0, 1, p.srvs[i % 16].GetAddressOf());
           c()->Draw(4, 0);
         }
       }},
      {"draws_states", "10000 draws, each changing blend, depth-stencil and rasterizer states", [] {}, [c](UINT) {
         float factor[4] = {1, 1, 1, 1};
         for (UINT i = 0; i < 10000; i++) {
           c()->OMSetBlendState(p.blends[i % 4].Get(), factor, ~0u);
           c()->OMSetDepthStencilState(p.depths[(i / 4) % 4].Get(), 0);
           c()->RSSetState(p.rasters[(i / 16) % 4].Get());
           c()->Draw(4, 0);
         }
       }},
      {"draws_same_states", "10000 draws, each setting the blend, depth-stencil and rasterizer states it already has "
                            "(an engine that sets every state for every draw)",
       [] {}, [c](UINT) {
         float factor[4] = {1, 1, 1, 1};
         for (UINT i = 0; i < 10000; i++) {
           c()->OMSetBlendState(p.blends[1].Get(), factor, ~0u);
           c()->OMSetDepthStencilState(p.depths[1].Get(), 0);
           c()->RSSetState(p.rasters[1].Get());
           c()->Draw(4, 0);
         }
       }},
      {"draws_cleared_stages", "10000 draws, each clearing the hull, domain and geometry stages that have no shader "
                               "(a state manager resetting the stages a draw does not use)",
       [] {}, [c](UINT) {
         for (UINT i = 0; i < 10000; i++) {
           c()->HSSetShader(nullptr, nullptr, 0);
           c()->DSSetShader(nullptr, nullptr, 0);
           c()->GSSetShader(nullptr, nullptr, 0);
           c()->Draw(4, 0);
         }
       }},
      {"draws_cleared_so", "10000 draws, each clearing the stream output targets, which none are bound to", [] {},
       [c](UINT) {
         for (UINT i = 0; i < 10000; i++) {
           c()->SOSetTargets(0, nullptr, nullptr);
           c()->Draw(4, 0);
         }
       }},
      {"draws_shaders", "10000 draws, each changing the pixel shader among 8", [] {}, [c](UINT) {
         for (UINT i = 0; i < 10000; i++) {
           c()->PSSetShader(p.ps[i % 8].Get(), nullptr, 0);
           c()->Draw(4, 0);
         }
       }},
      {"draws_same_buffers", "10000 indexed draws, each setting the input layout, vertex and index buffer it already has",
       [] {}, [c](UINT) {
         c()->VSSetShader(p.vs_input.Get(), nullptr, 0);
         c()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         UINT stride = 16, offset = 0;
         for (UINT i = 0; i < 10000; i++) {
           c()->IASetInputLayout(p.layout.Get());
           c()->IASetVertexBuffers(0, 1, p.vbs[0].GetAddressOf(), &stride, &offset);
           c()->IASetIndexBuffer(p.ibs[0].Get(), DXGI_FORMAT_R16_UINT, 0);
           c()->DrawIndexed(6, 0, 0);
         }
       }},
      {"draws_buffers", "10000 indexed draws, each binding one of 8 vertex and index buffers (input layout)", [] {},
       [c](UINT) {
         c()->VSSetShader(p.vs_input.Get(), nullptr, 0);
         c()->IASetInputLayout(p.layout.Get());
         c()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
         UINT stride = 16, offset = 0;
         for (UINT i = 0; i < 10000; i++) {
           c()->IASetVertexBuffers(0, 1, p.vbs[i % 8].GetAddressOf(), &stride, &offset);
           c()->IASetIndexBuffer(p.ibs[(i / 8) % 8].Get(), DXGI_FORMAT_R16_UINT, 0);
           c()->DrawIndexed(6, 0, 0);
         }
       }},
      {"sprites", "5000 sprites, each its 4 vertices written with Map NO_OVERWRITE into a ring (UI, particles)", [] {},
       [c](UINT) {
         c()->VSSetShader(p.vs_input.Get(), nullptr, 0);
         c()->IASetInputLayout(p.layout.Get());
         UINT stride = 16, offset = 0;
         c()->IASetVertexBuffers(0, 1, p.ring_vb.GetAddressOf(), &stride, &offset);
         for (UINT i = 0; i < 5000; i++) {
           D3D11_MAPPED_SUBRESOURCE mapped;
           bool wrap = p.ring_at + 64 > (4u << 20);
           if (wrap)
             p.ring_at = 0;
           c()->Map(p.ring_vb.Get(), 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
           float *v = reinterpret_cast<float *>(static_cast<char *>(mapped.pData) + p.ring_at);
           float x = -1.0f + (i % 64) / 32.0f, y = -1.0f + (i / 64 % 64) / 32.0f;
           float quad[16] = {x, y, 0, 0, x + 0.02f, y, 1, 0, x, y + 0.02f, 0, 1, x + 0.02f, y + 0.02f, 1, 1};
           memcpy(v, quad, sizeof(quad));
           c()->Unmap(p.ring_vb.Get(), 0);
           c()->Draw(4, p.ring_at / 16);
           p.ring_at += 64;
         }
       }},
      {"instanced", "200 instanced draws of 4096 quads each", [] {}, [c](UINT) {
         c()->VSSetShader(p.vs_instanced.Get(), nullptr, 0);
         for (UINT i = 0; i < 200; i++) {
           c()->VSSetConstantBuffers(0, 1, p.cbs[i % 64].GetAddressOf());
           c()->DrawInstanced(4, 4096, 0, 0);
         }
       }},
      {"targets", "40 render target switches among 4 720p targets, each cleared and drawn 50 times, then sampled",
       [] {}, [c](UINT) {
         float clear[4] = {0.1f, 0.2f, 0.3f, 1};
         for (UINT k = 0; k < 40; k++) {
           ID3D11ShaderResourceView *none = nullptr;
           c()->PSSetShaderResources(0, 1, &none);
           c()->OMSetRenderTargets(1, p.rtvs[k % 4].GetAddressOf(), nullptr);
           c()->ClearRenderTargetView(p.rtvs[k % 4].Get(), clear);
           for (UINT i = 0; i < 50; i++)
             c()->Draw(4, 0);
         }
         c()->OMSetRenderTargets(1, p.rtv.GetAddressOf(), p.dsv.Get());
       }},
      {"targets_noclear", "the targets scene without its clears (what the clears cost)", [] {}, [c](UINT) {
         for (UINT k = 0; k < 40; k++) {
           ID3D11ShaderResourceView *none = nullptr;
           c()->PSSetShaderResources(0, 1, &none);
           c()->OMSetRenderTargets(1, p.rtvs[k % 4].GetAddressOf(), nullptr);
           for (UINT i = 0; i < 50; i++)
             c()->Draw(4, 0);
         }
         c()->OMSetRenderTargets(1, p.rtv.GetAddressOf(), p.dsv.Get());
       }},
      {"create_depth_initial", "a 1920x1080 D24_UNORM_S8_UINT texture made with initial data and released, each frame", [] {},
       [](UINT f) {
         static std::vector<uint32_t> texels(1920 * 1080);
         for (size_t i = 0; i < texels.size(); i += 4096)
           texels[i] = f + uint32_t(i);
         D3D11_TEXTURE2D_DESC desc{1920, 1080, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_DEPTH_STENCIL};
         D3D11_SUBRESOURCE_DATA data{texels.data(), 1920 * 4};
         ComPtr<ID3D11Texture2D> depth;
         CHECK(p.device->CreateTexture2D(&desc, &data, &depth));
       }},
      {"texture_updates", "200 64x64 UpdateSubresource uploads into a 1024x1024 atlas (fonts, UI)", [] {}, [c](UINT f) {
         static std::vector<uint32_t> pixels(64 * 64, 0xff00ff00);
         for (UINT i = 0; i < 200; i++) {
           D3D11_BOX box{(i % 16) * 64, (i / 16 % 16) * 64, 0, (i % 16) * 64 + 64, (i / 16 % 16) * 64 + 64, 1};
           pixels[0] = f + i;
           c()->UpdateSubresource(p.atlas.Get(), 0, &box, pixels.data(), 64 * 4, 0);
         }
       }},
      {"occlusion", "500 occlusion queries, one around each draw, read back two frames later", [] {}, [c](UINT f) {
         for (UINT i = 0; i < 500; i++) {
           if (f) {
             UINT64 samples;
             c()->GetData(p.occlusion[i].Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
           }
           c()->Begin(p.occlusion[i].Get());
           c()->Draw(4, 0);
           c()->End(p.occlusion[i].Get());
         }
       }},
      {"readback", "a 256x256 copy into a staging texture, mapped for reading in the same frame", [] {}, [c](UINT) {
         D3D11_BOX box{0, 0, 0, 256, 256, 1};
         c()->CopySubresourceRegion(p.staging.Get(), 0, 0, 0, 0, p.target.Get(), 0, &box);
         D3D11_MAPPED_SUBRESOURCE mapped;
         if (SUCCEEDED(c()->Map(p.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
           c()->Unmap(p.staging.Get(), 0);
       }},
      {"mixed", "a game-like frame: 3000 draws, each a dynamic constant buffer, a texture of 16, a shader of 8 and "
                "a state of 4",
       [] {}, [c](UINT) {
         float factor[4] = {1, 1, 1, 1};
         c()->VSSetConstantBuffers(0, 1, p.dynamic_cb.GetAddressOf());
         for (UINT i = 0; i < 3000; i++) {
           D3D11_MAPPED_SUBRESOURCE mapped;
           c()->Map(p.dynamic_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
           draw_constants(static_cast<float *>(mapped.pData), i);
           c()->Unmap(p.dynamic_cb.Get(), 0);
           c()->PSSetConstantBuffers(0, 1, p.dynamic_cb.GetAddressOf());
           c()->PSSetShader(i % 3 ? p.ps_tex.Get() : p.ps[i % 8].Get(), nullptr, 0);
           c()->PSSetShaderResources(0, 1, p.srvs[i % 16].GetAddressOf());
           if (i % 10 == 0)
             c()->OMSetBlendState(p.blends[i / 10 % 4].Get(), factor, ~0u);
           c()->Draw(4, 0);
         }
       }},
  };
}

static double
now_ms() {
  static LARGE_INTEGER frequency{};
  if (!frequency.QuadPart)
    QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(frequency.QuadPart);
}

static double
percentile(std::vector<double> v, double q) {
  if (v.empty())
    return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))];
}

int
main(int argc, char **argv) {
  auto all = scenes();
  if (argc < 2 || !strcmp(argv[1], "list")) {
    for (auto &s : all)
      printf("%s: %s\n", s.name, s.description);
    return 0;
  }
  auto scene = std::find_if(all.begin(), all.end(), [&](auto &s) { return !strcmp(s.name, argv[1]); });
  if (scene == all.end()) {
    printf("failed: no scene %s\n", argv[1]);
    return 1;
  }
  UINT frames = argc > 2 ? atoi(argv[2]) : 200, warmup = argc > 3 ? atoi(argv[3]) : 30;
  setup();
  scene->init();
  constexpr UINT inflight = 3;
  ComPtr<ID3D11Query> done[inflight], disjoint[inflight], begin[inflight], end[inflight];
  for (UINT i = 0; i < inflight; i++) {
    D3D11_QUERY_DESC event{D3D11_QUERY_EVENT}, dj{D3D11_QUERY_TIMESTAMP_DISJOINT}, ts{D3D11_QUERY_TIMESTAMP};
    CHECK(p.device->CreateQuery(&event, &done[i]));
    CHECK(p.device->CreateQuery(&dj, &disjoint[i]));
    CHECK(p.device->CreateQuery(&ts, &begin[i]));
    CHECK(p.device->CreateQuery(&ts, &end[i]));
  }
  std::vector<double> api, frame, gpu;
  double first = 0;
  for (UINT f = 0; f < frames + warmup; f++) {
    UINT slot = f % inflight;
    double start = now_ms();
    // the frame two before this one's slot has completed before its queries are used again
    if (f >= inflight) {
      BOOL finished = FALSE;
      while (p.context->GetData(done[slot].Get(), &finished, sizeof(finished), 0) != S_OK || !finished)
        ;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
      UINT64 t0 = 0, t1 = 0;
      if (p.context->GetData(disjoint[slot].Get(), &dj, sizeof(dj), 0) == S_OK &&
          p.context->GetData(begin[slot].Get(), &t0, sizeof(t0), 0) == S_OK &&
          p.context->GetData(end[slot].Get(), &t1, sizeof(t1), 0) == S_OK && !dj.Disjoint && f >= warmup + inflight)
        gpu.push_back(double(t1 - t0) * 1000.0 / double(dj.Frequency));
    }
    p.context->Begin(disjoint[slot].Get());
    p.context->End(begin[slot].Get());
    base_state();
    double recording = now_ms();
    scene->frame(f);
    double recorded = now_ms();
    p.context->End(end[slot].Get());
    p.context->End(disjoint[slot].Get());
    p.context->End(done[slot].Get());
    p.context->Flush();
    double ended = now_ms();
    if (f == 0)
      first = ended - start;
    if (f >= warmup) {
      api.push_back(recorded - recording);
      frame.push_back(ended - start);
    }
  }
  p.context->Flush();
  printf("scene %s\n", scene->name);
  printf("api median %.3f p95 %.3f ms\n", percentile(api, 0.5), percentile(api, 0.95));
  printf("frame median %.3f p95 %.3f ms\n", percentile(frame, 0.5), percentile(frame, 0.95));
  printf("gpu median %.3f p95 %.3f ms\n", percentile(gpu, 0.5), percentile(gpu, 0.95));
  printf("first %.3f ms\n", first);
  printf("D3D11_PERF_PROBE_OK %s\n", scene->name);
  return 0;
}
