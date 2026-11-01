// contract: a fence reaches the value a queue's Signal names only once all the work submitted to the queue before the
// Signal has completed, from every ExecuteCommandLists before it, so what that work wrote is there for the CPU as soon
// as the fence is seen: "the fence is signaled when the GPU reaches the Signal" (Microsoft Learn,
// ID3D12CommandQueue::Signal); SetEventOnCompletion's event "is signaled when the fence reaches" the value, and with
// no event the call "does not return until the fence reaches" it (ID3D12Fence::SetEventOnCompletion). nothing else
// is called between the fence and the read, as a title reads back right after its fence. Mullion's Metal 3 queue ends
// the command buffer it predicts a Signal to follow with a signal of an event of the buffer's slot, and retires it
// when that comes, before Metal calls it completed; Metal 4 waits for its queue's event (d3d12_completion has one
// submission a round).
// each round submits, through three ExecuteCommandLists: a clear of a render target to the round's number and a copy
// of the target into a readback buffer, as two lists of one (two submissions every fourth round, so the count before
// a Signal changes and the prediction misses); a long dispatch that writes the round's number into
// every word; and a long dispatch that adds the round's number to each word into another buffer, with a copy of that
// into a readback buffer. the last submission's work is long too, so a fence signalled when it begins is caught, and
// the second's results reach the readback only through it. it signals, waits for the fence through an event, by
// polling GetCompletedValue or by SetEventOnCompletion with no event, and reads both readbacks at once through the
// pointers they were mapped at before the first round.
#include "d3d12_test.hpp"
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer Round : register(b0) { uint round; uint zero; uint add; };
RWByteAddressBuffer source : register(u0);
RWByteAddressBuffer words : register(u1);
[numthreads(64, 1, 1)] void cs(uint id : SV_DispatchThreadID) {
  // long enough that a read before the work completes sees the round before: `zero` is 0, which the compiler cannot
  // know, so the loads stay
  uint sum = 0;
  [loop] for (uint i = 0; i < 64; i++) sum += words.Load(((id * 7 + i * 4099) & (COUNT - 1)) * 4);
  words.Store(id * 4, round + (add ? source.Load(id * 4) : 0) + sum * zero);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  constexpr UINT count = 1 << 20, rounds = 48, side = 512;
  auto code = compiler.compile(hlsl, "cs", "cs", {"COUNT=" + std::to_string(count)});
  if (!expect(!code.empty(), "the shader does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  // a list of each kind with an allocator of its own: the clear, the target's copy, the first dispatch, and the second
  // with the copy of its words
  enum { Clear, CopyTarget, Dispatch, AddAndCopy, Lists };
  ComPtr<ID3D12CommandAllocator> allocators[Lists];
  ComPtr<ID3D12GraphicsCommandList> lists[Lists];
  for (auto &allocator : allocators)
    CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));

  D3D12_ROOT_PARAMETER parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 3};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[2].Descriptor = {1, 0};
  auto rs = root_signature(device.Get(), {3, parameters});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  auto words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto sums = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, count * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  constexpr UINT pitch = side * 4;
  auto texels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pitch * side, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));
  if (!expect(rs && words && sums && readback && texels && target, "the root signature, buffers or target are refused"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  for (UINT i = 0; i < Lists; i++) {
    CHECK(device->CreateCommandList(0, queue_desc.Type, allocators[i].Get(), i >= Dispatch ? pso.Get() : nullptr,
                                    IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }

  // mapped once, as titles keep a readback buffer mapped: the pointers read what the GPU wrote, with nothing between
  UINT *mapped_words;
  float *mapped_texels;
  D3D12_RANGE range{0, count * 4}, texel_range{0, pitch * side};
  CHECK(readback->Map(0, &range, (void **)&mapped_words));
  CHECK(texels->Map(0, &texel_range, (void **)&mapped_texels));
  const char *ways[] = {"event", "polled", "blocking"};
  UINT wrong_rounds = 0, timed_out = 0;
  for (UINT round = 1; round <= rounds; round++) {
    for (UINT i = 0; i < Lists; i++) {
      CHECK(allocators[i]->Reset());
      CHECK(lists[i]->Reset(allocators[i].Get(), i >= Dispatch ? pso.Get() : nullptr));
    }
    // words = round, then sums = words + round
    for (UINT add = 0; add < 2; add++) {
      auto dispatch = lists[Dispatch + add].Get();
      dispatch->SetComputeRootSignature(rs.Get());
      const UINT constants[] = {round, 0, add};
      dispatch->SetComputeRoot32BitConstants(0, 3, constants, 0);
      dispatch->SetComputeRootUnorderedAccessView(1, words->GetGPUVirtualAddress());
      dispatch->SetComputeRootUnorderedAccessView(2, (add ? sums : words)->GetGPUVirtualAddress());
      dispatch->Dispatch(count / 64, 1, 1);
    }
    auto copy = lists[AddAndCopy].Get();
    transition(copy, sums.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy->CopyBufferRegion(readback.Get(), 0, sums.Get(), 0, count * 4);
    transition(copy, sums.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const float color[4] = {float(round), 0, 0, 0};
    lists[Clear]->ClearRenderTargetView(rtv, color, 0, nullptr);

    auto copy_target = lists[CopyTarget].Get();
    transition(copy_target, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to{texels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, side, side, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    copy_target->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(copy_target, target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (auto &list : lists)
      CHECK(list->Close());

    ID3D12CommandList *first[] = {lists[Clear].Get(), lists[CopyTarget].Get()}, *second[] = {lists[Dispatch].Get()},
                      *third[] = {lists[AddAndCopy].Get()};
    // every fourth round the clear and its copy are two submissions: the queue's count of command buffers before a
    // Signal changes, as a title's may
    if (round % 4)
      queue->ExecuteCommandLists(2, first);
    else
      for (auto one : first)
        queue->ExecuteCommandLists(1, &one);
    queue->ExecuteCommandLists(1, second);
    queue->ExecuteCommandLists(1, third);
    CHECK(queue->Signal(fence.Get(), round));
    // bounded: a fence that never comes fails the round instead of hanging the run
    auto way = round % 3;
    if (way == 0) {
      CHECK(fence->SetEventOnCompletion(round, event));
      timed_out += WaitForSingleObject(event, 10000) != WAIT_OBJECT_0;
    } else if (way == 1) {
      auto deadline = GetTickCount64() + 10000;
      while (fence->GetCompletedValue() < round && GetTickCount64() < deadline)
        Sleep(0);
      timed_out += fence->GetCompletedValue() < round;
    } else {
      CHECK(fence->SetEventOnCompletion(round, nullptr));
    }
    UINT wrong = 0, wrong_texels = 0;
    for (UINT i = 0; i < count; i += 4099)
      wrong += mapped_words[i] != 2 * round;
    wrong += mapped_words[count - 1] != 2 * round;
    for (UINT y = 0; y < side; y += 37)
      for (UINT x = 0; x < side; x += 41)
        wrong_texels += mapped_texels[y * side + x] != float(round);
    wrong_texels += mapped_texels[side * side - 1] != float(round);
    if ((wrong || wrong_texels) && !wrong_rounds++)
      printf("round %u (%s): %u sampled words and %u sampled texels are not the round's, the first word %u, the first "
             "texel %g\n", round, ways[way], wrong, wrong_texels, mapped_words[0], mapped_texels[0]);
  }
  D3D12_RANGE none{0, 0};
  readback->Unmap(0, &none);
  texels->Unmap(0, &none);
  CloseHandle(event);
  expect(!timed_out, "%u rounds' fences did not come in 10 s", timed_out);
  expect(!wrong_rounds, "%u of %u rounds read before their work completed", wrong_rounds, rounds);
  expect(device->GetDeviceRemovedReason() == S_OK, "the device is removed");
  return verdict();
}
