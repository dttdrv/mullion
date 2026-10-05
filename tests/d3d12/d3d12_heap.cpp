// contract: SM 6.6 shaders reach resources through the descriptor heaps themselves (ResourceDescriptorHeap,
// SamplerDescriptorHeap) under a root signature that directly indexes them, each resource typed where it is used:
// constant, typed, structured and raw buffers, a depth texture compared through a comparison sampler, and UAVs (a
// typed texture, and a structured buffer with a counter), all found by indices a bound root constant supplies. every
// value read comes back through a UAV from the heap.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer c : register(b0) { uint first; };
struct Data { uint value; };
[numthreads(1, 1, 1)]
void cs() {
  ConstantBuffer<Data> data = ResourceDescriptorHeap[first + 0];
  Buffer<uint> typed = ResourceDescriptorHeap[first + 1];
  StructuredBuffer<uint> structured = ResourceDescriptorHeap[first + 2];
  Texture2D<float> depth = ResourceDescriptorHeap[first + 3];
  RWStructuredBuffer<uint> result = ResourceDescriptorHeap[first + 4];
  RWByteAddressBuffer raw = ResourceDescriptorHeap[first + 5];
  RWTexture2D<uint> image = ResourceDescriptorHeap[first + 6];
  RWStructuredBuffer<uint> counted = ResourceDescriptorHeap[first + 7];
  SamplerComparisonState less = SamplerDescriptorHeap[0];
  result[0] = data.value;
  result[1] = typed[1];
  result[2] = structured[2];
  result[3] = depth.SampleCmpLevelZero(less, float2(0.5, 0.5), BELOW) > 0.5;
  result[4] = depth.SampleCmpLevelZero(less, float2(0.5, 0.5), ABOVE) > 0.5;
  raw.Store(4, data.value + 1);
  image[uint2(1, 0)] = data.value + 2;
  result[5] = image[uint2(1, 0)];
  result[6] = counted.IncrementCounter();
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: descriptor heap indexing is DXIL only\n");
    return 77;
  }
  // the depth texel, and comparison references on either side of it
  const float depth_value = 0.25f, below = 0.125f, above = 0.5f;
  const UINT first = 3, value = 0x5a5a, elements = 4, count = 7;
  auto cs = compiler.compile(
      hlsl, "cs", "cs_6_6", {"BELOW=" + std::to_string(below), "ABOVE=" + std::to_string(above)}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 1;
  auto rs = root_signature(
      device.Get(), {1, &param, 0, nullptr,
                     D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                         D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED}
  );
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));

  // one upload buffer holds the constant buffer (256 bytes) and then the element buffer, i at element i
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  mapped[0] = value;
  mapped[1] = count;
  for (UINT i = 0; i < elements; i++)
    mapped[64 + i] = i;
  upload->Unmap(0, nullptr);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto raw = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  // the counter starts from the upload buffer's second dword
  auto counted = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto counter = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 4, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  ComPtr<ID3D12Resource> depth, image;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));

  D3D12_RESOURCE_DESC image_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &image_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&image)
  ));

  ComPtr<ID3D12DescriptorHeap> resources, samplers, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC resources_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, first + 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  D3D12_DESCRIPTOR_HEAP_DESC samplers_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&resources_desc, IID_PPV_ARGS(&resources)));
  CHECK(device->CreateDescriptorHeap(&samplers_desc, IID_PPV_ARGS(&samplers)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto slot = [&](UINT i) {
    auto h = resources->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (first + i) * step;
    return h;
  };
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{upload->GetGPUVirtualAddress(), 256};
  device->CreateConstantBufferView(&cbv, slot(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC typed{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  typed.Buffer = {64, elements};
  device->CreateShaderResourceView(upload.Get(), &typed, slot(1));
  D3D12_SHADER_RESOURCE_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  structured.Buffer = {64, elements, 4};
  device->CreateShaderResourceView(upload.Get(), &structured, slot(2));
  D3D12_SHADER_RESOURCE_VIEW_DESC texture{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  texture.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(depth.Get(), &texture, slot(3));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  uav.Buffer = {0, 64, 4};
  device->CreateUnorderedAccessView(out.Get(), nullptr, &uav, slot(4));
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
  raw_uav.Buffer = {0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
  device->CreateUnorderedAccessView(raw.Get(), nullptr, &raw_uav, slot(5));
  D3D12_UNORDERED_ACCESS_VIEW_DESC image_uav{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D};
  device->CreateUnorderedAccessView(image.Get(), nullptr, &image_uav, slot(6));
  device->CreateUnorderedAccessView(counted.Get(), counter.Get(), &uav, slot(7));
  D3D12_SAMPLER_DESC sampler{D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
  device->CreateDepthStencilView(depth.Get(), &dsv, dsvs->GetCPUDescriptorHandleForHeapStart());

  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 516, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(counter.Get(), 0, upload.Get(), 4, 4);
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ClearDepthStencilView(dsvs->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, depth_value, 0, 0, nullptr);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {resources.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRoot32BitConstants(0, 1, &first, 0);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), raw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, 256);
  list->CopyBufferRegion(readback.Get(), 256, raw.Get(), 0, 256);
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 512, counter.Get(), 0, 4);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  // D3D12 compares the reference against the texel: LESS passes when the reference is below it
  const UINT want[] = {value, 1, 2, below < depth_value, above < depth_value, value + 2, count};
  const char *names[] = {"constant buffer", "typed buffer", "structured buffer", "compare below",
                         "compare above",   "typed UAV",    "UAV counter"};
  unsigned failures = 0;
  for (UINT i = 0; i < std::size(want); i++)
    if (got[i] != want[i] && ++failures)
      printf("%s: %#x, want %#x\n", names[i], got[i], want[i]);
  if (got[64 + 1] != value + 1 && ++failures)
    printf("raw buffer: %#x, want %#x\n", got[64 + 1], value + 1);
  // the increment returned the old count and left the next
  if (got[128] != count + 1 && ++failures)
    printf("UAV counter after: %u, want %u\n", got[128], count + 1);
  printf("%s: %u of %zu reads wrong\n", failures ? "failed" : "passed", failures, std::size(want) + 2);
  return failures != 0;
}
