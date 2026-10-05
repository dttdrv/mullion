// contract: what the input assembler makes of a vertex element of each format (D3D11.3 8.20, 3.2.3 and 19.1.3.3):
// UNORM is c / (2^n - 1); SNORM is c / (2^(n-1) - 1), at least -1; UINT and SINT are the integer; FLOAT of 16 or 32
// bits is the number; the components a format does not have are 0, and 1 in the last. here through a pipeline that
// streams the vertices out, whose first stage fetches them itself.
// the buffer is bytes that differ from one to the next; each format reads it at its own stride.
#include "d3d11_test.hpp"
#include <cmath>
#include <functional>

static const char hlsl[] = R"hlsl(
struct F { float4 v : VALUE; };
struct U { uint4 v : VALUE; };
struct I { int4 v : VALUE; };
F as_float(float4 v : DATA) { F o; o.v = v; return o; }
U as_uint(uint4 v : DATA) { U o; o.v = v; return o; }
I as_int(int4 v : DATA) { I o; o.v = v; return o; }
)hlsl";

// half to float, by the format's fields
static float
half(UINT h) {
  UINT sign = h >> 15, exponent = h >> 10 & 31, mantissa = h & 1023;
  float value = exponent == 0    ? std::ldexp((float)mantissa, -24)
                : exponent == 31 ? (mantissa ? NAN : INFINITY)
                                 : std::ldexp(float(mantissa | 1024), int(exponent) - 25);
  return sign ? -value : value;
}

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  enum Kind { Unorm, Snorm, Uint, Sint, Float };
  // a format: its kind, the bits of each component in order (0 for one it does not have)
  struct Format {
    const char *name;
    DXGI_FORMAT format;
    Kind kind;
    UINT bits[4];
    bool bgra = false;
  } formats[] = {
      {"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, Unorm, {8, 8, 8, 8}},
      {"B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, Unorm, {8, 8, 8, 8}, true},
      {"R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM, Snorm, {8, 8, 8, 8}},
      {"R8G8B8A8_UINT", DXGI_FORMAT_R8G8B8A8_UINT, Uint, {8, 8, 8, 8}},
      {"R8G8B8A8_SINT", DXGI_FORMAT_R8G8B8A8_SINT, Sint, {8, 8, 8, 8}},
      {"R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM, Unorm, {8, 8}},
      {"R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM, Snorm, {8, 8}},
      {"R8G8_UINT", DXGI_FORMAT_R8G8_UINT, Uint, {8, 8}},
      {"R8G8_SINT", DXGI_FORMAT_R8G8_SINT, Sint, {8, 8}},
      {"R8_UNORM", DXGI_FORMAT_R8_UNORM, Unorm, {8}},
      {"R8_SNORM", DXGI_FORMAT_R8_SNORM, Snorm, {8}},
      {"R8_UINT", DXGI_FORMAT_R8_UINT, Uint, {8}},
      {"R8_SINT", DXGI_FORMAT_R8_SINT, Sint, {8}},
      {"R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM, Unorm, {16, 16, 16, 16}},
      {"R16G16B16A16_SNORM", DXGI_FORMAT_R16G16B16A16_SNORM, Snorm, {16, 16, 16, 16}},
      {"R16G16B16A16_UINT", DXGI_FORMAT_R16G16B16A16_UINT, Uint, {16, 16, 16, 16}},
      {"R16G16B16A16_SINT", DXGI_FORMAT_R16G16B16A16_SINT, Sint, {16, 16, 16, 16}},
      {"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, Float, {16, 16, 16, 16}},
      {"R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM, Unorm, {16, 16}},
      {"R16G16_SNORM", DXGI_FORMAT_R16G16_SNORM, Snorm, {16, 16}},
      {"R16G16_UINT", DXGI_FORMAT_R16G16_UINT, Uint, {16, 16}},
      {"R16G16_SINT", DXGI_FORMAT_R16G16_SINT, Sint, {16, 16}},
      {"R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT, Float, {16, 16}},
      {"R16_UNORM", DXGI_FORMAT_R16_UNORM, Unorm, {16}},
      {"R16_SNORM", DXGI_FORMAT_R16_SNORM, Snorm, {16}},
      {"R16_UINT", DXGI_FORMAT_R16_UINT, Uint, {16}},
      {"R16_SINT", DXGI_FORMAT_R16_SINT, Sint, {16}},
      {"R16_FLOAT", DXGI_FORMAT_R16_FLOAT, Float, {16}},
      {"R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT, Float, {32, 32, 32, 32}},
      {"R32G32B32A32_UINT", DXGI_FORMAT_R32G32B32A32_UINT, Uint, {32, 32, 32, 32}},
      {"R32G32B32A32_SINT", DXGI_FORMAT_R32G32B32A32_SINT, Sint, {32, 32, 32, 32}},
      {"R32G32B32_FLOAT", DXGI_FORMAT_R32G32B32_FLOAT, Float, {32, 32, 32}},
      {"R32G32B32_UINT", DXGI_FORMAT_R32G32B32_UINT, Uint, {32, 32, 32}},
      {"R32G32B32_SINT", DXGI_FORMAT_R32G32B32_SINT, Sint, {32, 32, 32}},
      {"R32G32_FLOAT", DXGI_FORMAT_R32G32_FLOAT, Float, {32, 32}},
      {"R32G32_UINT", DXGI_FORMAT_R32G32_UINT, Uint, {32, 32}},
      {"R32G32_SINT", DXGI_FORMAT_R32G32_SINT, Sint, {32, 32}},
      {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT, Float, {32}},
      {"R32_UINT", DXGI_FORMAT_R32_UINT, Uint, {32}},
      {"R32_SINT", DXGI_FORMAT_R32_SINT, Sint, {32}},
      {"R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, Unorm, {10, 10, 10, 2}},
  };
  // bytes that differ, and reach the ends of the ranges: no two neighbours alike, with 0x00, 0x7f, 0x80 and 0xff
  const UINT vertices = 12, most = 16;
  std::vector<uint8_t> bytes(vertices * most);
  const uint8_t ends[] = {0x00, 0xff, 0x80, 0x7f, 0x81, 0x01};
  for (UINT i = 0; i < bytes.size(); i++)
    bytes[i] = i % 5 == 0 ? ends[i / 5 % std::size(ends)] : uint8_t(i * 37 + 11);
  D3D11_BUFFER_DESC vb_desc{(UINT)bytes.size(), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
  D3D11_SUBRESOURCE_DATA vb_data{bytes.data()};
  ComPtr<ID3D11Buffer> vb;
  CHECK(device->CreateBuffer(&vb_desc, &vb_data, &vb));
  D3D11_BUFFER_DESC so_desc{vertices * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
  ComPtr<ID3D11Buffer> so;
  CHECK(device->CreateBuffer(&so_desc, nullptr, &so));
  const D3D11_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, 4, 0};
  const UINT so_stride = 16;

  unsigned wrong = 0, checked = 0;
  for (auto &f : formats) {
    const char *entry_point = f.kind == Uint ? "as_uint" : f.kind == Sint ? "as_int" : "as_float";
    auto code = compile(hlsl, entry_point, "vs");
    if (!code) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> stream;
    ComPtr<ID3D11InputLayout> layout;
    const D3D11_INPUT_ELEMENT_DESC element{"DATA", 0, f.format, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    CHECK(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
    CHECK(device->CreateGeometryShaderWithStreamOutput(
        code->GetBufferPointer(), code->GetBufferSize(), &entry, 1, &so_stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &stream
    ));
    if (FAILED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout))) {
      printf("%s: no input layout\n", f.name);
      wrong++;
      continue;
    }
    UINT stride = (f.bits[0] + f.bits[1] + f.bits[2] + f.bits[3]) / 8, offset = 0;
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->GSSetShader(stream.Get(), nullptr, 0);
    context->SOSetTargets(1, so.GetAddressOf(), &offset);
    context->Draw(vertices, 0);
    context->SOSetTargets(0, nullptr, nullptr);
    auto got = read(device.Get(), context.Get(), so.Get());
    for (UINT v = 0; v < vertices; v++) {
      // the element's bits, its components from the lowest
      uint64_t element_bits[2] = {};
      memcpy(element_bits, bytes.data() + v * stride, stride);
      UINT at = 0;
      for (UINT c = 0; c < 4; c++) {
        UINT bits = f.bits[c], out = f.bgra && c < 3 ? 2 - c : c;
        UINT raw = 0;
        if (bits) {
          raw = UINT((element_bits[at / 64] >> at % 64 | (at % 64 + bits > 64 ? element_bits[1] << (64 - at % 64) : 0)) &
                     (bits == 32 ? ~0u : (1u << bits) - 1));
          at += bits;
        }
        int32_t integer = bits == 32 ? (int32_t)raw : (int32_t)(raw << (32 - std::max(bits, 1u))) >> (32 - std::max(bits, 1u));
        UINT have = got[v * 4 + out];
        float number;
        memcpy(&number, &have, sizeof(number));
        bool ok;
        char want[64];
        if (f.kind == Uint || f.kind == Sint) {
          UINT value = !bits ? c == 3 : f.kind == Uint ? raw : (UINT)integer;
          ok = have == value;
          snprintf(want, sizeof(want), "%#x", value);
        } else {
          float value = !bits            ? c == 3
                        : f.kind == Unorm ? raw / float((1u << bits) - 1)
                        : f.kind == Snorm ? std::max(integer / float((1u << (bits - 1)) - 1), -1.0f)
                        : bits == 16     ? half(raw)
                                         : [&] { float x; memcpy(&x, &raw, 4); return x; }();
          // a NaN stays one; a normalized value is within half a step of a 24-bit fraction
          ok = std::isnan(value) ? std::isnan(number) : f.kind == Float ? number == value : std::fabs(number - value) <= 1.0f / (1 << 23);
          snprintf(want, sizeof(want), "%g", value);
        }
        checked++;
        if (!ok && wrong++ < 16)
          printf("%s: vertex %u, component %u is %#x (%g), want %s\n", f.name, v, out, have, number, want);
      }
    }
  }
  printf("%s: %u wrong of %u components of %zu formats\n", wrong ? "failed" : "passed", wrong, checked, std::size(formats));
  return wrong != 0;
}
