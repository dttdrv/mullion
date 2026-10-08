// contract: a queue serializes whole submissions from different threads, including its signals and tile mappings.
// a read of two words written across a split must therefore return both old values or both new values.
// "Applications can submit command lists to any command queue from multiple threads. The runtime will perform the
// work of serializing these requests in the order of submission." (Microsoft Learn, Executing and synchronizing
// command lists, Executing command Lists). "Any thread may submit a command list to any command queue at any time,
// and the runtime will automatically serialize submission of the command list in the command queue while preserving
// the submission order." (Design philosophy of command queues and command lists, GPU work submission).
// "Use this method to set a fence value from the GPU side." (ID3D12CommandQueue::Signal, Remarks).
#include "d3d12_test.hpp"
#include <functional>
#include <iterator>

int
main() {
  const UINT rounds = 2048;
  const DWORD timeout_ms = 10000, test_timeout_ms = 60000;
  auto finished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(finished != nullptr, "watchdog event was created"))
    return verdict();
  auto watchdog = CreateThread(
      nullptr, 0,
      [](void *event) -> DWORD {
        if (WaitForSingleObject(event, test_timeout_ms) != WAIT_OBJECT_0) {
          printf("failed: test did not finish within %lu ms\n", test_timeout_ms);
          ExitProcess(1);
        }
        return 0;
      },
      finished, 0, nullptr
  );
  if (!expect(watchdog != nullptr, "watchdog was created"))
    return verdict();
  const UINT64 word = sizeof(UINT), pair = 2 * word;
  const UINT64 predicates[] = {0, 1};
  const D3D12_PREDICATION_OP ops[] = {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO};
  const UINT split_counts[] = {0, 1, 2};
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue, reader;
  D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&reader)));
  ComPtr<ID3D12Fence> done, signalled;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&signalled)));
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (rounds + 1) * word, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto predicate = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(predicates), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, pair, D3D12_RESOURCE_STATE_COMMON);
  auto scratch = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, word, D3D12_RESOURCE_STATE_COMMON);
  auto snapshot = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, pair, D3D12_RESOURCE_STATE_COMMON);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * pair + word, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && predicate && words && scratch && snapshot && readback, "buffers were created"))
    return verdict();
  UINT *epochs;
  CHECK(upload->Map(0, nullptr, (void **)&epochs));
  for (UINT i = 0; i <= rounds; i++)
    epochs[i] = i;
  upload->Unmap(0, nullptr);
  void *mapped;
  CHECK(predicate->Map(0, nullptr, &mapped));
  memcpy(mapped, predicates, sizeof(predicates));
  predicate->Unmap(0, nullptr);

  enum { Writer, Observer, Final, Transfer, Lists };
  ComPtr<ID3D12CommandAllocator> allocators[Lists];
  ComPtr<ID3D12GraphicsCommandList> lists[Lists], tail;
  for (UINT i = 0; i < Lists; i++) {
    CHECK(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, desc.Type, allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  CHECK(device->CreateCommandList(0, desc.Type, allocators[Writer].Get(), nullptr, IID_PPV_ARGS(&tail)));
  CHECK(tail->Close());
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  auto go = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!expect(event && go, "events were created"))
    return verdict();
  auto wait = [&](HANDLE handle, const char *what) {
    if (!expect(
            WaitForSingleObject(handle, timeout_ms) == WAIT_OBJECT_0, "%s completed within %lu ms", what, timeout_ms
        ))
      ExitProcess(verdict());
  };
  UINT64 completed = 0;
  auto drain = [&](ID3D12CommandQueue *of) {
    CHECK(of->Signal(done.Get(), ++completed));
    CHECK(done->SetEventOnCompletion(completed, event));
    wait(event, "GPU work");
    CHECK(device->GetDeviceRemovedReason());
    return 0;
  };
  auto copy_word = [&](ID3D12GraphicsCommandList *list, ID3D12Resource *to, UINT64 offset, UINT epoch) {
    transition(list, to, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(to, offset, upload.Get(), epoch * word, word);
    transition(list, to, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
  };
  auto read_words = [&](ID3D12GraphicsCommandList *list, ID3D12Resource *to, UINT64 offset) {
    transition(list, words.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(to, offset, words.Get(), 0, pair);
    transition(list, words.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  };

  struct Submission {
    std::function<HRESULT()> work;
    HANDLE go{}, ready{};
    DWORD timeout{};
    HRESULT result = S_OK;
  };
  auto submit_thread = [](void *data) -> DWORD {
    auto &s = *static_cast<Submission *>(data);
    if (!ReleaseSemaphore(s.ready, 1, nullptr) || WaitForSingleObject(s.go, s.timeout) != WAIT_OBJECT_0) {
      s.result = E_FAIL;
      return 1;
    }
    s.result = s.work();
    return FAILED(s.result);
  };
  Submission submissions[2];
  auto ready = CreateSemaphoreA(nullptr, 0, std::size(submissions), nullptr);
  if (!expect(ready != nullptr, "ready semaphore was created"))
    return verdict();
  auto race = [&] {
    if (!expect(ResetEvent(go), "start event was reset"))
      ExitProcess(verdict());
    HANDLE threads[std::size(submissions)];
    for (UINT i = 0; i < std::size(submissions); i++) {
      submissions[i].go = go;
      submissions[i].ready = ready;
      submissions[i].timeout = timeout_ms;
      threads[i] = CreateThread(nullptr, 0, submit_thread, &submissions[i], 0, nullptr);
      if (!expect(threads[i] != nullptr, "thread %u was created", i))
        ExitProcess(verdict());
      wait(ready, "thread ready");
    }
    if (!expect(SetEvent(go), "start event was set"))
      ExitProcess(verdict());
    if (!expect(
            WaitForMultipleObjects(std::size(threads), threads, TRUE, timeout_ms) == WAIT_OBJECT_0,
            "submissions returned within %lu ms", timeout_ms
        ))
      ExitProcess(verdict());
    for (UINT i = 0; i < std::size(threads); i++) {
      CloseHandle(threads[i]);
      CHECK(submissions[i].result);
    }
    return 0;
  };
  for (bool signal : {false, true}) {
    step(
        signal ? "ExecuteCommandLists and Signal, with the snapshot read on a second queue, %u epochs"
               : "ExecuteCommandLists from two threads, %u epochs",
        rounds
    );
    CHECK(allocators[Writer]->Reset());
    CHECK(lists[Writer]->Reset(allocators[Writer].Get(), nullptr));
    copy_word(lists[Writer].Get(), words.Get(), 0, 0);
    copy_word(lists[Writer].Get(), words.Get(), word, 0);
    CHECK(lists[Writer]->Close());
    ID3D12CommandList *initial = lists[Writer].Get();
    queue->ExecuteCommandLists(1, &initial);
    if (drain(queue.Get()))
      return 1;
    UINT mixed = 0;
    for (UINT epoch = 1; epoch <= rounds; epoch++) {
      const UINT splits = split_counts[epoch % std::size(split_counts)];
      const UINT value = (epoch / std::size(ops)) % std::size(predicates);
      const auto op = ops[epoch % std::size(ops)];
      const bool skipped = (predicates[value] == 0) == (op == D3D12_PREDICATION_OP_EQUAL_ZERO);
      const bool two_lists = (epoch / (std::size(predicates) * std::size(ops))) % 2;
      for (UINT i = 0; i < Lists; i++) {
        CHECK(allocators[i]->Reset());
        CHECK(lists[i]->Reset(allocators[i].Get(), nullptr));
      }
      copy_word(lists[Writer].Get(), scratch.Get(), 0, 0);
      copy_word(lists[Writer].Get(), words.Get(), 0, epoch);
      if (two_lists) {
        CHECK(lists[Writer]->Close());
        CHECK(tail->Reset(allocators[Writer].Get(), nullptr));
      }
      auto rest = two_lists ? tail.Get() : lists[Writer].Get();
      transition(rest, scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      for (UINT split = 0; split < splits; split++) {
        rest->SetPredication(predicate.Get(), value * sizeof(predicates[0]), op);
        rest->CopyBufferRegion(scratch.Get(), 0, upload.Get(), epoch * word, word);
        rest->SetPredication(nullptr, 0, op);
      }
      transition(rest, scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      copy_word(rest, words.Get(), word, epoch);
      if (signal)
        transition(lists[Observer].Get(), snapshot.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      read_words(lists[Observer].Get(), signal ? snapshot.Get() : readback.Get(), 0);
      if (signal) {
        transition(lists[Observer].Get(), snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(
            lists[Transfer].Get(), snapshot.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        lists[Transfer]->CopyBufferRegion(readback.Get(), 0, snapshot.Get(), 0, pair);
        transition(
            lists[Transfer].Get(), snapshot.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON
        );
      }
      read_words(lists[Final].Get(), readback.Get(), pair);
      transition(lists[Final].Get(), scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      lists[Final]->CopyBufferRegion(readback.Get(), 2 * pair, scratch.Get(), 0, word);
      transition(lists[Final].Get(), scratch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      CHECK(rest->Close());
      CHECK(lists[Observer]->Close());
      CHECK(lists[Final]->Close());
      CHECK(lists[Transfer]->Close());
      CHECK(forget(readback.Get()));
      ID3D12CommandList *writes[] = {lists[Writer].Get(), tail.Get()}, *reads[] = {lists[Observer].Get()};
      const UINT64 fence_value = (signal ? rounds : 0) + epoch;
      if (signal) {
        CHECK(reader->Wait(signalled.Get(), fence_value));
        ID3D12CommandList *transfer = lists[Transfer].Get();
        reader->ExecuteCommandLists(1, &transfer);
      }
      submissions[0].work = [&] {
        queue->ExecuteCommandLists(two_lists ? std::size(writes) : 1, writes);
        return S_OK;
      };
      submissions[1].work = [&] {
        queue->ExecuteCommandLists(std::size(reads), reads);
        return signal ? queue->Signal(signalled.Get(), fence_value) : S_OK;
      };
      if (race())
        return 1;
      if (drain(signal ? reader.Get() : queue.Get()))
        return 1;
      ID3D12CommandList *final = lists[Final].Get();
      queue->ExecuteCommandLists(1, &final);
      if (drain(queue.Get()))
        return 1;
      const UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      const bool consistent = got[0] == got[1] && (got[0] == epoch || got[0] == epoch - 1);
      if (!consistent) {
        mixed++;
        expect(
            false, "epoch %u, %u splits, %u lists: X=%u, Y=%u, want both %u or %u", epoch, splits, two_lists ? 2 : 1,
            got[0], got[1], epoch, epoch - 1
        );
      }
      expect(got[2] == epoch && got[3] == epoch, "epoch %u: final X=%u, Y=%u", epoch, got[2], got[3]);
      expect(got[4] == (splits && !skipped ? epoch : 0), "epoch %u: predicated copies wrote %u", epoch, got[4]);
      readback->Unmap(0, nullptr);
    }
    printf("%u of %u rounds observed an invalid pair\n", mixed, rounds);
    expect(mixed == 0, "%u rounds interleaved submissions", mixed);
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_1) {
    step("UpdateTileMappings and CopyTileMappings racing a split read, %u epochs", rounds);
    const UINT64 tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    const UINT heap_tiles = 2;
    D3D12_RESOURCE_DESC tiled{
        D3D12_RESOURCE_DIMENSION_BUFFER, 0, heap_tiles * tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR
    };
    ComPtr<ID3D12Resource> backing, view;
    CHECK(device->CreateReservedResource(&tiled, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&backing)));
    tiled.Width = tile;
    CHECK(device->CreateReservedResource(&tiled, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&view)));
    ComPtr<ID3D12Heap> heap;
    D3D12_HEAP_DESC heap_desc{heap_tiles * tile, {D3D12_HEAP_TYPE_DEFAULT}, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    const D3D12_TILED_RESOURCE_COORDINATE first{};
    const D3D12_TILE_REGION_SIZE one{1}, all{heap_tiles};
    const UINT first_heap_tile = 0;
    queue->UpdateTileMappings(
        backing.Get(), 1, &first, &all, heap.Get(), 1, nullptr, &first_heap_tile, nullptr, D3D12_TILE_MAPPING_FLAG_NONE
    );
    queue->UpdateTileMappings(
        view.Get(), 1, &first, &one, heap.Get(), 1, nullptr, &first_heap_tile, nullptr, D3D12_TILE_MAPPING_FLAG_NONE
    );
    CHECK(allocators[Writer]->Reset());
    CHECK(lists[Writer]->Reset(allocators[Writer].Get(), nullptr));
    for (UINT at = 0; at < heap_tiles; at++)
      copy_word(lists[Writer].Get(), backing.Get(), at * tile, 0);
    D3D12_RESOURCE_BARRIER alias{D3D12_RESOURCE_BARRIER_TYPE_ALIASING};
    alias.Aliasing = {backing.Get(), view.Get()};
    lists[Writer]->ResourceBarrier(1, &alias);
    CHECK(lists[Writer]->Close());
    ID3D12CommandList *initial = lists[Writer].Get();
    queue->ExecuteCommandLists(1, &initial);
    if (drain(queue.Get()))
      return 1;
    UINT mixed = 0;
    for (UINT epoch = 1; epoch <= rounds; epoch++) {
      const UINT at = epoch % heap_tiles;
      for (UINT i : {Writer, Observer, Final}) {
        CHECK(allocators[i]->Reset());
        CHECK(lists[i]->Reset(allocators[i].Get(), nullptr));
      }
      alias.Aliasing = {view.Get(), backing.Get()};
      lists[Writer]->ResourceBarrier(1, &alias);
      copy_word(lists[Writer].Get(), backing.Get(), at * tile, epoch);
      copy_word(lists[Writer].Get(), scratch.Get(), 0, 0);
      alias.Aliasing = {backing.Get(), view.Get()};
      lists[Writer]->ResourceBarrier(1, &alias);
      transition(lists[Observer].Get(), view.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      lists[Observer]->CopyBufferRegion(readback.Get(), 0, view.Get(), 0, word);
      transition(lists[Observer].Get(), scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
      lists[Observer]->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
      lists[Observer]->CopyBufferRegion(scratch.Get(), 0, upload.Get(), epoch * word, word);
      lists[Observer]->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
      transition(lists[Observer].Get(), scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
      lists[Observer]->CopyBufferRegion(readback.Get(), word, view.Get(), 0, word);
      transition(lists[Observer].Get(), view.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      transition(lists[Final].Get(), view.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      lists[Final]->CopyBufferRegion(readback.Get(), pair, view.Get(), 0, word);
      transition(lists[Final].Get(), view.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      transition(lists[Final].Get(), scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      lists[Final]->CopyBufferRegion(readback.Get(), pair + word, scratch.Get(), 0, word);
      transition(lists[Final].Get(), scratch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      for (UINT i : {Writer, Observer, Final})
        CHECK(lists[i]->Close());
      CHECK(forget(readback.Get()));
      ID3D12CommandList *prepared = lists[Writer].Get(), *reads = lists[Observer].Get();
      queue->ExecuteCommandLists(1, &prepared);
      submissions[0].work = [&] {
        if ((epoch / heap_tiles) % 2) {
          queue->UpdateTileMappings(
              view.Get(), 1, &first, &one, heap.Get(), 1, nullptr, &at, nullptr, D3D12_TILE_MAPPING_FLAG_NONE
          );
        } else {
          const D3D12_TILED_RESOURCE_COORDINATE from{at};
          queue->CopyTileMappings(view.Get(), &first, backing.Get(), &from, &one, D3D12_TILE_MAPPING_FLAG_NONE);
        }
        return S_OK;
      };
      submissions[1].work = [&] {
        queue->ExecuteCommandLists(1, &reads);
        return S_OK;
      };
      if (race())
        return 1;
      ID3D12CommandList *final = lists[Final].Get();
      queue->ExecuteCommandLists(1, &final);
      if (drain(queue.Get()))
        return 1;
      const UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      if (got[0] != got[1] || (got[0] != epoch && got[0] != epoch - 1)) {
        mixed++;
        expect(false, "epoch %u: mapping changed inside the read, X=%u, Y=%u", epoch, got[0], got[1]);
      }
      expect(got[2] == epoch, "epoch %u: final mapping reads %u", epoch, got[2]);
      expect(got[3] == epoch, "epoch %u: predicated copy wrote %u", epoch, got[3]);
      readback->Unmap(0, nullptr);
    }
    printf("%u of %u rounds observed a mapping inside a submission\n", mixed, rounds);
    expect(mixed == 0, "%u rounds interleaved mappings", mixed);
  } else {
    printf("tile mapping race unavailable: TiledResourcesTier is %u\n", options.TiledResourcesTier);
  }
  CloseHandle(ready);
  CloseHandle(go);
  CloseHandle(event);
  SetEvent(finished);
  wait(watchdog, "watchdog");
  CloseHandle(watchdog);
  CloseHandle(finished);
  return verdict();
}
