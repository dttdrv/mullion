// contract: integer operations that Metal's compiler, or the converter's own IR, got wrong once folded (tests/fuzz.py
// found each; docs/workarounds.md "In the shader converter"), computed as D3D11.3 says, on values the compiler cannot
// see:
// - udiv by zero gives 0xffffffff for the quotient and the remainder (22.13.43), also where a firstbithigh reads the
//   remainder: firstbithigh(c % c) is 31 for c = 0. (the converter's LLVM division by a zero divisor was undefined)
// - firstbithigh and countbits of all ones that is a comparison's mask: 31 and 32 (Metal took it as 16 bits wide;
//   MULLION_AIRCONV_SKIP=sign-bit-counts brings that back)
// - a constant divided by a variable: 0xfffffffe / d and 0xffffffff % d for 4096 divisors (Metal got some wrong;
//   MULLION_AIRCONV_SKIP=constant-dividends)
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
Buffer<uint> inputs : register(t0);
RWStructuredBuffer<uint> outputs : register(u0);
[numthreads(64, 1, 1)]
void cs(uint t : SV_DispatchThreadID) {
  uint c = inputs[0], d = inputs[1 + t], a = inputs[2], b = inputs[3];
  // each in a thread of its own: Metal's folding of one depends on what else the thread does with its values
  if (t == 0)
    outputs[0] = c % c;
  if (t == 1)
    outputs[1] = firstbithigh(c % c) ^ uint((a ^ c) == c);
  if (t == 2)
    outputs[2] = countbits(uint(b == 127u) % uint(c == 2147483647u));
  if (t == 3)
    outputs[3] = countbits(uint2(b == 127u, a == 3u) % uint2(c == 2147483647u, c == 1u)).x;
  outputs[4 + t * 2] = 0xfffffffeu / d;
  outputs[5 + t * 2] = 0xffffffffu % d;
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  auto code = compile(hlsl, "cs", "cs");
  if (!code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs));
  constexpr UINT kThreads = 4096;
  std::vector<uint32_t> inputs(1 + kThreads);
  for (UINT i = 0; i < kThreads; i++)
    inputs[1 + i] = i % 2 ? 3 + i % 61 : 1 + i * 2654435761u % 100000; // 3 and small primes' neighbours, and a spread
  inputs[0] = 0; // c
  // a and b, the divisors of threads 1 and 2: firstbithigh(c % c) ^ (a == 0), and 1 % 0 for b == 127
  inputs[2] = 0x10000;
  inputs[3] = 127;
  D3D11_BUFFER_DESC in_desc{UINT(inputs.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
  D3D11_SUBRESOURCE_DATA init{inputs.data()};
  ComPtr<ID3D11Buffer> in;
  CHECK(device->CreateBuffer(&in_desc, &init, &in));
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{DXGI_FORMAT_R32_UINT, D3D11_SRV_DIMENSION_BUFFER};
  srv_desc.Buffer = {0, UINT(inputs.size())};
  ComPtr<ID3D11ShaderResourceView> srv;
  CHECK(device->CreateShaderResourceView(in.Get(), &srv_desc, &srv));
  constexpr UINT kOutputs = 4 + kThreads * 2;
  D3D11_BUFFER_DESC out_desc{kOutputs * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
  ComPtr<ID3D11Buffer> out;
  CHECK(device->CreateBuffer(&out_desc, nullptr, &out));
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER};
  uav_desc.Buffer = {0, kOutputs, 0};
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateUnorderedAccessView(out.Get(), &uav_desc, &uav));
  context->CSSetShader(cs.Get(), nullptr, 0);
  context->CSSetShaderResources(0, 1, srv.GetAddressOf());
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
  context->Dispatch(kThreads / 64, 1, 1);
  auto words = read(device.Get(), context.Get(), out.Get());
  if (words.size() != kOutputs) {
    printf("failed: readback\n");
    return 1;
  }
  int wrong = 0;
  auto check = [&](uint32_t got, uint32_t want, const char *what, uint32_t divisor = 0) {
    if (got != want && wrong++ < 8) {
      if (divisor)
        printf("wrong: %s %u = 0x%08x, want 0x%08x\n", what, divisor, got, want);
      else
        printf("wrong: %s 0x%08x, want 0x%08x\n", what, got, want);
    }
  };
  check(words[0], 0xffffffff, "c % c, c = 0:");
  check(words[1], 31, "firstbithigh(c % c) ^ (a == c), c = 0:");
  check(words[2], 32, "countbits(1 % 0):");
  check(words[3], 32, "countbits(uint2(1, 0) % uint2(0, 0)).x:");
  for (UINT i = 4; i < kThreads; i++) { // threads 0 to 3 have the cases above in their divisors' words
    uint32_t d = inputs[1 + i];
    check(words[4 + i * 2], 0xfffffffeu / d, "0xfffffffe / d, d =", d);
    check(words[5 + i * 2], 0xffffffffu % d, "0xffffffff % d, d =", d);
  }
  if (wrong) {
    printf("failed: %d wrong results\n", wrong);
    return 1;
  }
  printf("passed: 0 wrong results\n");
  return 0;
}
