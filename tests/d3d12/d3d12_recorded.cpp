// contract: with the GPU error diagnostic set, bytes recorded for GPU reads retain their encoded values until
// completion; GPU-written bytes are excluded, replay gets a fresh baseline, and queue release reports comparisons.
// "It's undefined behavior to call Reset on a command allocator while it has a command list still being executed."
// (ID3D12CommandAllocator::Reset, Remarks, Microsoft Learn). This test resets only after a fence completes.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandallocator-reset
// "For the purpose of rasterization, a point is represented as a square of width 1 oriented to the RenderTarget."
// (D3D11.3 3.4.6). Points lie at pixel centers, so each query returns one sample.
// with ONE source and destination factors and ADD (D3D11.3 17.1), each point adds its value in its column.
// a count buffer limits indirect work; predication skips draws whose predicate equals its operation.
#include "d3d12_rays.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

static const UINT commands = [] {
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  return UINT(system.dwAllocationGranularity / sizeof(D3D12_DRAW_ARGUMENTS) + 1);
}();
static const UINT counted = commands - 1, queries = commands + 1;
static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; };
RWStructuredBuffer<uint> result : register(u0);
struct V { float4 pos : SV_Position; float amount : VALUE; };
V vs(float column : COLUMN) {
  V v; v.pos = float4((column + 0.5) * 2 / COMMANDS - 1, 0, 0, 1); v.amount = value; return v;
}
float ps(V v) : SV_Target { return v.amount; }
[maxvertexcount(1)] void gs(point V v[1], inout PointStream<V> stream) { stream.Append(v[0]); }
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { result[id.x] = value + id.x; }
struct CP { float4 pos : POS; };
CP patch_vs(uint id : SV_VertexID) { CP v; v.pos = float4((id & 1) * 2.0 - 1, (id >> 1) * 2.0 - 1, 0, 1); return v; }
struct PC { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };
PC pc() { PC v; v.edges[0] = v.edges[1] = v.edges[2] = v.edges[3] = 1; v.inside[0] = v.inside[1] = 1; return v; }
[domain("quad")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(4)]
[patchconstantfunc("pc")]
CP hs(InputPatch<CP, 4> v, uint id : SV_OutputControlPointID) { return v[id]; }
[domain("quad")]
V ds(PC constants, float2 uv : SV_DomainLocation, const OutputPatch<CP, 4> v) {
  V o; o.pos = lerp(lerp(v[0].pos, v[1].pos, uv.x), lerp(v[2].pos, v[3].pos, uv.x), uv.y); o.amount = value; return o;
}
)hlsl";

// DXR, DispatchRays: "Launch the threads of a ray generation shader." One thread writes the root constant.
// DXR, POSTBUILD_INFO_SERIALIZATION: "Space requirements for serializing an acceleration structure".
// https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
static const char ray_hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; };
RWStructuredBuffer<uint> result : register(u0);
[shader("raygeneration")] void raygen() { result[0] = value; }
)hlsl";

static int
child(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const std::vector<std::string> defines = {"COMMANDS=" + std::to_string(commands)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines), gs = compiler.compile(hlsl, "gs", "gs", defines),
       pvs = compiler.compile(hlsl, "patch_vs", "vs", defines), hs = compiler.compile(hlsl, "hs", "hs", defines),
       ds = compiler.compile(hlsl, "ds", "ds", defines);
  if (vs.empty() || ps.empty() || cs.empty() || gs.empty() || pvs.empty() || hs.empty() || ds.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  auto library = compiler.dxc ? compiler.compile(ray_hlsl, "", "lib_6_3") : std::string();
  if (compiler.dxc && library.empty()) {
    printf("failed: ray HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList2> list;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  if (!strcmp(argv[3], "empty")) {
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    queue.Reset();
    return verdict();
  }
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  params[1].Descriptor = {0, 0};
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params, 1, &sampler,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
  if (!expect(bool(rs), "root signature was created"))
    return verdict();
  D3D12_INPUT_ELEMENT_DESC element{
      "COLUMN", 0, DXGI_FORMAT_R32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
      D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {&element, 1};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso, geometry, tessellation, output, compute;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  desc.GS = bytecode(gs);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&geometry)));
  const D3D12_SO_DECLARATION_ENTRY entry{0, "VALUE", 0, 0, 1, 0};
  const UINT so_stride = sizeof(float);
  desc.StreamOutput = {&entry, 1, &so_stride, 1, 0};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&output)));
  desc.StreamOutput = {};
  desc.GS = {};
  desc.VS = bytecode(pvs);
  desc.HS = bytecode(hs);
  desc.DS = bytecode(ds);
  desc.InputLayout = {};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&tessellation)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, commands, 1, 1, 1, DXGI_FORMAT_R32_FLOAT,
      {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
      nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12QueryHeap> occlusion;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&occlusion)));
  auto vb = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indirect = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(D3D12_DRAW_ARGUMENTS),
      D3D12_RESOURCE_STATE_GENERIC_READ);
  auto dispatch = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  auto count = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(UINT64), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto data = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, commands * sizeof(UINT),
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, so_stride, D3D12_RESOURCE_STATE_STREAM_OUT);
  auto filled = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  auto init = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(UINT64), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto results = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, queries * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST
  );
  auto read = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, commands * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto so_read = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, so_stride + sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST
  );
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(
      vb && indirect && dispatch && count && data && out && filled && init && results && read && so_read && pixels,
      "all buffers were created"))
    return verdict();
  void *mapped;
  CHECK(vb->Map(0, nullptr, &mapped));
  for (UINT i = 0; i < commands; i++)
    static_cast<float *>(mapped)[i] = i;
  vb->Unmap(0, nullptr);
  CHECK(indirect->Map(0, nullptr, &mapped));
  for (UINT i = 0; i < commands; i++)
    static_cast<D3D12_DRAW_ARGUMENTS *>(mapped)[i] = {1, 1, i, 0};
  indirect->Unmap(0, nullptr);
  CHECK(dispatch->Map(0, nullptr, &mapped));
  *static_cast<D3D12_DISPATCH_ARGUMENTS *>(mapped) = {commands, 1, 1};
  dispatch->Unmap(0, nullptr);
  CHECK(count->Map(0, nullptr, &mapped));
  *static_cast<UINT64 *>(mapped) = counted;
  count->Unmap(0, nullptr);
  CHECK(init->Map(0, nullptr, &mapped));
  *static_cast<UINT64 *>(mapped) = 0;
  init->Unmap(0, nullptr);
  ComPtr<ID3D12CommandSignature> draw_sig, dispatch_sig, changing_sig;
  D3D12_INDIRECT_ARGUMENT_DESC draw_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
      dispatch_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw_arg};
  CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw_sig)));
  signature = {sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &dispatch_arg};
  CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch_sig)));
  D3D12_INDIRECT_ARGUMENT_DESC changing[] = {{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT},
      {D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW}, draw_arg};
  changing[1].VertexBuffer.Slot = 0;
  changing[0].Constant = {0, 0, 1};
  signature = {
      sizeof(UINT) + sizeof(D3D12_VERTEX_BUFFER_VIEW) + sizeof(D3D12_DRAW_ARGUMENTS),
      UINT(std::size(changing)), changing
  };
  CHECK(device->CreateCommandSignature(&signature, rs.Get(), IID_PPV_ARGS(&changing_sig)));
  auto changed = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * signature.ByteStride, D3D12_RESOURCE_STATE_GENERIC_READ
  );
  if (!expect(bool(changed), "changing argument buffer was created"))
    return verdict();
  CHECK(changed->Map(0, nullptr, &mapped));
  for (UINT i = 0; i < commands; i++) {
    auto at = static_cast<char *>(mapped) + i * signature.ByteStride;
    const UINT value = 2;
    const D3D12_VERTEX_BUFFER_VIEW replacement{vb->GetGPUVirtualAddress() + i * sizeof(float),
        UINT((commands - i) * sizeof(float)), sizeof(float)};
    const D3D12_DRAW_ARGUMENTS draw{1, 1, 0, 0};
    memcpy(at, &value, sizeof(value));
    memcpy(at + sizeof(value), &replacement, sizeof(replacement));
    memcpy(at + sizeof(value) + sizeof(replacement), &draw, sizeof(draw));
  }
  changed->Unmap(0, nullptr);
  const D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), UINT(commands * sizeof(float)), sizeof(float)};
  const D3D12_VIEWPORT viewport{0, 0, float(commands), 1, 0, 1};
  const D3D12_RECT rect{0, 0, LONG(commands), 1};
  const float black[4] = {};
  for (UINT recording = 0; recording < 2; recording++) {
    step("recording %u, replay twice, %u indirect commands and %u queries", recording, commands, queries);
    if (recording) {
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
    }
    list->CopyBufferRegion(filled.Get(), 0, init.Get(), 0, sizeof(UINT64));
    transition(list.Get(), filled.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRoot32BitConstant(0, 1, 0);
    list->SetGraphicsRootUnorderedAccessView(1, data->GetGPUVirtualAddress());
    list->SetPipelineState(pso.Get());
    list->IASetVertexBuffers(0, 1, &view);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &rect);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    for (UINT q = 0; q < queries; q++) {
      list->BeginQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, q);
      list->DrawInstanced(1, 1, q % commands, 0);
      list->EndQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, q);
    }
    list->DrawIndexedInstanced(1, 1, 0, 0, 0);
    list->ExecuteIndirect(draw_sig.Get(), commands, indirect.Get(), 0, nullptr, 0);
    list->ExecuteIndirect(draw_sig.Get(), commands, indirect.Get(), 0, count.Get(), 0);
    list->ExecuteIndirect(changing_sig.Get(), commands, changed.Get(), 0, nullptr, 0);
    list->SetGraphicsRoot32BitConstant(0, 1, 0);
    list->IASetVertexBuffers(0, 1, &view);
    for (auto op : {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}) {
      list->SetPredication(count.Get(), 0, op);
      list->DrawInstanced(1, 1, 0, 0);
      list->ExecuteIndirect(draw_sig.Get(), 1, indirect.Get(), 0, nullptr, 0);
      list->SetPipelineState(geometry.Get());
      list->DrawInstanced(1, 1, 0, 0);
      list->SetPipelineState(tessellation.Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
      list->DrawInstanced(4, 1, 0, 0);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      list->SetPipelineState(pso.Get());
      transition(list.Get(), filled.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyBufferRegion(filled.Get(), 0, init.Get(), 0, sizeof(UINT64));
      transition(list.Get(), filled.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      list->SetPredication(nullptr, 0, op);
    }
    list->SetPipelineState(geometry.Get());
    list->DrawInstanced(1, 1, 0, 0);
    list->SetPipelineState(output.Get());
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW so_view{
        out->GetGPUVirtualAddress(), so_stride, filled->GetGPUVirtualAddress()
    };
    list->SOSetTargets(0, 1, &so_view);
    list->DrawInstanced(1, 1, 0, 0);
    list->SetPipelineState(tessellation.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    list->DrawInstanced(4, 1, 0, 0);
    list->SetPipelineState(compute.Get());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRoot32BitConstant(0, 7, 0);
    list->SetComputeRootUnorderedAccessView(1, data->GetGPUVirtualAddress());
    list->ExecuteIndirect(dispatch_sig.Get(), 1, dispatch.Get(), 0, nullptr, 0);
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER immediate{data->GetGPUVirtualAddress(), 7};
    transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->WriteBufferImmediate(1, &immediate, nullptr);
    transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(read.Get(), 0, data.Get(), 0, commands * sizeof(UINT));
    transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, queries, results.Get(), 0);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(so_read.Get(), 0, out.Get(), 0, so_stride);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
    transition(list.Get(), filled.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(so_read.Get(), so_stride, filled.Get(), 0, sizeof(UINT64));
    transition(list.Get(), filled.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(list->Close());
    for (UINT replay = 0; replay < 2; replay++) {
      CHECK(execute(device.Get(), queue.Get(), list.Get()));
      const char *got;
      CHECK(pixels->Map(0, nullptr, (void **)&got));
      const auto row = reinterpret_cast<const float *>(got + footprint.Offset);
      for (UINT x = 0; x < commands; x++) {
        const float query_draws = 1 + (x == 0 ? 1 : 0);
        const float indirect_draws = 1 + (x < counted ? 1 : 0) + 2;
        const float fixed_draws = (x == 0 ? 1 + 1 + 1 : 0) + 1;
        const float predicated_draws = (x == 0 ? 1 + 1 + 1 : 0) + 1;
        const float want = query_draws + indirect_draws + fixed_draws + predicated_draws;
        expect(row[x] == want, "pixel %u = %g, want %g", x, row[x], want);
      }
      pixels->Unmap(0, nullptr);
      CHECK(read->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < commands; i++)
        expect(reinterpret_cast<const UINT *>(got)[i] == 7 + i, "dispatch word %u differs", i);
      read->Unmap(0, nullptr);
      CHECK(results->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < queries; i++)
        expect(reinterpret_cast<const UINT64 *>(got)[i] == 1, "query %u differs", i);
      results->Unmap(0, nullptr);
      CHECK(so_read->Map(0, nullptr, (void **)&got));
      UINT64 size;
      memcpy(&size, got + so_stride, sizeof(size));
      expect(*reinterpret_cast<const float *>(got) == 1 && size == so_stride, "stream output differs");
      so_read->Unmap(0, nullptr);
    }
  }
  ComPtr<ID3D12Device5> ray_device;
  ComPtr<ID3D12GraphicsCommandList4> ray_list;
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
  if (options.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
    printf("skipped: deferred postbuild and predicated rays, ray tracing unavailable\n");
  } else {
    CHECK(device.As(&ray_device));
    CHECK(list.As(&ray_list));
    step("two submissions reuse the queue's deferred postbuild recording");
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    RayScene scene;
    CHECK(scene.create(ray_device.Get()));
    if (!expect(scene.triangle_memory && scene.box_memory && scene.scene_memory && scene.scratch,
        "postbuild scene buffers created"))
      return verdict();
    scene.build(ray_list.Get(), scene.triangle_inputs, scene.triangle_memory.Get());
    scene.build(ray_list.Get(), scene.box_inputs, scene.box_memory.Get());
    scene.build_scene(ray_list.Get(), scene.triangle_memory.Get(), scene.box_memory.Get());
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    const D3D12_GPU_VIRTUAL_ADDRESS sources[] = {
        scene.triangle_memory->GetGPUVirtualAddress(), scene.scene_memory->GetGPUVirtualAddress()
    };
    std::vector<const Structure *> referenced;
    for (auto &instance : scene.instances)
      if (instance.structure && std::find(referenced.begin(), referenced.end(), instance.structure) == referenced.end())
        referenced.push_back(instance.structure);
    using Information = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC;
    const UINT submissions = std::size(sources);
    auto information = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, submissions * sizeof(Information),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto info_read = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, submissions * sizeof(Information),
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(information && info_read, "postbuild buffers created"))
      return verdict();
    ComPtr<ID3D12CommandAllocator> post_allocators[submissions];
    ComPtr<ID3D12GraphicsCommandList4> post_lists[submissions];
    for (UINT i = 0; i < submissions; i++) {
      CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&post_allocators[i])));
      CHECK(device->CreateCommandList(0, queue_desc.Type, post_allocators[i].Get(), nullptr,
          IID_PPV_ARGS(&post_lists[i])));
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{
          information->GetGPUVirtualAddress() + i * sizeof(Information),
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION
      };
      post_lists[i]->EmitRaytracingAccelerationStructurePostbuildInfo(&info, 1, &sources[i]);
      CHECK(post_lists[i]->Close());
      ID3D12CommandList *submitted[] = {post_lists[i].Get()};
      queue->ExecuteCommandLists(UINT(std::size(submitted)), submitted);
    }
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    transition(list.Get(), information.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(info_read.Get(), 0, information.Get(), 0, submissions * sizeof(Information));
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(info_read->Map(0, nullptr, &mapped));
    for (UINT i = 0; i < submissions; i++) {
      auto &info = static_cast<Information *>(mapped)[i];
      expect(info.SerializedSizeInBytes >= sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER) +
          info.NumBottomLevelAccelerationStructurePointers * sizeof(D3D12_GPU_VIRTUAL_ADDRESS),
          "postbuild submission %u has no room for its header and pointers", i);
      expect(i ? info.NumBottomLevelAccelerationStructurePointers >= referenced.size()
               : info.NumBottomLevelAccelerationStructurePointers == 0, "postbuild submission %u pointers differ", i);
    }
    info_read->Unmap(0, nullptr);
    if (!compiler.dxc || options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
      printf("skipped: predicated indirect rays require DXIL and ray tracing tier 1.1\n");
    } else {
      char log_path[MAX_PATH];
      GetEnvironmentVariableA("DXMT_LOG_PATH", log_path, sizeof(log_path));
      std::ofstream(std::filesystem::path(log_path) / "rays.txt");
      step("predicated indirect rays register roots and static samplers after the resolver");
      D3D12_DXIL_LIBRARY_DESC dxil{bytecode(library)};
      D3D12_GLOBAL_ROOT_SIGNATURE global{rs.Get()};
      D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
      D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
      const D3D12_STATE_SUBOBJECT subobjects[] = {
          {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil},
          {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config}
      };
      D3D12_STATE_OBJECT_DESC state_desc{
          D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(std::size(subobjects)), subobjects
      };
      ComPtr<ID3D12StateObject> state;
      ComPtr<ID3D12StateObjectProperties> properties;
      CHECK(ray_device->CreateStateObject(&state_desc, IID_PPV_ARGS(&state)));
      CHECK(state.As(&properties));
      auto table = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
          D3D12_RESOURCE_STATE_GENERIC_READ);
      auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_RAYS_DESC),
          D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!expect(table && arguments, "ray buffers created"))
        return verdict();
      CHECK(table->Map(0, nullptr, &mapped));
      auto identifier = properties->GetShaderIdentifier(L"raygen");
      if (!expect(identifier != nullptr, "raygen identifier found"))
        return verdict();
      memcpy(mapped, identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
      table->Unmap(0, nullptr);
      D3D12_DISPATCH_RAYS_DESC rays{};
      rays.RayGenerationShaderRecord = {table->GetGPUVirtualAddress(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
      rays.Width = rays.Height = rays.Depth = 1;
      CHECK(arguments->Map(0, nullptr, &mapped));
      memcpy(mapped, &rays, sizeof(rays));
      arguments->Unmap(0, nullptr);
      D3D12_INDIRECT_ARGUMENT_DESC ray_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS};
      D3D12_COMMAND_SIGNATURE_DESC ray_desc{sizeof(rays), 1, &ray_arg};
      ComPtr<ID3D12CommandSignature> ray_sig;
      CHECK(device->CreateCommandSignature(&ray_desc, nullptr, IID_PPV_ARGS(&ray_sig)));
      for (auto op : {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}) {
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER initial{data->GetGPUVirtualAddress(), 0};
        transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->WriteBufferImmediate(1, &initial, nullptr);
        transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(rs.Get());
        list->SetComputeRoot32BitConstant(0, 7, 0);
        list->SetComputeRootUnorderedAccessView(1, data->GetGPUVirtualAddress());
        ray_list->SetPipelineState1(state.Get());
        list->SetPredication(count.Get(), 0, op);
        list->ExecuteIndirect(ray_sig.Get(), 1, arguments.Get(), 0, nullptr, 0);
        list->SetPredication(nullptr, 0, op);
        transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(read.Get(), 0, data.Get(), 0, sizeof(UINT));
        transition(list.Get(), data.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(read->Map(0, nullptr, &mapped));
        const UINT want = op == D3D12_PREDICATION_OP_EQUAL_ZERO ? 7 : 0;
        expect(*static_cast<UINT *>(mapped) == want, "predicated ray returned %u, want %u",
            *static_cast<UINT *>(mapped), want);
        read->Unmap(0, nullptr);
      }
    }
  }
  queue.Reset();
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 3)
    return child(argc, argv);
  char exe[MAX_PATH], temporary[MAX_PATH], directory[MAX_PATH];
  if (!expect(
      GetModuleFileNameA(nullptr, exe, sizeof(exe)) && GetTempPathA(sizeof(temporary), temporary), "paths found"
  ))
    return verdict();
  for (const char *mode : {"empty", "work"}) {
    for (const char *setting : {static_cast<const char *>(nullptr), "1", "2"}) {
      step("%s child, DXMT_D3D12_GPU_ERRORS=%s", mode, setting ? setting : "unset");
      if (!expect(GetTempFileNameA(temporary, "rec", 0, directory), "temporary name found"))
        return verdict();
      DeleteFileA(directory);
      if (!expect(CreateDirectoryA(directory, nullptr), "log directory created"))
        return verdict();
      SetEnvironmentVariableA("DXMT_LOG_PATH", directory);
      SetEnvironmentVariableA("DXMT_LOG_LEVEL", "info");
      SetEnvironmentVariableA("DXMT_D3D12_GPU_ERRORS", setting);
      SetEnvironmentVariableA("DXMT_D3D12_ISOLATE", nullptr);
      std::string command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child " + mode;
      STARTUPINFOA startup{sizeof(startup)};
      PROCESS_INFORMATION process{};
      if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
          "child started"))
        return verdict();
      const auto waited = WaitForSingleObject(process.hProcess, 60000);
      if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 60000);
      }
      DWORD status = 1;
      GetExitCodeProcess(process.hProcess, &status);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      if (status == 77)
        return 77;
      expect(waited == WAIT_OBJECT_0 && status == 0, "child exit %lu, wait %lu", status, waited);
      std::string logs;
      for (auto &entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".log") {
          std::ifstream file(entry.path());
          logs.append(std::istreambuf_iterator<char>(file), {});
        }
      }
      const std::string changed_prefix = "D3D12 recording changed: ", fault_prefix = "D3D12 recording test fault: ";
      std::vector<std::string> expected, reported;
      std::istringstream messages(logs);
      std::string message;
      while (std::getline(messages, message)) {
        if (auto at = message.find(fault_prefix); at != std::string::npos)
          expected.push_back(message.substr(at + fault_prefix.size()));
        if (auto at = message.find(changed_prefix); at != std::string::npos)
          reported.push_back(message.substr(at + changed_prefix.size()));
      }
      for (auto &wanted : expected) {
        auto got = std::find(reported.begin(), reported.end(), wanted);
        if (expect(got != reported.end(), "incorrect overwrite report: expected %s; got %s", wanted.c_str(),
            reported.empty() ? "no difference line" : reported.front().c_str())) {
          expect(false, "record overwrite detected: expected %s; got %s", wanted.c_str(), got->c_str());
          reported.erase(got);
        }
      }
      for (auto &got : reported)
        expect(false, "unexpected overwrite report: %s", got.c_str());
      if (!setting) {
        expect(logs.find("D3D12 recording ") == std::string::npos, "diagnostic logged with variable unset");
      } else {
        auto at = logs.find("D3D12 recording totals:");
        expect(at != std::string::npos, "no recording totals in child log");
        std::map<std::string, std::pair<UINT64, UINT64>> totals;
        std::istringstream lines(logs);
        std::string line;
        while (std::getline(lines, line)) {
          auto kind = line.find("D3D12 recording kind: ");
          if (kind == std::string::npos)
            continue;
          std::istringstream fields(line.substr(kind + strlen("D3D12 recording kind: ")));
          std::string name;
          std::getline(fields, name, ';');
          UINT64 compared = 0, changed = 0;
          std::string counted, differed;
          fields >> compared >> counted >> changed >> differed;
          expect(
              fields && counted == "compared;" && differed == "differed" && changed == 0, "bad totals: %s", line.c_str()
          );
          expect(totals.emplace(name, std::pair{compared, changed}).second, "duplicate kind: %s", line.c_str());
        }
        if (at != std::string::npos) {
          std::istringstream fields(logs.substr(at + strlen("D3D12 recording totals:")));
          UINT64 compared = 0, differed = 0, sum = 0, changes = 0;
          std::string counted, altered;
          fields >> compared >> counted >> differed >> altered;
          for (auto &[kind, values] : totals)
            sum += values.first, changes += values.second;
          expect(fields && counted == "compared;" && altered == "differed" && compared == sum && differed == changes,
              "summary does not match per-kind totals");
        }
        if (!strcmp(mode, "empty")) {
          expect(totals["vertex buffer table"].first == 0, "no draw recorded a vertex table");
        } else {
          expect(totals["indirect draw"].first == 2 * 2 * (3 + 2), "indirect draw total differs");
          const bool rays = std::filesystem::exists(std::filesystem::path(directory) / "rays.txt");
          expect(totals["indirect dispatch"].first == 2 * 2 + (rays ? 2 : 0), "indirect dispatch total differs");
          if (rays)
            expect(totals["instance count"].first > 0, "no comparisons of the top-level instance count");
          for (const char *kind : {"root argument table", "vertex buffer table", "static samplers", "predicate words",
               "predicate zero", "predicate header", "predicated arguments", "occlusion slots", "occlusion sums",
               "geometry inputs", "tessellation inputs",
               "stream output targets", "immediate data", "zero indices"})
            expect(totals[kind].first > 0, "no comparisons of %s", kind);
        }
      }
      std::filesystem::remove_all(directory);
    }
  }
  return verdict();
}
