// contract: SetPredication skips every operation of its region when its buffer's value meets the op, and runs every
// one otherwise (Direct3D 12 "Predication"), however much the list records in the region. here a region holds more
// ExecuteIndirect commands than the memory a command list starts with has room for the arguments of (their root
// arguments alone are 35 MB), so the region goes on past it; vkd3d-proton's
// test_execute_indirect_state_predication, which passes on Windows, has such a region too.
// each command dispatches one thread that writes its own element; a second region of the opposite verdict is the
// control. the list runs twice, with EQUAL_ZERO and with NOT_EQUAL_ZERO: the long region is skipped once and runs
// once, and each element says which.
#include "d3d12_test.hpp"
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint element; };
RWStructuredBuffer<uint> o : register(u0);
[numthreads(1, 1, 1)] void cs() { o[element] = element + 1; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!cs.empty(), "HLSL did not compile"))
    return verdict();
  // a command's root arguments are a qword for each root parameter: 16 parameters, 8192 commands at most in each
  // of 32 calls. a call runs 4 of them, as the count buffer says
  constexpr UINT kParameters = 16, kMost = 8192, kCalls = 32, kRun = 4, kLong = kCalls * kRun, kElements = kLong + kRun;
  struct Command {
    UINT element;
    D3D12_DISPATCH_ARGUMENTS dispatch;
  };

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[kParameters] = {};
  // b0 is the shader's and u0 its output; b1 on are nothing's
  for (UINT i = 0; i < kParameters; i++) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[i].Constants = {i ? i - 1 : 0, 0, 1};
  }
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {kParameters, params});
  CHECK(rs ? S_OK : E_FAIL);
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT}, {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
  arguments[0].Constant = {0, 0, 1};
  D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(Command), 2, arguments};
  ComPtr<ID3D12CommandSignature> sig;
  CHECK(device->CreateCommandSignature(&sig_desc, rs.Get(), IID_PPV_ARGS(&sig)));

  // the upload buffer: the predicate's two values (0 and 1), the count, then the commands, with room past the last
  // call's for the most a call may run
  const UINT count_at = 16, commands_at = 32, zeros_at = commands_at + (kElements + kMost) * sizeof(Command);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, zeros_at + kElements * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kElements * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kElements * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && output && readback, "buffers were not made"))
    return verdict();
  uint8_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  memset(mapped, 0, zeros_at + kElements * sizeof(UINT));
  const UINT64 one = 1;
  memcpy(mapped + 8, &one, sizeof(one));
  memcpy(mapped + count_at, &kRun, sizeof(kRun));
  for (UINT i = 0; i < kElements; i++) {
    Command command{i, {1, 1, 1}};
    memcpy(mapped + commands_at + i * sizeof(Command), &command, sizeof(command));
  }
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the predicate of the long region is the 0, the control's the 1
  const D3D12_PREDICATION_OP ops[] = {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO};
  for (auto op : ops) {
    bool skips = op == D3D12_PREDICATION_OP_EQUAL_ZERO;
    step("the long region is %s", skips ? "skipped" : "run");
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->CopyBufferRegion(output.Get(), 0, upload.Get(), zeros_at, kElements * sizeof(UINT));
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pso.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->SetPredication(upload.Get(), 0, op);
    for (UINT call = 0; call < kCalls; call++)
      list->ExecuteIndirect(sig.Get(), kMost, upload.Get(), commands_at + call * kRun * sizeof(Command), upload.Get(), count_at);
    list->SetPredication(upload.Get(), 8, op);
    list->ExecuteIndirect(sig.Get(), kMost, upload.Get(), commands_at + kLong * sizeof(Command), upload.Get(), count_at);
    list->SetPredication(nullptr, 0, op);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned wrong = 0;
    // a mark for each call, the control's last: X where an element of it is not what the predicate makes it
    char calls[kCalls + 2] = {};
    for (UINT i = 0; i < kElements; i++) {
      // the long region's elements, then the control's, which has the other verdict
      UINT want = (i < kLong) != skips ? i + 1 : 0;
      wrong += got[i] != want;
      if (got[i] != want || !calls[i / kRun])
        calls[i / kRun] = got[i] != want ? 'X' : '.';
    }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u of %u elements wrong, by call: %s", wrong, kElements, calls);
  }
  return verdict();
}
