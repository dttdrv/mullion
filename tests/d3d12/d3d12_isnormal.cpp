// contract: DXIL opcode11 is IsNormal, an IsSpecialFloat operation on float32 (DXIL.rst, Instructions).
// D3D11.3 3.1.3.1 says "Refer to IEEE-754 for topics not mentioned." binary32 normals have an exponent
// distinct from zero and the all-ones exponent. zeros, subnormals, infinities and NaNs are not normal.
// older HLSL has no isnormal intrinsic. replace opcode8 in a compiled classification shader with opcode11,
// preserving all resource/signature metadata, then assemble and structurally validate before device creation.
// https://github.com/microsoft/DirectXShaderCompiler/blob/v1.7.2308/docs/DXIL.rst#isnormal
#include "dxil_assembly.hpp"
#include <array>
#include <climits>
#include <limits>
#include <regex>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint> inputs : register(t0);
RWStructuredBuffer<uint2> output : register(u0);
[numthreads(GROUP, 1, 1)] void cs(uint t : SV_DispatchThreadID) {
  if (t < COUNT) output[t] = uint2(isnan(asfloat(inputs[t])), inputs[t]);
}
)hlsl";

static std::string
normal_shader(const Compiler &compiler, const std::string &seed) {
  auto ir = dxil_disassemble(compiler, seed);
  const std::regex instruction(R"(@dx\.op\.isSpecialFloat\.f32\(i32 8,)");
  if (!expect(
          std::distance(std::sregex_iterator(ir.begin(), ir.end(), instruction), std::sregex_iterator()) == 1,
          "classification seed must contain exactly one float32 IsNaN call"
      ))
    return {};
  return dxil_assemble(std::regex_replace(ir, instruction, "@dx.op.isSpecialFloat.f32(i32 11,"), "isnormal");
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: native DXIL classification requires DXC\n");
    return 77;
  }
  static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == sizeof(UINT));
  const UINT fraction_bits = std::numeric_limits<float>::digits - 1;
  const UINT exponent_bits = sizeof(float) * CHAR_BIT - fraction_bits - 1;
  const UINT fraction_mask = (UINT(1) << fraction_bits) - 1;
  const UINT exponent_mask = (UINT(1) << exponent_bits) - 1;
  const UINT sign_bit = UINT(1) << (sizeof(float) * CHAR_BIT - 1);
  std::vector<UINT> fractions{0, fraction_mask};
  for (UINT bit = 0; bit < fraction_bits; bit++) {
    fractions.push_back(UINT(1) << bit);
    fractions.push_back(fraction_mask ^ (UINT(1) << bit));
  }
  std::vector<UINT> inputs;
  for (UINT sign : {0u, sign_bit})
    for (UINT exponent = 0; exponent <= exponent_mask; exponent++)
      for (UINT fraction : fractions)
        inputs.push_back(sign | exponent << fraction_bits | fraction);
  inputs.push_back(0);
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  auto seed = compiler.compile(
      hlsl, "cs", "cs_6_0", {"GROUP=" + std::to_string(group), "COUNT=" + std::to_string(inputs.size())}
  );
  if (!expect(!seed.empty(), "classification seed HLSL"))
    return verdict();
  auto code = normal_shader(compiler, seed);
  if (code.empty())
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(root), "IsNormal root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> pipeline;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{root.Get(), bytecode(code)};
  const HRESULT created = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
  if (!expect(SUCCEEDED(created), "native float32 IsNormal pipeline HRESULT %#lx", created))
    return verdict();
  using Pair = std::array<UINT, 2>;
  const UINT total = UINT((inputs.size() + group - 1) / group) * group;
  const UINT64 bytes = UINT64(total) * sizeof(Pair);
  auto input =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, inputs.size() * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(input && initial && output && readback && indirect, "classification buffers"))
    return verdict();
  void *mapped;
  CHECK(input->Map(0, nullptr, &mapped));
  memcpy(mapped, inputs.data(), inputs.size() * sizeof(UINT));
  input->Unmap(0, nullptr);
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  initial->Unmap(0, nullptr);
  CHECK(indirect->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{UINT((inputs.size() + group - 1) / group), 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  indirect->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(dispatch), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT path = 0; path < 2; path++) {
    step("native float32 IsNormal, all exponents and mantissa boundaries, %s", path ? "indirect" : "direct");
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipeline.Get()));
    list->CopyResource(output.Get(), initial.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    if (path)
      list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
    else
      list->Dispatch(dispatch.ThreadGroupCountX, dispatch.ThreadGroupCountY, dispatch.ThreadGroupCountZ);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    Pair *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    bool correct = true;
    for (UINT i = 0; i < total; i++) {
      const UINT exponent = i < inputs.size() ? (inputs[i] >> fraction_bits) & exponent_mask : 0;
      const Pair want =
          i < inputs.size() ? Pair{exponent != 0 && exponent != exponent_mask, inputs[i]} : Pair{UINT_MAX, UINT_MAX};
      if (!expect(
              got[i] == want, "element %u input %#x: class %#x bits %#x, expected class %#x bits %#x", i,
              i < inputs.size() ? inputs[i] : 0, got[i][0], got[i][1], want[0], want[1]
          )) {
        correct = false;
        break;
      }
    }
    readback->Unmap(0, nullptr);
    if (!correct)
      return verdict();
  }
  return verdict();
}
