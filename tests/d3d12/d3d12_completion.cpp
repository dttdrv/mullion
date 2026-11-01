// contract: a fence reaches the value a queue's Signal names only once the work submitted before the Signal has
// completed, so what that work wrote is there for the CPU as soon as the fence is seen: "the fence is signaled when
// the GPU reaches the Signal" (Microsoft Learn, ID3D12CommandQueue::Signal), and SetEventOnCompletion's event "is
// signaled when the fence reaches" the value (ID3D12Fence::SetEventOnCompletion). nothing else is called between the
// fence and the read, as a title reads back right after its fence (Mullion signals a Metal 4 command buffer's fences
// when the GPU's event says its work is done, before Metal's feedback, which GetDeviceRemovedReason waits for).
// each round runs a long dispatch that writes the round's number into every word, copies the words to a readback
// buffer, signals, and then waits through an event or by polling GetCompletedValue, and reads the words at once
// through the pointer the buffer was mapped at before the first round.
#include "d3d12_test.hpp"
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer Round : register(b0) { uint round; uint zero; };
RWByteAddressBuffer words : register(u0);
[numthreads(64, 1, 1)] void cs(uint id : SV_DispatchThreadID) {
  // long enough that a read before the work completes sees the round before: `zero` is 0, which the compiler cannot
  // know, so the loads stay
  uint sum = 0;
  [loop] for (uint i = 0; i < 64; i++) sum += words.Load(((id * 7 + i * 4099) & (COUNT - 1)) * 4);
  words.Store(id * 4, round + sum * zero);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  constexpr UINT count = 1 << 20, rounds = 32;
  auto code = compiler.compile(hlsl, "cs", "cs", {"COUNT=" + std::to_string(count)});
  if (!expect(!code.empty(), "the shader does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 2};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {2, parameters});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  auto words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, count * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(rs && words && readback, "the root signature or buffers are refused"))
    return verdict();
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // mapped once, as titles keep a readback buffer mapped: the pointer reads what the GPU wrote, with nothing between
  UINT *mapped;
  D3D12_RANGE range{0, count * 4};
  CHECK(readback->Map(0, &range, (void **)&mapped));
  UINT wrong_rounds = 0;
  for (UINT round = 1; round <= rounds; round++) {
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    list->SetComputeRootSignature(rs.Get());
    const UINT constants[] = {round, 0};
    list->SetComputeRoot32BitConstants(0, 2, constants, 0);
    list->SetComputeRootUnorderedAccessView(1, words->GetGPUVirtualAddress());
    list->Dispatch(count / 64, 1, 1);
    transition(list.Get(), words.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, words.Get(), 0, count * 4);
    transition(list.Get(), words.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    CHECK(queue->Signal(fence.Get(), round));
    if (round & 1) {
      CHECK(fence->SetEventOnCompletion(round, event));
      WaitForSingleObject(event, INFINITE);
    } else {
      while (fence->GetCompletedValue() < round)
        Sleep(0);
    }
    UINT wrong = 0;
    for (UINT i = 0; i < count; i += 4099)
      wrong += mapped[i] != round;
    wrong += mapped[count - 1] != round;
    if (wrong && !wrong_rounds++)
      printf("round %u (%s): %u sampled words are not the round's, the first %u\n", round,
             round & 1 ? "event" : "polled", wrong, mapped[0]);
  }
  D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  CloseHandle(event);
  expect(!wrong_rounds, "%u of %u rounds read before their work completed", wrong_rounds, rounds);
  return verdict();
}
