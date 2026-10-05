// contract: a tessellated draw runs the vertex shader per control point, the hull shader's control-point function per
// output point and its patch-constant function once per patch (reading the patch's output points), the fixed
// tessellator at the factors that function writes, and the domain shader per domain point with the patch's points,
// patch constants and domain location. a quad patch covers the target at integer factor N from a root constant; the
// domain shader outputs (u^2, v^2), which the rasterizer interpolates linearly inside each of the N x N cells, so each
// pixel holds u (u0 + u1) - u0 u1 for the cell edges u0, u1 around it, not the u^2 or u of an untessellated patch.
// indexed draws fetch the control points in reverse, which turns the patch half a turn; an index past the view reads 0
// (D3D12), so a view one index short of 3, 2, 1, 5 draws as 3, 2, 1, 0. the same draws run through ExecuteIndirect,
// one command also carrying the factor and the index buffer view, at a 4-byte offset. a predicated draw is skipped
// when its predicate says so, leaving the clear color, and draws when it does not. a domain shader with cull
// distances puts the first cell boundary's points and those left of it behind one, and the last boundary's and
// those below it behind another, as NaN: the cells with all their points behind are discarded, their neighbours,
// with some, drawn whole (D3D11.3 15.4.2).
#include "d3d12_test.hpp"
#include <cmath>
#include <functional>
#include <optional>

static const char hlsl[] = R"hlsl(
cbuffer c : register(b0) { float factor; };
struct CP { float2 corner : CORNER; };
CP vs(uint id : SV_VertexID) {
  CP o;
  o.corner = float2(id & 1, id >> 1);
  return o;
}
struct HCP { float4 pos : POS; };
struct PC {
  float edges[4] : SV_TessFactor;
  float inside[2] : SV_InsideTessFactor;
  float scale : SCALE;
};
PC pc(OutputPatch<HCP, 4> p) {
  PC o;
  [unroll] for (int i = 0; i < 4; i++)
    o.edges[i] = factor;
  o.inside[0] = o.inside[1] = factor;
  // the patch spans clip space: 1
  o.scale = abs(p[1].pos.x - p[0].pos.x) / 2;
  return o;
}
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(4)]
[patchconstantfunc("pc")]
HCP hs(InputPatch<CP, 4> ip, uint i : SV_OutputControlPointID) {
  HCP o;
  o.pos = float4(ip[i].corner * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
struct DSO { float4 pos : SV_Position; float2 f : F; };
[domain("quad")]
DSO ds(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<HCP, 4> p) {
  DSO o;
  o.pos = lerp(lerp(p[0].pos, p[1].pos, uv.x), lerp(p[2].pos, p[3].pos, uv.x), uv.y);
  o.f = uv * uv * pc.scale;
  return o;
}
struct DSC { float4 pos : SV_Position; float2 f : F; float2 cull : SV_CullDistance; };
[domain("quad")]
DSC ds_cull(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<HCP, 4> p) {
  DSO plain = ds(pc, uv, p);
  DSC o;
  o.pos = plain.pos;
  o.f = plain.f;
  // a point's cell boundary along each axis
  float2 boundary = uv * factor;
  o.cull = float2(boundary.x < 1.5 ? -1 : 1, boundary.y > factor - 1.5 ? asfloat(0x7fc00000) : 0);
  return o;
}
float4 ps(DSO i) : SV_Target { return float4(i.f, 0, 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), hs = compiler.compile(hlsl, "hs", "hs"),
       ds = compiler.compile(hlsl, "ds", "ds"), ds_cull = compiler.compile(hlsl, "ds_cull", "ds"),
       ps = compiler.compile(hlsl, "ps", "ps");
  for (auto *code : {&vs, &hs, &ds, &ds_cull, &ps})
    if (code->empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }

  const UINT size = 32, factor = 4;
  const float tolerance = 1e-3f;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 1;
  auto rs = root_signature(device.Get(), {1, &param});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32_FLOAT;
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
  ComPtr<ID3D12PipelineState> pso, cull_pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  desc.DS = bytecode(ds_cull);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&cull_pso)));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // the four control points reversed, in both index formats, then reversed with a last index past its view
  const uint16_t indices16[] = {3, 2, 1, 0}, past16[] = {3, 2, 1, 5};
  const uint32_t indices32[] = {3, 2, 1, 0};
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 64, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *mapped;
  CHECK(indices->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, indices16, sizeof(indices16));
  memcpy(mapped + 8, indices32, sizeof(indices32));
  memcpy(mapped + 24, past16, sizeof(past16));
  indices->Unmap(0, nullptr);
  auto ib = indices->GetGPUVirtualAddress();
  const D3D12_INDEX_BUFFER_VIEW view16{ib, sizeof(indices16), DXGI_FORMAT_R16_UINT},
      view32{ib + 8, sizeof(indices32), DXGI_FORMAT_R32_UINT}, view_past{ib + 24, 3 * 2, DXGI_FORMAT_R16_UINT};

  // indirect commands: a draw, an indexed draw, and an indexed draw carrying the factor and its index buffer view
  struct __attribute__((packed)) Carried {
    float factor;
    D3D12_INDEX_BUFFER_VIEW view;
    D3D12_DRAW_INDEXED_ARGUMENTS draw;
  };
  const D3D12_DRAW_ARGUMENTS draw_args{4, 1, 0, 0};
  const D3D12_DRAW_INDEXED_ARGUMENTS indexed_args{4, 1, 0, 0, 0};
  const Carried carried{(float)factor, view16, indexed_args};
  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  CHECK(args->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, &draw_args, sizeof(draw_args));
  memcpy(mapped + 64, &indexed_args, sizeof(indexed_args));
  memcpy(mapped + 128, &carried, sizeof(carried));
  args->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC draw_desc{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
      indexed_desc{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED}, carried_desc[3] = {
                                                                  {D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT},
                                                                  {D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW},
                                                                  {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED},
                                                              };
  carried_desc[0].Constant = {0, 0, 1};
  const D3D12_COMMAND_SIGNATURE_DESC sig_descs[] = {
      {sizeof(draw_args), 1, &draw_desc}, {sizeof(indexed_args), 1, &indexed_desc}, {sizeof(carried), 3, carried_desc}
  };
  ComPtr<ID3D12CommandSignature> sigs[3];
  for (int i = 0; i < 3; i++)
    CHECK(device->CreateCommandSignature(&sig_descs[i], i == 2 ? rs.Get() : nullptr, IID_PPV_ARGS(&sigs[i])));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // the interpolated u^2 at coordinate t of an axis cut into `factor` cells
  auto expected = [&](float t) {
    float u0 = std::floor(t * factor) / factor, u1 = u0 + 1.0f / factor;
    return t * (u0 + u1) - u0 * u1;
  };
  struct Case {
    const char *name;
    bool reversed;
    std::function<void(ID3D12GraphicsCommandList *)> draw;
    // a predicate on 64 bits of zeros: EQUAL_ZERO skips the draw
    std::optional<D3D12_PREDICATION_OP> predicate{};
    // through the domain shader with cull distances
    bool culls = false;
  };
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 8, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *zero_bytes;
  CHECK(zeros->Map(0, nullptr, &zero_bytes));
  memset(zero_bytes, 0, 8);
  const Case cases[] = {
      {"non-indexed", false, [&](auto list) { list->DrawInstanced(4, 1, 0, 0); }},
      {"16-bit indexed", true,
       [&](auto list) {
         list->IASetIndexBuffer(&view16);
         list->DrawIndexedInstanced(4, 1, 0, 0, 0);
       }},
      {"32-bit indexed", true,
       [&](auto list) {
         list->IASetIndexBuffer(&view32);
         list->DrawIndexedInstanced(4, 1, 0, 0, 0);
       }},
      {"index past the view", true,
       [&](auto list) {
         list->IASetIndexBuffer(&view_past);
         list->DrawIndexedInstanced(4, 1, 0, 0, 0);
       }},
      {"indirect", false, [&](auto list) { list->ExecuteIndirect(sigs[0].Get(), 1, args.Get(), 0, nullptr, 0); }},
      {"indirect indexed", true,
       [&](auto list) {
         list->IASetIndexBuffer(&view32);
         list->ExecuteIndirect(sigs[1].Get(), 1, args.Get(), 64, nullptr, 0);
       }},
      {"indirect carrying its view", true,
       [&](auto list) {
         float none = 0; // the command sets the factor
         list->SetGraphicsRoot32BitConstants(0, 1, &none, 0);
         list->ExecuteIndirect(sigs[2].Get(), 1, args.Get(), 128, nullptr, 0);
       }},
      {"predicated away", false, [&](auto list) { list->DrawInstanced(4, 1, 0, 0); }, D3D12_PREDICATION_OP_EQUAL_ZERO},
      {"indexed, predicated away", true,
       [&](auto list) {
         list->IASetIndexBuffer(&view16);
         list->DrawIndexedInstanced(4, 1, 0, 0, 0);
       },
       D3D12_PREDICATION_OP_EQUAL_ZERO},
      {"predicated, drawing", false, [&](auto list) { list->DrawInstanced(4, 1, 0, 0); }, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO},
      {"cull distances", false, [&](auto list) { list->DrawInstanced(4, 1, 0, 0); }, {}, true},
      {"cull distances, indexed", true,
       [&](auto list) {
         list->IASetIndexBuffer(&view16);
         list->DrawIndexedInstanced(4, 1, 0, 0, 0);
       },
       {}, true},
  };
  unsigned failures = 0;
  for (auto &c : cases) {
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), c.culls ? cull_pso.Get() : pso.Get()));
    const float clear[4] = {-1, -1, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    D3D12_RECT scissor{0, 0, size, size};
    list->SetGraphicsRootSignature(rs.Get());
    float root_factor = factor;
    list->SetGraphicsRoot32BitConstants(0, 1, &root_factor, 0);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    if (c.predicate)
      list->SetPredication(zeros.Get(), 0, *c.predicate);
    c.draw(list.Get());
    list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    char *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    unsigned mismatches = 0;
    float linear = 0; // how far the expectation is from an untessellated patch's, so the check can tell them apart
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto got = (const float *)(pixels + y * footprint.Footprint.RowPitch) + 2 * x;
        float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
        if (c.reversed)
          u = 1 - u, v = 1 - v;
        // the first column of cells and the last row are behind a cull distance
        bool behind = c.culls && (std::floor(u * factor) == 0 || std::floor(v * factor) == factor - 1);
        bool skipped = c.predicate == D3D12_PREDICATION_OP_EQUAL_ZERO || behind;
        float want[2] = {skipped ? clear[0] : expected(u), skipped ? clear[1] : expected(v)};
        linear = std::max(linear, std::abs(expected(u) - u * u));
        // written as a match, so a NaN pixel does not pass
        bool match = std::abs(got[0] - want[0]) <= tolerance && std::abs(got[1] - want[1]) <= tolerance;
        if (!match && mismatches++ < 4)
          printf("%s: pixel %u,%u is %f,%f, want %f,%f\n", c.name, x, y, got[0], got[1], want[0], want[1]);
      }
    readback->Unmap(0, nullptr);
    if (linear <= 10 * tolerance) {
      printf("failed: the expectation does not tell tessellated from untessellated\n");
      return 1;
    }
    printf("%s: %u of %u pixels differ\n", c.name, mismatches, size * size);
    failures += mismatches;
  }
  printf("%s\n", failures ? "failed" : "passed");
  return failures != 0;
}
