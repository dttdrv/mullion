// contract: a typed buffer view reads the buffer's elements from its FirstElement on, wherever in the buffer that is:
// "FirstElement: the index of the first element to be accessed by the view" (D3D12_BUFFER_SRV), and a view holds at
// most 2^D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP elements, not a buffer.
// Mullion reads a typed view through one Metal texture buffer over the whole buffer, at the view's first element
// (Buffer::prepareAllocationViews), and Metal's texture buffers hold at most 2^28 texels (its feature set tables).
// an R32 buffer one window past 2^28 elements is written through a raw view, element i holding i, then read through
// a typed view whose window straddles 2^28, and through one past it
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint first; }
RWByteAddressBuffer whole : register(u0);
RWByteAddressBuffer read_back : register(u1);
Buffer<uint> window : register(t0);
[numthreads(64, 1, 1)] void fill(uint i : SV_DispatchThreadID) { whole.Store((first + i) * 4, first + i); }
[numthreads(64, 1, 1)] void copy(uint i : SV_DispatchThreadID) { read_back.Store(i * 4, window[i]); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto fill = compiler.compile(hlsl, "fill", "cs"), copy = compiler.compile(hlsl, "copy", "cs");
  if (!expect(!fill.empty() && !copy.empty(), "shaders compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const UINT64 metal_texels = UINT64(1) << 28, span = 4096, group = 64;
  const UINT64 elements = metal_texels + span;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[4] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].Constants = {0, 0, 1};
  params[1].Descriptor = {0, 0};
  params[2].Descriptor = {1, 0};
  params[3].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {4, params});
  ComPtr<ID3D12PipelineState> fill_pso, copy_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(fill)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&fill_pso)));
  pso_desc.CS = bytecode(copy);
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&copy_pso)));
  auto whole = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, elements * sizeof(UINT),
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto read_back = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, span * sizeof(UINT),
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, span * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(whole && read_back && readback, "a buffer of %llu elements and its read back are made",
              (unsigned long long)elements))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  // a window that straddles Metal's limit, and one wholly past it
  for (UINT64 first : {metal_texels - span / 2, metal_texels}) {
    step("a typed view of elements %llu to %llu", (unsigned long long)first, (unsigned long long)(first + span - 1));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER,
                                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    view.Buffer = {first, UINT(span)};
    device->CreateShaderResourceView(whole.Get(), &view, heap->GetCPUDescriptorHandleForHeapStart());
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), fill_pso.Get()));
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    UINT first32 = UINT(first);
    list->SetComputeRoot32BitConstants(0, 1, &first32, 0);
    list->SetComputeRootUnorderedAccessView(1, whole->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, read_back->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(3, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(UINT(span / group), 1, 1);
    transition(list.Get(), whole.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(copy_pso.Get());
    list->Dispatch(UINT(span / group), 1, 1);
    transition(list.Get(), whole.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list.Get(), read_back.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), read_back.Get());
    transition(list.Get(), read_back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT off = 0;
    for (UINT64 i = 0; i < span; i++)
      if (got[i] != first + i && off++ < 3)
        printf("  element %llu reads %u\n", (unsigned long long)(first + i), got[i]);
    readback->Unmap(0, nullptr);
    expect(!off, "elements %llu on: %u of %llu read wrong", (unsigned long long)first, off, (unsigned long long)span);
  }
  return verdict();
}
