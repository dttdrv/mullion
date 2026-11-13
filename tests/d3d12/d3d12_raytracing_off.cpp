// contract: MULLION_D3D12_RAYTRACING=0 reports no ray tracing: "D3D12_RAYTRACING_TIER_NOT_SUPPORTED: No support for
// raytracing on the device" (D3D12_RAYTRACING_TIER), so an application that checks for it before using it leaves it
// out. the device is otherwise made and works: a copy of a buffer reaches its destination. its feature level is then
// below 12_2, which has "DirectX Raytracing: Tier 1.1" (Direct3D feature levels, 12_2), and device creation refuses
// 12_2 while it takes the level the device reports.
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
  static const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                          D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{UINT(std::size(all)), all};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)));
  expect(levels.MaxSupportedFeatureLevel < D3D_FEATURE_LEVEL_12_2, "feature level %#x without ray tracing",
         levels.MaxSupportedFeatureLevel);
  expect(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_2, __uuidof(ID3D12Device), nullptr) == E_INVALIDARG,
         "12_2 was not refused without ray tracing");
  expect(D3D12CreateDevice(nullptr, levels.MaxSupportedFeatureLevel, __uuidof(ID3D12Device), nullptr) == S_FALSE,
         "the level the device reports was refused");

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
