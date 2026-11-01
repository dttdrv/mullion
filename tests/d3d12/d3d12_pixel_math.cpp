// contract: frac, sin and cos in a pixel shader, which Mullion compiles with fast math, obey their D3D tables for
// every input class, scalar and vector, ordinary and precise, 32-bit and native 16-bit (-enable-16bit-types, SM 6.2).
// frc: "dest = src0 - round_ni(src0)", and TableFrc gives NaN for -inf, +inf and NaN (D3D11.3 22.10.7,
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/frc--sm4---asm-): the expected value is that
// subtraction rounded once, never a negative zero. sincos: "The maximum absolute error is 0.0008 in the interval from
// -100*Pi to +100*Pi", any finite angle's results are in [-1, 1], and TableSinCos gives NaN for infinities and NaN,
// the sign's zero as the sine of a zero or a 32-bit denormal, and 1 as its cosine (D3D11.3 22.10.20). 32-bit denormals
// are flushed to sign-preserved zero on input to float math (D3D11.3 3.1.3.2), 16-bit ones are kept (3.1.5), so a half
// denormal's sine is held to the error bound like any small angle's. a 16-bit result is the 32-bit bound plus its own
// rounding, half a half ulp below 1 (2^-12): D3D gives no separate bound for it. inputs: zeros, denormals, the
// smallest normals, values whose fraction rounds to 1 (a negative one too small to have a fraction), integers at and
// near 2^23 (2^10 for half), the largest finite, infinities, NaNs with other payloads, angles around +-100 pi and
// past it up to the largest float, every power of two and seeded random values. each value is in every lane once.
// results go through an integer render target and a UAV, as bits, so no later operation can hide a NaN or a sign.
#include "d3d12_test.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint4> input : register(t0);
RWStructuredBuffer<uint4> output : register(u1);
struct V { float4 pos : SV_Position; };
V vs(uint id : SV_VertexID) {
  V v;
  float2 uv = float2((id << 1) & 2, id & 2);
  v.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return v;
}
#if HALF
#define T float16_t
#define T4 float16_t4
#define BITS(x) uint4(asuint16(x))
#define BIT(x) uint(asuint16(x))
#define VALUE(u) asfloat16(uint16_t4(u))
#else
#define T float
#define T4 float4
#define BITS(x) asuint(x)
#define BIT(x) asuint(x)
#define VALUE(u) asfloat(u)
#endif
uint4 ps(V v) : SV_Target0 {
  uint i = uint(v.pos.y) * WIDTH + uint(v.pos.x);
  PRECISE T4 x = VALUE(input[i]);
  PRECISE T4 s = sin(x), c = cos(x), f = frac(x);
  // the same operations one component at a time, each lane taking another
  PRECISE T s0 = sin(x.y), c0 = cos(x.z), f0 = frac(x.w), f1 = frac(x.x);
  output[i * 3 + 0] = BITS(s);
  output[i * 3 + 1] = BITS(c);
  output[i * 3 + 2] = uint4(BIT(f1), BIT(s0), BIT(c0), BIT(f0));
  return BITS(f);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr double accuracy_limit = 100 * std::numbers::pi, bound = 0.0008, half_rounding = 1.0 / 4096;
  const UINT width = 64, lanes = 4;

  // the 32-bit inputs
  std::vector<float> floats;
  {
    const float integral = std::ldexp(1.0f, std::numeric_limits<float>::digits - 1), limit = float(accuracy_limit);
    for (float a : {0.0f, std::numeric_limits<float>::denorm_min(),
                    std::nextafter(std::numeric_limits<float>::min(), 0.0f), std::numeric_limits<float>::min(),
                    1e-30f, 1e-8f, 0.25f, 0.5f, 1.0f, 3.75f, std::nextafter(1.0f, 0.0f), 2.5f, 1e3f,
                    std::nextafter(integral, 0.0f), integral, integral + 2, 1234.5678f,
                    std::numeric_limits<float>::max(), std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN(), std::bit_cast<float>(0x7fa00001u),
                    std::bit_cast<float>(0x7f800001u), float(std::numbers::pi), float(std::numbers::pi / 2),
                    std::nextafter(float(std::numbers::pi), 0.0f), std::nextafter(float(std::numbers::pi), 4.0f),
                    std::nextafter(limit, 0.0f), limit, std::nextafter(limit, 1e9f), 1e4f, 12345.678f, 1e6f,
                    std::ldexp(1.0f, 21), std::ldexp(1.0f, 22) + 3, 1e8f, 1e10f, 1e20f, 1e30f})
      floats.insert(floats.end(), {a, -a});
    for (int e = std::numeric_limits<float>::min_exponent - 1; e < std::numeric_limits<float>::max_exponent; e++)
      floats.insert(floats.end(), {std::ldexp(1.0f, e), -std::ldexp(1.5f, e)});
    std::mt19937 random(20261031);
    std::uniform_real_distribution<float> angles(-1000, 1000);
    for (int i = 0; i < 512; i++)
      floats.push_back(angles(random));
  }
  // the 16-bit inputs, as bits
  std::vector<uint16_t> halves;
  {
    using half = _Float16;
    for (half a : {half(0), std::bit_cast<half>(uint16_t(1)), std::bit_cast<half>(uint16_t(0x3ff)),
                   std::bit_cast<half>(uint16_t(0x400)), half(0.25), half(0.5), half(1), half(3.75), half(1023.5),
                   half(1024), half(1025), std::bit_cast<half>(uint16_t(0x3bff)), half(std::numbers::pi),
                   half(accuracy_limit), half(1000), half(12345), half(65504), std::bit_cast<half>(uint16_t(0x7c00))})
      halves.insert(halves.end(), {std::bit_cast<uint16_t>(a), std::bit_cast<uint16_t>(half(-a))});
    halves.insert(halves.end(), {0x7e00, 0xfe00, 0x7c01, 0x7d55});
    for (int e = -24; e < 16; e++)
      halves.insert(halves.end(), {std::bit_cast<uint16_t>(half(std::ldexp(1.0f, e))),
                                   std::bit_cast<uint16_t>(half(-std::ldexp(1.5f, e)))});
    std::mt19937 random(31102026);
    std::uniform_real_distribution<float> angles(-400, 400);
    for (int i = 0; i < 256; i++)
      halves.push_back(std::bit_cast<uint16_t>(half(angles(random))));
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));

  for (bool half : {false, true})
    for (bool precise : {false, true}) {
      if (half && !compiler.dxc)
        continue; // native 16-bit types are shader model 6.2
      const UINT n = half ? halves.size() : floats.size(), height = (n + width - 1) / width, count = width * height;
      std::vector<std::array<UINT, lanes>> input(count);
      for (UINT i = 0; i < count; i++)
        for (UINT lane = 0; lane < lanes; lane++) {
          UINT k = (i + lane) % n;
          input[i][lane] = half ? halves[k] : std::bit_cast<UINT>(floats[k]);
        }
      std::vector<std::string> defines = {"WIDTH=" + std::to_string(width), "HALF=" + std::to_string(half),
                                          std::string("PRECISE=") + (precise ? "precise" : "")};
      std::vector<std::wstring> flags;
      if (half)
        flags.push_back(L"-enable-16bit-types");
      auto vs = compiler.compile(hlsl, "vs", half ? "vs_6_2" : "vs", defines, flags),
           ps = compiler.compile(hlsl, "ps", half ? "ps_6_2" : "ps", defines, flags);
      if (!expect(!vs.empty() && !ps.empty(), "half=%u precise=%u did not compile", half, precise))
        return verdict();
      step("frac, sin and cos, %s%s, over %u inputs in a pixel shader", half ? "16-bit" : "32-bit",
           precise ? ", precise" : "", n);

      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
      desc.SampleDesc = {1, 0};
      ComPtr<ID3D12PipelineState> pso;
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

      const UINT64 input_bytes = count * sizeof(input[0]), output_bytes = 3 * input_bytes,
                   pitch = width * 16, target_at = (input_bytes + output_bytes + 511) & ~UINT64(511);
      auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input_bytes + output_bytes,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_at + pitch * height,
                             D3D12_RESOURCE_STATE_COPY_DEST);
      UINT *mapped;
      CHECK(upload->Map(0, nullptr, (void **)&mapped));
      memcpy(mapped, input.data(), input_bytes);
      // an infinity's bits are never an expected result: a write that never happened cannot pass
      for (UINT64 i = input_bytes / 4; i < (input_bytes + output_bytes) / 4; i++)
        mapped[i] = 0x7f800000;
      upload->Unmap(0, nullptr);
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
                                      DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R32G32B32A32_UINT};
      ComPtr<ID3D12Resource> target;
      CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)));
      ComPtr<ID3D12DescriptorHeap> rtvs;
      D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
      CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
      auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);

      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                      IID_PPV_ARGS(&list)));
      list->CopyBufferRegion(output.Get(), 0, upload.Get(), input_bytes, output_bytes);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      // an integer target clears to the integer each value is: an infinity's bits
      const float infinity = float(0x7f800000u), cleared[4] = {infinity, infinity, infinity, infinity};
      list->ClearRenderTargetView(rtv, cleared, 0, nullptr);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootShaderResourceView(0, upload->GetGPUVirtualAddress());
      list->SetGraphicsRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, 1, 0, 0);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, output_bytes);
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{target.Get()};
      dst.PlacedFootprint = {target_at, {DXGI_FORMAT_R32G32B32A32_UINT, width, height, 1, UINT(pitch)}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));

      uint8_t *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      auto value = [&](UINT bits) -> double {
        return half ? double(std::bit_cast<_Float16>(uint16_t(bits))) : double(std::bit_cast<float>(bits));
      };
      // what the table and the bounds allow. `op`: 0 frac, 1 sin, 2 cos
      auto allowed = [&](int op, UINT in, UINT out) {
        if (half && out >> 16)
          return false;
        double a = value(in), g = value(out);
        if (!std::isfinite(a))
          return std::isnan(g);
        bool zero = half ? (in & 0x7fff) == 0 : (in & 0x7f800000) == 0;
        if (zero)
          a = std::copysign(0.0, a); // a 32-bit denormal is flushed; a 16-bit zero is a zero
        if (op == 0) {
          double fraction = a - std::floor(a);
          double want = half ? double(_Float16(fraction)) : double(float(fraction));
          return g == want && !std::signbit(g);
        }
        if (zero) {
          UINT want = op == 2 ? (half ? 0x3c00 : 0x3f800000) : (half ? in & 0x8000 : in & 0x80000000);
          return out == want;
        }
        if (!(g >= -1 && g <= 1))
          return false;
        double want = op == 1 ? std::sin(a) : std::cos(a);
        return std::abs(a) > accuracy_limit || std::abs(g - want) <= bound + (half ? half_rounding : 0);
      };
      static const char *names[] = {"frac", "sin", "cos"};
      for (UINT i = 0; i < count; i++) {
        auto record = [&](UINT k) { return (const UINT *)(got + (i * 3 + k) * sizeof(input[0])); };
        auto texel = (const UINT *)(got + target_at + i / width * pitch + i % width * 16);
        for (UINT lane = 0; lane < lanes; lane++) {
          // the vector results, then the scalars: lane 0 frac of x.x, 1 sin of x.y, 2 cos of x.z, 3 frac of x.w
          const std::pair<int, UINT> results[] = {{0, texel[lane]}, {1, record(0)[lane]}, {2, record(1)[lane]},
                                                  {lane == 0 || lane == 3 ? 0 : lane, record(2)[lane]}};
          for (auto [op, bits] : results)
            expect(allowed(op, input[i][lane], bits), "%s precise=%u pixel %u lane %u: %s(%#x) returned %#x",
                   half ? "16-bit" : "32-bit", precise, i, lane, names[op], input[i][lane], bits);
        }
      }
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
