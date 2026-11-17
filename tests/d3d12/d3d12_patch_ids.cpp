// contract: a pixel shader after tessellation sees the PrimitiveID of the patch its triangle came from. "When a patch
// topology is used, PrimitiveID identifies which patch in the Draw*() call is being processed ... for a given patch,
// all of the primitives generated for it have the same PrimitiveID" (D3D11.3 11.7.9.1), and "When Draw*Instanced()
// is used, the PrimitiveID resets to its starting value whenever a new instance begins" (8.17); the pixel shader gets
// it as the first active stage after the domain shader (4.4.4). each quad patch covers its own 3x3 pixels and is
// tessellated at factor 3, so each of its pixels is in another of its triangles; instances have rows of their own.
// every pixel must be its patch's index in its instance plus one, drawn directly, indexed and through ExecuteIndirect
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer Size : register(b0) { uint patches, instances; };
struct Point { float2 at : PIXEL; };
Point vs(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  uint patch = id / 4, corner = id % 4;
  Point p;
  p.at = 3 * float2(patch + corner % 2, instance + corner / 2);
  return p;
}
struct Factors { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
Factors factors() {
  Factors f;
  f.edges[0] = f.edges[1] = f.edges[2] = f.edges[3] = f.inside[0] = f.inside[1] = 3;
  return f;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(4)]
[patchconstantfunc("factors")] Point hs(InputPatch<Point, 4> patch, uint i : SV_OutputControlPointID) { return patch[i]; }
[domain("quad")] float4 ds(Factors f, float2 uv : SV_DomainLocation, const OutputPatch<Point, 4> patch) : SV_Position {
  float2 at = lerp(lerp(patch[0].at, patch[1].at, uv.x), lerp(patch[2].at, patch[3].at, uv.x), uv.y);
  return float4(at.x / (3 * patches) * 2 - 1, 1 - at.y / (3 * instances) * 2, 0, 1);
}
uint ps(uint id : SV_PrimitiveID) : SV_Target { return id + 1; }
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

  const UINT patches = 200, instances = 3, width = 3 * patches, height = 3 * instances, size[2] = {patches, instances};
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.HS = bytecode(hs);
  desc.DS = bytecode(ds);
  desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 readback_size;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST);
  // the indexed draw's indices name the vertices in order; the indirect draw's arguments are the direct draw's
  const UINT vertices = 4 * patches;
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertices * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS),
                          D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!readback || !indices || !arguments) {
    printf("failed: buffers not created\n");
    return 1;
  }
  UINT *index;
  CHECK(indices->Map(0, nullptr, (void **)&index));
  for (UINT i = 0; i < vertices; i++)
    index[i] = i;
  indices->Unmap(0, nullptr);
  D3D12_DRAW_ARGUMENTS *draw_arguments;
  CHECK(arguments->Map(0, nullptr, (void **)&draw_arguments));
  *draw_arguments = {vertices, instances, 0, 0};
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  enum { Direct, Indexed, Indirect, Ways };
  const char *names[] = {"direct", "indexed", "indirect"};
  unsigned failures = 0;
  for (int way = 0; way < Ways; way++) {
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    // a pixel no draw wrote reads 0, which no patch gives
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRoot32BitConstants(0, 2, size, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (way == Direct)
      list->DrawInstanced(vertices, instances, 0, 0);
    if (way == Indexed) {
      D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), vertices * UINT(sizeof(UINT)), DXGI_FORMAT_R32_UINT};
      list->IASetIndexBuffer(&ibv);
      list->DrawIndexedInstanced(vertices, instances, 0, 0, 0);
    }
    if (way == Indirect)
      list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{
        target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
    };
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    unsigned wrong = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        UINT value = ((const UINT *)(got + y * footprint.Footprint.RowPitch))[x], want = x / 3 + 1;
        if (value != want && wrong++ < 4)
          printf("%s: pixel (%u, %u) of instance %u is %u, want %u\n", names[way], x, y, y / 3, value, want);
      }
    readback->Unmap(0, nullptr);
    if (wrong)
      printf("%s: %u of %u pixels wrong\n", names[way], wrong, width * height);
    failures += wrong != 0;
  }
  printf("%s: %u of %d ways of drawing gave a pixel another patch's ID\n", failures ? "failed" : "passed", failures,
         int(Ways));
  return failures != 0;
}
