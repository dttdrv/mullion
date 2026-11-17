// contract: a tessellated draw records and draws whatever its hull shader outputs, as a list holds any number of
// draws. one draw of 4096 patches in 80 instances, each patch 32 control points of 8 registers, has 1.25 GB of hull
// outputs, past the 1 GB a device records its lists' GPU data into: where a draw kept them there, its list would not
// close. each patch covers the whole target at factor 1 and the pixel shader adds 1 (blend one + one) from values the
// last control point carries, so every pixel reads the number of patches: drawn directly and indexed
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct Point { float4 at : PIXEL; float4 extra[7] : EXTRA; };
Point vs(uint id : SV_VertexID) {
  uint corner = id % 4;
  Point p;
  p.at = float4(corner % 2, corner / 2, 0, 0);
  [unroll] for (uint k = 0; k < 7; k++)
    p.extra[k] = k + 1;
  return p;
}
struct Factors { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
Factors factors() {
  Factors f;
  f.edges[0] = f.edges[1] = f.edges[2] = f.edges[3] = f.inside[0] = f.inside[1] = 1;
  return f;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(32)]
[patchconstantfunc("factors")] Point hs(InputPatch<Point, 32> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}
struct Vertex { float4 position : SV_Position; float value : VALUE; };
[domain("quad")] Vertex ds(Factors f, float2 uv : SV_DomainLocation, const OutputPatch<Point, 32> patch) {
  float2 at = lerp(lerp(patch[0].at.xy, patch[1].at.xy, uv.x), lerp(patch[2].at.xy, patch[3].at.xy, uv.x), uv.y);
  Vertex v;
  v.position = float4(at.x * 2 - 1, 1 - at.y * 2, 0.5, 1);
  float sum = 0;
  [unroll] for (uint k = 0; k < 7; k++)
    sum += patch[31].extra[k].x + patch[31].extra[k].w;
  v.value = sum / 56;
  return v;
}
float ps(Vertex v) : SV_Target { return v.value; }
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

  const UINT patches = 4096, instances = 80, size = 8, vertices = 32 * patches;
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
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
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
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
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertices * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!readback || !indices) {
    printf("failed: buffers not created\n");
    return 1;
  }
  UINT *index;
  CHECK(indices->Map(0, nullptr, (void **)&index));
  for (UINT i = 0; i < vertices; i++)
    index[i] = i;
  indices->Unmap(0, nullptr);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (int indexed = 0; indexed < 2; indexed++) {
    step("drawn %s", indexed ? "indexed" : "directly");
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
    D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (indexed) {
      D3D12_INDEX_BUFFER_VIEW view{indices->GetGPUVirtualAddress(), vertices * UINT(sizeof(UINT)), DXGI_FORMAT_R32_UINT};
      list->IASetIndexBuffer(&view);
      list->DrawIndexedInstanced(vertices, instances, 0, 0, 0);
    } else {
      list->DrawInstanced(vertices, instances, 0, 0);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (!expect(SUCCEEDED(hr), "the list ran: 0x%08x", unsigned(hr)))
      continue;
    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT right = 0, first = UINT_MAX;
    float seen = 0;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        float value = ((const float *)(got + y * footprint.Footprint.RowPitch))[x];
        if (value == float(patches * instances))
          right++;
        else if (first == UINT_MAX)
          first = y * size + x, seen = value;
      }
    readback->Unmap(0, nullptr);
    expect(right == size * size, "%u of %u pixels read %u; pixel (%u, %u) reads %g", right, size * size,
           patches * instances, first % size, first / size, seen);
  }
  return verdict();
}
