// contract: pipeline statistics count application draws and dispatches between BeginQuery and EndQuery, including
// overlapping ranges, and resolving a range writes one D3D12_QUERY_DATA_PIPELINE_STATISTICS per query.
// "only the difference between two independant statistic requests will provide meaningful information";
// "Number of individual Compute Shader threads invoked" (D3D11.3 20.4.7). A triangle list without indexing has
// "valid VSInvocations only 12" for four triangles (20.4.7): each submitted vertex runs once. Each quad has two
// triangles wholly within the clip volume: "1 primitive will be generated if the original triangle is not clipped
// at all" (20.4.7). Its pixels cover the target on every draw.
// sources: https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics
// clears and copies are implementation work, excluded by 20.1. A seed varies draw and dispatch sizes; replay changes
// the indirect dispatch and its input, so counts or output left by an earlier execution cannot stand in for new work.
// the argument and count buffers are overwritten after ExecuteIndirect, before EndQuery and resolve; that later
// work cannot change the number of threads already invoked.
// "CSInvocations must honor Compute Shader invocations from both Dispatch() and DispatchIndirect() APIs."
// (20.4.7). The two dispatches write separate regions; an empty dispatch writes neither and counts no threads.
// "If any of the Dispatch arguments are 0, while the command will be sent to the driver, the effect is that
// nothing happens." (D3D11.3 18.6.2). every combination of zero dimensions leaves the output and counts unchanged.
// resolves preserve the ended value in the heap, both when repeated and when recorded in another list.
// "when a NULL pixel shader is bound to the pipeline, PSInvocations does not increment" (20.4.7). a geometry shader
// that passes each triangle on, without a pixel shader, runs once per triangle and outputs one: "Number of times a
// geometry shader was invoked", "Number of primitives output by a geometry shader" (20.4.7).
// "The value of PSInvocations may include or exclude helper pixels for 2x2 stamps" (20.4.7). Each triangle
// can invoke at most all four lanes of every stamp touching the target, so two such stamp grids bound a quad.
// "OCCLUSION measures multi-samples (not pixels)"; "If the Pixel Shader outputs depth, then PSInvocations
// must increment as expected, even if the output depth fails" (20.4.7). none of these shaders requests sample
// frequency. a cut means "the strip defined up to the previous index is to be completed, and the next index
// is a new strip" (8.12); the expected count sums the authored strips, or one strip when cuts are disabled.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  const float2 corners[] = {
    float2(-1, 1), float2(1, 1), float2(-1, -1),
    float2(-1, -1), float2(1, 1), float2(1, -1)
  };
  return float4(corners[id % VERTICES], 0.5, 1);
}
uint ps() : SV_Target { return 1; }
uint ps_depth(out float depth : SV_Depth) : SV_Target { depth = 0.5; return 1; }
float4 vs_strip(uint id : SV_VertexID) : SV_Position {
  float2 corners[] = {float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1)};
  return float4(corners[id % 4], 0.5, 1);
}
struct Vertex { float4 position : SV_Position; };
[maxvertexcount(3)] void gs(triangle Vertex input[3], inout TriangleStream<Vertex> output) {
  for (uint i = 0; i < 3; i++)
    output.Append(input[i]);
}
StructuredBuffer<uint> values : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[numthreads(TX, TY, TZ)]
void cs(uint3 id : SV_DispatchThreadID) {
  output[id.x + WIDTH * (id.y + HEIGHT * id.z)] = values[0];
}
)hlsl";

int
main(int argc, char **argv) {
  const UINT seed = argc > 2 ? strtoul(argv[2], nullptr, 0) : 0;
  std::minstd_rand next(seed);
  const UINT width = 11, height = 7, replays = 3, vertices_per_triangle = 3, triangles_per_quad = 2,
             vertices_per_quad = vertices_per_triangle * triangles_per_quad;
  const std::array<UINT, 3> threads{2, 3, 2}, flat_threads{threads[0], threads[1], 1},
      groups{UINT(1 + next() % 3), UINT(1 + next() % 2), UINT(1 + next() % 2)};
  const UINT cs_width = threads[0] * (groups[0] + replays - 1), cs_height = threads[1] * groups[1];
  const UINT64 threads_per_group = UINT64(threads[0]) * threads[1] * threads[2],
               direct_invocations = UINT64(flat_threads[0]) * flat_threads[1] * flat_threads[2] *
                                    groups[0] * groups[1] * groups[2],
               plane_words = UINT64(cs_width) * cs_height * threads[2] * groups[2], output_words = 2 * plane_words;
  const UINT instances = 1 + next() % 3, first = 1 + next() % 3, nested = 1 + next() % 3, discarded = 1 + next() % 3,
             no_pixel = 1 + next() % 3, geometry = 1 + next() % 3;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::vector<std::string> defines{
      "TX=" + std::to_string(threads[0]), "TY=" + std::to_string(threads[1]),
      "WIDTH=" + std::to_string(cs_width), "HEIGHT=" + std::to_string(cs_height),
      "VERTICES=" + std::to_string(vertices_per_quad), "TZ=" + std::to_string(threads[2])};
  step("seed %u, %s: compile VS, PS and CS", seed, compiler.dxc ? "dxil" : "dxbc");
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines), depth_ps = compiler.compile(hlsl, "ps_depth", "ps", defines),
       strip_vs = compiler.compile(hlsl, "vs_strip", "vs", defines), gs = compiler.compile(hlsl, "gs", "gs", defines);
  defines.back() = "TZ=" + std::to_string(flat_threads[2]);
  auto cs_flat = compiler.compile(hlsl, "cs", "cs", defines);
  if (vs.empty() || ps.empty() || cs.empty() || cs_flat.empty() || depth_ps.empty() || strip_vs.empty() || gs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  enum { Empty, All, First, Nested, Reused, Compute, NoPixel, Geometry, Queries };
  step("seed %u: create a pipeline statistics heap", seed);
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, Queries};
  ComPtr<ID3D12QueryHeap> queries;
  const HRESULT created = device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
  if (!expect(created == S_OK && queries, "CreateQueryHeap returned %#lx", created))
    return verdict();
  const UINT64 result_stride = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
               result_bytes = (Queries + 2) * result_stride;
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto repeated = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto gpu_results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (1 + output_words) * sizeof(UINT),
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_words * sizeof(UINT),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto words =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_words * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(results && repeated && gpu_results && source && output && words, "buffers could not be made"))
    return verdict();

  auto graphics_rs = root_signature(device.Get(), {});
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC root_desc{UINT(std::size(parameters)), parameters};
  auto compute_rs = root_signature(device.Get(), root_desc);
  if (!expect(graphics_rs && compute_rs, "root signatures could not be made"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{graphics_rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.RasterizerState.DepthClipEnable = TRUE;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  graphics.SampleDesc = {1, 0};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{compute_rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> draw_pso, dispatch_pso, flat_dispatch_pso, no_pixel_pso, geometry_pso;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&draw_pso)));
  graphics.PS = {};
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&no_pixel_pso)));
  graphics.GS = bytecode(gs);
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&geometry_pso)));
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&dispatch_pso)));
  compute.CS = bytecode(cs_flat);
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&flat_dispatch_pso)));
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  const UINT64 argument_bytes = signature_desc.ByteStride + sizeof(UINT);
  auto argument_source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 2 * argument_bytes + sizeof(UINT64),
                                D3D12_RESOURCE_STATE_GENERIC_READ);
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, argument_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(arguments && argument_source, "indirect arguments could not be made"))
    return verdict();
  D3D12_DISPATCH_ARGUMENTS *dispatch_arguments;
  CHECK(argument_source->Map(0, nullptr, (void **)&dispatch_arguments));
  memset(dispatch_arguments, 0, 2 * argument_bytes + sizeof(UINT64));
  auto indirect_count =
      reinterpret_cast<UINT *>(reinterpret_cast<char *>(dispatch_arguments) + signature_desc.ByteStride);
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                              0,
                              width,
                              height,
                              1,
                              1,
                              DXGI_FORMAT_R32_UINT,
                              {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  struct IndexedArguments {
    D3D12_INDEX_BUFFER_VIEW view;
    D3D12_DRAW_INDEXED_ARGUMENTS draw;
  };
  D3D12_INDIRECT_ARGUMENT_DESC indexed_desc[2]{};
  indexed_desc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
  indexed_desc[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  D3D12_COMMAND_SIGNATURE_DESC indexed_signature_desc{sizeof(IndexedArguments), UINT(std::size(indexed_desc)),
                                                      indexed_desc};
  ComPtr<ID3D12CommandSignature> indexed_signature;
  CHECK(device->CreateCommandSignature(&indexed_signature_desc, nullptr, IID_PPV_ARGS(&indexed_signature)));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 pixel_bytes;
  device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &pixel_bytes);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixel_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(pixels), "pixel readback could not be made"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), draw_pso.Get(),
                                  IID_PPV_ARGS(&list)));
  D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  list->SetGraphicsRootSignature(graphics_rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  auto begin = [&](UINT index) { list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index); };
  auto end = [&](UINT index) { list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index); };
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(output.Get(), 0, source.Get(), sizeof(UINT), output_words * sizeof(UINT));
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  step("seed %u: empty, overlapping, reused and compute ranges", seed);
  begin(Empty);
  const float black[4]{};
  list->ClearRenderTargetView(rtv, black, 0, nullptr);
  end(Empty);
  begin(All);
  begin(First);
  list->DrawInstanced(vertices_per_quad * first, instances, 0, 0);
  begin(Nested);
  list->DrawInstanced(vertices_per_quad * nested, instances, 0, 0);
  end(Nested);
  end(First);
  begin(Reused);
  list->DrawInstanced(vertices_per_quad * discarded, instances, 0, 0);
  end(Reused);
  begin(Reused);
  end(Reused);
  begin(Compute);
  list->SetPipelineState(flat_dispatch_pso.Get());
  list->SetComputeRootSignature(compute_rs.Get());
  list->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  list->Dispatch(groups[0], groups[1], groups[2]);
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + plane_words * sizeof(UINT));
  list->SetPipelineState(dispatch_pso.Get());
  for (UINT zeros = 1; zeros < (1u << groups.size()); zeros++)
    list->Dispatch(zeros & 1 ? 0 : groups[0], zeros & 2 ? 0 : groups[1], zeros & 4 ? 0 : groups[2]);
  list->CopyBufferRegion(arguments.Get(), 0, argument_source.Get(), 0, argument_bytes);
  transition(list.Get(), arguments.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  for (auto op : {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO}) {
    list->SetPredication(argument_source.Get(), 2 * argument_bytes, op);
    list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, arguments.Get(), signature_desc.ByteStride);
  }
  list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
  transition(list.Get(), arguments.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(arguments.Get(), 0, argument_source.Get(), argument_bytes, argument_bytes);
  end(Compute);
  end(All);
  begin(NoPixel);
  list->SetPipelineState(no_pixel_pso.Get());
  list->DrawInstanced(vertices_per_quad * no_pixel, instances, 0, 0);
  end(NoPixel);
  begin(Geometry);
  list->SetPipelineState(geometry_pso.Get());
  list->DrawInstanced(vertices_per_quad * geometry, instances, 0, 0);
  end(Geometry);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, First, 1, repeated.Get(), result_stride);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, First, 1, repeated.Get(),
                         2 * result_stride);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, First, results.Get(), result_stride);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, First, Queries - First, results.Get(),
                         (First + 1) * result_stride);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(words.Get(), 0, output.Get(), 0, output_words * sizeof(UINT));
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      to{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  CHECK(list->Close());

  UINT64 quads[Queries]{};
  quads[All] = UINT64(first + nested + discarded) * instances;
  quads[First] = UINT64(first + nested) * instances;
  quads[Nested] = UINT64(nested) * instances;
  quads[NoPixel] = UINT64(no_pixel) * instances;
  quads[Geometry] = UINT64(geometry) * instances;
  UINT *input;
  CHECK(source->Map(0, nullptr, (void **)&input));
  memset(input + 1, 0, output_words * sizeof(UINT));
  for (UINT replay = 0; replay < replays; replay++) {
    step("seed %u, replay %u: counts, resolve guards, pixels and compute writes", seed, replay);
    const UINT indirect_groups_x = groups[0] + replay;
    const UINT count = replay == 1 ? 0 : replay + 1;
    const UINT64 indirect_invocations = count ? threads_per_group * indirect_groups_x * groups[1] * groups[2] : 0;
    *dispatch_arguments = {indirect_groups_x, groups[1], groups[2]};
    *indirect_count = count;
    const UINT64 predicate = replay == replays - 1 ? UINT64(1) << (sizeof(UINT) * 8) : 0;
    memcpy(reinterpret_cast<char *>(dispatch_arguments) + 2 * argument_bytes, &predicate, sizeof(predicate));
    *input = next();
    CHECK(forget(results.Get()));
    CHECK(forget(words.Get()));
    CHECK(forget(pixels.Get()));
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    const unsigned char *bytes;
    CHECK(results->Map(0, nullptr, (void **)&bytes));
    auto guard = [](auto byte) { return byte == 0xff; };
    expect(std::all_of(bytes, bytes + result_stride, guard) &&
               std::all_of(bytes + (Queries + 1) * result_stride, bytes + result_bytes, guard),
           "resolve wrote beyond its region");
    for (UINT q = 0; q < Queries; q++) {
      const UINT64 primitives = triangles_per_quad * quads[q];
      D3D12_QUERY_DATA_PIPELINE_STATISTICS got;
      memcpy(&got, bytes + (q + 1) * result_stride, sizeof(got));
      expect(got.IAVertices == vertices_per_triangle * primitives, "query %u: IA vertices %llu, want %llu", q,
             got.IAVertices, vertices_per_triangle * primitives);
      expect(got.IAPrimitives == primitives, "query %u: IA primitives %llu, want %llu", q, got.IAPrimitives,
             primitives);
      expect(got.VSInvocations == vertices_per_triangle * primitives, "query %u: VS invocations %llu, want %llu", q,
             got.VSInvocations, vertices_per_triangle * primitives);
      expect(got.CInvocations == primitives && got.CPrimitives == primitives,
             "query %u: clip invocations %llu, primitives %llu, want %llu", q, got.CInvocations, got.CPrimitives,
             primitives);
      const UINT64 shaded_quads = q == NoPixel || q == Geometry ? 0 : quads[q];
      const UINT stamp_width = 2, stamp_height = 2;
      const UINT64 minimum_pixels = UINT64(width) * height * shaded_quads,
                   maximum_pixels = UINT64((width + stamp_width - 1) / stamp_width) *
                                    ((height + stamp_height - 1) / stamp_height) * stamp_width * stamp_height *
                                    triangles_per_quad * shaded_quads;
      expect(got.PSInvocations >= minimum_pixels && got.PSInvocations <= maximum_pixels,
             "query %u: PS count %llu, want [%llu, %llu]", q, got.PSInvocations, minimum_pixels, maximum_pixels);
      const UINT64 geometry_primitives = q == Geometry ? primitives : 0;
      expect(got.GSPrimitives == geometry_primitives &&
                 (q == Geometry ? got.GSInvocations == primitives : quads[q] || !got.GSInvocations),
             "query %u: GS invocations %llu, primitives %llu, want %llu", q, got.GSInvocations, got.GSPrimitives,
             geometry_primitives);
      expect(!got.HSInvocations && !got.DSInvocations, "query %u: unused stage count", q);
      const UINT64 compute_invocations = q == All || q == Compute ? direct_invocations + indirect_invocations : 0;
      expect(got.CSInvocations == compute_invocations, "query %u: CS invocations %llu, want %llu", q, got.CSInvocations,
             compute_invocations);
    }
    const unsigned char *again;
    CHECK(repeated->Map(0, nullptr, (void **)&again));
    for (UINT resolve = 1; resolve <= 2; resolve++)
      expect(!memcmp(again + resolve * result_stride, bytes + (First + 1) * result_stride, result_stride),
             "resolve %u lost the ended graphics counts", resolve);
    repeated->Unmap(0, nullptr);
    results->Unmap(0, nullptr);
    const UINT *written;
    CHECK(words->Map(0, nullptr, (void **)&written));
    for (UINT64 i = 0; i < output_words; i++) {
      const UINT x = i % cs_width, dispatched_width = threads[0] * (i < plane_words ? groups[0] : indirect_groups_x);
      const UINT64 z = (i % plane_words) / (UINT64(cs_width) * cs_height),
                   dispatched_depth = UINT64(i < plane_words ? flat_threads[2] : threads[2]) * groups[2];
      const UINT want = x < dispatched_width && z < dispatched_depth && (i < plane_words || count) ? *input : 0;
      if (!expect(written[i] == want, "compute word %llu: %u, want %u", i, written[i], want))
        break;
    }
    words->Unmap(0, nullptr);
    CHECK(pixels->Map(0, nullptr, (void **)&bytes));
    for (UINT y = 0; y < height; y++) {
      auto row = reinterpret_cast<const UINT *>(bytes + footprint.Offset + y * footprint.Footprint.RowPitch);
      expect(std::all_of(row, row + width, [](UINT value) { return value == 1; }), "row %u was not drawn", y);
    }
    pixels->Unmap(0, nullptr);
  }
  step("statistics span more visibility slots than the Metal feature table's Apple7+ offset limit");
  CHECK(list->Reset(allocator.Get(), draw_pso.Get()));
  list->SetGraphicsRootSignature(graphics_rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  // Metal Feature Set Tables, maximum visibility query offset: 256 KB from Apple7. two slots per bracket.
  const UINT visibility_bytes = 256 * 1024, brackets = visibility_bytes / (2 * sizeof(UINT64)) + 1;
  begin(All);
  for (UINT bracket = 0; bracket < brackets; bracket++) {
    begin(First);
    list->DrawInstanced(vertices_per_quad, 1, 0, 0);
    end(First);
  }
  end(All);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, All, 1, results.Get(), 0);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  D3D12_QUERY_DATA_PIPELINE_STATISTICS *window;
  CHECK(results->Map(0, nullptr, (void **)&window));
  expect(window->IAVertices == UINT64(vertices_per_quad) * brackets &&
             window->IAPrimitives == UINT64(triangles_per_quad) * brackets &&
             window->PSInvocations >= UINT64(width) * height * brackets,
         "statistics lost draws across visibility windows: vertices %llu, primitives %llu, pixels %llu",
         window->IAVertices, window->IAPrimitives, window->PSInvocations);
  results->Unmap(0, nullptr);

  step("close without a resolve, then resolve twice from another list into readback and default heaps");
  CHECK(list->Reset(allocator.Get(), dispatch_pso.Get()));
  list->SetComputeRootSignature(compute_rs.Get());
  list->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  begin(Compute);
  list->Dispatch(groups[0], groups[1], groups[2]);
  end(Compute);
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));
  ComPtr<ID3D12CommandAllocator> resolve_allocator;
  ComPtr<ID3D12GraphicsCommandList> resolve_list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&resolve_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, resolve_allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&resolve_list)));
  for (UINT resolve = 1; resolve <= 2; resolve++) {
    resolve_list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, Compute, 1, results.Get(),
                                   resolve * result_stride);
    resolve_list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, Compute, 1, gpu_results.Get(),
                                   resolve * result_stride);
  }
  transition(resolve_list.Get(), gpu_results.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  resolve_list->CopyBufferRegion(repeated.Get(), 0, gpu_results.Get(), 0, result_bytes);
  CHECK(resolve_list->Close());
  CHECK(execute(device.Get(), queue.Get(), resolve_list.Get()));
  const unsigned char *direct, *copied;
  CHECK(results->Map(0, nullptr, (void **)&direct));
  CHECK(repeated->Map(0, nullptr, (void **)&copied));
  D3D12_QUERY_DATA_PIPELINE_STATISTICS expected{};
  expected.CSInvocations = threads_per_group * groups[0] * groups[1] * groups[2];
  for (UINT resolve = 1; resolve <= 2; resolve++) {
    expect(!memcmp(direct + resolve * result_stride, &expected, sizeof(expected)),
           "readback resolve %u lost the closed list's counts", resolve);
    expect(!memcmp(copied + resolve * result_stride, &expected, sizeof(expected)),
           "default heap resolve %u lost the closed list's counts", resolve);
  }
  repeated->Unmap(0, nullptr);
  results->Unmap(0, nullptr);
  source->Unmap(0, nullptr);
  argument_source->Unmap(0, nullptr);
  auto read_statistics = [&](UINT64 primitives, UINT64 minimum, UINT64 maximum) {
    D3D12_QUERY_DATA_PIPELINE_STATISTICS *got;
    if (!expect(SUCCEEDED(results->Map(0, nullptr, (void **)&got)), "statistics could not be mapped"))
      return;
    expect(got->IAPrimitives == primitives && got->CInvocations == primitives,
           "strip primitives IA %llu clipper %llu, want %llu", got->IAPrimitives, got->CInvocations, primitives);
    expect(got->PSInvocations >= minimum && got->PSInvocations <= maximum, "PS invocations %llu, want [%llu, %llu]",
           got->PSInvocations, minimum, maximum);
    results->Unmap(0, nullptr);
  };
  D3D12_DESCRIPTOR_HEAP_DESC depth_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  ComPtr<ID3D12DescriptorHeap> depth_heap;
  CHECK(device->CreateDescriptorHeap(&depth_heap_desc, IID_PPV_ARGS(&depth_heap)));
  const auto depth_handle = depth_heap->GetCPUDescriptorHandleForHeapStart();
  for (UINT samples : {1u, 4u}) {
    D3D12_RESOURCE_DESC color_desc = texture;
    color_desc.SampleDesc = {samples, 0};
    ComPtr<ID3D12Resource> color, depth;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&color)));
    D3D12_RESOURCE_DESC depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                          nullptr, IID_PPV_ARGS(&depth)));
    device->CreateRenderTargetView(color.Get(), nullptr, rtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, depth_handle);
    for (UINT shader_kind = 0; shader_kind < 3; shader_kind++)
      for (bool passes : {false, true}) {
        step("%u samples, shader %u, depth passes %u", samples, shader_kind, passes);
        auto desc = graphics;
        desc.PS = shader_kind == 0 ? D3D12_SHADER_BYTECODE{} : bytecode(shader_kind == 1 ? ps : depth_ps);
        desc.SampleDesc = {samples, 0};
        desc.DSVFormat = depth_desc.Format;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        ComPtr<ID3D12PipelineState> pipeline;
        CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
        CHECK(list->Reset(allocator.Get(), pipeline.Get()));
        list->SetGraphicsRootSignature(graphics_rs.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, &depth_handle);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->ClearDepthStencilView(depth_handle, D3D12_CLEAR_FLAG_DEPTH, passes ? 1.0f : 0.0f, 0, 0, nullptr);
        begin(All);
        list->DrawInstanced(vertices_per_quad, 1, 0, 0);
        end(All);
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, All, 1, results.Get(), 0);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const UINT64 covered = UINT64(width) * height;
        const UINT64 minimum = shader_kind && (passes || shader_kind == 2) ? covered : 0;
        const UINT64 maximum = shader_kind ? triangles_per_quad * ((width + 1) / 2) * ((height + 1) / 2) * 4 : 0;
        read_statistics(triangles_per_quad, minimum, maximum);
      }
  }
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  for (DXGI_FORMAT format : {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT})
    for (auto topology : {D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP})
      for (UINT strips : {1u, 2u, 19u})
        for (UINT edge_cuts : {0u, 1u, 2u, 3u})
          for (bool enabled : {false, true}) {
            const UINT cut =
                format == DXGI_FORMAT_R16_UINT ? D3D12_16BIT_INDEX_STRIP_CUT_VALUE : D3D12_32BIT_INDEX_STRIP_CUT_VALUE;
            const UINT buffer_offset_indices = 2, start_index = 3,
                       leading_indices = buffer_offset_indices + start_index;
            std::vector<UINT> index(leading_indices, cut);
            UINT64 primitives = 0;
            const UINT first_vertices = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ? 2 : 3;
            if (edge_cuts & 1)
              index.push_back(cut);
            for (UINT strip = 0; strip < strips; strip++) {
              if (strip) {
                index.push_back(cut);
                if (strip % 2)
                  index.push_back(cut);
              }
              const UINT vertices = 3 + (strip ? strip % 3 : width * height);
              for (UINT v = 0; v < vertices; v++)
                index.push_back(v % 4);
              primitives += vertices - first_vertices + 1;
            }
            if (edge_cuts & 2)
              index.push_back(cut);
            if (!enabled)
              primitives = index.size() - leading_indices - first_vertices + 1;
            std::vector<UINT16> narrow(index.begin(), index.end());
            const UINT stride = format == DXGI_FORMAT_R16_UINT ? sizeof(UINT16) : sizeof(UINT);
            auto ib =
                buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, index.size() * stride, D3D12_RESOURCE_STATE_GENERIC_READ);
            void *mapped;
            CHECK(ib->Map(0, nullptr, &mapped));
            memcpy(mapped, format == DXGI_FORMAT_R16_UINT ? (void *)narrow.data() : (void *)index.data(),
                   index.size() * stride);
            ib->Unmap(0, nullptr);
            auto desc = graphics;
            desc.VS = bytecode(strip_vs);
            desc.PS = bytecode(ps);
            desc.PrimitiveTopologyType = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
                                             ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                             : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            // the geometry shader of the pipelines before takes triangles, which a pipeline of lines does not make:
            // Direct3D 12 refuses the two together
            if (topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP)
              desc.GS = {};
            desc.IBStripCutValue = !enabled                         ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED
                                   : format == DXGI_FORMAT_R16_UINT ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                                                    : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
            ComPtr<ID3D12PipelineState> pipeline;
            CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
            D3D12_INDEX_BUFFER_VIEW view{ib->GetGPUVirtualAddress() + buffer_offset_indices * stride,
                                         (UINT(index.size()) - buffer_offset_indices) * stride, format};
            for (UINT instances : {1u, 3u})
              for (bool indirect : {false, true}) {
                step("format %u, topology %u, strips %u, edge cuts %u, enabled %u, instances %u, indirect %u", format,
                     topology, strips, edge_cuts, enabled, instances, indirect);
                const std::array<IndexedArguments, 2> draws{
                    {{view, {UINT(index.size()) - leading_indices, instances, start_index}},
                     {view, {UINT(index.size()) - leading_indices, instances, start_index}}}};
                auto indirect_arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(draws) + sizeof(UINT),
                                                 D3D12_RESOURCE_STATE_GENERIC_READ);
                CHECK(indirect_arguments->Map(0, nullptr, &mapped));
                memcpy(mapped, draws.data(), sizeof(draws));
                const UINT count = 1;
                memcpy((BYTE *)mapped + sizeof(draws), &count, sizeof(count));
                indirect_arguments->Unmap(0, nullptr);
                CHECK(list->Reset(allocator.Get(), pipeline.Get()));
                list->SetGraphicsRootSignature(graphics_rs.Get());
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->IASetPrimitiveTopology(topology);
                list->IASetIndexBuffer(&view);
                begin(All);
                if (indirect)
                  list->ExecuteIndirect(indexed_signature.Get(), draws.size(), indirect_arguments.Get(), 0,
                                        indirect_arguments.Get(), sizeof(draws));
                else
                  list->DrawIndexedInstanced(index.size() - leading_indices, instances, start_index, 0, 0);
                end(All);
                list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, All, 1, results.Get(), 0);
                CHECK(submit(device.Get(), queue.Get(), list.Get()));
                const UINT64 covered = topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                           ? UINT64(width / 2) * (height / 2) * strips * instances
                                           : 0;
                read_statistics(primitives * instances, covered, ~UINT64(0));
              }
          }
  step("zero maximum indirect commands leave the statistics empty");
  CHECK(list->Reset(allocator.Get(), draw_pso.Get()));
  list->SetGraphicsRootSignature(graphics_rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  begin(All);
  list->ExecuteIndirect(indexed_signature.Get(), 0, arguments.Get(), 0, nullptr, 0);
  end(All);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, All, 1, results.Get(), 0);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  read_statistics(0, 0, 0);
  return verdict();
}
