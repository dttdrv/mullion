// contract: a view of no resource reads zeros. Direct3D 12 says so of null descriptors ("Null descriptors": a shader
// that reads one gets zeros, https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptors-overview), and a
// root constant buffer view of address 0 is the root argument that names no buffer: the first dispatch reads one
// beside a root view of a buffer whose words it knows, and wants zeros of the first and the words of the second.
// Direct3D 12 leaves undefined what a shader reads through a root view that was never set, a descriptor that was
// never written (here one CopyDescriptorsSimple took from a heap shaders do not see over a view of a real buffer),
// and a sampler that was never written. the second dispatch reads through all three, and the test checks only what
// stays defined: the list runs, the device is not removed, and the dispatch's last store arrives.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer absent : register(b0) { uint4 none; };
cbuffer present : register(b1) { uint4 words; };
StructuredBuffer<uint> copied : register(t0);
Texture2D<float> image : register(t1);
SamplerState unwritten : register(s0);
RWBuffer<uint> read : register(u0);
[numthreads(1, 1, 1)] void cs() {
  for (uint i = 0; i < 4; i++) {
    read[i] = none[i] ^ MASK;
    read[4 + i] = words[i] ^ MASK;
  }
  read[8] = copied[0] + uint(image.SampleLevel(unwritten, 0.5, 0));
  read[9] = MASK;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr UINT mask = 0x5a5a5a5a, stores = 10;
  const UINT words[4] = {1, 0x80000000u, ~0u, 0x1234};
  auto cs = compiler.compile(hlsl, "cs", "cs", {"MASK=" + std::to_string(mask)});
  if (!expect(!cs.empty(), "the shader did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  D3D12_DESCRIPTOR_RANGE ranges[3] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1}
  };
  D3D12_ROOT_PARAMETER params[4] = {
      {D3D12_ROOT_PARAMETER_TYPE_CBV},
      {D3D12_ROOT_PARAMETER_TYPE_CBV},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}
  };
  params[0].Descriptor = {0, 0}, params[1].Descriptor = {1, 0};
  params[2].DescriptorTable = {2, ranges}, params[3].DescriptorTable = {1, &ranges[2]};
  auto rs = root_signature(device.Get(), {4, params});
  if (!expect(rs != nullptr, "the root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  auto constants = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
      D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto elements = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, stores * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, stores * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(constants && elements && out && readback, "the buffers"))
    return verdict();
  void *mapped;
  CHECK(constants->Map(0, nullptr, &mapped));
  memcpy(mapped, words, sizeof(words));
  constants->Unmap(0, nullptr);
  CHECK(elements->Map(0, nullptr, &mapped));
  memcpy(mapped, &words[3], sizeof(UINT));
  elements->Unmap(0, nullptr);
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(
      &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(&texture)
  ));

  // the views shaders read, a heap of views they do not, whose descriptor nothing writes, and two samplers, of which
  // the second is never written
  ComPtr<ID3D12DescriptorHeap> views, staged, samplers;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  D3D12_DESCRIPTOR_HEAP_DESC staged_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1};
  D3D12_DESCRIPTOR_HEAP_DESC samplers_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&staged_desc, IID_PPV_ARGS(&staged)));
  CHECK(device->CreateDescriptorHeap(&samplers_desc, IID_PPV_ARGS(&samplers)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_SAMPLER_DESC written{
      D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP
  };
  written.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&written, samplers->GetCPUDescriptorHandleForHeapStart());
  auto view = views->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{
      DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
  };
  srv.Buffer.NumElements = 1;
  srv.Buffer.StructureByteStride = sizeof(UINT);
  device->CreateShaderResourceView(elements.Get(), &srv, view);
  device->CreateShaderResourceView(texture.Get(), nullptr, {view.ptr + increment});
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER};
  uav.Buffer.NumElements = stores;
  device->CreateUnorderedAccessView(out.Get(), nullptr, &uav, {view.ptr + 2 * increment});

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));

  for (UINT pass = 0; pass < 2; pass++) {
    step(
        pass ? "read through a root view never set, a descriptor never written and a sampler never written"
             : "read a root constant buffer view of address 0"
    );
    if (pass) {
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      device->CopyDescriptorsSimple(
          1, view, staged->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
      );
    }
    ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(rs.Get());
    if (!pass)
      list->SetComputeRootConstantBufferView(0, 0);
    list->SetComputeRootConstantBufferView(1, constants->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2, views->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootDescriptorTable(
        3, {samplers->GetGPUDescriptorHandleForHeapStart().ptr +
            pass * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)}
    );
    list->Dispatch(1, 1, 1);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), out.Get());
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(forget(readback.Get()));
    HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
    if (!expect(ran == S_OK, "the list left the device removed: %#lx", ran))
      return verdict();
    const UINT *read;
    CHECK(readback->Map(0, nullptr, (void **)&read));
    for (UINT i = 0; i < 4; i++) {
      if (!pass)
        expect(read[i] == mask, "word %u of the constant buffer of address 0: %#x, want 0", i, read[i] ^ mask);
      expect(
          read[4 + i] == (words[i] ^ mask), "word %u of the bound constant buffer: %#x, want %#x", i,
          read[4 + i] ^ mask, words[i]
      );
    }
    expect(read[9] == mask, "the dispatch's last store: %#x, want %#x", read[9], mask);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
