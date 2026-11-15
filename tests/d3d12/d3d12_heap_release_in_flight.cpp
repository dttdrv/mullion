// contract: the work a queue was given runs as recorded when the application releases, right after
// ExecuteCommandLists, the descriptor heaps that work reads its resources and samplers through, and the process
// goes on. Direct3D 12 does not allow the release (the debug layer's OBJECT_DELETED_WHILE_STILL_IN_USE), Windows'
// drivers mostly survive it, and games do it: vkd3d-proton keeps what is released in flight until the pending
// submissions complete (libs/vkd3d/resource.c, d3d12_device_add_queue_timeline_deferred_decref), and
// d3d12_release_in_flight holds Mullion to the same for resources and heaps. here a dispatch reads a buffer and a
// texture through a table in a CBV/SRV/UAV heap and samples the texture with a sampler of a sampler heap, and both
// heaps are released as soon as the list is given. every word of the output has the buffer's word and the texture's
// colour. in even rounds the queue waits for a fence before the list, which is signalled only after the release: the
// list's work then starts once the heaps are gone.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> src : register(t0);
Texture2D<float4> tex : register(t1);
SamplerState smp : register(s0);
RWStructuredBuffer<uint> dst : register(u0);
[numthreads(64, 1, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
  float4 colour = tex.SampleLevel(smp, float2(0.5, 0.5), 0);
  dst[tid.x] = src[tid.x] << 16 | uint(colour.r * 255 + 0.5) << 8 | uint(colour.g * 255 + 0.5);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (code.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE resources{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2},
      samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1};
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[0].DescriptorTable = {1, &resources};
  params[1].DescriptorTable = {1, &samplers};
  auto rs = root_signature(device.Get(), {3, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  constexpr UINT kWords = 4096, kRed = 64, kGreen = 128;
  constexpr UINT64 kBytes = UINT64(kWords) * sizeof(UINT);
  auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, kBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(input && output && readback, "buffers"))
    return verdict();
  // a texture of one colour, which a clear gives it
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,     4, 4, 1, 1,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,         {1, 0}};
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
      IID_PPV_ARGS(&texture)
  ));
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
  const float colour[4] = {kRed / 255.0f, kGreen / 255.0f, 0, 1};
  list->ClearRenderTargetView(rtv, colour, 0, nullptr);
  transition(
      list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
  );
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  ComPtr<ID3D12Fence> gate, done;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)));
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  UINT64 gated = 0, signalled = 0;
  for (UINT round = 0; round < 8; round++) {
    const bool waits = !(round & 1);
    step("round %u, %s", round, waits ? "the list behind a fence" : "the list at once");
    // word w of the buffer is w + round + 1
    UINT *words;
    CHECK(input->Map(0, nullptr, (void **)&words));
    for (UINT w = 0; w < kWords; w++)
      words[w] = w + round + 1;
    input->Unmap(0, nullptr);
    CHECK(forget(readback.Get()));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
    };
    ComPtr<ID3D12DescriptorHeap> heap, sampler_heap;
    CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    CHECK(device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&sampler_heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{
        DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    view.Buffer = {0, kWords, sizeof(UINT)};
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(input.Get(), &view, handle);
    device->CreateShaderResourceView(texture.Get(), nullptr, {handle.ptr + increment});
    D3D12_SAMPLER_DESC sampler{
        D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP
    };
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    ID3D12DescriptorHeap *heaps[] = {heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootDescriptorTable(1, sampler_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
    list->Dispatch(kWords / 64, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(list->Close());
    ID3D12CommandList *lists[] = {list.Get()};
    if (waits)
      CHECK(queue->Wait(gate.Get(), ++gated));
    queue->ExecuteCommandLists(1, lists);
    // released while the list is in flight
    heap.Reset();
    sampler_heap.Reset();
    if (waits)
      CHECK(gate->Signal(gated));
    CHECK(queue->Signal(done.Get(), ++signalled));
    CHECK(done->SetEventOnCompletion(signalled, event));
    WaitForSingleObject(event, INFINITE);
    if (!expect(SUCCEEDED(device->GetDeviceRemovedReason()), "the device is removed"))
      return verdict();
    const UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned wrong = 0;
    for (UINT w = 0; w < kWords; w++) {
      UINT want = (w + round + 1) << 16 | kRed << 8 | kGreen;
      if (got[w] != want && wrong++ < 4)
        printf("  word %u is %#x, want %#x\n", w, got[w], want);
    }
    readback->Unmap(0, nullptr);
    expect(!wrong, "round %u: %u of %u words wrong", round, wrong, kWords);
  }
  CloseHandle(event);
  return verdict();
}
