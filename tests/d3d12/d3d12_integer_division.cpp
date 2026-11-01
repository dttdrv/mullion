// contract: an unsigned integer division or remainder by zero gives 0xffffffff ("Divide by zero returns 0xffffffff for
// both quotient and remainder", DXIL.rst, UDiv; D3D11.3 22.12.3, udiv), and the shader goes on as written after it. the
// divisors are root constants the compiler cannot see: a quotient and a remainder by zero, a remainder by zero in a
// loop, and a test of the divisor after each division, which has to find it zero (LLVM takes a division as proof that
// its divisor is not zero, and folds such a test away). signed division and remainder by zero, and INT_MIN / -1, are
// left undefined by D3D: their values are not checked, only that a test of the divisor after them still finds it zero,
// and that signed division by nonzero divisors truncates toward zero.
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer Values : register(b0) { uint a; uint zero; uint minimum; uint minus_one; uint negative; uint two; };
RWByteAddressBuffer results : register(u0);
[numthreads(1, 1, 1)] void cs() {
  uint q = a / zero;
  results.Store(0, q);
  results.Store(4, zero == 0 ? 111 : 222);
  uint r = a % zero;
  results.Store(8, r);
  results.Store(12, zero == 0 ? 111 : 222);
  uint sum = 0;
  [loop] for (uint i = 0; i < 4; i++) sum += (a + i) % zero;
  results.Store(16, sum);
  int sq = int(a) / int(zero), sr = int(a) % int(zero);
  results.Store(20, zero == 0 ? 111 : 222);
  int oq = int(minimum) / int(minus_one), orem = int(minimum) % int(minus_one);
  results.Store(24, minus_one == 0xffffffff ? 111 : 222);
  results.Store(28, uint(int(negative) / int(two)));
  results.Store(32, uint(int(negative) % int(two)));
  results.Store(36, uint(int(negative) / -int(two)));
  // kept, so the divisions above stay; their values are not checked
  results.Store(40, uint(sq + sr + oq + orem) & 0);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!code.empty(), "the shader does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 6};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {2, parameters});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  constexpr UINT words = 11;
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(rs && results && readback, "the root signature or buffers are refused"))
    return verdict();

  const UINT values[6] = {7, 0, 0x80000000u, 0xffffffffu, (UINT)-7, 2};
  list->SetComputeRootSignature(rs.Get());
  list->SetPipelineState(pso.Get());
  list->SetComputeRoot32BitConstants(0, 6, values, 0);
  list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, 256);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  const char *what[words] = {"7 / 0",
                             "the divisor after 7 / 0",
                             "7 % 0",
                             "the divisor after 7 % 0",
                             "the sum of 4 remainders by 0 in a loop",
                             "the divisor after a signed division by 0",
                             "the divisor after INT_MIN / -1",
                             "-7 / 2",
                             "-7 % 2",
                             "-7 / -2",
                             "nothing"};
  const UINT want[words] = {0xffffffffu, 111, 0xffffffffu, 111, 0xfffffffcu, 111, 111, (UINT)-3, (UINT)-1, 3, 0};
  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  for (UINT i = 0; i < words; i++)
    if (got[i] != want[i] && ++wrong)
      printf("%s is %08x, want %08x\n", what[i], got[i], want[i]);
  readback->Unmap(0, nullptr);
  expect(!wrong, "%u wrong results", wrong);
  return verdict();
}
