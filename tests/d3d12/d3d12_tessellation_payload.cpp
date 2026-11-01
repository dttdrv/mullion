// contract: a hull shader's largest factor is its maxtessfactor, 64 by default (D3D11.3 11.7.10), however much it
// outputs: a patch whose factors are 64 is tessellated at 64, into the points Direct3D's reference tessellator makes
// (11.7.1). the hull shader here outputs 32 control points of 27 registers each, which with the most workloads a
// triangle patch at 64 makes nearly fills a mesh stage's payload (16 KB), one patch a threadgroup: a payload that
// counted anything a pipeline does not use would leave no room for factor 64, and the patch would come out coarser.
// with point output each point runs the pixel shader once, which counts them.
#include "d3d12_test.hpp"
#include "../reference/reference.hpp"

static const char hlsl[] = R"hlsl(
struct VSO { float4 x : X; };
VSO vs() {
  VSO o;
  o.x = 0;
  return o;
}
struct HCP { float4 r[27] : R; };
struct PC {
  float edges[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
};
PC pc() {
  PC o;
  o.edges[0] = o.edges[1] = o.edges[2] = o.inside = 64;
  return o;
}
// every register of every point is written, and the last one's last read, so none is left out
[domain("tri")] [partitioning("integer")] [outputtopology("point")] [outputcontrolpoints(32)] [patchconstantfunc("pc")]
HCP hs(InputPatch<VSO, 1> ip, uint id : SV_OutputControlPointID) {
  HCP o;
  [unroll] for (int k = 0; k < 27; k++)
    o.r[k] = ip[0].x + float4(k, id, 1, 0);
  return o;
}
[domain("tri")]
float4 ds(PC pc, float3 location : SV_DomainLocation, const OutputPatch<HCP, 32> op) : SV_Position {
  // r[26] of point 31 is (26, 31, 1, 0): the position is the viewport's one pixel's center
  return float4(op[31].r[26].x - 26, op[31].r[26].y - 31, 0.5, op[31].r[26].z);
}
RWByteAddressBuffer points : register(u0);
void ps() { points.InterlockedAdd(0, 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), hs = compiler.compile(hlsl, "hs", "hs"),
       ds = compiler.compile(hlsl, "ds", "ds"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || hs.empty() || ds.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.HS = bytecode(hs);
  desc.DS = bytecode(ds);
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
    printf("failed: no pipeline\n");
    return 1;
  }

  auto counter = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto zero = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(zero->Map(0, nullptr, &mapped));
  memset(mapped, 0, 256);
  zero->Unmap(0, nullptr);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(counter.Get(), 0, zero.Get(), 0, 256);
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  D3D12_RECT scissor{0, 0, 1, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
  list->DrawInstanced(1, 1, 0, 0);
  transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, counter.Get(), 0, 256);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const float factors[4] = {64, 64, 64, 64};
  size_t want = reference::points(reference::triangle, reference::integer, factors).size();
  UINT *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  UINT got = out[0];
  readback->Unmap(0, nullptr);
  if (got != want) {
    printf("failed: a patch of factor 64 made %u points, want %zu\n", got, want);
    return 1;
  }
  printf("passed: a patch of factor 64 made its %zu points\n", want);
  return 0;
}
