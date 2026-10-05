// contract: the depth bounds test (D3D12 Depth Bounds Test spec, and its conformance tests). a pixel passes when the
// depth buffer's stored value lies inside [min, max], inclusively; a pixel shader forced early writes neither its
// target nor its UAV where it fails, and occlusion queries do not count it. the range may come from a bundle, a wide
// range passes all, an inverted one none, NaN counts as 0, and a pipeline without the test or a pass without a depth
// buffer passes all. each case draws into its own band of pixels whose stored depths are `stored`.
#include "d3d12_test.hpp"
#include <cfloat>
#include <cmath>

// a pipeline stream entry: its type, then its data, at pointer alignment
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Data> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Data data;
};

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float depth; uint value; };
RWStructuredBuffer<uint> o : register(u1); // pixel shader UAVs follow the targets
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}
[earlydepthstencil] uint ps(float4 p : SV_Position) : SV_Target {
  o[uint(p.x)] = value;
  return value;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)));
  if (!options.DepthBoundsTestSupported) {
    printf("skipped: no depth bounds test\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  const float stored[] = {0.0f, 0.3f, 0.5f, 0.6f, 1.0f};
  const UINT band = std::size(stored);
  struct Case {
    float min, max;
    bool test, depth_buffer, bundle;
  } cases[] = {
      {0.3f, 0.6f, true, true, false},     {0.3f, 0.6f, true, true, true},  {-FLT_MAX, FLT_MAX, true, true, false},
      {FLT_MAX, -FLT_MAX, true, true, false}, {0.3f, 0.6f, false, true, false}, {0.3f, 0.6f, true, false, false},
      {NAN, 0.5f, true, true, false},
  };
  const UINT width = band * std::size(cases);

  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants.Num32BitValues = 2;
  params[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {2, params});
  // pipelines: one writes depth everywhere; the others test it, with the depth bounds test or without, or without a
  // depth buffer
  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
  } stream{};
  stream.rs.data = rs.Get(), stream.vs.data = bytecode(vs), stream.ps.data = bytecode(ps);
  stream.targets.data = {{DXGI_FORMAT_R32_UINT}, 1};
  stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  stream.rasterizer.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.rasterizer.data.DepthClipEnable = TRUE;
  auto pipeline = [&](bool write, bool test, bool depth_buffer) -> ComPtr<ID3D12PipelineState> {
    stream.depth.data = {depth_buffer, write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS};
    stream.depth.data.DepthBoundsTestEnable = test;
    stream.depth_format.data = depth_buffer ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pso;
    return SUCCEEDED(device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso))) ? pso : nullptr;
  };
  auto fill = pipeline(true, false, true);
  if (!fill) {
    printf("failed: pipeline\n");
    return 1;
  }

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto depth_desc = target_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT, depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, width * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // read back: the target's row, the UAV, then the occlusion counts
  const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, row = (width * 4 + pitch - 1) / pitch * pitch;
  const UINT64 counts_at = row + (width * 4 + 7) / 8 * 8;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, counts_at + std::size(cases) * 8, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, (UINT)std::size(cases)};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), fill.Get(), IID_PPV_ARGS(&list)));
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  list->RSSetViewports(1, &viewport);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  auto columns = [&](UINT first, UINT count) {
    D3D12_RECT rect{(LONG)first, 0, LONG(first + count), 1};
    list->RSSetScissorRects(1, &rect);
  };
  auto draw = [&](float z, UINT value) {
    const UINT constants[2] = {*reinterpret_cast<const UINT *>(&z), value};
    list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);
  };
  // stored depths, then the UAV's zeros (the fill draws wrote it)
  for (UINT x = 0; x < width; x++)
    columns(x, 1), draw(stored[x % band], 0);
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  uav.UAV.pResource = out.Get();
  list->ResourceBarrier(1, &uav);
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);

  // what the list uses lives until it runs
  std::vector<ComPtr<ID3D12PipelineState>> psos;
  std::vector<ComPtr<ID3D12GraphicsCommandList1>> bundles;
  for (UINT c = 0; c < std::size(cases); c++) {
    auto &k = cases[c];
    auto &pso = psos.emplace_back(pipeline(false, k.test, k.depth_buffer));
    if (!pso) {
      printf("failed: pipeline of case %u\n", c);
      return 1;
    }
    list->SetPipelineState(pso.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, k.depth_buffer ? &dsv : nullptr);
    columns(c * band, band);
    if (k.bundle) {
      auto &bundle = bundles.emplace_back();
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)));
      bundle->OMSetDepthBounds(k.min, k.max);
      CHECK(bundle->Close());
      list->ExecuteBundle(bundle.Get());
    } else {
      list->OMSetDepthBounds(k.min, k.max);
    }
    list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, c);
    draw(0, c + 1);
    list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, c);
  }
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, std::size(cases), readback.Get(), counts_at);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, width, 1, 1, row}};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  list->CopyBufferRegion(readback.Get(), row, out.Get(), 0, width * 4);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  unsigned failures = 0;
  for (UINT c = 0; c < std::size(cases); c++) {
    auto &k = cases[c];
    float lo = std::isnan(k.min) ? 0 : k.min, hi = std::isnan(k.max) ? 0 : k.max;
    UINT passed = 0;
    for (UINT i = 0; i < band; i++) {
      bool pass = !k.test || !k.depth_buffer || (lo <= stored[i] && stored[i] <= hi);
      UINT x = c * band + i, want = pass ? c + 1 : 0;
      passed += pass;
      UINT written = got[row / 4 + x];
      if ((got[x] != want || written != want) && failures++ < 12)
        printf("case %u, stored %g: target %u, UAV %u, want %u\n", c, stored[i], got[x], written, want);
    }
    UINT64 count = reinterpret_cast<UINT64 *>(reinterpret_cast<char *>(got) + counts_at)[c];
    if (count != passed && failures++ < 12)
      printf("case %u: occlusion %llu, want %u\n", c, count, passed);
  }
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong values over %zu cases\n", failures ? "failed" : "passed", failures, std::size(cases));
  return failures != 0;
}
