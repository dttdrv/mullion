// contract: "All power of 2 sizes in the range reported by the driver must be supported for selection by the
// shader" (DirectX-Specs, Wave Size, Device Behavior,
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_WaveSize.html). allowed sizes are 4,8,16,32,64,128;
// pipeline creation fails outside the reported range. each accepted shader must execute at the requested size.
// CPU lane membership determines counts and exclusive prefixes without assuming a thread-to-lane mapping.
#include "d3d12_test.hpp"
#include <array>
#include <map>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint4> output : register(u0);
[WaveSize(WAVE)] [numthreads(GROUP, 1, 1)]
void cs(uint t : SV_GroupIndex) {
  output[t] = uint4(WaveGetLaneCount(), WaveGetLaneIndex(), WaveReadLaneFirst(t), WavePrefixCountBits(true));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: WaveSize requires DXC\n");
    return 77;
  }
  const UINT sizes[] = {4, 8, 16, 32, 64, 128};
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  std::array<std::string, std::size(sizes)> shaders;
  for (UINT i = 0; i < std::size(sizes); i++) {
    step("compile WaveSize(%u)", sizes[i]);
    shaders[i] =
        compiler.compile(hlsl, "cs", "cs_6_6", {"WAVE=" + std::to_string(sizes[i]), "GROUP=" + std::to_string(group)});
    if (!expect(!shaders[i].empty(), "WaveSize(%u) HLSL", sizes[i]))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  if (!expect(
          options.WaveOps && options.WaveLaneCountMin >= sizes[0] &&
              options.WaveLaneCountMax <= sizes[std::size(sizes) - 1] &&
              options.WaveLaneCountMin <= options.WaveLaneCountMax,
          "wave support/range %d %u..%u", options.WaveOps, options.WaveLaneCountMin, options.WaveLaneCountMax
      ))
    return verdict();
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  using Record = std::array<UINT, 4>;
  const UINT bytes = group * sizeof(Record);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(output && initial && readback, "wave buffers"))
    return verdict();
  void *mapped;
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  initial->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect dispatch buffer"))
    return verdict();
  CHECK(indirect->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{1, 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  indirect->Unmap(0, nullptr);
  for (UINT i = 0; i < std::size(sizes); i++) {
    UINT wave = sizes[i];
    step("create WaveSize(%u) against reported range %u..%u", wave, options.WaveLaneCountMin, options.WaveLaneCountMax);
    bool supported = wave >= options.WaveLaneCountMin && wave <= options.WaveLaneCountMax;
    ComPtr<ID3D12PipelineState> pso;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(shaders[i])};
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (!expect(
            SUCCEEDED(hr) == supported && (!supported || bool(pso)), "WaveSize(%u) pipeline HRESULT %#lx, pointer %p",
            wave, hr, pso.Get()
        ) ||
        !supported)
      continue;
    for (bool indirect_path : {false, true}) {
      step("execute WaveSize(%u), indirect=%d", wave, indirect_path);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      list->CopyResource(output.Get(), initial.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      if (indirect_path)
        list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
      else
        list->Dispatch(1, 1, 1);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      Record *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      std::map<UINT, std::map<UINT, UINT>> waves;
      for (UINT t = 0; t < group; t++) {
        auto r = got[t];
        if (expect(
                r[0] == wave && r[1] < wave && r[2] < group, "thread %u width/lane/leader %u/%u/%u", t, r[0], r[1], r[2]
            ))
          expect(waves[r[2]].emplace(r[1], t).second, "thread %u duplicates lane %u", t, r[1]);
      }
      for (auto &[head, members] : waves) {
        UINT prefix = 0;
        expect(members.begin()->second == head, "wave leader %u, first thread %u", head, members.begin()->second);
        for (auto [lane, t] : members)
          expect(got[t][3] == prefix++, "thread %u prefix %u", t, got[t][3]);
      }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
