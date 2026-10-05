// contract: what a mesh or amplification shader may do with its outputs (D3D12 mesh shader specification, "Shared
// Output Arrays", "SetMeshOutputCounts", "DispatchMesh intrinsic"; cull distances: D3D11.3 15.4.2 and 15.4.3):
// - an attribute may be written in parts, at different places of the shader, and the last value written is used;
// - a row of an array attribute may be chosen at run time;
// - as many scalar attributes as fit the signature reach the pixel shader, each with its own value, and so do clip
//   and cull distances the pixel shader reads;
// - a primitive is discarded when, for one cull distance, all its vertices are negative or NaN, whichever threads
//   wrote its vertices, indices and SV_CullPrimitive; a primitive with one vertex in is drawn whole, not clipped;
// - DispatchMesh is a barrier for the amplification shader's group, so the mesh shaders get the payload all its
//   threads built, and its arguments are the first thread's where the threads disagree.
// every case draws triangles that cover known pixels of a row of its own; the rest of the target stays untouched.
#include "d3d12_test.hpp"
#include <algorithm>
#include <iterator>
#include <map>

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint one; uint rows; uint nan_bits; };
// vertex `corner` of a triangle that covers the centers of the pixels from (x, y) to the right that `width` reaches,
// and no other: 1.4 reaches one, 4.6 three
float4 corner(uint x, uint y, uint corner, float width) {
  float2 at = float2(x, y) + float2(corner == 1 ? width : 0, corner == 2 ? 1.4 : 0);
  return float4(at / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
}

// parts of the position and of an attribute in one block, the rest in a block the compiler cannot join with it
struct Parts { float4 pos : SV_Position; float2 color : COLOR; };
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_parts(uint g : SV_GroupID, out vertices Parts v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(3, 1);
  uint c;
  for (c = 0; c < 3; c++) {
    v[c].pos.xy = corner(g, PARTS_ROW, c, 1.4).xy;
    v[c].color.y = 5 + g;
  }
  i[0] = uint3(0, 1, 2);
  if (one)
    for (c = 0; c < 3; c++) {
      v[c].pos.zw = float2(0, 1);
      v[c].color.x = 2;
    }
}
uint ps_parts(Parts v) : SV_Target { return (uint)(v.color.x + 0.5) * 100 + (uint)(v.color.y + 0.5); }

// rows of an array attribute chosen by a loop whose length the compiler does not know, and a clip distance the pixel
// shader reads
struct Rows { float4 pos : SV_Position; float2 uv[ROWS] : UV; float clip : SV_ClipDistance; };
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_rows(uint g : SV_GroupID, out vertices Rows v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint c = 0; c < 3; c++) {
    v[c].pos = corner(g, ROWS_ROW, c, 1.4);
    v[c].clip = 2;
    for (uint j = 0; j < rows; j++)
      v[c].uv[j] = float2(j + 1, g + 1);
  }
  i[0] = uint3(0, 1, 2);
}
uint ps_rows(Rows v) : SV_Target {
  uint sum = (uint)(v.clip * 8 + 0.5) * 10000 + (uint)(v.uv[ROWS - 1].y + 0.5) * 1000, scale = 1;
  for (uint j = 0; j < ROWS; j++, scale *= 10)
    sum += (uint)(v.uv[j].x + 0.5) * scale;
  return sum;
}

// MANY scalar attributes
struct Many { float4 pos : SV_Position; MANY_FIELDS };
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_many(uint g : SV_GroupID, out vertices Many v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint c = 0; c < 3; c++) {
    v[c].pos = corner(g, MANY_ROW, c, 1.4);
    MANY_SET
  }
  i[0] = uint3(0, 1, 2);
}
uint ps_many(Many v) : SV_Target { return (uint)(MANY_SUM + 0.5); }

// pattern 2g + t is thread t's of group g: three vertices with two cull distances each, coded 0 for zero, 1 for
// positive, 2 for negative and 3 for NaN. each thread writes its own primitive's vertices and the other's indices and
// SV_CullPrimitive
struct Cull { float4 pos : SV_Position; nointerpolation uint value : VALUE; float2 cull : SV_CullDistance; };
struct Culled { bool culled : SV_CullPrimitive; };
static const uint codes[PATTERNS][2][3] = CODES;
static const uint culls[PATTERNS] = CULLS;
float distance_of(uint code) { return code == 3 ? asfloat(nan_bits) : code == 2 ? -1.0 : (float)code; }
[numthreads(2, 1, 1)] [outputtopology("triangle")]
void ms_cull(uint g : SV_GroupID, uint t : SV_GroupThreadID, out vertices Cull v[6], out indices uint3 i[2],
             out primitives Culled p[2]) {
  SetMeshOutputCounts(6, 2);
  uint pattern = 2 * g + t, other = 1 - t;
  for (uint c = 0; c < 3; c++) {
    v[t * 3 + c].pos = corner(0, CULL_ROW + pattern, c, 4.6);
    v[t * 3 + c].value = 70 + pattern;
    v[t * 3 + c].cull = float2(distance_of(codes[pattern][0][c]), distance_of(codes[pattern][1][c]));
  }
  i[other] = uint3(other * 3 + 2, other * 3 + 1, other * 3);
  p[other].culled = culls[2 * g + other];
}
// the second distance uninterpolated: the primitive's first vertex's
uint ps_cull(float4 pos : SV_Position, nointerpolation uint value : VALUE, nointerpolation float2 cull : SV_CullDistance)
    : SV_Target {
  return value + (uint)((cull.y + 2) * 16 + 0.5) * 100;
}

// a payload every thread of the group writes one entry of, sent without a barrier of the shader's own
struct Shared { uint values[THREADS]; };
groupshared Shared together;
[numthreads(THREADS, 1, 1)]
void as_shared(uint t : SV_GroupIndex, uint a : SV_GroupID) {
  together.values[t] = 3 * t + a + 1;
  DispatchMesh(1, 1, 1, together);
}
struct Value { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_shared(in payload Shared s, out vertices Value v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(3, 1);
  uint sum = 0;
  for (uint k = 0; k < THREADS; k++)
    sum += s.values[k];
  for (uint c = 0; c < 3; c++) {
    v[c].pos = corner(s.values[0] - 1, SHARED_ROW, c, 1.4);
    v[c].value = sum;
  }
  i[0] = uint3(0, 1, 2);
}
// arguments that differ by thread: the first thread's count
struct Own { uint value; };
[numthreads(THREADS, 1, 1)]
void as_first(uint t : SV_GroupIndex) {
  Own own;
  own.value = 500 + t;
  DispatchMesh(t == 0 ? FIRST_GROUPS : FIRST_GROUPS + 3, 1, 1, own);
}
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_first(uint g : SV_GroupID, in payload Own own, out vertices Value v[3], out indices uint3 i[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint c = 0; c < 3; c++) {
    v[c].pos = corner(g, FIRST_ROW, c, 1.4);
    v[c].value = own.value;
  }
  i[0] = uint3(0, 1, 2);
}
uint ps_value(Value v) : SV_Target { return v.value; }
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
  // 30 registers of four scalars beside the position: Metal passes 124 components to a pixel shader, Direct3D 128
  const UINT size = 32, groups = 3, array_rows = 3, many = 120, threads = 128, first_groups = 2;
  const UINT parts_row = 0, rows_row = 2, many_row = 4, shared_row = 6, first_row = 8, cull_row = 10;
  enum : UINT { zero, in, out, nan };
  struct Pattern {
    UINT codes[2][3];
    bool culls;
  };
  const Pattern patterns[] = {
      {{{in, in, in}, {in, in, in}}, false},          // drawn
      {{{out, out, out}, {in, in, in}}, false},       // all out by the first distance
      {{{in, out, in}, {in, in, in}}, false},         // one vertex out: drawn whole
      {{{in, in, in}, {out, out, out}}, false},       // all out by the second distance
      {{{nan, nan, nan}, {in, in, in}}, false},       // NaN is out
      {{{nan, out, in}, {zero, zero, zero}}, false},  // one vertex in
      {{{in, in, in}, {in, in, in}}, true},           // culled by the shader
      {{{out, nan, out}, {zero, zero, zero}}, false}, // out by the first, whatever the second
      {{{zero, out, out}, {out, in, out}}, false},    // zero is in
      {{{out, out, in}, {in, out, zero}}, false},     // out by neither
  };
  const UINT pattern_count = std::size(patterns);
  auto distance = [](UINT code) { return code == out ? -1.f : (float)code; };
  auto list = [](auto &&each, UINT n) {
    std::string s = "{";
    for (UINT i = 0; i < n; i++)
      s += (i ? "," : "") + each(i);
    return s + "}";
  };
  auto triple = [&](const UINT *codes) { return list([&](UINT c) { return std::to_string(codes[c]); }, 3); };
  std::string fields, set, sum = "0";
  // attribute n of group g: the same at a triangle's three vertices, so the pixel shader gets exactly it
  auto attribute = [](UINT n, UINT g) { return (n * 7 + g * 3) % 13; };
  for (UINT n = 0; n < many; n++) {
    auto name = "a" + std::to_string(n);
    fields += "float " + name + " : A" + std::to_string(n) + "; ";
    set += "v[c]." + name + " = (" + std::to_string(n) + " * 7 + g * 3) % 13; ";
    sum += " + v." + name + " * " + std::to_string(n + 1);
  }
  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size),
      "PARTS_ROW=" + std::to_string(parts_row),
      "ROWS_ROW=" + std::to_string(rows_row),
      "ROWS=" + std::to_string(array_rows),
      "MANY_ROW=" + std::to_string(many_row),
      "MANY_FIELDS=" + fields,
      "MANY_SET=" + set,
      "MANY_SUM=" + sum,
      "SHARED_ROW=" + std::to_string(shared_row),
      "FIRST_ROW=" + std::to_string(first_row),
      "FIRST_GROUPS=" + std::to_string(first_groups),
      "THREADS=" + std::to_string(threads),
      "CULL_ROW=" + std::to_string(cull_row),
      "PATTERNS=" + std::to_string(pattern_count),
      "CODES=" + list(
                     [&](UINT p) {
                       return list([&](UINT d) { return triple(patterns[p].codes[d]); }, 2);
                     },
                     pattern_count
                 ),
      "CULLS=" + list([&](UINT p) { return std::to_string(patterns[p].culls); }, pattern_count),
  };
  auto compile = [&](const char *entry, const char *stage) { return compiler.compile(hlsl, entry, stage, defines); };
  struct Case {
    const char *name;
    std::string as, ms, ps;
    UINT dispatched;
    ComPtr<ID3D12PipelineState> pso;
  } cases[] = {
      {"parts", {}, compile("ms_parts", "ms_6_5"), compile("ps_parts", "ps_6_5"), groups},
      {"rows", {}, compile("ms_rows", "ms_6_5"), compile("ps_rows", "ps_6_5"), groups},
      {"many", {}, compile("ms_many", "ms_6_5"), compile("ps_many", "ps_6_5"), groups},
      {"shared", compile("as_shared", "as_6_5"), compile("ms_shared", "ms_6_5"), compile("ps_value", "ps_6_5"), groups},
      {"first", compile("as_first", "as_6_5"), compile("ms_first", "ms_6_5"), compile("ps_value", "ps_6_5"), 1},
      {"cull", {}, compile("ms_cull", "ms_6_5"), compile("ps_cull", "ps_6_5"), pattern_count / 2},
  };
  for (auto &c : cases)
    if (c.ms.empty() || c.ps.empty()) {
      printf("skipped: mesh shaders are shader model 6.5\n");
      return 77;
    }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)));
  if (options.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
    printf("failed: mesh shaders are not supported\n");
    return 1;
  }
  const UINT constants[] = {1, array_rows, 0x7fc00000};
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, (UINT)std::size(constants)};
  auto rs = root_signature(device.Get(), {1, &parameter});

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
  } stream;
  stream.rs.data = rs.Get();
  stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.raster.data.DepthClipEnable = TRUE;
  stream.targets.data.NumRenderTargets = 1;
  stream.targets.data.RTFormats[0] = DXGI_FORMAT_R32_UINT;
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  for (auto &c : cases) {
    stream.as.data = c.as.empty() ? D3D12_SHADER_BYTECODE{} : bytecode(c.as);
    stream.ms.data = bytecode(c.ms);
    stream.ps.data = bytecode(c.ps);
    if (FAILED(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&c.pso)))) {
      printf("failed: no pipeline for the %s case\n", c.name);
      return 1;
    }
  }

  // what the shaders draw
  const UINT untouched = ~0u;
  std::map<std::pair<UINT, UINT>, UINT> want;
  for (UINT g = 0; g < groups; g++) {
    want[{g, parts_row}] = 2 * 100 + 5 + g;
    UINT rows = 2 * 8 * 10000 + (g + 1) * 1000, scale = 1, sum = 0;
    for (UINT j = 0; j < array_rows; j++, scale *= 10)
      rows += (j + 1) * scale;
    want[{g, rows_row}] = rows;
    for (UINT n = 0; n < many; n++)
      sum += attribute(n, g) * (n + 1);
    want[{g, many_row}] = sum;
    // amplification group g's threads write 3t + g + 1
    want[{g, shared_row}] = 3 * threads * (threads - 1) / 2 + threads * (g + 1);
  }
  for (UINT g = 0; g < first_groups; g++)
    want[{g, first_row}] = 500;
  for (UINT p = 0; p < pattern_count; p++) {
    bool culled = patterns[p].culls;
    for (auto &codes : patterns[p].codes)
      culled |= std::all_of(codes, codes + 3, [](UINT code) { return code == out || code == nan; });
    // the indices name the corners backwards, so the first vertex is the last corner
    if (!culled)
      for (UINT x = 0; x < 3; x++)
        want[{x, cull_row + p}] = 70 + p + (UINT)((distance(patterns[p].codes[1][2]) + 2) * 16 + 0.5f) * 100;
  }

  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> commands;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
  const float clear[4] = {(float)untouched, 0, 0, 0};
  commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
  commands->SetGraphicsRootSignature(rs.Get());
  commands->SetGraphicsRoot32BitConstants(0, (UINT)std::size(constants), constants, 0);
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  for (auto &c : cases) {
    commands->SetPipelineState(c.pso.Get());
    commands->DispatchMesh(c.dispatched, 1, 1);
  }
  transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
  commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), commands.Get()));
  uint8_t *pixels;
  CHECK(readback->Map(0, nullptr, (void **)&pixels));

  unsigned failures = 0;
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      auto value = reinterpret_cast<const UINT *>(pixels + row * y)[x];
      auto it = want.find({x, y});
      auto expected = it == want.end() ? untouched : it->second;
      if (value != expected && failures++ < 16)
        printf("pixel %u,%u: %u, want %u\n", x, y, value, expected);
    }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %zu pixels drawn of %u\n", want.size(), size * size);
  return 0;
}
