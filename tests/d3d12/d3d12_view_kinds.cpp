// contract (as games rely on it; the D3D12 runtime leaves it to drivers, which all honor it): a buffer view of 32-bit
// elements reads the same memory whichever kind of buffer the shader declares it as. a shader that declares a
// StructuredBuffer<uint> (stride 4) or a ByteAddressBuffer and is given a typed R32_UINT view (Buffer<uint>'s) reads
// the buffer's words at the view's elements, as Hogwarts Legacy's lighting reads its tile lists (written through
// RWBuffer<uint>) through a typed view in a StructuredBuffer slot. each view starts past the buffer's start
// (FirstElement), and each shader reads every element and its count. (a Buffer<uint> given a structured or raw view,
// the other way, VKD3D-Proton reads too; Mullion does not yet, and this test leaves it out)
#include "d3d12_test.hpp"
#include <vector>

static const char hlsl[] = R"hlsl(
#if KIND == 0
StructuredBuffer<uint> words : register(t0);
uint word(uint i) { return words[i]; }
uint count() { uint n, stride; words.GetDimensions(n, stride); return n; }
#elif KIND == 1
ByteAddressBuffer words : register(t0);
uint word(uint i) { return words.Load(i * 4); }
uint count() { uint n; words.GetDimensions(n); return n / 4; }
#else
Buffer<uint> words : register(t0);
uint word(uint i) { return words[i]; }
uint count() { uint n; words.GetDimensions(n); return n; }
#endif
RWStructuredBuffer<uint> results : register(u0);
[numthreads(64, 1, 1)] void cs(uint i : SV_DispatchThreadID) {
  results[i] = word(i);
  if (i == 0)
    results[64] = count();
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const char *kinds[] = {"StructuredBuffer<uint>", "ByteAddressBuffer", "Buffer<uint>"};
  std::string shaders[3];
  for (int k = 0; k < 3; k++)
    if (!expect(!(shaders[k] = compiler.compile(hlsl, "cs", "cs", {"KIND=" + std::to_string(k)})).empty(),
                "%s did not compile", kinds[k]))
      return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  ComPtr<ID3D12PipelineState> pipelines[3];
  for (int k = 0; k < 3; k++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(shaders[k])};
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines[k])));
  }

  // 128 words of 0x10000 * i + i + 1, a view of 64 of them from word 16
  const UINT total = 128, first = 16, count = 64;
  auto data = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    UINT *mapped;
    CHECK(data->Map(0, nullptr, (void **)&mapped));
    for (UINT i = 0; i < total; i++)
      mapped[i] = 0x10000 * i + i + 1;
    data->Unmap(0, nullptr);
  }
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 65 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 65 * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(hd.Type);
  // views: typed R32_UINT, structured of stride 4, raw
  const char *views[] = {"a typed R32_UINT view", "a structured view", "a raw view"};
  for (UINT v = 0; v < 3; v++) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{v == 0 ? DXGI_FORMAT_R32_UINT : v == 1 ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS,
                                         D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    desc.Buffer = {first, count, v == 1 ? 4u : 0u, v == 2 ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(data.Get(), &desc,
                                     {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(v) * increment});
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, qd.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  // each shader with each view of another kind, and with its own (the test's own check)
  const int pairs[][2] = {{0, 0}, {0, 2}, {1, 1}, {1, 2}, {2, 2}};
  const int own_view[] = {1, 2, 0};
  for (auto [shader, other] : pairs) {
    int view = shader == other ? own_view[shader] : (other == 0 ? 1 : other == 1 ? 2 : 0);
    step("%s given %s", kinds[shader], views[view]);
    CHECK(forget(back.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipelines[shader].Get()));
    list->SetDescriptorHeaps(1, heap.GetAddressOf());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, {heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(view) * increment});
    list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(back.Get(), 0, results.Get(), 0, 65 * 4);
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(list->Close());
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    UINT *got;
    CHECK(back->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT i = 0; i < count; i++)
      if (got[i] != 0x10000 * (first + i) + first + i + 1 && wrong++ < 3)
        expect(false, "element %u is %#x, not %#x", i, got[i], 0x10000 * (first + i) + first + i + 1);
    expect(!wrong, "%u of %u elements are wrong", wrong, count);
    expect(got[64] == count, "it has %u elements, not %u", got[64], count);
    back->Unmap(0, nullptr);
  }
  return verdict();
}
