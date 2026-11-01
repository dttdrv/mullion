// contract: ExecuteIndirect runs the commands of its own call and no others, whichever allocators earlier calls were
// recorded with: "CommandCount = min(CommandCount, MaxCommandCount)" (Microsoft Learn,
// ID3D12GraphicsCommandList::ExecuteIndirect, Remarks), and an allocator's memory is the runtime's again once it is
// reset or released after the GPU is done (ID3D12CommandAllocator::Reset, Remarks). Mullion keeps the indirect command
// buffers it makes for ExecuteIndirect and gives them to later calls, across allocators too (dttdrv/mullion#32), so a
// call may get one that an earlier call of another allocator filled with more commands.
// in the first round, four allocators each run 64 commands into a buffer of their own; two are then released and two
// reset twice. in the second, new allocators run fewer commands (a count buffer), as many, or more, into new buffers.
// command n adds n + 1 to word n of its buffer: every buffer holds exactly its own call's words afterwards, and the
// first round's buffers are unchanged by the second.
#include "d3d12_test.hpp"
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer Command : register(b0) { uint n; };
RWByteAddressBuffer words : register(u0);
[numthreads(1, 1, 1)] void cs() { words.InterlockedAdd(n * 4, n + 1); }
)hlsl";

constexpr UINT kWords = 256;

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

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 1};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {2, parameters});
  if (!expect(rs != nullptr, "the root signature is refused"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  struct Command {
    UINT n;
    D3D12_DISPATCH_ARGUMENTS dispatch;
  };
  D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  arguments[0].Constant = {0, 0, 1};
  arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(Command), 2, arguments};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, rs.Get(), IID_PPV_ARGS(&signature)));

  // the commands, and the counts the count buffer offers, one per call
  auto commands = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kWords * sizeof(Command), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 64 * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(commands && counts, "the buffers are refused"))
    return verdict();
  Command *mapped;
  CHECK(commands->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < kWords; i++)
    mapped[i] = {i, {1, 1, 1}};
  commands->Unmap(0, nullptr);

  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 fenced = 0;
  auto wait = [&]() -> int {
    CHECK(queue->Signal(fence.Get(), ++fenced));
    if (fence->GetCompletedValue() < fenced) {
      auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      CHECK(fence->SetEventOnCompletion(fenced, event));
      WaitForSingleObject(event, INFINITE);
      CloseHandle(event);
    }
    return 0;
  };

  struct Call {
    UINT max, count; // count < max reads the count buffer
    ComPtr<ID3D12Resource> words;
  };
  UINT count_slot = 0;
  UINT *count_values;
  CHECK(counts->Map(0, nullptr, (void **)&count_values));
  // one list per call, each of its own allocator
  auto run = [&](std::vector<ComPtr<ID3D12CommandAllocator>> &allocators, std::vector<Call> &calls) -> int {
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    for (size_t i = 0; i < calls.size(); i++) {
      auto &call = calls[i];
      call.words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kWords * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device->CreateCommandList(0, queue_desc.Type, allocators[i].Get(), pso.Get(), IID_PPV_ARGS(&list)));
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootUnorderedAccessView(1, call.words->GetGPUVirtualAddress());
      ID3D12Resource *count_buffer = nullptr;
      UINT64 count_offset = 0;
      if (call.count < call.max) {
        count_values[count_slot] = call.count;
        count_buffer = counts.Get(), count_offset = count_slot++ * sizeof(UINT);
      }
      list->ExecuteIndirect(signature.Get(), call.max, commands.Get(), 0, count_buffer, count_offset);
      CHECK(list->Close());
      lists.push_back(list);
    }
    std::vector<ID3D12CommandList *> submitted;
    for (auto &list : lists)
      submitted.push_back(list.Get());
    queue->ExecuteCommandLists(submitted.size(), submitted.data());
    return wait();
  };
  // reads a buffer back and compares it with its call's words
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kWords * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> check_allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&check_allocator)));
  auto check = [&](const Call &call, const char *round, size_t index) -> int {
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(check_allocator->Reset());
    CHECK(device->CreateCommandList(0, queue_desc.Type, check_allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    transition(list.Get(), call.words.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, call.words.Get(), 0, kWords * 4);
    transition(list.Get(), call.words.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(list->Close());
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    UINT *words;
    CHECK(readback->Map(0, nullptr, (void **)&words));
    UINT wrong = 0;
    for (UINT n = 0; n < kWords; n++) {
      UINT want = n < call.count ? n + 1 : 0;
      if (words[n] != want && !wrong++)
        printf("%s call %zu (%u of at most %u): word %u holds %u, want %u\n", round, index, call.count, call.max, n,
               words[n], want);
    }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%s call %zu: %u wrong words", round, index, wrong);
    return 0;
  };

  std::vector<ComPtr<ID3D12CommandAllocator>> first(4), second(5);
  for (auto &allocator : first)
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  std::vector<Call> first_calls(4, Call{64, 64});
  if (run(first, first_calls))
    return 1;
  // two allocators gone, two reset with nothing recorded since (what their last use made stays unused)
  first[0].Reset(), first[1].Reset();
  for (int i = 0; i < 2; i++)
    for (int j = 2; j < 4; j++)
      CHECK(first[j]->Reset());

  for (auto &allocator : second)
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  // the call of more commands asks first, while what the first round left is there to be taken
  std::vector<Call> second_calls = {{128, 100}, {64, 3}, {64, 64}, {64, 40}, {64, 0}};
  if (run(second, second_calls))
    return 1;
  counts->Unmap(0, nullptr);
  for (size_t i = 0; i < first_calls.size(); i++)
    if (check(first_calls[i], "first", i))
      return 1;
  for (size_t i = 0; i < second_calls.size(); i++)
    if (check(second_calls[i], "second", i))
      return 1;
  return verdict();
}
