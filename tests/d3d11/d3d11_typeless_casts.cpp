// contract: a view of a typeless texture in another format of its family reads and writes the texture's bits as that
// format (D3D11.3 4.1, "Typeless Formats": a view "reinterprets" the resource's data; 19.1.2 lists the families),
// whenever the view is made: before the texture is first used, after it holds rendered, cleared or initial texels, for
// every subresource of mips, arrays and multisampled textures, through a deferred context, and while another view of
// the texture stays bound. a view in the other format writes the texture as its first use too: ClearView "sets all the
// elements in a resource view to one value" (Microsoft Learn, ID3D11DeviceContext1::ClearView) through a render
// target in rectangles and through unordered access. the expectations are the bits the writes encode (D3D11.3 3.2.3.6's float to UNORM
// conversion of k/255, half and float32 encodings of exactly representable values), read back as integers.
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

static const char hlsl[] = R"hlsl(
cbuffer at : register(b0) { uint layer, mip; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 pattern(uint2 p) {
  return uint4(p.x * 7 + p.y * 13 + layer * 17 + mip * 29, p.x * 3 + p.y * 61 + 1, p.x * p.y + layer, p.x + p.y * 5 + mip);
}
float4 ps_unorm8(float4 pos : SV_Position) : SV_Target { return float4(pattern(uint2(pos.xy)) & 255) / 255; }
uint4 ps_uint8(float4 pos : SV_Position) : SV_Target { return pattern(uint2(pos.xy)) & 255; }
float4 ps_half(float4 pos : SV_Position) : SV_Target { return float4(pattern(uint2(pos.xy)) % 64) * 0.5 - 8; }
float ps_float(float4 pos : SV_Position) : SV_Target { return float(pattern(uint2(pos.xy)).x * 1000 + 3) * 0.125; }
float4 ps_unorm10(float4 pos : SV_Position) : SV_Target {
  uint4 b = pattern(uint2(pos.xy)) & uint4(1023, 1023, 1023, 3); return float4(b) / float4(1023, 1023, 1023, 3);
}
#if SAMPLES > 1
Texture2DMSArray<uint4> t : register(t0);
#else
Texture2DArray<uint4> t : register(t0);
#endif
RWByteAddressBuffer o : register(u0);
[numthreads(8, 8, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
  uint layer = id.z / (MIPS * SAMPLES), rest = id.z % (MIPS * SAMPLES), level = rest / SAMPLES, sample = rest % SAMPLES;
  if (id.x >= (W >> level) || id.y >= (H >> level))
    return;
#if SAMPLES > 1
  uint4 v = t.Load(int3(id.xy, layer), sample);
#else
  uint4 v = t.Load(int4(id.xy, layer, level));
#endif
  o.Store4(((id.z * H + id.y) * W + id.x) * 16, v);
}
)hlsl";

static const UINT W = 8, H = 8;

static uint32_t
half_bits(float value) {
  // the values written are multiples of 0.5 within [-8, 24): normal halves, exact
  if (value == 0)
    return 0;
  uint32_t sign = value < 0 ? 0x8000 : 0;
  int exponent;
  float fraction = std::frexp(std::fabs(value), &exponent); // [0.5, 1)
  return sign | uint32_t(exponent - 1 + 15) << 10 | uint32_t((fraction * 2 - 1) * 1024);
}

enum class Kind { Unorm8, Uint8, Half, Float, Unorm10, Cleared8, ClearedFloat };

// the color Kind::Cleared8 clears to, and its UNORM bits
static const float clear_color[4] = {10 / 255.0f, 20 / 255.0f, 200 / 255.0f, 1};
static const uint32_t clear_bits[4] = {10, 20, 200, 255};
// the color Kind::ClearedFloat clears an R32 FLOAT view to
static const float clear_float[4] = {1.5f, 0, 0, 0};

// how a Cleared kind clears: ClearRenderTargetView, ClearView of the render target in two rectangles (the partial
// clear's own path), or ClearView of an unordered access view in the write format
enum class Clear { Target, TargetRects, Unordered };

struct Case {
  const char *name;
  DXGI_FORMAT typeless, write, read;
  Kind kind;
  UINT mips = 1, slices = 1, samples = 1;
  bool initial = false;  // the texels come from initial data, and nothing is drawn
  bool view_first = false; // the reading view is made before the texture is first used
  bool deferred = false;   // the read is recorded on a deferred context
  Clear clear = Clear::Target;
};

// the four words a texel of (x, y) in `layer` and `level` reads as, from the pattern the pixel shaders write
static std::array<uint32_t, 4>
expected(const Case &c, UINT x, UINT y, UINT layer, UINT level) {
  uint32_t p[4] = {x * 7 + y * 13 + layer * 17 + level * 29, x * 3 + y * 61 + 1, x * y + layer, x + y * 5 + level};
  std::array<uint32_t, 4> out{};
  for (int i = 0; i < 4; i++)
    switch (c.kind) {
    case Kind::Unorm8:
    case Kind::Uint8:
      out[i] = p[i] & 255;
      break;
    case Kind::Half:
      out[i] = half_bits(float(p[i] % 64) * 0.5f - 8);
      break;
    case Kind::Float:
      out[i] = i ? (i == 3 ? 1 : 0) : std::bit_cast<uint32_t>(float(p[0] * 1000 + 3) * 0.125f);
      break;
    case Kind::Unorm10:
      out[i] = p[i] & (i == 3 ? 3 : 1023);
      break;
    case Kind::Cleared8:
      out[i] = clear_bits[i];
      break;
    case Kind::ClearedFloat:
      out[i] = i ? (i == 3 ? 1 : 0) : std::bit_cast<uint32_t>(clear_float[0]);
      break;
    }
  return out;
}

static const char *
pixel_entry(Kind kind) {
  switch (kind) {
  case Kind::Unorm8:
    return "ps_unorm8";
  case Kind::Uint8:
    return "ps_uint8";
  case Kind::Half:
    return "ps_half";
  case Kind::Float:
    return "ps_float";
  default:
    return "ps_unorm10";
  }
}

static int
run(ID3D11Device *device, ID3D11DeviceContext *immediate, const Case &c) {
  step("%s", c.name);
  const UINT layers = c.slices, subresources = c.mips * c.slices;
  const std::vector<std::string> defines{"W=" + std::to_string(W), "H=" + std::to_string(H),
                                         "MIPS=" + std::to_string(c.mips), "SAMPLES=" + std::to_string(c.samples)};
  auto vertex = compile(hlsl, "vs", "vs", defines), pixel = compile(hlsl, pixel_entry(c.kind), "ps", defines),
       compute = compile(hlsl, "cs", "cs", defines);
  if (!expect(vertex && pixel && compute, "HLSL did not compile"))
    return 0;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreateComputeShader(compute->GetBufferPointer(), compute->GetBufferSize(), nullptr, &cs));

  D3D11_TEXTURE2D_DESC desc{W, H, c.mips, layers, c.typeless, {c.samples, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
  if (c.clear == Clear::Unordered)
    desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
  ComPtr<ID3D11DeviceContext1> immediate1;
  CHECK(immediate->QueryInterface(IID_PPV_ARGS(&immediate1)));
  ComPtr<ID3D11Texture2D> texture;
  std::vector<std::array<uint32_t, 4>> initial(W * H);
  if (c.initial) {
    std::vector<uint32_t> bytes(W * H);
    for (UINT y = 0; y < H; y++)
      for (UINT x = 0; x < W; x++) {
        auto e = expected(c, x, y, 0, 0);
        bytes[y * W + x] = e[0] | e[1] << 8 | e[2] << 16 | e[3] << 24;
      }
    D3D11_SUBRESOURCE_DATA data{bytes.data(), W * 4};
    CHECK(device->CreateTexture2D(&desc, &data, &texture));
  } else {
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{c.read};
  if (c.samples > 1) {
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
    srv_desc.Texture2DMSArray = {0, layers};
  } else {
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray = {0, c.mips, 0, layers};
  }
  ComPtr<ID3D11ShaderResourceView> srv;
  if (c.view_first)
    CHECK(device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv));

  if (!c.initial) {
    struct { UINT layer, mip, pad[2]; } at{};
    D3D11_BUFFER_DESC cb_desc{sizeof(at), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    ComPtr<ID3D11Buffer> cb;
    CHECK(device->CreateBuffer(&cb_desc, nullptr, &cb));
    immediate->VSSetShader(vs.Get(), nullptr, 0);
    immediate->PSSetShader(ps.Get(), nullptr, 0);
    immediate->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT layer = 0; layer < layers; layer++)
      for (UINT mip = 0; mip < c.mips; mip++) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{c.write};
        if (c.samples > 1) {
          rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
          rtv_desc.Texture2DMSArray = {layer, 1};
        } else {
          rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
          rtv_desc.Texture2DArray = {mip, layer, 1};
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv));
        // a clear is the view's first use: nothing else runs on the context between the view and the clear
        if (c.kind == Kind::Cleared8 || c.kind == Kind::ClearedFloat) {
          const float *color = c.kind == Kind::Cleared8 ? clear_color : clear_float;
          const LONG w = W >> mip, h = H >> mip;
          const D3D11_RECT halves[2] = {{0, 0, w, h / 2}, {0, h / 2, w, h}};
          if (c.clear == Clear::Target) {
            immediate->ClearRenderTargetView(rtv.Get(), color);
          } else if (c.clear == Clear::TargetRects) {
            immediate1->ClearView(rtv.Get(), color, halves, 2);
          } else {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{c.write, D3D11_UAV_DIMENSION_TEXTURE2DARRAY};
            uav_desc.Texture2DArray = {mip, layer, 1};
            ComPtr<ID3D11UnorderedAccessView> uav;
            CHECK(device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav));
            immediate1->ClearView(uav.Get(), color, nullptr, 0);
          }
          continue;
        }
        at = {layer, mip};
        immediate->UpdateSubresource(cb.Get(), 0, nullptr, &at, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, float(W >> mip), float(H >> mip), 0, 1};
        immediate->RSSetViewports(1, &viewport);
        immediate->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        immediate->Draw(3, 0);
      }
    immediate->OMSetRenderTargets(0, nullptr, nullptr);
  }
  if (!c.view_first)
    CHECK(device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv));

  const UINT words = W * H * subresources * c.samples * 4;
  D3D11_BUFFER_DESC out_desc{words * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS};
  std::vector<uint32_t> poison(words, 0xdeadbeef);
  D3D11_SUBRESOURCE_DATA out_data{poison.data()};
  ComPtr<ID3D11Buffer> out;
  CHECK(device->CreateBuffer(&out_desc, &out_data, &out));
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER};
  uav_desc.Buffer = {0, words, D3D11_BUFFER_UAV_FLAG_RAW};
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateUnorderedAccessView(out.Get(), &uav_desc, &uav));

  ComPtr<ID3D11DeviceContext> deferred;
  if (c.deferred)
    CHECK(device->CreateDeferredContext(0, &deferred));
  auto context = c.deferred ? deferred.Get() : immediate;
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetShaderResources(0, 1, srv.GetAddressOf());
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
  context->Dispatch(W / 8, H / 8, subresources * c.samples);
  if (c.deferred) {
    ComPtr<ID3D11CommandList> list;
    CHECK(deferred->FinishCommandList(FALSE, &list));
    immediate->ExecuteCommandList(list.Get(), FALSE);
  }
  immediate->ClearState();

  auto got = read(device, immediate, out.Get());
  if (!expect(got.size() == words, "the result buffer was not read"))
    return 0;
  unsigned shown = 0;
  for (UINT layer = 0; layer < layers; layer++)
    for (UINT mip = 0; mip < c.mips; mip++)
      for (UINT sample = 0; sample < c.samples; sample++)
        for (UINT y = 0; y < (H >> mip); y++)
          for (UINT x = 0; x < (W >> mip); x++) {
            UINT z = (layer * c.mips + mip) * c.samples + sample;
            auto want = expected(c, x, y, layer, mip);
            auto have = &got[((z * H + y) * W + x) * 4];
            if (std::equal(want.begin(), want.end(), have))
              continue;
            if (shown++ < 4)
              expect(false, "layer %u mip %u sample %u (%u, %u): %08x %08x %08x %08x, wanted %08x %08x %08x %08x", layer,
                     mip, sample, x, y, have[0], have[1], have[2], have[3], want[0], want[1], want[2], want[3]);
            else
              trace::wrong++;
          }
  return 0;
}

static const char copy_hlsl[] = R"hlsl(
Texture2D<float4> s : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint4 ps(float4 pos : SV_Position) : SV_Target { return uint4(s.Load(int3(pos.xy, 0)) * 255 + 0.5); }
)hlsl";

// the texels of `seed`, a word of four UNORM bytes each
static uint32_t
texel(UINT x, UINT y, uint32_t seed) {
  return ((x * 7 + y * 13 + seed * 50) & 255) | ((x * 3 + y * 61 + seed) & 255) << 8 | ((x * y + seed * 3) & 255) << 16 |
         ((x + y * 5 + seed * 11) & 255) << 24;
}

// a view bound before its texture moves reads the texture's new allocation after the move: the pixel stage keeps a
// UNORM view of the texture bound across the move another view's cast makes, and reads texels written after it
static int
bound_across_move(ID3D11Device *device, ID3D11DeviceContext *immediate) {
  step("RGBA8 read through a view bound across its texture's move");
  auto vertex = compile(copy_hlsl, "vs", "vs"), pixel = compile(copy_hlsl, "ps", "ps");
  if (!expect(vertex && pixel, "HLSL did not compile"))
    return 0;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps));
  D3D11_TEXTURE2D_DESC desc{W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> texture, target, staging;
  CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D};
  srv_desc.Texture2D = {0, 1};
  ComPtr<ID3D11ShaderResourceView> unorm, uint_view;
  CHECK(device->CreateShaderResourceView(texture.Get(), &srv_desc, &unorm));
  desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  CHECK(device->CreateTexture2D(&desc, nullptr, &target));
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));

  immediate->VSSetShader(vs.Get(), nullptr, 0);
  immediate->PSSetShader(ps.Get(), nullptr, 0);
  immediate->PSSetShaderResources(0, 1, unorm.GetAddressOf());
  immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D11_VIEWPORT viewport{0, 0, float(W), float(H), 0, 1};
  immediate->RSSetViewports(1, &viewport);
  immediate->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  std::vector<uint32_t> bytes(W * H);
  auto upload = [&](uint32_t seed) {
    for (UINT y = 0; y < H; y++)
      for (UINT x = 0; x < W; x++)
        bytes[y * W + x] = texel(x, y, seed);
    immediate->UpdateSubresource(texture.Get(), 0, nullptr, bytes.data(), W * 4, 0);
  };
  upload(1);
  immediate->Draw(3, 0); // the binding is encoded of the texture's first allocation
  srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
  CHECK(device->CreateShaderResourceView(texture.Get(), &srv_desc, &uint_view));
  immediate->Draw(3, 0); // the texture moves before this draw, its UNORM view still bound
  upload(2);
  immediate->Draw(3, 0);
  immediate->ClearState();

  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  immediate->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  unsigned shown = 0;
  for (UINT y = 0; y < H; y++)
    for (UINT x = 0; x < W; x++) {
      auto have = reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch) + x * 4;
      uint32_t want = texel(x, y, 2);
      bool same = true;
      for (int i = 0; i < 4; i++)
        same &= have[i] == ((want >> (i * 8)) & 255);
      if (same)
        continue;
      if (shown++ < 4)
        expect(false, "(%u, %u): %u %u %u %u, wanted %08x", x, y, have[0], have[1], have[2], have[3], want);
      else
        trace::wrong++;
    }
  immediate->Unmap(staging.Get(), 0);
  return 0;
}

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  const DXGI_FORMAT rgba8 = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  const Case cases[] = {
      {"RGBA8 drawn as UNORM, read as UINT through a view made after", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM,
       DXGI_FORMAT_R8G8B8A8_UINT, Kind::Unorm8},
      {"RGBA8 drawn as UNORM, read as UINT through a view made before", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM,
       DXGI_FORMAT_R8G8B8A8_UINT, Kind::Unorm8, 1, 1, 1, false, true},
      {"RGBA8 drawn through a UINT view, read as UINT", rgba8, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UINT,
       Kind::Uint8},
      {"RGBA8 with 2 mips and 3 slices, read as UINT", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT,
       Kind::Unorm8, 2, 3},
      {"RGBA8 with 4 samples, read as UINT", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, Kind::Unorm8,
       1, 2, 4},
      {"RGBA8 from initial data, read as UINT", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT,
       Kind::Unorm8, 1, 1, 1, true},
      {"RGBA8 read as UINT on a deferred context", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT,
       Kind::Unorm8, 1, 1, 1, false, false, true},
      {"RGBA8 cleared as UNORM, read as UINT", rgba8, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT,
       Kind::Cleared8, 2, 2},
      {"RGBA16 drawn as FLOAT, read as UINT", DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
       DXGI_FORMAT_R16G16B16A16_UINT, Kind::Half},
      {"R32 drawn as FLOAT, read as UINT", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT,
       Kind::Float},
      {"R32 cleared by ClearView in rectangles as FLOAT, read as UINT", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
       DXGI_FORMAT_R32_UINT, Kind::ClearedFloat, 1, 1, 1, false, false, false, Clear::TargetRects},
      {"R32 cleared by ClearView of unordered access as FLOAT, read as UINT", DXGI_FORMAT_R32_TYPELESS,
       DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, Kind::ClearedFloat, 1, 1, 1, false, false, false, Clear::Unordered},
      {"RGB10A2 drawn as UNORM, read as UINT", DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM,
       DXGI_FORMAT_R10G10B10A2_UINT, Kind::Unorm10},
  };
  for (auto &c : cases)
    if (run(device.Get(), immediate.Get(), c))
      return 1;
  bound_across_move(device.Get(), immediate.Get());
  return verdict();
}
