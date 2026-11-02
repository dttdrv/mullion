// contract: MULLION_D3D12_RAYTRACING=0 reports no ray tracing: "D3D12_RAYTRACING_TIER_NOT_SUPPORTED: No support for
// raytracing on the device" (D3D12_RAYTRACING_TIER), so an application that checks for it before using it leaves it
// out. the device is otherwise made and works: a copy of a buffer reaches its destination.
#include "d3d12_test.hpp"

int
main(int argc, char **argv) {
  SetEnvironmentVariableA("MULLION_D3D12_RAYTRACING", "0");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  expect(options.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED, "ray tracing tier %u is reported",
         options.RaytracingTier);

  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT *words;
  CHECK(upload->Map(0, nullptr, (void **)&words));
  words[0] = 0x1234;
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 4);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  CHECK(readback->Map(0, nullptr, (void **)&words));
  expect(words[0] == 0x1234, "the copy wrote %#x", words[0]);
  readback->Unmap(0, nullptr);
  return verdict();
}
