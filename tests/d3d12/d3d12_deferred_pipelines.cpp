// contract: a pipeline draws as soon as it is created, whatever is still compiling. A D3D12 pipeline is ready once
// CreateGraphicsPipelineState returns (ID3D12Device::CreateGraphicsPipelineState); Mullion leaves Metal's compile of
// it to compile threads (DeferredPipeline), and a draw of it waits for that compile or does it itself.
// - pipelines created on several threads at once, each drawn at once in one command list, draw what they write;
// - pipelines released before their compile has run take nothing down with them, and later pipelines still draw;
// - a pipeline created again from the same description, while the first is still compiling, draws too;
// - a tessellated pipeline and a mesh shader pipeline (DXIL), whose Metal pipelines are left to those threads as
//   well, draw as soon as they are created, and ones released at once take nothing down.
// the target is one row with a column per pipeline: pipeline n writes its own value into column n.
#include "d3d12_test.hpp"
#include <algorithm>
#include <thread>

#define COVER \
  R"hlsl(
cbuffer C : register(b0) { uint column; };
float4 cover(uint id : SV_VertexID) : SV_Position {
  // a triangle over the column's pixel, which is the middle of its pixel at this column
  float2 uv = float2((id << 1) & 2, id & 2);
  float x = (column + 0.5) * 2 / WIDTH - 1;
  return float4(x + (uv.x - 0.5) * 4 / WIDTH, uv.y * -2 + 1, 0, 1);
}
)hlsl"

static const char hlsl[] = COVER R"hlsl(
uint ps() : SV_Target { return VALUE; }
// the same triangle as a patch of three control points, tessellated at factor 1 into itself
struct Point { float4 pos : POSITION; };
Point corner(uint id : SV_VertexID) {
  Point p;
  p.pos = cover(id);
  return p;
}
struct Factors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors() {
  Factors f;
  f.edges[0] = f.edges[1] = f.edges[2] = f.inside = 1;
  return f;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("factors")]
Point hs(InputPatch<Point, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }
[domain("tri")]
float4 ds(Factors f, float3 at : SV_DomainLocation, const OutputPatch<Point, 3> patch) : SV_Position {
  return patch[0].pos * at.x + patch[1].pos * at.y + patch[2].pos * at.z;
}
)hlsl";

// and as a mesh shader's one triangle
static const char mesh_hlsl[] = COVER R"hlsl(
struct Corner { float4 pos : SV_Position; };
[outputtopology("triangle")] [numthreads(1, 1, 1)]
void ms(out vertices Corner corners[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint i = 0; i < 3; i++)
    corners[i].pos = cover(i);
  triangles[0] = uint3(0, 1, 2);
}
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

int
main(int argc, char **argv) {
  // no compile threads (DeferredPipeline): each pipeline is made by its draw, or never when it is released first,
  // whatever the threads would have finished by the time the list is recorded
  SetEnvironmentVariableA("MULLION_D3D12_DEFER_PIPELINES", "draw");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  const UINT drawn = 48, released = 48, threads = 4;
  // after the plain pipelines' columns and the last one's: the tessellated pipeline's and the mesh shader pipeline's
  const UINT tessellated_column = drawn + 1, mesh_column = drawn + 2, width = drawn + 3;
  const UINT tessellated_value = 2000, mesh_value = 3000;
  const std::vector<std::string> width_define = {"WIDTH=" + std::to_string(width), "VALUE=0"};
  auto cover = compiler.compile(hlsl, "cover", "vs", width_define);
  // each pipeline a pixel shader of its own, so that each is a Metal pipeline of its own to compile
  std::vector<std::string> pixel_shaders(drawn + released);
  for (UINT i = 0; i < drawn + released; i++)
    pixel_shaders[i] =
        compiler.compile(hlsl, "ps", "ps", {"WIDTH=" + std::to_string(width), "VALUE=" + std::to_string(1000 + i)});
  auto value_defines = [&](UINT value) {
    return std::vector<std::string>{"WIDTH=" + std::to_string(width), "VALUE=" + std::to_string(value)};
  };
  auto corner = compiler.compile(hlsl, "corner", "vs", width_define), hs = compiler.compile(hlsl, "hs", "hs", width_define),
       ds = compiler.compile(hlsl, "ds", "ds", width_define);
  // the ones drawn, then the ones released at once
  std::string tessellated_ps[2] = {compiler.compile(hlsl, "ps", "ps", value_defines(tessellated_value)),
                                   compiler.compile(hlsl, "ps", "ps", value_defines(tessellated_value + 1))};
  std::string mesh_ps[2] = {compiler.compile(hlsl, "ps", "ps", value_defines(mesh_value)),
                            compiler.compile(hlsl, "ps", "ps", value_defines(mesh_value + 1))};
  if (cover.empty() || std::any_of(pixel_shaders.begin(), pixel_shaders.end(), [](auto &s) { return s.empty(); }) ||
      corner.empty() || hs.empty() || ds.empty() || tessellated_ps[0].empty() || tessellated_ps[1].empty() ||
      mesh_ps[0].empty() || mesh_ps[1].empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // mesh shaders are Shader Model 6.5's, which only the DXIL front end compiles
  std::string ms;
  if (compiler.dxc && (ms = compiler.compile(mesh_hlsl, "ms", "ms_6_5", width_define)).empty()) {
    printf("failed: the mesh shader did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 1;
  auto rs = root_signature(device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(cover);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};

  // created on several threads at once; the ones to release are released straight away
  std::vector<ComPtr<ID3D12PipelineState>> pipelines(drawn);
  std::vector<HRESULT> results(drawn + released, S_OK);
  std::vector<std::thread> creators;
  for (UINT t = 0; t < threads; t++)
    creators.emplace_back([&, t] {
      for (UINT i = t; i < drawn + released; i += threads) {
        auto own = desc;
        own.PS = bytecode(pixel_shaders[i]);
        ComPtr<ID3D12PipelineState> pipeline;
        results[i] = device->CreateGraphicsPipelineState(&own, IID_PPV_ARGS(&pipeline));
        if (i < drawn)
          pipelines[i] = pipeline;
      }
    });
  for (auto &creator : creators)
    creator.join();
  // the same description as pipeline 0's, created again while it may still be compiling, draws in the last column
  ComPtr<ID3D12PipelineState> again;
  {
    auto own = desc;
    own.PS = bytecode(pixel_shaders[0]);
    CHECK(device->CreateGraphicsPipelineState(&own, IID_PPV_ARGS(&again)));
  }
  unsigned failures = 0;
  for (UINT i = 0; i < drawn + released; i++)
    if (FAILED(results[i]) && failures++ < 12)
      printf("pipeline %u: created with %#lx\n", i, results[i]);
  // a tessellated pipeline to draw, after one released as soon as it is created
  ComPtr<ID3D12PipelineState> tessellated;
  for (UINT i : {1u, 0u}) {
    auto own = desc;
    own.VS = bytecode(corner);
    own.HS = bytecode(hs);
    own.DS = bytecode(ds);
    own.PS = bytecode(tessellated_ps[i]);
    own.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    tessellated.Reset();
    CHECK(device->CreateGraphicsPipelineState(&own, IID_PPV_ARGS(&tessellated)));
  }
  // and a mesh shader pipeline, likewise
  ComPtr<ID3D12PipelineState> mesh;
  ComPtr<ID3D12Device2> device2;
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)));
  bool mesh_shaders = !ms.empty();
  if (mesh_shaders && options.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
    printf("failed: mesh shaders are not supported\n");
    return 1;
  }
  if (mesh_shaders) {
    CHECK(device.As(&device2));
    struct {
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
      Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    } stream;
    stream.rs.data = rs.Get();
    stream.ms.data = bytecode(ms);
    stream.raster.data = desc.RasterizerState;
    stream.targets.data.NumRenderTargets = 1;
    stream.targets.data.RTFormats[0] = DXGI_FORMAT_R32_UINT;
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    for (UINT i : {1u, 0u}) {
      stream.ps.data = bytecode(mesh_ps[i]);
      mesh.Reset();
      CHECK(device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&mesh)));
    }
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  width,
                                  1,
                                  1,
                                  1,
                                  DXGI_FORMAT_R32_UINT,
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT row;
  UINT64 size;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &row, nullptr, nullptr, &size);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)width, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT i = 0; i <= drawn; i++) {
    list->SetPipelineState(i < drawn ? pipelines[i].Get() : again.Get());
    list->SetGraphicsRoot32BitConstant(0, i, 0);
    // only its own column: the triangle covers its neighbours' pixels too
    D3D12_RECT column{(LONG)i, 0, LONG(i + 1), 1};
    list->RSSetScissorRects(1, &column);
    list->DrawInstanced(3, 1, 0, 0);
  }
  auto column = [&](UINT at) {
    list->SetGraphicsRoot32BitConstant(0, at, 0);
    D3D12_RECT own{(LONG)at, 0, LONG(at + 1), 1};
    list->RSSetScissorRects(1, &own);
  };
  list->SetPipelineState(tessellated.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
  column(tessellated_column);
  list->DrawInstanced(3, 1, 0, 0);
  if (mesh_shaders) {
    ComPtr<ID3D12GraphicsCommandList6> list6;
    CHECK(list.As(&list6));
    list->SetPipelineState(mesh.Get());
    column(mesh_column);
    list6->DispatchMesh(1, 1, 1);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = row;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i <= drawn; i++) {
    UINT want = i < drawn ? 1000 + i : 1000;
    if (got[i] != want && failures++ < 12)
      printf("column %u is %u, want %u\n", i, got[i], want);
  }
  // without mesh shaders their column keeps the clear's zero
  const std::pair<UINT, UINT> others[] = {{tessellated_column, tessellated_value},
                                          {mesh_column, mesh_shaders ? mesh_value : 0}};
  for (auto [at, want] : others)
    if (got[at] != want && failures++ < 12)
      printf("column %u is %u, want %u\n", at, got[at], want);
  readback->Unmap(0, nullptr);
  printf(
      "%s: %u wrong over %u pipelines drawn as created on %u threads, %u released at once, a tessellated one%s\n",
      failures ? "failed" : "passed", failures, drawn + 1, threads, released, mesh_shaders ? " and a mesh shader's" : ""
  );
  return failures != 0;
}
