// contract: compute-shader math and bit operations give, for every input, what the D3D definition gives on the CPU:
// integer and bit results exactly, f16 conversions exactly for values half represents, transcendentals within
// kTranscendental (D3D11.3's sin/cos bound, absolute, or relative for unbounded results). inputs include values
// half cannot hold, so the f32tof16 rounding and overflow rules are exercised.
// min and max return the operand that is not NaN ("If one source operand is NaN, then the other source operand is
// returned", D3D11.3 22.10.10, 22.10.11), and _sat is min(1, max(0, value)) by those rules, so "sat(NaN) returns 0"
// (22.19.1).
#include "d3d12_test.hpp"
#include <bit>
#include <cfloat>
#include <cmath>
#include <vector>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint2> input : register(t0);
RWStructuredBuffer<uint4> ints : register(u0);
RWStructuredBuffer<float4> floats0 : register(u1);
RWStructuredBuffer<float4> floats1 : register(u2);
RWStructuredBuffer<float4> floats2 : register(u3);
[numthreads(64, 1, 1)] void cs(uint3 t : SV_DispatchThreadID) {
  uint a = input[t.x].x, b = input[t.x].y;
  float x = asfloat(a), y = asfloat(b);
  uint rb = reversebits(b), sad = msad4(a, uint2(b, a), uint4(1, 2, 3, 4)).x;
  ints[t.x * 3] = uint4(firstbithigh(a), firstbitlow(a), firstbithigh(int(b)), countbits(a) + (rb << 8));
  ints[t.x * 3 + 1] = uint4(f32tof16(x), asuint(f16tof32(b)), ((a >> 3) & 0x7f), asuint(int(b) >> 5));
  ints[t.x * 3 + 2] = uint4(sad, (a & ~(0x3f << 4)) | ((b & 0x3f) << 4),
                            asuint((int(a) << 7) >> 20), a / (b | 1) + a % (b | 1));
  float s = frac(x) * 2 - 1; // [-1, 1)
  floats0[t.x] = float4(tan(s), acos(s), asin(s), atan(x));
  floats1[t.x] = float4(cosh(s), sinh(s), tanh(x), atan2(y, x));
  floats2[t.x] = float4(saturate(x), min(x, y), max(x, y), saturate(x + y));
}
)hlsl";

// D3D11.3 sincos: "The maximum absolute error is 0.0008"; the other transcendentals, which DXBC builds from
// sincos, exp and log, are held to the same bound
constexpr float kTranscendental = 0.0008f;

// D3D11.3 floating point rules: denorms are flushed to sign-preserved zero on input to float math
static float
flushed(UINT bits) {
  float f = std::bit_cast<float>(bits);
  return std::fpclassify(f) == FP_SUBNORMAL ? std::copysign(0.0f, f) : f;
}

// D3D11.3 3.2.2: float to half rounds toward zero; past the half range, the largest finite half; infinity stays
static UINT
half_toward_zero(float f) {
  UINT bits = std::bit_cast<UINT>(f), sign = bits >> 16 & 0x8000, abs = bits & 0x7fffffff;
  int exponent = (int)(abs >> 23) - 127 + 15;
  if (abs == 0x7f800000)
    return sign | 0x7c00;
  if (exponent >= 31)
    return sign | 0x7bff;
  if (exponent > 0)
    return sign | exponent << 10 | (abs >> 13 & 0x3ff);
  // a half denormal keeps the mantissa bits that fit
  return exponent < -10 ? sign : sign | ((abs & 0x7fffff) | 0x800000) >> (14 - exponent);
}

static UINT
firsthigh(UINT v) {
  return v ? 31 - std::countl_zero(v) : ~0u;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[5] = {
      {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  for (UINT i = 1; i < std::size(params); i++)
    params[i].Descriptor.ShaderRegister = i - 1;
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  // inputs: bit patterns covering zero, all ones, single bits, sign boundaries and ordinary floats
  std::vector<UINT> in;
  for (UINT i = 0; i < 32; i++)
    in.insert(in.end(), {1u << i, ~(1u << i)});
  for (float f :
       {0.0f, -0.0f, 0.5f, -0.75f, 1.0f, 3.25f, -100.5f, 1e-3f, 65504.0f, 0.1f, 2.0f, -2.5f, 1.00075f, 65519.0f,
        70000.0f, -1e9f, 5.96e-8f, 3e-5f})
    in.push_back(std::bit_cast<UINT>(f));
  in.insert(in.end(), {0u, ~0u, 0x7fffffffu, 0x80000000u, 0x3c00u, 0x7bffu, 0x8001u, 0x12345678u});
  in.resize((in.size() + 63) / 64 * 64, 0x3f800000u);
  const UINT n = in.size();
  std::vector<UINT> pairs(2 * n);
  for (UINT i = 0; i < n; i++)
    pairs[2 * i] = in[i], pairs[2 * i + 1] = in[(i * 7 + 3) % n];

  const UINT64 ints_bytes = n * 48, floats_bytes = n * 16;
  auto src = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, n * 8, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto dst = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, ints_bytes + 3 * floats_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, ints_bytes + 3 * floats_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped;
  CHECK(src->Map(0, nullptr, &mapped));
  memcpy(mapped, pairs.data(), n * 8);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootShaderResourceView(0, src->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, dst->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(2, dst->GetGPUVirtualAddress() + ints_bytes);
  list->SetComputeRootUnorderedAccessView(3, dst->GetGPUVirtualAddress() + ints_bytes + floats_bytes);
  list->SetComputeRootUnorderedAccessView(4, dst->GetGPUVirtualAddress() + ints_bytes + 2 * floats_bytes);
  list->Dispatch(n / 64, 1, 1);
  transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), dst.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  auto ints = (UINT *)out;
  auto floats = (float *)(out + ints_bytes);
  unsigned mismatches = 0;
  auto exact = [&](const char *what, UINT i, UINT got, UINT want) {
    if (got != want && mismatches++ < 12)
      printf("%s, input %u (%#x, %#x): got %#x, want %#x\n", what, i, pairs[2 * i], pairs[2 * i + 1], got, want);
  };
  auto close = [&](const char *what, UINT i, float got, double want, bool relative) {
    double error = std::fabs(got - want) / (relative ? std::max(1.0, std::fabs(want)) : 1.0);
    if (!(error <= kTranscendental) && !(std::isnan(got) && std::isnan(want)) && mismatches++ < 12)
      printf("%s, input %u: got %g, want %g\n", what, i, got, want);
  };
  for (UINT i = 0; i < n; i++) {
    UINT a = pairs[2 * i], b = pairs[2 * i + 1];
    float x = flushed(a), y = flushed(b), s = (x - std::floor(x)) * 2 - 1;
    auto r = ints + 12 * i;
    exact("firstbithigh", i, r[0], firsthigh(a));
    exact("firstbitlow", i, r[1], a ? std::countr_zero(a) : ~0u);
    exact("firstbithigh signed", i, r[2], firsthigh((int)b < 0 ? ~b : b));
    exact("countbits + reversebits", i, r[3], std::popcount(a) + (__builtin_bitreverse32(b) << 8));
    if (!std::isnan(x))
      exact("f32tof16", i, r[4], half_toward_zero(x));
    // a NaN converts to some NaN; D3D does not fix its payload
    auto f16 = (float)std::bit_cast<_Float16>((uint16_t)b);
    exact(
        "f16tof32", i, std::isnan(f16) && std::isnan(std::bit_cast<float>(r[5])) ? 0 : r[5],
        std::isnan(f16) ? 0 : std::bit_cast<UINT>(f16)
    );
    exact("ubfe", i, r[6], (a >> 3) & 0x7f);
    exact("ashr", i, r[7], (UINT)((int)b >> 5));
    UINT msad = 1; // accumulator x
    for (int k = 0; k < 4; k++) {
      int ref = (a >> 8 * k) & 0xff, src = (b >> 8 * k) & 0xff;
      msad += ref ? std::abs(ref - src) : 0;
    }
    exact("msad4", i, r[8], msad);
    exact("bfi", i, r[9], (a & ~(0x3fu << 4)) | ((b & 0x3f) << 4));
    exact("ibfe", i, r[10], (UINT)(((int)(a << 7)) >> 20));
    exact("udiv + urem", i, r[11], a / (b | 1) + a % (b | 1));
    auto f0 = floats + 4 * i, f1 = floats + 4 * (n + i), f2 = floats + 4 * (2 * n + i);
    // the operand that is not NaN, else the one the comparison takes; a result may be flushed or not, and the
    // comparison does not tell the zeros apart
    auto same = [&](const char *what, float got, float want) {
      bool ok = std::isnan(want) ? std::isnan(got) : flushed(std::bit_cast<UINT>(got)) == want;
      if (!ok && mismatches++ < 12)
        printf("%s, input %u (%#x, %#x): got %g, want %g\n", what, i, a, b, got, want);
    };
    auto most = [](float l, float r) { return std::isnan(l) ? r : std::isnan(r) ? l : l >= r ? l : r; };
    auto least = [](float l, float r) { return std::isnan(l) ? r : std::isnan(r) ? l : l < r ? l : r; };
    auto saturated = [&](float v) { return least(1, most(0, v)); };
    same("saturate", f2[0], saturated(x));
    same("min", f2[1], least(x, y));
    same("max", f2[2], most(x, y));
    same("saturate of a sum", f2[3], saturated(flushed(std::bit_cast<UINT>(x + y))));
    if (!std::isfinite(x))
      continue;
    close("tan", i, f0[0], std::tan((double)s), true);
    close("acos", i, f0[1], std::acos((double)s), false);
    close("asin", i, f0[2], std::asin((double)s), false);
    close("atan", i, f0[3], std::atan((double)x), false);
    close("cosh", i, f1[0], std::cosh((double)s), true);
    close("sinh", i, f1[1], std::sinh((double)s), true);
    // d3dcompiler_47 builds tanh from exp(2x), which overflows past ln(FLT_MAX) / 2 on any D3D device
    if (compiler.dxc || std::fabs(x) < std::log(FLT_MAX) / 2)
      close("tanh", i, f1[2], std::tanh((double)x), false);
    if (std::isfinite(y) && (x != 0 || y != 0))
      close("atan2", i, f1[3], std::atan2((double)y, (double)x), false);
  }
  printf("%s: %u mismatches over %u inputs\n", mismatches ? "failed" : "passed", mismatches, n);
  return mismatches != 0;
}
