// contract: CopyBufferRegion copies exactly NumBytes from SrcOffset to DstOffset and preserves every other byte.
// "Specifies a UINT64 offset (in bytes)" and "Specifies the number of bytes to copy" (Microsoft Learn).
// D3D11.3 5.6.2: "the regions to copy will be in raw bytes". snapshots after every copy distinguish a dropped
// early operation from a later overlapping copy. all three queue types run upload and default sources.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copybufferregion
#include "d3d12_test.hpp"
#include <algorithm>

int main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT64 allocation = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  const UINT64 bytes = 2 * allocation + 3;
  const UINT snapshots = 32;
  std::vector<UINT64> edges{0, 1, sizeof(UINT) - 1, sizeof(UINT), sizeof(UINT) + 1};
  for (UINT64 boundary : {UINT64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT), UINT64(system.dwPageSize), allocation})
    for (UINT64 n : {boundary - 1, boundary, boundary + 1})
      edges.push_back(n);
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  auto value = [](UINT64 i) { return UINT8(i ^ (i >> 8) ^ (i >> 16)); };
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes * snapshots, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(source && initial && back, "buffer region resources"))
    return verdict();
  UINT8 *input, *seed;
  CHECK(source->Map(0, nullptr, (void **)&input));
  CHECK(initial->Map(0, nullptr, (void **)&seed));
  for (UINT64 i = 0; i < bytes; i++) {
    input[i] = value(i);
    seed[i] = ~value(i);
  }
  source->Unmap(0, nullptr);
  initial->Unmap(0, nullptr);
  struct Copy { UINT64 from, to, count; };
  std::vector<Copy> copies;
  for (auto from : edges)
    for (auto to : edges)
      for (auto count : edges)
        if (count)
          copies.push_back({from, to, count});
  copies.push_back({0, bytes - 1, 1});
  copies.push_back({bytes - 1, 0, 1});
  copies.push_back({0, 0, bytes});
  for (UINT phase = 0; phase < 2; phase++) {
    CHECK(source->Map(0, nullptr, (void **)&input));
    for (UINT64 i = 0; i < bytes; i++)
      input[i] = phase ? UINT8(~value(i)) : value(i);
    source->Unmap(0, nullptr);
    for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY})
    for (bool upload : {false, true}) {
      step("queue %u, %s source, %zu byte-region copies", type, upload ? "upload" : "default", copies.size());
      ComPtr<ID3D12CommandQueue> queue;
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      D3D12_COMMAND_QUEUE_DESC q{type};
      CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
      CHECK(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      auto gpu_source = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON);
      auto destination = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON);
      if (!expect(gpu_source && destination, "default buffers"))
        return verdict();
      list->CopyBufferRegion(gpu_source.Get(), 0, source.Get(), 0, bytes);
      transition(list.Get(), gpu_source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      for (size_t first = 0; first < copies.size(); first += snapshots) {
        const UINT count = std::min<size_t>(snapshots, copies.size() - first);
        step("phase %u queue %u, %s source, copies %zu..%zu", phase, type, upload ? "upload" : "default", first, first + count);
        CHECK(forget(back.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        for (UINT i = 0; i < count; i++) {
          const auto &copy = copies[first + i];
          list->CopyBufferRegion(destination.Get(), 0, initial.Get(), 0, bytes);
          list->CopyBufferRegion(destination.Get(), copy.to, upload ? source.Get() : gpu_source.Get(),
                                  copy.from, copy.count);
          transition(list.Get(), destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(back.Get(), bytes * i, destination.Get(), 0, bytes);
          transition(list.Get(), destination.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const UINT8 *got;
        CHECK(back->Map(0, nullptr, (void **)&got));
        bool same = true;
        for (UINT i = 0; i < count; i++) {
          const auto &copy = copies[first + i];
          for (UINT64 byte = 0; byte < bytes; byte++) {
            UINT8 copied = value(copy.from + byte - copy.to);
            UINT8 want = byte >= copy.to && byte - copy.to < copy.count ? (phase ? UINT8(~copied) : copied) :
                                                                                      UINT8(~value(byte));
            if (got[bytes * i + byte] != want) {
              expect(false, "src %llu dst %llu count %llu: byte %llu is %#x, want %#x", copy.from, copy.to,
                     copy.count, byte, got[bytes * i + byte], want);
              same = false;
              break;
            }
          }
        }
        back->Unmap(0, nullptr);
        if (!same)
          return verdict();
      }
    }
  }
  return verdict();
}
