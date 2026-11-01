// contract: a geometry shader's emits past its declared maxvertexcount are dropped, everywhere: "Hardware must enforce
// the limit above by stopping writes if the Geometry Shader program continues after emitting the declared maximum
// number of vertices", and the limit counts every emit, on every stream: "Even if no output Buffers happen to be bound
// to a Stream and a vertex is output (and therefore dropped), it still counts against the limit" (D3D11.3 functional
// spec, 13.3). each shader loops over a root constant the compilers cannot see, so it emits more than it declares at
// run time only. a dropped vertex reaches neither the rasterizer nor stream output, and makes no primitive in the
// statistics (D3D12_QUERY_DATA_SO_STATISTICS, D3D12_QUERY_DATA_PIPELINE_STATISTICS::GSPrimitives):
// - a point stream of maxvertexcount(2) emits 4 points per input point, each at a pixel of its own;
// - a triangle strip of maxvertexcount(4) emits 8 vertices, whose 6 triangles would each cover a pixel of their own;
// - two point streams of maxvertexcount(2): one point to stream 1, then two to stream 0, of which only the first
//   is within the limit;
// - a stream-output-only point stream of maxvertexcount(512), more vertices than a Metal mesh has (256): legal, since
//   its one-scalar output stays within the 1024 scalars a geometry shader may output (13.3). it emits 520.
// each pixel starts at a value no case writes, and each stream output word at one no case streams.
#include "d3d12_test.hpp"

static constexpr UINT inputs = 3, width = 8 * inputs, clear_value = 99, unwritten = 0xeeeeeeee, big = 512;

static const char hlsl[] = R"hlsl(
cbuffer Emits : register(b0) { uint emits; };
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = id; return v; }
struct P { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
// at pixel row `y` (0 its top, 1 its bottom) and column `x` of the one-pixel-high target
P at(float x, float y, uint value) {
  P o;
  o.pos = float4(x / WIDTH * 2 - 1, 1 - y * 2, 0.5, 1);
  o.value = value;
  return o;
}
// input point p's point j at pixel 4p + j, valued 4p + j + 1
[maxvertexcount(2)]
void gs_points(point V v[1], inout PointStream<P> s) {
  for (uint j = 0; j < emits; j++)
    s.Append(at(4 * v[0].id + j + 0.5, 0.5, 4 * v[0].id + j + 1));
}
// input point p's strip over pixels 8p to 8p + 7, its vertices alternately at the top and the bottom of the row:
// triangle n covers pixel 8p + n, valued from its leading vertex as 8p + n + 1
[maxvertexcount(4)]
void gs_strip(point V v[1], inout TriangleStream<P> s) {
  for (uint j = 0; j < emits; j++)
    s.Append(at(8 * v[0].id + (j & ~1u), j & 1, 8 * v[0].id + j + 1));
}
// a point to stream 1, then `emits` to stream 0
[maxvertexcount(2)]
void gs_streams(point V v[1], inout PointStream<P> s0, inout PointStream<P> s1) {
  s1.Append(at(4 * v[0].id + 3.5, 0.5, 100 + v[0].id));
  for (uint j = 0; j < emits; j++)
    s0.Append(at(4 * v[0].id + j + 0.5, 0.5, 4 * v[0].id + j + 1));
}
struct W { uint value : VALUE; };
[maxvertexcount(BIG)]
void gs_big(point V v[1], inout PointStream<W> s) {
  for (uint j = 0; j < emits; j++) {
    W o;
    o.value = v[0].id * 4096 + j;
    s.Append(o);
  }
}
uint ps(P p) : SV_Target { return p.value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width), "BIG=" + std::to_string(big)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       gs_points = compiler.compile(hlsl, "gs_points", "gs", defines),
       gs_strip = compiler.compile(hlsl, "gs_strip", "gs", defines),
       gs_streams = compiler.compile(hlsl, "gs_streams", "gs", defines),
       gs_big = compiler.compile(hlsl, "gs_big", "gs", defines);
  if (!expect(!vs.empty() && !ps.empty() && !gs_points.empty() && !gs_strip.empty() && !gs_streams.empty() &&
                  !gs_big.empty(),
              "HLSL did not compile"))
    return verdict();

  step("create the device");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, 1};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
  auto rs = root_signature(device.Get(), {1, &parameter, 0, nullptr,
                                          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                              D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
  CHECK(rs ? S_OK : E_FAIL);

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, DXGI_FORMAT_R32_UINT,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  // each stream's buffer holds `capacity` words, then its filled size
  const UINT capacity = inputs * big + 64;
  const UINT64 stream_bytes = 4ull * capacity, filled_at = stream_bytes, stream_size = filled_at + 8;
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, stream_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint32_t *words;
  CHECK(initial->Map(0, nullptr, (void **)&words));
  for (UINT i = 0; i < capacity; i++)
    words[i] = unwritten;
  words[capacity] = words[capacity + 1] = 0;
  initial->Unmap(0, nullptr);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));

  enum Kind { points, strip, streams, stream_output_only };
  struct Case {
    Kind kind;
    const char *name;
    const std::string &gs;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
    UINT emits, rasterized_stream;
    bool two_streams;
    // what each pixel holds, and each stream's words, for input point p
    std::vector<UINT> pixels, stream[2];
    UINT primitives[2];
  };
  std::vector<Case> cases = {
      {points, "point stream", gs_points, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, 4, 0, false},
      {strip, "triangle strip", gs_strip, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 8, 0, false},
      {streams, "two streams", gs_streams, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, 2, 0, true},
      {stream_output_only, "512 vertices, stream output only", gs_big, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, big + 8,
       D3D12_SO_NO_RASTERIZED_STREAM, false},
  };
  for (auto &c : cases) {
    c.pixels.assign(width, clear_value);
    for (UINT p = 0; p < inputs; p++) {
      if (c.kind == points) {
        // points 0 and 1 are within the limit, 2 and 3 past it
        for (UINT j = 0; j < 2; j++)
          c.pixels[4 * p + j] = 4 * p + j + 1, c.stream[0].push_back(4 * p + j + 1);
      } else if (c.kind == strip) {
        // vertices 0 to 3 make triangles 0 and 1; vertices 4 to 7 would make 2 to 5
        for (UINT n = 0; n < 2; n++)
          c.pixels[8 * p + n] = 8 * p + n + 1;
        for (UINT v : {1, 2, 3, 2, 4, 3})
          c.stream[0].push_back(8 * p + v);
      } else if (c.kind == streams) {
        // stream 1's point, then stream 0's first; stream 0's second is the third emit
        c.pixels[4 * p] = 4 * p + 1;
        c.stream[0].push_back(4 * p + 1);
        c.stream[1].push_back(100 + p);
      } else {
        for (UINT j = 0; j < big; j++)
          c.stream[0].push_back(p * 4096 + j);
      }
    }
    c.primitives[0] = UINT(c.stream[0].size() / (c.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE ? 3 : 1));
    c.primitives[1] = UINT(c.stream[1].size());
  }

  for (auto &c : cases) {
    step("%s: create the pipeline", c.name);
    const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 1, 0}, {1, "VALUE", 0, 0, 1, 1}};
    const UINT strides[] = {4, 4};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
    desc.GS = bytecode(c.gs);
    desc.StreamOutput = {entries, c.two_streams ? 2u : 1u, strides, c.two_streams ? 2u : 1u, c.rasterized_stream};
    // the stream-output-only shader has no position for a pixel shader to take
    if (c.rasterized_stream != D3D12_SO_NO_RASTERIZED_STREAM)
      desc.PS = bytecode(ps);
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT created = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (!expect(SUCCEEDED(created), "CreateGraphicsPipelineState returned %#lx", created))
      continue;

    step("%s: draw %u points", c.name, inputs);
    ComPtr<ID3D12Resource> streams[2];
    for (auto &s : streams)
      s = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, stream_size, D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pixels_at = 0, streams_at = row,
                 statistics_at = streams_at + 2 * stream_size,
                 pipeline_at = statistics_at + 2 * sizeof(D3D12_QUERY_DATA_SO_STATISTICS),
                 readback_size = pipeline_at + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, readback_size, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(forget(readback.Get()));
    ComPtr<ID3D12QueryHeap> so_queries, pipeline_queries;
    D3D12_QUERY_HEAP_DESC so_desc{D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 2},
        pipeline_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1};
    CHECK(device->CreateQueryHeap(&so_desc, IID_PPV_ARGS(&so_queries)));
    CHECK(device->CreateQueryHeap(&pipeline_desc, IID_PPV_ARGS(&pipeline_queries)));

    CHECK(allocator->Reset());
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    const float clear[4] = {(float)clear_value, 0, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2];
    for (UINT s = 0; s < 2; s++) {
      list->CopyBufferRegion(streams[s].Get(), 0, initial.Get(), 0, stream_size);
      transition(list.Get(), streams[s].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      auto base = streams[s]->GetGPUVirtualAddress();
      views[s] = {base, stream_bytes, base + filled_at};
    }
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRoot32BitConstant(0, c.emits, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)width, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SOSetTargets(0, c.two_streams ? 2 : 1, views);
    list->BeginQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    list->BeginQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1);
    list->BeginQuery(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    list->DrawInstanced(inputs, 1, 0, 0);
    list->EndQuery(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    list->EndQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1);
    list->EndQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    list->ResolveQueryData(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, readback.Get(), statistics_at);
    list->ResolveQueryData(
        so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1, 1, readback.Get(),
        statistics_at + sizeof(D3D12_QUERY_DATA_SO_STATISTICS)
    );
    list->ResolveQueryData(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, readback.Get(), pipeline_at);
    for (UINT s = 0; s < 2; s++) {
      transition(list.Get(), streams[s].Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), streams_at + s * stream_size, streams[s].Get(), 0, stream_size);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION placed{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        texture{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    placed.PlacedFootprint = {pixels_at, {DXGI_FORMAT_R32_UINT, width, 1, 1, (UINT)row}};
    list->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (!expect(SUCCEEDED(hr), "the draw failed: %#lx", hr))
      return verdict();

    step("%s: read back", c.name);
    uint8_t *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    auto word = [&](UINT64 at) { return *reinterpret_cast<const uint32_t *>(got + at); };
    unsigned reported = 0;
    for (UINT x = 0; x < width; x++)
      if (word(pixels_at + 4 * x) != c.pixels[x] && reported++ < 8)
        expect(false, "pixel %u holds %u, want %u", x, word(pixels_at + 4 * x), c.pixels[x]);
    for (UINT s = 0; s < 2; s++) {
      const UINT64 at = streams_at + s * stream_size;
      const auto &want = c.stream[s];
      for (UINT i = 0; i < capacity; i++) {
        UINT expected = i < want.size() ? want[i] : unwritten;
        if (word(at + 4 * i) != expected && reported++ < 16)
          expect(false, "stream %u word %u holds %#x, want %#x", s, i, word(at + 4 * i), expected);
      }
      auto filled = *reinterpret_cast<const UINT64 *>(got + at + filled_at);
      expect(filled == 4 * want.size(), "stream %u filled size %llu, want %zu", s, filled, 4 * want.size());
      auto &statistics = reinterpret_cast<const D3D12_QUERY_DATA_SO_STATISTICS *>(got + statistics_at)[s];
      expect(statistics.NumPrimitivesWritten == c.primitives[s], "stream %u primitives written %llu, want %u", s,
             statistics.NumPrimitivesWritten, c.primitives[s]);
      expect(statistics.PrimitivesStorageNeeded == c.primitives[s], "stream %u primitives needed %llu, want %u", s,
             statistics.PrimitivesStorageNeeded, c.primitives[s]);
    }
    auto &pipeline = *reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(got + pipeline_at);
    expect(pipeline.GSInvocations == inputs, "GSInvocations %llu, want %u", pipeline.GSInvocations, inputs);
    expect(pipeline.GSPrimitives == c.primitives[0] + c.primitives[1], "GSPrimitives %llu, want %u",
           pipeline.GSPrimitives, c.primitives[0] + c.primitives[1]);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
