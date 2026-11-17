// contract: a tessellated draw rasterizes each triangle its tessellator makes once, whatever the pipeline tests. a
// pipeline that tests the depth bounds over [0, 1], which every stored depth passes ("the depth value stored in the
// depth buffer ... is within the range", OMSetDepthBounds), and writes depth draws as one that does not. each quad
// patch covers its own 3x3 pixels at factor 3 (d3d12_patch_ids' patches) and the pixel shader adds 1 to an R32_FLOAT
// target (blend one + one), so every pixel reads 1: drawn directly and indexed, each in four lists, one after each
// Reset (the room a pass's compute tessellator takes is sized at a Reset, so a first list may have none)
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
  return float4(at.x / (3 * patches) * 2 - 1, 1 - at.y / (3 * instances) * 2, 0.5, 1);
}
float ps() : SV_Target { return 1; }
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

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
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2));
  if (!options2.DepthBoundsTestSupported) {
    printf("skipped: no depth bounds test\n");
    return 77;
  }
  ComPtr<ID3D12Device2> device2;
  CHECK(device.As(&device2));

  const UINT patches = 200, instances = 3, width = 3 * patches, height = 3 * instances, size[2] = {patches, instances};
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  auto pipeline = [&](bool bounds, ComPtr<ID3D12PipelineState> &out) {
    struct alignas(void *) {
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE> hs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE> ds;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    } stream;
    stream.rs.data = rs.Get();
    stream.vs.data = bytecode(vs);
    stream.hs.data = bytecode(hs);
    stream.ds.data = bytecode(ds);
    stream.ps.data = bytecode(ps);
    auto &blend = stream.blend.data.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.sample_mask.data = UINT_MAX;
    stream.raster.data.FillMode = D3D12_FILL_MODE_SOLID;
    stream.raster.data.CullMode = D3D12_CULL_MODE_NONE;
    stream.raster.data.DepthClipEnable = TRUE;
    auto &depth = stream.depth.data;
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.DepthBoundsTestEnable = bounds;
    stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    stream.targets.data.NumRenderTargets = 1;
    stream.targets.data.RTFormats[0] = DXGI_FORMAT_R32_FLOAT;
    stream.depth_format.data = DXGI_FORMAT_D32_FLOAT;
    stream.samples.data = {1, 0};
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    return device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&out));
  };
  ComPtr<ID3D12PipelineState> psos[2];
  CHECK(pipeline(false, psos[0]));
  CHECK(pipeline(true, psos[1]));

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  auto depth_desc = target_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT};
  clear.DepthStencil.Depth = 1;
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)
  ));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 readback_size;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
  const UINT vertices = 4 * patches;
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
  ComPtr<ID3D12GraphicsCommandList1> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (int bounds = 0; bounds < 2; bounds++)
    for (int indexed = 0; indexed < 2; indexed++)
      for (int round = 0; round < 4; round++) {
        step("%s, drawn %s, list %d", bounds ? "the depth bounds tested" : "no depth bounds test",
             indexed ? "indexed" : "directly", round);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), psos[bounds].Get()));
        const float zero[4] = {};
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetGraphicsRoot32BitConstants(0, 2, size, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
        D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->OMSetDepthBounds(0, 1);
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
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        BYTE *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        UINT once = 0, first = UINT_MAX;
        float seen = 0;
        for (UINT y = 0; y < height; y++)
          for (UINT x = 0; x < width; x++) {
            float value = ((const float *)(got + y * footprint.Footprint.RowPitch))[x];
            if (value == 1)
              once++;
            else if (first == UINT_MAX)
              first = y * width + x, seen = value;
          }
        readback->Unmap(0, nullptr);
        expect(once == width * height, "%u of %u pixels drawn once; pixel (%u, %u) reads %g", once, width * height,
               first % width, first / width, seen);
      }
  return verdict();
}
