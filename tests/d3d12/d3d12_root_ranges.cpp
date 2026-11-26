// contract: descriptor-table ranges use explicit offsets or append after the preceding range; an unbounded last
// range preserves the same register mapping. "The contents of the descriptors can be changed by the application
// after the command list has been recorded" (Root Signature Version 1.1, DESCRIPTORS_VOLATILE).
// static descriptors are initialized before recording; volatile descriptors/data change only before GPU execution.
// DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS retains zero-returning buffer bounds checks. every referenced
// descriptor and every returned dimension is checked, across gaps and the finite/unbounded range boundary.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html
#include "d3d12_test.hpp"
#include <array>
#include <limits>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> first_data[HALF] : register(t0, SPACE);
StructuredBuffer<uint> last_data[LAST_COUNT] : register(LAST_REGISTER, SPACE);
RWStructuredBuffer<uint4> results : register(u0);
cbuffer control : register(b0) { uint descriptor; uint element; uint slot; };
[numthreads(1, 1, 1)] void cs() {
  uint n, stride;
  uint value;
  if (descriptor < HALF) {
    first_data[descriptor].GetDimensions(n, stride);
    value = first_data[descriptor][element];
  } else {
    last_data[descriptor - HALF].GetDimensions(n, stride);
    value = last_data[descriptor - HALF][element];
  }
  results[slot] = uint4(value, n, stride, descriptor);
}
)hlsl";

int main(int argc, char **argv) {
  constexpr UINT per_range = 3, descriptors = 2 * per_range, elements = 3, controls = 3;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT spaces[] = {0, 1, D3D12_SYSTEM_RESERVED_REGISTER_SPACE_VALUES_START - 1};
  std::string shaders[std::size(spaces)][2];
  for (UINT i = 0; i < std::size(spaces); i++)
    for (bool unbounded : {false, true}) {
      const std::vector<std::string> defines = {
          "SPACE=space" + std::to_string(spaces[i]), "HALF=" + std::to_string(per_range),
          "LAST_REGISTER=t" + std::to_string(per_range),
          "LAST_COUNT=" + (unbounded ? std::string() : std::to_string(per_range))};
      if (!compiler.dxc && unbounded) {
        std::string source;
        for (const auto &define : defines) {
          auto split = define.find('=');
          source += "#define " + define.substr(0, split) + " " + define.substr(split + 1) + "\n";
        }
        source += hlsl;
        ComPtr<ID3DBlob> code, errors;
        // D3DCOMPILE Constants: "Directs the compiler to enable unbounded descriptor tables."
        // https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/d3dcompile-constants
        HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "cs", "cs_5_1",
                                D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES, 0, &code, &errors);
        if (FAILED(hr) && errors)
          printf("cs: %.*s\n", int(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer()));
        if (SUCCEEDED(hr))
          shaders[i][unbounded] = kept(
              std::string(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize()), "cs");
      } else
        shaders[i][unbounded] = compiler.compile(hlsl, "cs", "cs", defines);
      if (!expect(!shaders[i][unbounded].empty(), "range shader space %u did not compile", spaces[i]))
        return verdict();
    }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_ROOT_SIGNATURE support{D3D_ROOT_SIGNATURE_VERSION_1_1};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &support, sizeof(support)));
  if (support.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1) {
    printf("skipped: root signature 1.1 not supported\n");
    return 77;
  }
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  constexpr UINT table_base = 2, first_offset = 3, gap = 2, largest_offset = first_offset + per_range + gap;
  constexpr UINT heap_slots = table_base + largest_offset + per_range + 1;
  using Words = std::array<UINT, 4>;
  const UINT max_results = descriptors * 4;
  auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, descriptors * elements * sizeof(UINT),
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, max_results * sizeof(Words),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, max_results * sizeof(Words), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(input && output && back, "range buffers"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, heap_slots,
                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(hd.Type);
  auto word = [](UINT descriptor, UINT element, UINT generation) {
    return (descriptor + 1) * (elements + 1) + element + generation * descriptors * (elements + 1);
  };
  const D3D12_DESCRIPTOR_RANGE_FLAGS flags[] = {
      D3D12_DESCRIPTOR_RANGE_FLAG_NONE, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC,
      D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
      D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE),
      D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE),
      D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS,
      D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE),
      D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE),
      D3D12_DESCRIPTOR_RANGE_FLAGS(D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC)};
  for (UINT s = 0; s < std::size(spaces); s++)
    for (auto flag : flags)
      for (bool appended : {false, true})
        for (bool unbounded : {false, true}) {
          const bool volatile_descriptors = flag & D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
          const bool volatile_data = flag & D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
          const bool bounded_reads = volatile_descriptors ||
                                    (flag & D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS);
          const UINT second_offset = first_offset + per_range + (appended ? 0 : gap);
          step("space %u, flags %#x, %s offsets, %s last range", spaces[s], flag,
               appended ? "appended" : "explicit", unbounded ? "unbounded" : "finite");
          D3D12_DESCRIPTOR_RANGE1 ranges[] = {
              {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, per_range, 0, spaces[s], flag, first_offset},
              {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, unbounded ? std::numeric_limits<UINT>::max() : per_range,
               per_range, spaces[s], flag, appended ? D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND : second_offset}};
          D3D12_ROOT_PARAMETER1 params[] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                            {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
          params[0].DescriptorTable = {UINT(std::size(ranges)), ranges};
          params[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
          params[2].Constants.Num32BitValues = controls;
          D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{D3D_ROOT_SIGNATURE_VERSION_1_1};
          rd.Desc_1_1 = {UINT(std::size(params)), params};
          ComPtr<ID3DBlob> blob, errors;
          HRESULT hr = D3D12SerializeVersionedRootSignature(&rd, &blob, &errors);
          if (FAILED(hr) && errors)
            printf("root serializer: %.*s\n", int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
          if (!expect(hr == S_OK && blob, "valid range signature serialization: %08lx", hr))
            continue;
          ComPtr<ID3D12RootSignature> root;
          CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
          ComPtr<ID3D12PipelineState> pso;
          D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(shaders[s][unbounded])};
          CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
          auto write_data = [&](UINT generation) -> HRESULT {
            UINT *mapped;
            HRESULT hr = input->Map(0, nullptr, (void **)&mapped);
            if (FAILED(hr))
              return hr;
            for (UINT d = 0; d < descriptors; d++)
              for (UINT e = 0; e < elements; e++)
                mapped[d * elements + e] = word(d, e, generation);
            input->Unmap(0, nullptr);
            return S_OK;
          };
          auto write_views = [&](bool reversed) {
            for (UINT d = 0; d < descriptors; d++) {
              D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER,
                                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
              view.Buffer = {UINT64(reversed ? descriptors - 1 - d : d) * elements, elements, sizeof(UINT)};
              const UINT at = table_base + (d < per_range ? first_offset + d : second_offset + d - per_range);
              device->CreateShaderResourceView(input.Get(), &view,
                  {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(at) * increment});
            }
          };
          CHECK(write_data(0));
          write_views(false);
          CHECK(forget(back.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pso.Get()));
          list->SetDescriptorHeaps(1, heap.GetAddressOf());
          list->SetComputeRootSignature(root.Get());
          list->SetComputeRootDescriptorTable(0, {heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(table_base) * increment});
          list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
          std::vector<Words> expected;
          for (UINT d = 0; d < descriptors; d++)
            for (UINT e : {0u, elements - 1, elements, std::numeric_limits<UINT>::max()}) {
              if (e >= elements && !bounded_reads)
                continue;
              const UINT control[] = {d, e, UINT(expected.size())};
              list->SetComputeRoot32BitConstants(2, controls, control, 0);
              list->Dispatch(1, 1, 1);
              expected.push_back({e < elements ? word(volatile_descriptors ? descriptors - 1 - d : d, e,
                                                       volatile_data ? 1 : 0) : 0,
                                   elements, sizeof(UINT), d});
            }
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(back.Get(), 0, output.Get(), 0, expected.size() * sizeof(Words));
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          CHECK(list->Close());
          if (volatile_descriptors)
            write_views(true);
          if (volatile_data)
            CHECK(write_data(1));
          CHECK(execute(device.Get(), queue.Get(), list.Get()));
          Words *got;
          CHECK(back->Map(0, nullptr, (void **)&got));
          for (UINT i = 0; i < expected.size(); i++)
            expect(got[i] == expected[i], "range result %u: %#x %u %u %u, want %#x %u %u %u", i,
                   got[i][0], got[i][1], got[i][2], got[i][3], expected[i][0], expected[i][1], expected[i][2], expected[i][3]);
          back->Unmap(0, nullptr);
        }
  return verdict();
}
