// contract: ID3D12Device::CreateShaderResourceView, CreateUnorderedAccessView, CreateRenderTargetView and
// CreateDepthStencilView, given a view the device refuses. the calls return nothing, and Direct3D leaves what such a
// descriptor holds undefined; Mullion writes the null descriptor of the slot's type in its place, as vkd3d-proton
// does for shader resource and unordered access views. what this holds to is that the slot no longer reaches the
// resource of the view it held before, which the application may since have released: a null shader resource view
// reads 0 ("SRVs which return default values", DirectX-Specs, Resource Binding, "NULL Descriptors"), and clearing
// through a null view changes nothing.
// a texture A (R32_UINT) holds 0x0a0a0a0a and a depth texture D holds 0.25. a shader-visible slot holds an unordered
// access view of A and another a shader resource view of A, a CPU-only slot an unordered access view of A, a render
// target slot a view of A and a depth stencil slot a view of D. each slot is then given a view of a texture B that
// is refused: R8G8_UNORM and R8G8_UINT (for the shader resource view, which the shader reads as uint) views of an
// R32_FLOAT texture, and a depth stencil view of a texture without ALLOW_DEPTH_STENCIL. A is cleared through the
// unordered access and render target slots, D through the depth stencil slot, and a compute shader reads through the
// shader resource slot. A must still hold 0x0a0a0a0a, D 0.25, and the shader must read 0.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createshaderresourceview
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
Texture2D<uint> through_slot : register(t0);
RWStructuredBuffer<uint> result : register(u0);
[numthreads(16, 1, 1)] void cs(uint i : SV_GroupIndex) { result[i] = through_slot.Load(int3(i % 4, i / 4, 0)); }
)hlsl";

static const UINT SIDE = 4, A_WORD = 0x0a0a0a0a;
static const float D_DEPTH = 0.25f;

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
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  params[1].Descriptor = {0, 0};
  auto root = root_signature(device.Get(), {(UINT)std::size(params), params});
  CHECK(root ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{root.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pipeline;
  CHECK(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)));

  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC a_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, SIDE, SIDE, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET},
      b_desc = a_desc, d_desc = a_desc;
  b_desc.Format = DXGI_FORMAT_R32_FLOAT;
  d_desc.Format = DXGI_FORMAT_D32_FLOAT, d_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE d_clear{DXGI_FORMAT_D32_FLOAT};
  d_clear.DepthStencil = {D_DEPTH, 0};
  ComPtr<ID3D12Resource> a, b, d;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &a_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&a)));
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &b_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&b)));
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        &d_clear, IID_PPV_ARGS(&d)));

  // the shader-visible heap: 0 the unordered access view, 1 the shader resource view
  ComPtr<ID3D12DescriptorHeap> visible, cpu_only, rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC visible_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      cpu_only_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1}, rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&visible_desc, IID_PPV_ARGS(&visible)));
  CHECK(device->CreateDescriptorHeap(&cpu_only_desc, IID_PPV_ARGS(&cpu_only)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto uav_cpu = visible->GetCPUDescriptorHandleForHeapStart(), srv_cpu = uav_cpu;
  srv_cpu.ptr += increment;
  auto uav_gpu = visible->GetGPUDescriptorHandleForHeapStart(), srv_gpu = uav_gpu;
  srv_gpu.ptr += increment;
  auto uav_cpu_only = cpu_only->GetCPUDescriptorHandleForHeapStart();
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();

  step("filling A and D through views of them");
  device->CreateUnorderedAccessView(a.Get(), nullptr, nullptr, uav_cpu);
  device->CreateUnorderedAccessView(a.Get(), nullptr, nullptr, uav_cpu_only);
  device->CreateShaderResourceView(a.Get(), nullptr, srv_cpu);
  device->CreateRenderTargetView(a.Get(), nullptr, rtv);
  device->CreateDepthStencilView(d.Get(), nullptr, dsv);
  ID3D12DescriptorHeap *heaps[] = {visible.Get()};
  list->SetDescriptorHeaps(1, heaps);
  const UINT a_words[4] = {A_WORD, A_WORD, A_WORD, A_WORD};
  list->ClearUnorderedAccessViewUint(uav_gpu, uav_cpu_only, a.Get(), a_words, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, D_DEPTH, 0, 0, nullptr);
  HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
  if (!expect(SUCCEEDED(hr), "the list ran with %08lx (the device was removed)", hr))
    return verdict();

  step("giving every slot a view of B that is refused");
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R8G8_UNORM, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(b.Get(), nullptr, &uav_desc, uav_cpu);
  device->CreateUnorderedAccessView(b.Get(), nullptr, &uav_desc, uav_cpu_only);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{DXGI_FORMAT_R8G8_UINT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                           D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv_desc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(b.Get(), &srv_desc, srv_cpu);
  D3D12_RENDER_TARGET_VIEW_DESC rtv_view{DXGI_FORMAT_R8G8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2D};
  device->CreateRenderTargetView(b.Get(), &rtv_view, rtv);
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
  device->CreateDepthStencilView(b.Get(), &dsv_view, dsv);

  step("clearing and reading through the slots");
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, SIDE * SIDE * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2];
  UINT64 a_total, d_total;
  device->GetCopyableFootprints(&a_desc, 0, 1, 0, &footprints[0], nullptr, nullptr, &a_total);
  device->GetCopyableFootprints(&d_desc, 0, 1, 0, &footprints[1], nullptr, nullptr, &d_total);
  const UINT64 placement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  footprints[1].Offset = (a_total + placement - 1) & ~(placement - 1);
  const UINT64 result_at = (footprints[1].Offset + d_total + 255) & ~UINT64(255);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_at + SIDE * SIDE * 4,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), pipeline.Get()));
  list->SetDescriptorHeaps(1, heaps);
  const UINT cleared_words[4] = {7, 7, 7, 7};
  const float cleared_color[4] = {9, 9, 9, 9};
  list->ClearUnorderedAccessViewUint(uav_gpu, uav_cpu_only, b.Get(), cleared_words, 0, nullptr);
  list->ClearRenderTargetView(rtv, cleared_color, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, nullptr);
  list->SetComputeRootSignature(root.Get());
  list->SetComputeRootDescriptorTable(0, srv_gpu);
  list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), a.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), d.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  ID3D12Resource *sources[2] = {a.Get(), d.Get()};
  for (UINT i = 0; i < 2; i++) {
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{sources[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = footprints[i];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  list->CopyBufferRegion(readback.Get(), result_at, result.Get(), 0, SIDE * SIDE * 4);
  hr = submit(device.Get(), queue.Get(), list.Get());
  if (!expect(SUCCEEDED(hr), "the list ran with %08lx (the device was removed)", hr))
    return verdict();

  BYTE *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  UINT d_word;
  memcpy(&d_word, &D_DEPTH, 4);
  for (UINT y = 0; y < SIDE; y++)
    for (UINT x = 0; x < SIDE; x++) {
      UINT texel;
      memcpy(&texel, got + footprints[0].Offset + y * footprints[0].Footprint.RowPitch + x * 4, 4);
      expect(texel == A_WORD, "A at %u,%u holds %08x, want %08x: a clear through a refused view reached it", x, y,
             texel, A_WORD);
      memcpy(&texel, got + footprints[1].Offset + y * footprints[1].Footprint.RowPitch + x * 4, 4);
      expect(texel == d_word, "D at %u,%u holds %08x, want %08x (0.25): a clear through a refused view reached it", x,
             y, texel, d_word);
      memcpy(&texel, got + result_at + (y * SIDE + x) * 4, 4);
      expect(texel == 0, "the shader read %08x at %u,%u through a refused view, want 0 (a null view)", texel, x, y);
    }
  readback->Unmap(0, nullptr);
  return verdict();
}
