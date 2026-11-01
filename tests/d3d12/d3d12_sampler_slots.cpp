// contract: a GPU that runs out of samplers says so once. Metal holds maxArgumentBufferSamplerCount "unique argument
// buffer samplers per app" (Apple, MTLDevice) and the newer ones sample wrong with no error, measured on an M1 Pro
// (1024 there, as the Metal feature set tables give Apple7 and Apple8 for samplers a stage reaches from an argument
// buffer, against 500 K from Apple9). a full sampler heap (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE) of samplers
// that differ in MinLOD by the GPU's own step of 1/64 makes past 1024 distinct Metal samplers, each with its cube
// twin, and far below 500 K. emulated as Apple7 or Apple8 (MULLION_GPU_FAMILY) the log names 1024 once; any other GPU
// says it at most once, with a limit below what was made.
// past the limit a sampler samples as the held one of its filters and address modes nearest in LOD clamps. three of
// MIRROR, the only samplers of that mode, are made first, at MinLOD 6, 2 and 9, and one at MinLOD 3 after the heap is
// full: it reads the mip a MinLOD 2 sampler reads where the limit is passed, and mip 3 where it is not. the first or
// the last of the three would read mip 6 or 9. the texture's mips hold their numbers, and SampleLevel asks for mip 0,
// which the sampler's MinLOD clamps (D3D11.3 7.18.11).
#include "d3d12_test.hpp"
#include <fstream>
#include <iterator>

static const char hlsl[] = R"hlsl(
Texture2D<float> mips : register(t0);
SamplerState samplers[4] : register(s0);
RWBuffer<float> read : register(u0);
[numthreads(1, 1, 1)] void cs() {
  read[0] = mips.SampleLevel(samplers[0], 0.5, 0);
  read[1] = mips.SampleLevel(samplers[1], 0.5, 0);
  read[2] = mips.SampleLevel(samplers[2], 0.5, 0);
  read[3] = mips.SampleLevel(samplers[3], 0.5, 0);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH], family[8] = {};
  auto length = GetTempPathA(sizeof(temporary), temporary);
  CHECK(length && length < sizeof(temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "smp", directory) ? S_OK : E_FAIL);
  CHECK(SetEnvironmentVariableA("MULLION_LOG_PATH", directory) ? S_OK : E_FAIL);
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);
  GetEnvironmentVariableA("MULLION_GPU_FAMILY", family, sizeof(family));
  const bool emulated = !strcmp(family, "7") || !strcmp(family, "8");

  step("fill a sampler heap with samplers one LOD step apart");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const UINT count = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE, steps = 64, lods = 14 * steps + 1;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, count,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> heap;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  auto mirror = [&](UINT slot, float min_lod) {
    D3D12_SAMPLER_DESC desc{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
                            D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
    desc.MinLOD = min_lod;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&desc, {heap->GetCPUDescriptorHandleForHeapStart().ptr + slot * increment});
  };
  const float held[3] = {6, 2, 9}, probe = 3;
  const UINT probes = std::size(held) + 1;
  for (UINT i = 0; i < std::size(held); i++)
    mirror(i, held[i]);
  handle.ptr += probes * increment;
  for (UINT i = probes; i < count; i++, handle.ptr += increment) {
    D3D12_SAMPLER_DESC desc{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP};
    desc.MinLOD = float(i % lods) / steps;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&desc, handle);
  }
  mirror(std::size(held), probe);

  step("sample through the three held MIRROR samplers and the one made last");
  const UINT size = 1 << 10, levels = 11;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, levels, DXGI_FORMAT_R32_FLOAT,
                                   {1, 0}};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[levels];
  UINT rows[levels];
  UINT64 bytes;
  device->GetCopyableFootprints(&texture_desc, 0, levels, 0, footprints, rows, nullptr, &bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, probes * sizeof(float), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, probes * sizeof(float), D3D12_RESOURCE_STATE_COPY_DEST);
  char *staged;
  CHECK(upload->Map(0, nullptr, (void **)&staged));
  for (UINT mip = 0; mip < levels; mip++)
    for (UINT row = 0; row < rows[mip]; row++)
      std::fill_n((float *)(staged + footprints[mip].Offset + row * footprints[mip].Footprint.RowPitch),
                  footprints[mip].Footprint.Width, float(mip));
  upload->Unmap(0, nullptr);

  D3D12_DESCRIPTOR_RANGE ranges[3] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
                                      {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, probes}};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].DescriptorTable = {2, ranges}, params[1].DescriptorTable = {1, &ranges[2]};
  auto rs = root_signature(device.Get(), {2, params});
  auto cs = compiler.compile(hlsl, "cs", "cs");
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto view = views->GetCPUDescriptorHandleForHeapStart();
  device->CreateShaderResourceView(texture.Get(), nullptr, view);
  view.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_FLOAT, D3D12_UAV_DIMENSION_BUFFER};
  uav.Buffer.NumElements = probes;
  device->CreateUnorderedAccessView(out.Get(), nullptr, &uav, view);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  for (UINT mip = 0; mip < levels; mip++) {
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}},
        from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[mip]}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {views.Get(), heap.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), out.Get());
  CHECK(forget(readback.Get()));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const float *read;
  CHECK(readback->Map(0, nullptr, (void **)&read));
  for (UINT i = 0; i < std::size(held); i++)
    expect(read[i] == held[i], "the held sampler at MinLOD %g read mip %g", held[i], read[i]);
  expect(read[std::size(held)] == (emulated ? held[1] : probe), "family %s: the sampler at MinLOD %g read mip %g",
         family, probe, read[std::size(held)]);
  readback->Unmap(0, nullptr);

  step("read the log");
  const std::string name = std::string(exe).substr(std::string(exe).find_last_of("\\/") + 1);
  const std::string path = std::string(directory) + "\\" + name.substr(0, name.find_last_of('.')) + "_d3d12.log";
  std::ifstream file(path);
  const std::string lines{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const char said[] = "Sampler: past the ";
  size_t first = lines.find(said);
  unsigned limit = first == std::string::npos ? 0 : std::stoul(lines.substr(first + sizeof(said) - 1));
  expect(first == std::string::npos || lines.find(said, first + 1) == std::string::npos, "said more than once");
  expect(emulated ? limit == 1024 : limit < 2 * lods, "family %s: the limit said is %u", family, limit);
  file.close();
  DeleteFileA(path.c_str());
  heap = nullptr;
  device = nullptr;
  RemoveDirectoryA(directory);
  return verdict();
}
