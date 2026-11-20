// contract: frac returns NaN for either infinity and NaN, with or without precise, in every shader stage.
// "Component-wise, extract fractional component." "dest = src0 - round_ni(src0)" (D3D11.3 22.10.7,
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/frc--sm4---asm-). TableFrc gives "NaN" for
// "-inf", "+inf" and "NaN", and "[+0 to 1)" for finite reals. finite controls have exactly representable
// fractions; the CPU subtracts their floor. results are stored as integers so no later floating-point operation
// can hide a NaN. scalar and vector expressions run in vertex and geometry shaders emulated with mesh shaders,
// and in compute, both precise and ordinary, over more than two compute groups and many geometry groups.
#include "d3d12_test.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint4> input : register(t0);
RWStructuredBuffer<uint4> output : register(u0);
uint4 fraction(uint id) {
  PRECISE VECTOR value = frac(asfloat(input[id].CHANNELS));
  uint4 result = 0;
  result.CHANNELS = asuint(value);
  return result;
}
struct V { float4 pos : SV_Position; uint id : ID; };
V vs(uint id : SV_VertexID) {
  output[VS_OFFSET + id] = fraction(id);
  V v;
  v.pos = float4(0, 0, 0, 1);
  v.id = id;
  return v;
}
[maxvertexcount(1)]
void gs(point V v[1], inout PointStream<V> stream) {
  output[GS_OFFSET + v[0].id] = fraction(v[0].id);
}
[numthreads(GROUP, 1, 1)]
void cs(uint id : SV_DispatchThreadID) {
  if (id < COUNT)
    output[CS_OFFSET + id] = fraction(id);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  using Record = std::array<UINT, 4>;
  const UINT components = std::tuple_size_v<Record>, group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  const UINT count = 2 * group + 1;
  static constexpr const char *stages[] = {"vs", "gs", "cs"};
  const UINT64 input_bytes = count * sizeof(Record), output_bytes = input_bytes * std::size(stages);
  struct Case {
    bool precise;
    UINT width;
    std::array<std::string, std::size(stages)> code;
  };
  std::vector<Case> cases;
  for (bool precise : {false, true})
    for (UINT width = 1; width <= components; width++) {
      Case c{precise, width};
      std::vector<std::string> defines = {
          "PRECISE=" + std::string(precise ? "precise" : ""),
          "VECTOR=float" + (width == 1 ? std::string() : std::to_string(width)),
          "CHANNELS=" + std::string("xyzw", width), "GROUP=" + std::to_string(group),
          "COUNT=" + std::to_string(count), "VS_OFFSET=0", "GS_OFFSET=" + std::to_string(count),
          "CS_OFFSET=" + std::to_string(2 * count),
      };
      for (UINT s = 0; s < std::size(stages); s++) {
        c.code[s] = compiler.compile(hlsl, stages[s], stages[s], defines);
        if (!expect(!c.code[s].empty(), "%s precise=%u width=%u did not compile", stages[s], precise, width))
          return verdict();
      }
      cases.push_back(c);
    }

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "root signature"))
    return verdict();
  const float integral = std::ldexp(1.0f, std::numeric_limits<float>::digits - 1);
  const float values[] = {
      0.0f, -0.0f, 0.25f, -0.25f, 1.0f, -1.0f, 3.75f, -3.75f,
      std::nextafter(1.0f, 0.0f), -std::nextafter(1.0f, 0.0f),
      std::nextafter(integral, 0.0f), -std::nextafter(integral, 0.0f), integral, -integral,
      std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
      std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::quiet_NaN(),
  };
  std::vector<Record> input(count);
  for (UINT i = 0; i < count; i++)
    for (UINT lane = 0; lane < components; lane++)
      input[i][lane] = std::bit_cast<UINT>(values[(i + lane) % std::size(values)]);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input_bytes + output_bytes,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, input.data(), input_bytes);
  // infinity is never an expected result, including the inactive lanes
  for (UINT64 i = input_bytes / sizeof(UINT); i < (input_bytes + output_bytes) / sizeof(UINT); i++)
    mapped[i] = std::bit_cast<UINT>(std::numeric_limits<float>::infinity());
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  for (auto &c : cases) {
    step("frac precise=%u width=%u over %u inputs in vertex, geometry and compute", c.precise, c.width, count);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(c.code[0])};
    desc.GS = bytecode(c.code[1]);
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> graphics, compute;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&graphics)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(c.code[2])};
    CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), graphics.Get(), IID_PPV_ARGS(&list)
    ));
    list->CopyBufferRegion(output.Get(), 0, upload.Get(), input_bytes, output_bytes);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootShaderResourceView(0, upload->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->DrawInstanced(count, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    barrier.UAV.pResource = output.Get();
    list->ResourceBarrier(1, &barrier);
    list->SetPipelineState(compute.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootShaderResourceView(0, upload->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->Dispatch((count + group - 1) / group, 1, 1);
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), output.Get());
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    Record *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT s = 0; s < std::size(stages); s++)
      for (UINT i = 0; i < count; i++)
        for (UINT lane = 0; lane < components; lane++) {
          UINT bits = got[s * count + i][lane];
          float a = std::bit_cast<float>(input[i][lane]), value = std::bit_cast<float>(bits);
          bool ok = lane >= c.width ? bits == 0
                    : !std::isfinite(a) ? std::isnan(value)
                                       : value == float(double(a) - std::floor(double(a))) && !std::signbit(value);
          expect(ok, "%s input=%u lane=%u frac(%#x) returned %#x", stages[s], i, lane, input[i][lane], bits);
        }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
