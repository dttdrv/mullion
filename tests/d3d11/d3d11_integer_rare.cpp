// contract: control flow and integer operations give the values the D3D11 rules give (D3D11.3 22.7, 22.12), checked
// against the same expressions on the CPU for 64 inputs: a loop that skips with continue, a switch with the [call]
// attribute, whose cases fxc makes subroutines (call, label), a conditional swap and an unsigned multiply-add.
#include "d3d11_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint4> results : register(u0);
cbuffer C : register(b0) { uint steps; };

[numthreads(64, 1, 1)]
void cs(uint i : SV_GroupIndex) {
  uint x = i * 2654435761u + 12345u;
  // continue: sum the odd steps' values
  uint sum = 0;
  for (uint k = 0; k < steps; k++) {
    if ((x >> k) & 1)
      continue;
    sum += k * x;
  }
  // a conditional swap of two values
  uint a = x & 0xffff, b = x >> 16;
  if (a > b) {
    uint t = a;
    a = b;
    b = t;
  }
  // a switch whose cases may be subroutines
  uint picked;
  [call] switch (i % 4) {
  case 0: picked = x ^ 0x5a5a5a5au; break;
  case 1: picked = x + steps; break;
  case 2: picked = x * 3; break;
  default: picked = ~x; break;
  }
  // unsigned multiply-add
  uint madded = mad(x, 7u, i);
  results[i] = uint4(sum, a * 65536 + b, picked, madded);
}
)hlsl";

int
main() {
  auto cs = compile(hlsl, "cs", "cs");
  if (!cs) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D11ComputeShader> shader;
  CHECK(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &shader));
  const UINT threads = 64, steps = 20;
  D3D11_BUFFER_DESC result_desc{threads * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
  ComPtr<ID3D11Buffer> results;
  CHECK(device->CreateBuffer(&result_desc, nullptr, &results));
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateUnorderedAccessView(results.Get(), nullptr, &uav));
  D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  const UINT words[4] = {steps};
  D3D11_SUBRESOURCE_DATA constants_data{words};
  ComPtr<ID3D11Buffer> constants;
  CHECK(device->CreateBuffer(&constants_desc, &constants_data, &constants));

  step("integer operations and control flow of 64 threads");
  context->CSSetShader(shader.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
  context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
  context->Dispatch(1, 1, 1);
  auto got = read(device.Get(), context.Get(), results.Get());
  if (!expect(got.size() == threads * 4, "readback size"))
    return verdict();
  for (UINT i = 0; i < threads; i++) {
    uint32_t x = i * 2654435761u + 12345u, sum = 0;
    for (uint32_t k = 0; k < steps; k++)
      if (!((x >> k) & 1))
        sum += k * x;
    uint32_t a = x & 0xffff, b = x >> 16;
    if (a > b)
      std::swap(a, b);
    uint32_t picked = i % 4 == 0 ? x ^ 0x5a5a5a5au : i % 4 == 1 ? x + steps : i % 4 == 2 ? x * 3 : ~x;
    uint32_t want[4] = {sum, a * 65536 + b, picked, x * 7 + i};
    const char *names[] = {"continue loop", "conditional swap", "[call] switch", "mad"};
    for (UINT c = 0; c < 4; c++)
      expect(got[i * 4 + c] == want[c], "%s of thread %u: %#x, want %#x", names[c], i, got[i * 4 + c], want[c]);
  }
  return verdict();
}
