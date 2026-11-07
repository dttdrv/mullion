// contract: each CopyBufferRegion copies NumBytes from the source at SrcOffset to the destination at DstOffset
// (ID3D12GraphicsCommandList::CopyBufferRegion), whatever the copies around it are. titles upload small blocks one
// after another; copies whose source and destination both follow on from the copy before are where a backend may
// join them, so the runs here are those and every way of almost being one: the source following on but not the
// destination and the other way round, a gap, a run in descending order, a run that switches source or destination
// buffer, and a run out of a buffer the list wrote before. each destination byte's value says which source byte it
// came from.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copybufferregion
#include "d3d12_test.hpp"
#include <vector>

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  constexpr UINT64 size = 64 * 1024, block = 256;
  // two sources whose words name their buffer and offset, and two destinations
  ComPtr<ID3D12Resource> sources[2], targets[2];
  for (UINT i = 0; i < 2; i++) {
    sources[i] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    targets[i] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(sources[i] && targets[i], "the buffers are refused"))
      return verdict();
    UINT16 *mapped;
    CHECK(sources[i]->Map(0, nullptr, (void **)&mapped));
    for (UINT64 at = 0; at < size / 2; at++)
      mapped[at] = UINT16(i << 15 | ((at + 1) & 0x7fff));
    sources[i]->Unmap(0, nullptr);
  }
  auto self = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size * 3, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(self && readback, "the buffers are refused"))
    return verdict();
  CHECK(forget(readback.Get()));

  // what each destination byte should hold: the source byte, or 0 where nothing was copied (default heaps start at 0)
  std::vector<UINT16> want[3];
  for (auto &w : want)
    w.assign(size / 2, 0);
  // no word of a source is 0, which a destination holds where nothing was copied
  auto source_value = [](UINT s, UINT64 byte) { return UINT16(s << 15 | ((byte / 2 + 1) & 0x7fff)); };
  struct Copy {
    UINT target, source;
    UINT64 dst, src, bytes;
  };
  std::vector<Copy> copies;
  auto run = [&](UINT target, UINT source, UINT64 dst, UINT64 src, UINT count, INT64 dst_step, INT64 src_step,
                 UINT64 bytes = UINT64(-1)) {
    if (bytes == UINT64(-1))
      bytes = block;
    for (UINT i = 0; i < count; i++)
      copies.push_back({target, source, dst + i * dst_step, src + i * src_step, bytes});
  };
  run(0, 0, 0, 0, 16, block, block);                      // both follow on
  run(0, 0, 0x1000 * 2, 0x1000, 8, 2 * block, block);     // the source follows on, the destination skips a block
  run(0, 0, 0x3000 * 2, 0x3000 * 2, 8, block, 2 * block); // the destination follows on, the source skips
  run(0, 1, 0x5000 * 2, 0x5000, 8, -INT64(block), -INT64(block)); // descending
  run(1, 0, 0, 0x100, 8, block, block, 64);                // short copies that follow on
  run(1, 0, 0x2000, 0x2000, 4, block, block);              // then the same offsets from the other source:
  run(1, 1, 0x2000 + 4 * block, 0x2000 + 4 * block, 4, block, block);
  run(0, 0, 0x9000, 0x9000, 4, block, block);              // then the other destination
  run(1, 0, 0x9000 + 4 * block, 0x9000 + 4 * block, 4, block, block);
  for (auto &c : copies) {
    list->CopyBufferRegion(targets[c.target].Get(), c.dst, sources[c.source].Get(), c.src, c.bytes);
    for (UINT64 b = 0; b < c.bytes; b += 2)
      want[c.target][(c.dst + b) / 2] = source_value(c.source, c.src + b);
  }
  // a run out of a buffer the list filled before, into the second half of another
  list->CopyBufferRegion(self.Get(), 0, sources[0].Get(), 0, size / 2);
  transition(list.Get(), self.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ComPtr<ID3D12Resource> half = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, size, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(half ? S_OK : E_FAIL);
  for (UINT i = 0; i < 8; i++)
    list->CopyBufferRegion(half.Get(), size / 2 + i * block, self.Get(), i * block, block);
  for (UINT64 b = 0; b < 8 * block; b += 2)
    want[2][(size / 2 + b) / 2] = source_value(0, b);
  ID3D12Resource *read[] = {targets[0].Get(), targets[1].Get(), half.Get()};
  for (UINT i = 0; i < 3; i++) {
    transition(list.Get(), read[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), i * size, read[i], 0, size);
  }
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));
  UINT16 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT wrong = 0;
  for (UINT i = 0; i < 3; i++)
    for (UINT64 at = 0; at < size / 2; at++)
      if (got[i * size / 2 + at] != want[i][at] && !wrong++)
        printf("destination %u byte %llu holds %04x, want %04x\n", i, at * 2, got[i * size / 2 + at], want[i][at]);
  readback->Unmap(0, nullptr);
  expect(!wrong, "%u wrong values", wrong);
  return verdict();
}
