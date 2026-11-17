// contract: a device whose DoublePrecisionFloatShaderOps is FALSE ("Specifies whether double types are allowed for
// shader operations", D3D12_FEATURE_DATA_D3D12_OPTIONS) refuses a pipeline whose shader uses them with E_INVALIDARG,
// leaves the pipeline null, and goes on: vkd3d-proton does the same (libs/vkd3d/device.c,
// d3d12_device_validate_shader_meta: "Attempting to use FP64 operations in shader ... but this is not supported",
// and libs/vkd3d/state.c returns E_INVALIDARG for it). a pipeline made after it from the same shader without doubles
// runs and writes what it computes.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
RWStructuredBuffer<uint> result : register(u0);
cbuffer Constants : register(b0) { uint low, high; };
[numthreads(1, 1, 1)] void cs() {
#if DOUBLES == 1
  double value = asdouble(low, high);
  result[0] = uint(value * 3.0);
#elif DOUBLES == 2
  // doubles no operation makes from integers' bits: the arithmetic alone is in them
  double value = low;
  result[0] = uint(value * 2.5 + high);
#else
  result[0] = low * 3;
#endif
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.DoublePrecisionFloatShaderOps) {
    printf("skipped: the device has double-precision shader operations\n");
    return 77;
  }
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 2;
  auto root = root_signature(device.Get(), {2, params});
  CHECK(root ? S_OK : E_FAIL);

  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root.Get()};
  for (const char *form : {"DOUBLES=1", "DOUBLES=2"}) {
    step("a shader with doubles, %s", form);
    auto doubles = compiler.compile(hlsl, "cs", "cs", {form});
    if (!expect(!doubles.empty(), "the shader with doubles does not compile"))
      return verdict();
    desc.CS = bytecode(doubles);
    ID3D12PipelineState *refused = (ID3D12PipelineState *)(uintptr_t)1;
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&refused));
    expect(hr == E_INVALIDARG && !refused, "a pipeline with doubles gives %08lx and %p", hr, refused);
    if (SUCCEEDED(hr) && refused)
      refused->Release();
  }

  step("the shader without doubles");
  auto plain = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!plain.empty(), "the shader without doubles does not compile"))
    return verdict();
  desc.CS = bytecode(plain);
  ComPtr<ID3D12PipelineState> pipeline;
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)));
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 4, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(output && readback, "the buffers are refused"))
    return verdict();
  CHECK(forget(readback.Get()));
  list->SetComputeRootSignature(root.Get());
  list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  const UINT constants[2] = {7, 0};
  list->SetComputeRoot32BitConstants(1, 2, constants, 0);
  list->Dispatch(1, 1, 1);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 4);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));
  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  expect(*got == 21, "the pipeline without doubles wrote %u, want 21", *got);
  readback->Unmap(0, nullptr);
  return verdict();
}
