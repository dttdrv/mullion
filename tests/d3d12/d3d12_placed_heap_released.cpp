// contract: ID3D12Device::CreatePlacedResource. a placed resource keeps the heap it was placed on: "A placed resource
// object holds a reference on the heap it is created on" (ID3D12Device::CreateHeap, Remarks), so an application may
// release its last reference to the heap while resources placed on it live, and go on using them. that reference is
// the runtime's own: the heap's count as the application sees it does not include it (vkd3d-proton's
// test_create_placed_resource, run on Windows, finds the heap's count still 1 with resources placed on it).
// a buffer on an upload heap and a texture on a default heap are placed, and both heaps are released at once: each
// Release must give 0. then the buffer is written through its mapping, the texture by a copy, another resource is
// made and released (which changes what the device keeps resident), and a compute shader in a list of its own reads
// both, the buffer through a root descriptor and the texture through a descriptor table. what it reads must be what was written,
// and the device must live.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createheap
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> placed_buffer : register(t0);
Texture2D<uint> placed_texture : register(t1);
RWStructuredBuffer<uint> result : register(u0);
[numthreads(16, 1, 1)] void cs(uint i : SV_GroupIndex) {
  result[i] = placed_buffer[i];
  result[16 + i] = placed_texture.Load(int3(i % 4, i / 4, 0));
}
)hlsl";

// the buffer's words and the texture's texels, which no zeroed or stale memory holds
static const UINT words = 16, side = 4;
static UINT
buffer_word(UINT i) {
  return 0xb0000000u + i;
}
static UINT
texture_word(UINT i) {
  return 0x7e000000u + i;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!cs.empty(), "the shader does not compile"))
    return verdict();
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Descriptor = {0, 0};
  params[1].DescriptorTable = {1, &range};
  params[2].Descriptor = {0, 0};
  auto root = root_signature(device.Get(), {(UINT)std::size(params), params});
  CHECK(root ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{root.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pipeline;
  CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)));

  step("placing a buffer on an upload heap and a texture on a default heap");
  D3D12_HEAP_DESC upload_desc{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, {D3D12_HEAP_TYPE_UPLOAD}, 0,
                              D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS},
      default_desc{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, {D3D12_HEAP_TYPE_DEFAULT}, 0,
                   D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
  ComPtr<ID3D12Heap> upload_heap, default_heap;
  CHECK(device->CreateHeap(&upload_desc, IID_PPV_ARGS(&upload_heap)));
  CHECK(device->CreateHeap(&default_desc, IID_PPV_ARGS(&default_heap)));
  D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, words * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR},
      texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}};
  ComPtr<ID3D12Resource> placed_buffer, placed_texture;
  CHECK(device->CreatePlacedResource(upload_heap.Get(), 0, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&placed_buffer)));
  CHECK(device->CreatePlacedResource(default_heap.Get(), 0, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                     IID_PPV_ARGS(&placed_texture)));

  step("releasing both heaps while their resources live");
  ULONG left = upload_heap.Detach()->Release();
  expect(left == 0, "the upload heap has %lu references left after the application's last", left);
  left = default_heap.Detach()->Release();
  expect(left == 0, "the default heap has %lu references left after the application's last", left);

  step("making and releasing another resource");
  buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 1 << 16, D3D12_RESOURCE_STATE_COMMON).Reset();

  step("writing the placed buffer through its mapping");
  UINT *mapped;
  CHECK(placed_buffer->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < words; i++)
    mapped[i] = buffer_word(i);
  placed_buffer->Unmap(0, nullptr);

  step("copying into the placed texture");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 total;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto texels = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  BYTE *bytes;
  CHECK(texels->Map(0, nullptr, (void **)&bytes));
  for (UINT i = 0; i < side * side; i++) {
    UINT word = texture_word(i);
    memcpy(bytes + (i / side) * footprint.Footprint.RowPitch + (i % side) * 4, &word, 4);
  }
  texels->Unmap(0, nullptr);
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * words * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * words * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));
  ComPtr<ID3D12DescriptorHeap> table;
  D3D12_DESCRIPTOR_HEAP_DESC table_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&table_desc, IID_PPV_ARGS(&table)));
  device->CreateShaderResourceView(placed_texture.Get(), nullptr, table->GetCPUDescriptorHandleForHeapStart());

  D3D12_TEXTURE_COPY_LOCATION dst{placed_texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      src{texels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  src.PlacedFootprint = footprint;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(list.Get(), placed_texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
  if (!expect(SUCCEEDED(hr), "the copy ran with %08lx (the device was removed)", hr))
    return verdict();
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  step("reading both from a compute shader, in a list of its own");
  ID3D12DescriptorHeap *heaps[] = {table.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root.Get());
  list->SetPipelineState(pipeline.Get());
  list->SetComputeRootShaderResourceView(0, placed_buffer->GetGPUVirtualAddress());
  list->SetComputeRootDescriptorTable(1, table->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(2, result->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), result.Get());
  hr = submit(device.Get(), queue.Get(), list.Get());
  if (!expect(SUCCEEDED(hr), "the list ran with %08lx (the device was removed)", hr))
    return verdict();

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < words; i++)
    expect(got[i] == buffer_word(i), "word %u of the placed buffer reads %08x, want %08x", i, got[i], buffer_word(i));
  for (UINT i = 0; i < side * side; i++)
    expect(got[words + i] == texture_word(i), "texel %u,%u of the placed texture reads %08x, want %08x", i % side,
           i / side, got[words + i], texture_word(i));
  readback->Unmap(0, nullptr);
  return verdict();
}
