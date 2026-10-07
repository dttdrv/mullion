// contract: a copy, a compute and a direct queue run their work in the order fences give it, however the
// application hands the work over. Wait "queues a GPU-side wait, and returns immediately", Signal sets the fence
// when the queue reaches it (ID3D12CommandQueue::Wait, ::Signal), and ExecuteCommandLists never waits for the GPU.
// each frame the copy queue copies the frame's number into a buffer, the compute queue waits for that, multiplies it
// and adds the frame, and the direct queue waits for that and copies the result out; the copy queue's next frame
// waits for the direct queue. the readback then holds one value a frame, which is only right in that order.
// - in order: 64 frames handed over without the CPU waiting once, so more lists are in flight than any queue of
//   the GPU's holds at once (Metal's command queues hold 32 uncompleted command buffers here);
// - waits first: a queue is given a wait and 48 lists behind it before the queue that signals the fence is given
//   anything. the calls return, and the lists run after the signal.
// - a wait, then tile mappings, then the signal the wait is for, all from one thread: UpdateTileMappings returns
//   while the queue holds its work behind the wait (the thread that would signal is the one calling), and the
//   mapping is there for the list after it, which writes the tile and reads it back;
// - signals from many threads at once, more a thread than the GPU's queue holds command buffers, each of its own
//   fence to its own value: every fence has its value once the queue's last signal has been reached. once behind
//   a list that keeps the GPU, so that the signals fill the GPU's queue and the threads wait for its places, and
//   once behind a wait that ends when half of them are in, so that held work is taken up while more arrives;
// - queues made and released without use, one after another: each release destroys its queue and returns.
// no wait of the test is unbounded: one that outlives its time is a wrong result, and the test ends.
#include "d3d12_test.hpp"
#include <atomic>
#include <thread>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint frame; };
RWStructuredBuffer<uint> number : register(u0);
RWStructuredBuffer<uint> results : register(u1);
[numthreads(1, 1, 1)] void cs() { results[frame] = number[0] * MULTIPLIER + frame; }
// work that keeps the GPU for a while: every thread steps a random number generator SPINS times and adds what it
// ends at to the number, so no step can be left out
[numthreads(64, 1, 1)] void cs_busy(uint3 thread : SV_DispatchThreadID) {
  uint random = thread.x;
  for (uint i = 0; i < SPINS; i++)
    random = random * 1664525 + 1013904223;
  InterlockedAdd(number[0], random);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT multiplier = 3, frames = 64, behind = 48, seconds = 60, spins = 1 << 16;
  std::vector<std::string> defines{"MULTIPLIER=" + std::to_string(multiplier), "SPINS=" + std::to_string(spins)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), cs_busy = compiler.compile(hlsl, "cs_busy", "cs", defines);
  if (cs.empty() || cs_busy.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                        {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].Constants = {0, 0, 1};
  parameters[2].Descriptor = {1, 0};
  auto rs = root_signature(device.Get(), {3, parameters});
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  // the numbers the copy queue copies, one a frame and one more for the waits-first pass
  const UINT count = std::max(frames, behind);
  auto numbers = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (frames + 1) * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto number = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * sizeof(UINT), D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, count * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  UINT *value;
  CHECK(numbers->Map(0, nullptr, (void **)&value));
  for (UINT i = 0; i <= frames; i++)
    value[i] = 1000 + 7 * i;

  enum { Copy, Compute, Direct, Queues };
  const D3D12_COMMAND_LIST_TYPE types[Queues] = {D3D12_COMMAND_LIST_TYPE_COPY, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                 D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queues[Queues];
  ComPtr<ID3D12CommandAllocator> allocators[Queues];
  ComPtr<ID3D12Fence> fences[Queues];
  for (UINT q = 0; q < Queues; q++) {
    D3D12_COMMAND_QUEUE_DESC desc{types[q]};
    CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queues[q])));
    CHECK(device->CreateCommandAllocator(types[q], IID_PPV_ARGS(&allocators[q])));
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[q])));
  }
  // a closed list of a queue's type, recorded by `record`: all lists of a queue share its allocator, one after
  // another, and none is reset before the test ends
  std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
  auto recorded = [&](UINT q, auto record) -> ID3D12CommandList * {
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, types[q], allocators[q].Get(), nullptr, IID_PPV_ARGS(&list))))
      return nullptr;
    record(list.Get());
    list->Close();
    lists.push_back(list);
    return list.Get();
  };
  auto copies = [&](UINT from) {
    return recorded(Copy, [&](auto list) { list->CopyBufferRegion(number.Get(), 0, numbers.Get(), from * sizeof(UINT), sizeof(UINT)); });
  };
  auto computes = [&](UINT frame) {
    return recorded(Compute, [&](auto list) {
      D3D12_RESOURCE_BARRIER barriers[2] = {{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION}, {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION}};
      barriers[0].Transition = {number.Get(), 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
      barriers[1].Transition = {results.Get(), 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
      list->ResourceBarrier(2, barriers);
      list->SetPipelineState(pso.Get());
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRoot32BitConstant(0, frame, 0);
      list->SetComputeRootUnorderedAccessView(1, number->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
      list->Dispatch(1, 1, 1);
      for (auto &b : barriers)
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      list->ResourceBarrier(2, barriers);
    });
  };
  auto reads = [&](UINT first, UINT n) {
    return recorded(Direct, [&](auto list) {
      transition(list, results.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), first * sizeof(UINT), results.Get(), first * sizeof(UINT), n * sizeof(UINT));
      transition(list, results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    });
  };
  // the fence at the value within the test's time, or a wrong result and the test's end: a queue that never gets
  // there does not let its objects go either
  auto reached = [&](UINT q, UINT64 v) {
    auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    bool done = SUCCEEDED(fences[q]->SetEventOnCompletion(v, event)) && WaitForSingleObject(event, seconds * 1000) == WAIT_OBJECT_0;
    if (!expect(done, "fence of queue %u at %llu after %u s, waiting for %llu", q, fences[q]->GetCompletedValue(), seconds, v))
      ExitProcess(verdict());
  };
  auto check = [&](UINT first, UINT n, auto want) {
    UINT *got;
    if (FAILED(readback->Map(0, nullptr, (void **)&got)))
      return;
    for (UINT i = first; i < first + n; i++)
      expect(got[i] == want(i), "result %u: %u, want %u", i, got[i], want(i));
    readback->Unmap(0, nullptr);
  };

  step("in order: %u frames through the copy, compute and direct queues without a wait of the CPU's", frames);
  for (UINT frame = 0; frame < frames; frame++) {
    ID3D12CommandList *copy = copies(frame), *compute = computes(frame), *read = reads(frame, 1);
    if (!copy || !compute || !read) {
      printf("failed: no command list for frame %u\n", frame);
      return 1;
    }
    CHECK(queues[Copy]->Wait(fences[Direct].Get(), frame));
    queues[Copy]->ExecuteCommandLists(1, &copy);
    CHECK(queues[Copy]->Signal(fences[Copy].Get(), frame + 1));
    CHECK(queues[Compute]->Wait(fences[Copy].Get(), frame + 1));
    queues[Compute]->ExecuteCommandLists(1, &compute);
    CHECK(queues[Compute]->Signal(fences[Compute].Get(), frame + 1));
    CHECK(queues[Direct]->Wait(fences[Compute].Get(), frame + 1));
    queues[Direct]->ExecuteCommandLists(1, &read);
    CHECK(queues[Direct]->Signal(fences[Direct].Get(), frame + 1));
  }
  reached(Direct, frames);
  check(0, frames, [&](UINT i) { return value[i] * multiplier + i; });

  step("waits first: a wait and %u lists on the compute queue, then the copy and the signal they wait for", behind);
  // the lists read the number the copy queue has not copied yet: only after the signal is it the new one
  std::vector<ID3D12CommandList *> waiting;
  for (UINT i = 0; i < behind; i++)
    waiting.push_back(computes(i));
  ID3D12CommandList *copy = copies(frames), *read = reads(0, behind);
  std::atomic<bool> handed_over = false;
  std::thread application([&] {
    queues[Compute]->Wait(fences[Copy].Get(), frames + 1);
    for (auto &list : waiting)
      queues[Compute]->ExecuteCommandLists(1, &list);
    queues[Compute]->Signal(fences[Compute].Get(), frames + 1);
    handed_over = true;
    queues[Copy]->ExecuteCommandLists(1, &copy);
    queues[Copy]->Signal(fences[Copy].Get(), frames + 1);
  });
  for (UINT waited = 0; !handed_over && waited < seconds * 10; waited++)
    Sleep(100);
  if (!expect(handed_over, "Wait, ExecuteCommandLists of %u lists and Signal have not returned after %u s", behind, seconds))
    ExitProcess(verdict());
  application.join();
  CHECK(queues[Direct]->Wait(fences[Compute].Get(), frames + 1));
  queues[Direct]->ExecuteCommandLists(1, &read);
  CHECK(queues[Direct]->Signal(fences[Direct].Get(), frames + 1));
  reached(Direct, frames + 1);
  check(0, behind, [&](UINT i) { return value[frames] * multiplier + i; });

  // what an application's thread calls, for at most the test's time: a call that does not return is a wrong result
  // and the test's end
  auto returns = [&](const char *what, auto calls) {
    std::atomic<bool> returned = false;
    std::thread thread([&] {
      calls();
      returned = true;
    });
    for (UINT waited = 0; !returned && waited < seconds * 10; waited++)
      Sleep(100);
    if (!expect(returned, "%s have not returned after %u s", what, seconds))
      ExitProcess(verdict());
    thread.join();
  };
  UINT64 direct_value = frames + 1, compute_value = frames + 1;

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_1) {
    step("a wait, tile mappings and the signal the wait is for, from one thread");
    const UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    D3D12_RESOURCE_DESC reserved_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 2 * tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                      D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    D3D12_HEAP_DESC heap_desc{tile, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    ComPtr<ID3D12Resource> reserved;
    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Fence> gate;
    CHECK(device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&reserved)));
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    // the buffer's second tile on the heap's only one; a number goes into it and comes back
    const D3D12_TILED_RESOURCE_COORDINATE second{1};
    const D3D12_TILE_REGION_SIZE one{1};
    const UINT heap_tile = 0, tiles = 1, number = frames - 1;
    CHECK(forget(readback.Get()));
    auto through = recorded(Direct, [&](auto list) {
      list->CopyBufferRegion(reserved.Get(), tile, numbers.Get(), number * sizeof(UINT), sizeof(UINT));
      transition(list, reserved.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, reserved.Get(), tile, sizeof(UINT));
    });
    returns("Wait, UpdateTileMappings and the other queue's Signal", [&] {
      queues[Direct]->Wait(gate.Get(), 1);
      queues[Direct]->UpdateTileMappings(reserved.Get(), 1, &second, &one, heap.Get(), 1, nullptr, &heap_tile, &tiles,
                                         D3D12_TILE_MAPPING_FLAG_NONE);
      queues[Copy]->Signal(gate.Get(), 1);
    });
    queues[Direct]->ExecuteCommandLists(1, &through);
    CHECK(queues[Direct]->Signal(fences[Direct].Get(), ++direct_value));
    reached(Direct, direct_value);
    check(0, 1, [&](UINT) { return value[number]; });
  }

  // a queue takes what it is given in the order of the calls, however many of its command buffers are on their way
  // (ID3D12CommandQueue::Signal: "Updates a fence to a specified value", once the work before it is done). a list
  // keeps the GPU for a moment (groups of 64 threads, SPINS steps each) with `ahead` signals behind it, and then
  // every thread signals at the same instant: for some `ahead` the threads meet the last free place of whatever
  // the queue keeps its work in, and each signal has to arrive all the same. `ahead` goes past any such size
  const UINT threads = 16, each = 4, most_ahead = 64, moment_ms = 40;
  ComPtr<ID3D12PipelineState> busy_pso;
  pso_desc.CS = bytecode(cs_busy);
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&busy_pso)));
  auto busy_for = [&](UINT groups) {
    return recorded(Compute, [&](auto list) {
      transition(list, number.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetPipelineState(busy_pso.Get());
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootUnorderedAccessView(1, number->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
      // rows of groups, past what one dimension of a dispatch takes
      const UINT across = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
      list->Dispatch(std::min(groups, across), (groups + across - 1) / across, 1);
      transition(list, number.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    });
  };
  // how many groups keep this GPU for the moment: from the time `measured` groups take over the time one takes, which
  // is the time of a list with nothing to do
  const UINT measured = 1 << 15;
  auto timed = [&](UINT groups) {
    auto probe = busy_for(groups);
    LARGE_INTEGER before, after, second;
    QueryPerformanceFrequency(&second);
    QueryPerformanceCounter(&before);
    queues[Compute]->ExecuteCommandLists(1, &probe);
    if (FAILED(queues[Compute]->Signal(fences[Compute].Get(), ++compute_value)))
      return 0.0;
    reached(Compute, compute_value);
    QueryPerformanceCounter(&after);
    return 1000.0 * (after.QuadPart - before.QuadPart) / second.QuadPart;
  };
  timed(1);
  const double idle_ms = timed(1), took_ms = timed(measured), work_ms = std::max(took_ms - idle_ms, 0.01);
  const UINT busy_groups = measured * moment_ms / work_ms;
  printf("%u groups keep the GPU %.2f ms (a list of one group takes %.2f ms): the list is of %u\n", measured, work_ms, idle_ms, busy_groups);
  auto busy = busy_for(busy_groups);
  std::vector<ComPtr<ID3D12Fence>> signalled(most_ahead + threads * each);
  for (auto &fence : signalled)
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  ComPtr<ID3D12Fence> held_behind, busy_done;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held_behind)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&busy_done)));
  UINT met = 0;
  // a round's signals give each fence the round's number; the last round is the one behind a wait
  for (UINT round = 1; round <= most_ahead + 1; round++) {
    bool held = round > most_ahead;
    UINT ahead = held ? 0 : round - 1;
    step(held ? "%u threads signal %u fences each on one queue at once, behind a wait that ends when half are in"
              : "%u threads signal %u fences each on one queue at the same instant, behind a list and %u signals",
         threads, each, ahead);
    std::atomic<UINT> in = 0, ready = 0;
    std::atomic<bool> go = false;
    returns("the threads' Signals", [&] {
      // the threads are there before the list is, so that it still keeps the GPU when they start
      std::vector<std::thread> pool;
      for (UINT t = 0; t < threads; t++)
        pool.emplace_back([&, t] {
          for (ready++; !go;)
            ;
          for (UINT fence = most_ahead + t * each; fence < most_ahead + (t + 1) * each; fence++, in++)
            queues[Compute]->Signal(signalled[fence].Get(), round);
        });
      while (ready < threads)
        Sleep(0);
      if (held) {
        queues[Compute]->Wait(held_behind.Get(), 1);
      } else {
        queues[Compute]->ExecuteCommandLists(1, &busy);
        queues[Compute]->Signal(busy_done.Get(), round);
      }
      // the signals ahead come from a thread of their own: past the queue's size they wait for the list
      pool.emplace_back([&] {
        for (UINT fence = 0; fence < ahead; fence++, in++)
          queues[Compute]->Signal(signalled[fence].Get(), round);
      });
      for (UINT waited = 0; in < ahead && waited < 5; waited++)
        Sleep(1);
      met += !held && busy_done->GetCompletedValue() < round;
      go = true;
      while (held && in < threads * each / 2)
        Sleep(0);
      if (held)
        held_behind->Signal(1);
      for (auto &thread : pool)
        thread.join();
    });
    CHECK(queues[Compute]->Signal(fences[Compute].Get(), ++compute_value));
    reached(Compute, compute_value);
    for (UINT fence = 0; fence < signalled.size(); fence++)
      if (fence < ahead || fence >= most_ahead)
        expect(signalled[fence]->GetCompletedValue() == round, "fence %u is at %llu after the queue's last signal, want %u",
               fence, signalled[fence]->GetCompletedValue(), round);
  }
  // a round whose list had ended before its threads started met no full queue
  // (a quarter of the rounds: how long a list keeps this GPU is measured once and moves)
  expect(met > most_ahead / 4, "the list kept the GPU until the threads started in %u of %u rounds", met, most_ahead);

  const UINT unused = 256;
  step("%u queues made and released without use", unused);
  returns("CreateCommandQueue and Release", [&] {
    for (UINT i = 0; i < unused; i++) {
      D3D12_COMMAND_QUEUE_DESC desc{types[i % Queues]};
      ID3D12CommandQueue *made = nullptr;
      HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&made));
      ULONG left = made ? made->Release() : 0;
      if (!expect(hr == S_OK && !left, "queue %u: CreateCommandQueue %08lx, and %lu references after its release", i, hr, left))
        break;
    }
  });
  return verdict();
}
