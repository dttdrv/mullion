// contract: wave operations use "all active non-helper lanes", and prefixes stop "not including this lane"
// (DXC Wave Intrinsics, https://github.com/microsoft/DirectXShaderCompiler/wiki/Wave-Intrinsics).
// "The bits that are greater than or equal to WaveGetLaneCount will be zero." Ballots check all four words.
// SM6.5 WaveMultiPrefix partitions active lanes by masks; singleton prefixes have the operation's identity
// (https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_5.html). integer results wrap in their type.
// WaveMatch: "The bit in the mask corresponding to the current lane will always be set to 1." NaNs check this
// guarantee and inactive bits separately, without assuming how different NaN encodings compare to one another.
// floating inputs use exact powers of two, signed zeros, infinities, both NaN classes and signed denorms.
// fp32 basic arithmetic explicitly uses ftz; its wave-denorm arithmetic needs a separate contract witness.
// fp16 preserves denorms. reductions permit their unspecified ordering,
// including either sign for a zero sum and either tied zero for min/max (D3D11.3 3.1.3.2).
// a separate movement pass preserves every IEEE class, including signed zero, NaNs and denormals (D3D11.3
// 3.1.3.2 data movement exception). SM6.6 1D quads use consecutive SV_GroupIndex values in Z order.
// "All non-quad related Wave Intrinsics are available in all shader stages." quad reads run only in PS/CS/MS/AS,
// as required by DXC v1.7.2308 lib/DXIL/DxilOperations.cpp; VS/HS/DS/GS do not permit those opcodes.
// pixel quads use scan-line order 0,1 / 2,3 (Microsoft Learn, Shader Model6.0). tiny triangles cover
// one or three pixel centers; a render-target readback verifies coverage independently of UAV writes.
// native i16 WaveActiveBit and i1 QuadReadLaneAt are valid DXIL overloads absent from HLSL. their seed calls
// are narrowed explicitly, retaining metadata, then assembled and structurally validated before device creation.
#include "dxil_assembly.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <regex>

enum Field {
  leader,
  lane_index,
  lane_count,
  first,
  any,
  all,
  equal,
  read_first,
  read_at,
  sum,
  product,
  minimum,
  maximum,
  prefix_sum,
  prefix_product,
  bit_and,
  bit_or,
  bit_xor,
  ballot,
  match = ballot + 4,
  multi_sum = match + 4,
  multi_product,
  multi_and,
  multi_or,
  multi_xor,
  multi_count,
  count,
  prefix_count,
  quad_x,
  quad_y,
  quad_diagonal,
  quad_lane,
  quad_dynamic = quad_lane + 4,
  fields
};
enum Stage { compute, pixel, vertex, geometry, hull, domain, mesh, amplification, stages };

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint2> values : register(t0);
StructuredBuffer<uint2> cases : register(t1);
RWStructuredBuffer<uint2> result : register(u0);
cbuffer C : register(b0) { uint movements_only; uint case_index; uint coverage; }
#if REAL && WIDTH == 16
T value(uint2 v) { return asfloat16(uint16_t(v.x)); }
uint2 bits(T v) { return uint2(asuint16(v), 0); }
#elif REAL
T value(uint2 v) { return asfloat(v.x); }
uint2 bits(T v) { return uint2(asuint(v), 0); }
#elif WIDTH == 64
T value(uint2 v) { return T(uint64_t(v.x) | uint64_t(v.y) << 32); }
uint2 bits(T v) { return uint2(uint(v), uint(uint64_t(v) >> 32)); }
#else
T value(uint2 v) { return T(v.x); }
uint2 bits(T v) { return uint2(uint(v) & TYPE_MASK, 0); }
#endif
void run(uint t, uint group) {
  uint2 c = cases[group];
  if (t >= c.x) return;
  uint b = (group * GROUP + t) * FIELDS;
  uint lane = WaveGetLaneIndex(), width = WaveGetLaneCount();
  result[b + LEADER] = uint2(WaveReadLaneFirst(t), 0);
  result[b + LANE_INDEX] = uint2(lane, 0);
  result[b + LANE_COUNT] = uint2(width, 0);
#if QUADS
#if !BOOL_TYPE
  result[b + QUAD_X] = bits(QuadReadAcrossX(value(values[group * GROUP + t])));
  result[b + QUAD_Y] = bits(QuadReadAcrossY(value(values[group * GROUP + t])));
  result[b + QUAD_DIAGONAL] = bits(QuadReadAcrossDiagonal(value(values[group * GROUP + t])));
#endif
  for (uint k = 0; k < 4; k++)
#if BOOL_TYPE
    result[b + QUAD_LANE + k] = bits(T(QuadReadLaneAt(uint(value(values[group * GROUP + t])), k)));
  result[b + QUAD_DYNAMIC] = bits(T(QuadReadLaneAt(uint(value(values[group * GROUP + t])), (t + group) % 4)));
#else
    result[b + QUAD_LANE + k] = bits(QuadReadLaneAt(value(values[group * GROUP + t]), k));
  result[b + QUAD_DYNAMIC] = bits(QuadReadLaneAt(value(values[group * GROUP + t]), (t + group) % 4));
#endif
#endif
  bool active = c.y == 0 || (c.y == 1 ? (lane & 1) != 0 : c.y == 2 ? lane + 1 == width : false);
  if (active) {
    T v = value(values[group * GROUP + t]);
    result[b + READ_FIRST] = bits(WaveReadLaneFirst(v));
    result[b + READ_AT] = bits(WaveReadLaneAt(v, WaveActiveMax(lane)));
#if BOOL_TYPE
    result[b + EQUAL] = uint2(WaveActiveAllEqual(v), 0);
#else
    uint4 matches = WaveMatch(v);
    for (uint k = 0; k < 4; k++) result[b + MATCH + k] = uint2(matches[k], 0);
    if (movements_only != 0) return;
    bool predicate = (t & 1) != 0;
    result[b + FIRST] = uint2(WaveIsFirstLane(), 0);
    result[b + ANY] = uint2(WaveActiveAnyTrue(predicate), 0);
    result[b + ALL] = uint2(WaveActiveAllTrue(predicate), 0);
    result[b + EQUAL] = uint2(WaveActiveAllEqual(v), 0);
    result[b + SUM] = bits(WaveActiveSum(v));
    result[b + PRODUCT] = bits(WaveActiveProduct(v));
    result[b + MINIMUM] = bits(WaveActiveMin(v));
    result[b + MAXIMUM] = bits(WaveActiveMax(v));
    result[b + PREFIX_SUM] = bits(WavePrefixSum(v));
    result[b + PREFIX_PRODUCT] = bits(WavePrefixProduct(v));
    uint4 votes = WaveActiveBallot(predicate), partition = WaveMatch(T(t % 3));
    for (uint k = 0; k < 4; k++) {
      result[b + BALLOT + k] = uint2(votes[k], 0);
    }
    result[b + MULTI_SUM] = bits(WaveMultiPrefixSum(v, partition));
    result[b + MULTI_PRODUCT] = bits(WaveMultiPrefixProduct(v, partition));
    result[b + MULTI_COUNT] = uint2(WaveMultiPrefixCountBits(predicate, partition), 0);
    result[b + COUNT] = uint2(WaveActiveCountBits(predicate), 0);
    result[b + PREFIX_COUNT] = uint2(WavePrefixCountBits(predicate), 0);
#if !REAL
#if ACTIVE_BITS
#if WIDTH == 16
    result[b + BIT_AND] = bits(T(WaveActiveBitAnd(uint(v))));
    result[b + BIT_OR] = bits(T(WaveActiveBitOr(uint(v))));
    result[b + BIT_XOR] = bits(T(WaveActiveBitXor(uint(v))));
#else
    result[b + BIT_AND] = bits(WaveActiveBitAnd(v));
    result[b + BIT_OR] = bits(WaveActiveBitOr(v));
    result[b + BIT_XOR] = bits(WaveActiveBitXor(v));
#endif
#endif
    result[b + MULTI_AND] = bits(WaveMultiPrefixBitAnd(v, partition));
    result[b + MULTI_OR] = bits(WaveMultiPrefixBitOr(v, partition));
    result[b + MULTI_XOR] = bits(WaveMultiPrefixBitXor(v, partition));
#endif
#endif
  }
}
[numthreads(GROUP, 1, 1)] void cs(uint t : SV_GroupIndex, uint group : SV_GroupID) { run(t, group); }
struct V { float4 pos : SV_Position; nointerpolation uint id : ID; };
V vs(uint i : SV_VertexID) {
#if STAGE == VERTEX
  run(i, case_index);
#endif
  V v; v.pos = float4(0, 0, 0, 1); v.id = i; return v;
}
float4 vs_fill(uint i : SV_VertexID, uint primitive : SV_InstanceID) : SV_Position {
  if (coverage == 0) return float4(i == 1 ? 3 : -1, i == 2 ? -3 : 1, 0, 1);
  uint2 quad = uint2(primitive % (TARGET / 2), primitive / (TARGET / 2));
  uint selected = primitive % 4;
  float2 center = 2 * quad + uint2(selected & 1, selected >> 1) + 0.5;
  float2 p = center + float2(i == 1 ? 0.2 : -0.1, i == 2 ? 0.2 : -0.1);
  if (coverage >= 2) p = 2 * quad + float2(i == 1 ? 2.25 : 0, i == 2 ? 2.25 : 0);
  return float4(p / TARGET * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps(float4 pos : SV_Position) : SV_Target {
  if (coverage == 3 && (uint(pos.x) & 1) != 0) discard;
  uint t = uint(pos.y) * TARGET + uint(pos.x);
  run(t, case_index); return t + 1;
}
[maxvertexcount(1)] void gs(point V vertices[1], inout PointStream<V> stream) {
  run(vertices[0].id, case_index); stream.Append(vertices[0]);
}
struct PC { float edges[2] : SV_TessFactor; };
PC pc(InputPatch<V, 1> vertices) {
  PC p; p.edges[0] = p.edges[1] = 1;
#if STAGE == HULL
  run(vertices[0].id, case_index);
#endif
  return p;
}
[domain("isoline")] [partitioning("integer")] [outputtopology("point")]
[outputcontrolpoints(1)] [patchconstantfunc("pc")]
V hs(InputPatch<V, 1> vertices) { return vertices[0]; }
[domain("isoline")] V ds(PC p, float2 location : SV_DomainLocation, const OutputPatch<V, 1> vertices) {
#if STAGE == DOMAIN
  if (location.x == 0) run(vertices[0].id, case_index);
#endif
  return vertices[0];
}
struct Payload { uint unused; };
groupshared Payload payload;
[numthreads(MESH_GROUP, 1, 1)] void amplify(uint i : SV_DispatchThreadID, uint local : SV_GroupIndex) {
  run(i, case_index);
  if (local == 0) payload.unused = 0;
  DispatchMesh(0, 1, 1, payload);
}
[numthreads(MESH_GROUP, 1, 1)] [outputtopology("triangle")]
void mesh(uint i : SV_DispatchThreadID, out vertices V vertices[3], out indices uint3 indices[1]) {
#if STAGE == MESH
  run(i, case_index);
#endif
  // no case outputs a triangle, but the validator wants a path that writes each declared output
  uint count = case_index == ~0u ? 3 : 0;
  SetMeshOutputCounts(count, count / 3);
  if (i < count) vertices[i] = (V)0;
  if (i < count / 3) indices[i] = uint3(0, 1, 2);
}
)hlsl";

static std::string
native_overload(const Compiler &compiler, const std::string &code, bool quad) {
  auto ir = dxil_disassemble(compiler, code);
  if (ir.empty())
    return {};
  const std::string name = quad ? "quadReadLaneAt" : "waveActiveBit";
  const std::string opcode = quad ? "122" : "120", type = quad ? "i1" : "i16", argument = quad ? "i32" : "i8";
  const std::regex call(
      "(%[\\w.$-]+) = call i32 @dx\\.op\\." + name + "\\.i32\\(i32 " + opcode + ", i32 ([%\\w.$-]+), " + argument +
      " ([%\\w.$-]+)\\)([^\\r\\n]*)"
  );
  std::string rewritten;
  size_t at = 0, count = 0;
  for (auto it = std::sregex_iterator(ir.begin(), ir.end(), call); it != std::sregex_iterator(); ++it) {
    const auto &m = *it;
    const std::string input = "%native.input." + std::to_string(count),
                      output = "%native.output." + std::to_string(count++);
    rewritten.append(ir, at, m.position() - at);
    rewritten += input + " = trunc i32 " + m[2].str() + " to " + type + "\n  " + output + " = call " + type +
                 " @dx.op." + name + "." + type + "(i32 " + opcode + ", " + type + " " + input + ", " + argument + " " +
                 m[3].str() + ")" + m[4].str() + "\n  " + m[1].str() + " = zext " + type + " " + output + " to i32";
    at = m.position() + m.length();
  }
  if (!count) {
    if (!expect(
            ir.find("@dx.op." + name + ".i32") == std::string::npos, "unrecognized %s native-overload seed",
            name.c_str()
        ))
      return {};
    return code;
  }
  rewritten.append(ir, at, std::string::npos);
  // the validator refuses a declaration nothing calls, which the seed's is once every call is narrowed
  if (rewritten.find("call i32 @dx.op." + name + ".i32(") == std::string::npos)
    rewritten = std::regex_replace(rewritten, std::regex("declare i32 @dx\\.op\\." + name + "\\.i32\\([^\\n]*\\n"), "");
  rewritten += "\ndeclare " + type + " @dx.op." + name + "." + type + "(i32, " + type + ", " + argument + ")\n";
  return dxil_assemble(rewritten, (name + "." + type).c_str());
}

struct Type {
  const char *name;
  UINT width;
  bool real, signed_integer;
  std::array<std::array<std::string, 7>, stages> code;
};

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE K, class T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind = K;
  T data{};
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: typed wave intrinsics require DXC\n");
    return 77;
  }
  Type types[] = {{"bool", 1},        {"int16_t", 16, false, true}, {"uint16_t", 16}, {"int", 32, false, true},
                  {"uint", 32},       {"int64_t", 64, false, true}, {"uint64_t", 64}, {"float16_t", 16, true},
                  {"float", 32, true}};
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  // DirectX-Specs MeshShader: "The threadgroup dimensions are limited by X*Y*Z <= 128."
  const UINT target = UINT(std::sqrt(group)), mesh_group = 128;
  const char *stage_names[] = {"compute", "pixel", "vertex", "geometry", "hull", "domain", "mesh", "amplification"};
  const char *entries[stages][7] = {
      {"cs"},
      {"vs_fill", nullptr, nullptr, nullptr, "ps"},
      {"vs"},
      {"vs", nullptr, nullptr, "gs"},
      {"vs", "hs", "ds"},
      {"vs", "hs", "ds"},
      {nullptr, nullptr, nullptr, nullptr, nullptr, "mesh"},
      {nullptr, nullptr, nullptr, nullptr, nullptr, "mesh", "amplify"}
  };
  auto quads = [](UINT stage) { return stage == compute || stage == pixel || stage >= mesh; };
  const char *names[] = {"LEADER",  "LANE_INDEX", "LANE_COUNT",     "FIRST",   "ANY",     "ALL",
                         "EQUAL",   "READ_FIRST", "READ_AT",        "SUM",     "PRODUCT", "MINIMUM",
                         "MAXIMUM", "PREFIX_SUM", "PREFIX_PRODUCT", "BIT_AND", "BIT_OR",  "BIT_XOR"};
  for (auto &type : types) {
    std::vector<std::string> defines = {
        std::string("T=") + type.name,
        "WIDTH=" + std::to_string(type.width),
        "REAL=" + std::to_string(type.real),
        "GROUP=" + std::to_string(group),
        "TARGET=" + std::to_string(target),
        "MESH_GROUP=" + std::to_string(mesh_group),
        "FIELDS=" + std::to_string(fields),
        "ACTIVE_BITS=" + std::to_string(!type.real && (type.width == 16 || (!type.signed_integer && type.width >= 32))),
        "TYPE_MASK=" + std::to_string(type.width == 16 ? UINT16_MAX : UINT32_MAX),
        "BOOL_TYPE=" + std::to_string(type.width == 1)
    };
    for (UINT f = 0; f < std::size(names); f++)
      defines.push_back(std::string(names[f]) + "=" + std::to_string(f));
    for (auto [name, f] : std::initializer_list<std::pair<const char *, Field>>{
             {"BALLOT", ballot},
             {"MATCH", match},
             {"MULTI_SUM", multi_sum},
             {"MULTI_PRODUCT", multi_product},
             {"MULTI_AND", multi_and},
             {"MULTI_OR", multi_or},
             {"MULTI_XOR", multi_xor},
             {"MULTI_COUNT", multi_count},
             {"COUNT", count},
             {"PREFIX_COUNT", prefix_count}
         })
      defines.push_back(std::string(name) + "=" + std::to_string(f));
    for (auto [name, f] : std::initializer_list<std::pair<const char *, Field>>{
             {"QUAD_X", quad_x},
             {"QUAD_Y", quad_y},
             {"QUAD_DIAGONAL", quad_diagonal},
             {"QUAD_LANE", quad_lane},
             {"QUAD_DYNAMIC", quad_dynamic}
         })
      defines.push_back(std::string(name) + "=" + std::to_string(f));
    for (UINT stage = 0; stage < stages; stage++) {
      auto macros = defines;
      macros.insert(
          macros.end(),
          {"STAGE=" + std::to_string(stage), "VERTEX=" + std::to_string(vertex), "HULL=" + std::to_string(hull),
           "DOMAIN=" + std::to_string(domain), "MESH=" + std::to_string(mesh), "QUADS=" + std::to_string(quads(stage))}
      );
      for (UINT part = 0; part < type.code[stage].size(); part++)
        if (const char *entry = entries[stage][part]) {
          std::string profile = std::string(part == 5 ? "ms" : part == 6 ? "as" : std::string(entry, 2)) + "_6_6";
          step("compile %s %s %s", type.name, stage_names[stage], entry);
          std::vector<std::wstring> flags{L"-denorm", L"ftz"};
          if (type.width == 16)
            flags.push_back(L"-enable-16bit-types");
          type.code[stage][part] = compiler.compile(hlsl, entry, profile.c_str(), macros, flags);
          if (!expect(!type.code[stage][part].empty(), "%s %s HLSL", type.name, entry))
            return verdict();
          if ((type.width == 16 && !type.real) || (type.width == 1 && quads(stage))) {
            type.code[stage][part] = native_overload(compiler, type.code[stage][part], type.width == 1);
            if (!expect(!type.code[stage][part].empty(), "%s %s native overload", type.name, entry))
              return verdict();
          }
        }
    }
  }
  ComPtr<ID3D12Device2> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)));
  if (!expect(
          options.WaveOps && options.Int64ShaderOps && options4.Native16BitShaderOpsSupported &&
              options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED && target * target == group,
          "required wave, int64 and native16 capabilities"
      ) ||
      !expect(
          options.WaveLaneCountMin >= 4 && options.WaveLaneCountMax <= 128, "wave range %u..%u",
          options.WaveLaneCountMin, options.WaveLaneCountMax
      ))
    return verdict();
  std::vector<std::array<UINT, 2>> cases;
  for (UINT n :
       {1u, options.WaveLaneCountMin - 1, options.WaveLaneCountMin, options.WaveLaneCountMin + 1,
        options.WaveLaneCountMax - 1, options.WaveLaneCountMax, options.WaveLaneCountMax + 1, group - 1, group})
    for (UINT mask = 0; mask < 4; mask++)
      cases.push_back({n, mask});
  std::sort(cases.begin(), cases.end());
  cases.erase(std::unique(cases.begin(), cases.end()), cases.end());
  auto sizes =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cases.size() * sizeof(cases[0]), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(sizes), "wave case buffer"))
    return verdict();
  void *mapped;
  CHECK(sizes->Map(0, nullptr, &mapped));
  memcpy(mapped, cases.data(), cases.size() * sizeof(cases[0]));
  sizes->Unmap(0, nullptr);
  const UINT64 total = cases.size() * group, bytes = total * fields * sizeof(uint64_t);
  auto input =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total * sizeof(uint64_t), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(sizes && input && initial && output && readback, "wave buffers"))
    return verdict();
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  initial->Unmap(0, nullptr);
  D3D12_ROOT_PARAMETER params[] = {
      {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  params[1].Descriptor.ShaderRegister = 1;
  params[3].Constants.Num32BitValues = 3;
  auto rs = root_signature(device.Get(), {std::size(params), params});
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC image_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      target,
      target,
      1,
      1,
      DXGI_FORMAT_R32_UINT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  ComPtr<ID3D12Resource> image;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &image_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&image)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(image.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 image_bytes;
  device->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  auto image_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, image_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(image_readback), "wave coverage readback"))
    return verdict();
  for (auto &type : types) {
    const uint64_t mask = UINT64_MAX >> (sizeof(uint64_t) * CHAR_BIT - type.width), sign = uint64_t(1)
                                                                                           << (type.width - 1);
    auto encoded = [&](double v) -> uint64_t {
      return type.width == 16 ? std::bit_cast<uint16_t>(_Float16(v)) : std::bit_cast<uint32_t>(float(v));
    };
    auto decoded = [&](uint64_t v) -> double {
      return type.width == 16 ? double(std::bit_cast<_Float16>(uint16_t(v)))
                              : double(std::bit_cast<float>(uint32_t(v)));
    };
    auto numeric = [&](uint64_t bits) {
      double value = decoded(bits);
      return type.width == 32 && std::fpclassify(std::bit_cast<float>(UINT(bits))) == FP_SUBNORMAL
                 ? std::copysign(0.0, value)
                 : value;
    };
    auto combine = [&](UINT op, uint64_t a, uint64_t b) -> uint64_t {
      if (type.real) {
        double x = numeric(a), y = numeric(b);
        auto bits = encoded(op == 0 ? x + y : op == 1 ? x * y : op == 2 ? std::min(x, y) : std::max(x, y));
        return type.width == 32 && std::fpclassify(std::bit_cast<float>(UINT(bits))) == FP_SUBNORMAL ? bits & sign
                                                                                                     : bits;
      }
      bool less = (a ^ (type.signed_integer ? sign : 0)) < (b ^ (type.signed_integer ? sign : 0));
      return (op == 0   ? a + b
              : op == 1 ? a * b
              : op == 2 ? (less ? a : b)
              : op == 3 ? (less ? b : a)
              : op == 4 ? a & b
              : op == 5 ? a | b
                        : a ^ b) &
             mask;
    };
    const uint64_t zero = type.real ? encoded(0) : 0, one = type.real ? encoded(1) : 1;
    const uint64_t edge[] = {0, 1, mask, sign, sign - 1, sign + 1, mask >> 1, uint64_t(1) << (type.width / 2)};
    for (UINT pattern = 0; pattern < (type.real ? 8u : 3u); pattern++) {
      bool movements_only = type.width == 1 || (type.real && (pattern == 3 || (type.width == 32 && pattern == 6)));
      step("%s pattern %u, partial waves and four activity masks", type.name, pattern);
      std::vector<uint64_t> values(total);
      for (UINT64 i = 0; i < total; i++)
        values[i] = type.real      ? encoded(
                                         pattern == 0   ? 1
                                         : pattern == 1 ? (i & 1 ? -1 : 2)
                                                        : 0
                                     )
                    : pattern == 0 ? mask
                    : pattern == 1 ? edge[i % std::size(edge)]
                                   : (uint64_t(1) << (i % type.width));
      if (type.real && movements_only) {
        const uint64_t half[] = {0,      0x8000, 1,      0x8001, 0x3ff,  0x83ff, 0x400,
                                 0x8400, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e01, 0x7c01};
        const uint64_t single[] = {0,           0x80000000u, 1,           0x80000001u, 0x7fffffu,
                                   0x807fffffu, 0x800000u,   0x80800000u, 0x7f7fffffu, 0xff7fffffu,
                                   0x7f800000u, 0xff800000u, 0x7fc00001u, 0x7f800001u};
        for (UINT64 i = 0; i < total; i++)
          values[i] = type.width == 16 ? half[i % std::size(half)] : single[i % std::size(single)];
      }
      if (type.real && pattern >= 4) {
        const uint64_t infinity = type.width == 16 ? 0x7c00u : 0x7f800000u;
        const UINT fraction_bits = type.width == 16 ? 10 : std::numeric_limits<float>::digits - 1;
        for (UINT64 i = 0; i < total; i++)
          values[i] =
              (i & 1 ? sign : 0) | (pattern == 4   ? infinity
                                    : pattern == 5 ? infinity | 1 | (i & 2 ? uint64_t(1) << (fraction_bits - 1) : 0)
                                    : pattern == 6 ? 1
                                                   : 0);
      }
      if (!type.real)
        for (auto &value : values)
          value &= mask;
      CHECK(input->Map(0, nullptr, &mapped));
      memcpy(mapped, values.data(), total * sizeof(uint64_t));
      input->Unmap(0, nullptr);
      for (UINT stage = 0; stage < stages; stage++) {
        auto &code = type.code[stage];
        auto byte = [&](UINT part) { return code[part].empty() ? D3D12_SHADER_BYTECODE{} : bytecode(code[part]); };
        ComPtr<ID3D12PipelineState> pso;
        if (stage == compute) {
          D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), byte(0)};
          CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
        } else if (stage >= mesh) {
          struct {
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> root;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
            Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
          } stream;
          stream.root.data = rs.Get(), stream.ms.data = byte(5), stream.as.data = byte(6);
          stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
          D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
          CHECK(device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)));
        } else {
          D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
          desc.pRootSignature = rs.Get();
          desc.VS = byte(0), desc.HS = byte(1), desc.DS = byte(2), desc.GS = byte(3), desc.PS = byte(4);
          desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
          desc.SampleMask = UINT_MAX;
          desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
          desc.PrimitiveTopologyType = stage == pixel  ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                       : stage >= hull ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                                       : D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
          if (stage == pixel)
            desc.NumRenderTargets = 1, desc.RTVFormats[0] = image_desc.Format;
          desc.SampleDesc = {1, 0};
          CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
        }
        D3D12_INDIRECT_ARGUMENT_DESC argument{
            stage == compute ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH
            : stage >= mesh  ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH
                             : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW
        };
        using Arguments = std::array<UINT, 4>;
        D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(Arguments), 1, &argument};
        ComPtr<ID3D12CommandSignature> signature;
        CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
        for (UINT coverage = 0; coverage < (stage == pixel ? 4u : 1u); coverage++) {
          const UINT instances = stage == pixel && coverage ? group / 4 : 1;
          auto visible = [&](UINT t) {
            if (stage != pixel || coverage == 0)
              return true;
            const UINT x = t % target, y = t / target, position = (y % 2) * 2 + x % 2;
            const UINT primitive = (y / 2) * (target / 2) + x / 2;
            return (coverage == 1 ? position == primitive % 4 : position != 3) && (coverage != 3 || x % 2 == 0);
          };
          std::vector<Arguments> arguments;
          if (stage == compute)
            arguments.push_back({UINT(cases.size()), 1, 1, 0});
          else
            for (auto c : cases)
              arguments.push_back(
                  {stage == pixel  ? 3u
                   : stage >= mesh ? (c[0] + mesh_group - 1) / mesh_group
                                   : c[0],
                   instances, stage >= mesh ? 1u : 0u, 0}
              );
          auto indirect = buffer(
              device.Get(), D3D12_HEAP_TYPE_UPLOAD, arguments.size() * sizeof(Arguments),
              D3D12_RESOURCE_STATE_GENERIC_READ
          );
          if (!expect(bool(indirect), "wave indirect buffer"))
            return verdict();
          CHECK(indirect->Map(0, nullptr, &mapped));
          memcpy(mapped, arguments.data(), arguments.size() * sizeof(Arguments));
          indirect->Unmap(0, nullptr);
          for (UINT path = 0; path < (stage == compute ? 2u : 3u); path++) {
            step(
                "%s pattern %u %s waves coverage %u, %s", type.name, pattern, stage_names[stage], coverage,
                path == 0   ? "direct"
                : path == 1 ? "indirect"
                            : "bundle"
            );
            CHECK(forget(readback.Get()));
            if (stage == pixel)
              CHECK(forget(image_readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pso.Get()));
            list->CopyResource(output.Get(), initial.Get());
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ComPtr<ID3D12CommandAllocator> bundle_allocator;
            ComPtr<ID3D12GraphicsCommandList6> bundle;
            if (path == 2) {
              CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
              CHECK(device->CreateCommandList(
                  0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(), IID_PPV_ARGS(&bundle)
              ));
            }
            auto commands = path == 2 ? bundle.Get() : list.Get();
            if (stage == pixel) {
              const float clear[4] = {};
              list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            }
            if (stage == compute) {
              commands->SetComputeRootSignature(rs.Get());
              commands->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
              commands->SetComputeRootShaderResourceView(1, sizes->GetGPUVirtualAddress());
              commands->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
              commands->SetComputeRoot32BitConstant(3, movements_only, 0);
              if (path == 1)
                commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
              else
                commands->Dispatch(cases.size(), 1, 1);
            } else {
              commands->SetGraphicsRootSignature(rs.Get());
              commands->SetGraphicsRootShaderResourceView(0, input->GetGPUVirtualAddress());
              commands->SetGraphicsRootShaderResourceView(1, sizes->GetGPUVirtualAddress());
              commands->SetGraphicsRootUnorderedAccessView(2, output->GetGPUVirtualAddress());
              commands->SetGraphicsRoot32BitConstant(3, movements_only, 0);
              commands->SetGraphicsRoot32BitConstant(3, coverage, 2);
              if (stage < mesh)
                commands->IASetPrimitiveTopology(
                    stage == pixel  ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                    : stage >= hull ? D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
                                    : D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                );
              D3D12_VIEWPORT viewport{0, 0, float(target), float(target), 0, 1};
              D3D12_RECT scissor{0, 0, LONG(target), LONG(target)};
              list->RSSetViewports(1, &viewport);
              list->RSSetScissorRects(1, &scissor);
              if (stage == pixel)
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
              for (UINT c = 0; c < cases.size(); c++) {
                commands->SetGraphicsRoot32BitConstant(3, c, 1);
                if (path == 1)
                  commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), c * sizeof(Arguments), nullptr, 0);
                else if (stage >= mesh)
                  commands->DispatchMesh((cases[c][0] + mesh_group - 1) / mesh_group, 1, 1);
                else
                  commands->DrawInstanced(stage == pixel ? 3 : cases[c][0], instances, 0, 0);
              }
            }
            if (bundle) {
              CHECK(bundle->Close());
              list->ExecuteBundle(bundle.Get());
            }
            transition(
                list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            list->CopyResource(readback.Get(), output.Get());
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            if (stage == pixel) {
              transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
              D3D12_TEXTURE_COPY_LOCATION from{image.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
              D3D12_TEXTURE_COPY_LOCATION to{
                  image_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}
              };
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
              transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            if (stage == pixel) {
              uint8_t *pixels;
              CHECK(image_readback->Map(0, nullptr, (void **)&pixels));
              bool correct = true;
              for (UINT t = 0; t < group; t++) {
                UINT value;
                memcpy(
                    &value,
                    pixels + footprint.Offset + (t / target) * footprint.Footprint.RowPitch +
                        (t % target) * sizeof(UINT),
                    sizeof(value)
                );
                if (!expect(
                        value == (visible(t) ? t + 1 : 0), "coverage %u pixel %u: %u expected %u", coverage, t, value,
                        visible(t) ? t + 1 : 0
                    )) {
                  correct = false;
                  break;
                }
              }
              image_readback->Unmap(0, nullptr);
              if (!correct)
                return verdict();
            }
            uint64_t *got;
            CHECK(readback->Map(0, nullptr, (void **)&got));
            for (UINT c = 0; c < cases.size(); c++) {
              auto [n, activity] = cases[c];
              auto row = [&](UINT t) { return got + (c * group + t) * fields; };
              auto active = [&](UINT lane, UINT width) {
                return activity == 0 || (activity == 1 ? (lane & 1) != 0 : activity == 2 && lane + 1 == width);
              };
              std::map<UINT, std::map<UINT, UINT>> waves;
              for (UINT t = 0; t < n; t++) {
                auto r = row(t);
                if (!visible(t)) {
                  for (UINT f = 0; f < fields; f++)
                    if (!expect(r[f] == UINT64_MAX, "case %u helper/discarded pixel %u wrote field %u", c, t, f)) {
                      readback->Unmap(0, nullptr);
                      return verdict();
                    }
                  continue;
                }
                if (stage == compute || stage >= mesh)
                  expect(r[lane_index] % 4 == t % 4, "case %u thread %u violates 1D quad lane mapping", c, t);
                if (!expect(
                        r[leader] < n && visible(UINT(r[leader])) && r[lane_count] >= options.WaveLaneCountMin &&
                            r[lane_count] <= options.WaveLaneCountMax && r[lane_index] < r[lane_count],
                        "case %u thread %u: leader/width/lane %llu/%llu/%llu", c, t, r[leader], r[lane_count],
                        r[lane_index]
                    )) {
                  readback->Unmap(0, nullptr);
                  return verdict();
                }
                expect(waves[r[leader]].emplace(r[lane_index], t).second, "case %u duplicate lane", c);
              }
              for (auto &[wave, lanes] : waves) {
                std::map<UINT, UINT> live;
                for (auto [lane, t] : lanes)
                  if (active(lane, row(t)[lane_count]))
                    live.emplace(lane, t);
                for (auto [lane, t] : lanes) {
                  auto r = row(t);
                  std::array<uint64_t, fields> want;
                  std::array<bool, fields> ignore{};
                  want.fill(UINT64_MAX);
                  want[leader] = wave, want[lane_index] = lane, want[lane_count] = r[lane_count];
                  if (quads(stage)) {
                    // quad operations require uniform flow across all four lanes.
                    bool complete_quad = true;
                    if (stage == pixel)
                      complete_quad = (t / target / 2 * 2 + 1) * target + t % target / 2 * 2 + 1 < n;
                    else
                      for (UINT k = 0; k < 4; k++)
                        complete_quad &= lanes.count((lane & ~3u) + k) != 0;
                    auto moved = [&](UINT field, UINT source) {
                      if (!complete_quad) {
                        ignore[field] = true;
                        return;
                      }
                      if (stage == pixel) {
                        const UINT position = field == quad_x          ? ((t % 2) ^ 1) + ((t / target) % 2) * 2
                                              : field == quad_y        ? t % 2 + (((t / target) % 2) ^ 1) * 2
                                              : field == quad_diagonal ? ((t % 2) ^ 1) + (((t / target) % 2) ^ 1) * 2
                                              : field == quad_dynamic  ? (t + c) % 4
                                                                       : field - quad_lane;
                        const UINT pixel =
                            (t / target / 2 * 2 + position / 2) * target + (t % target / 2 * 2 + position % 2);
                        if (pixel >= n)
                          ignore[field] = true;
                        else
                          want[field] = values[c * group + pixel];
                      } else {
                        auto found = lanes.find(source);
                        if (found == lanes.end())
                          ignore[field] = true;
                        else
                          want[field] = values[c * group + found->second];
                      }
                    };
                    if (type.width != 1) {
                      moved(quad_x, lane ^ 1);
                      moved(quad_y, lane ^ 2);
                      moved(quad_diagonal, lane ^ 3);
                    }
                    for (UINT k = 0; k < 4; k++)
                      moved(quad_lane + k, (lane & ~3u) + k);
                    moved(quad_dynamic, (lane & ~3u) + (t + c) % 4);
                  }
                  if (live.count(lane)) {
                    auto v = values[c * group + t];
                    want[read_first] = values[c * group + live.begin()->second];
                    want[read_at] = values[c * group + live.rbegin()->second];
                    if (type.width == 1) {
                      want[equal] = 1;
                      for (auto [other, u] : live)
                        want[equal] &= values[c * group + u] == v;
                    }
                    bool ambiguous_match = type.real && std::isnan(decoded(v));
                    if (type.real && decoded(v) == 0)
                      for (auto [other, u] : live)
                        ambiguous_match |= decoded(values[c * group + u]) == 0 && values[c * group + u] != v;
                    if (type.real && (movements_only || ambiguous_match)) {
                      std::array<uint64_t, 4> active_mask{};
                      for (auto [other, u] : live)
                        active_mask[other / 32] |= uint64_t(1) << (other % 32);
                      bool valid = expect(
                          (r[match + lane / 32] & (uint64_t(1) << (lane % 32))) != 0,
                          "case %u lane %u IEEE WaveMatch omits itself", c, lane
                      );
                      for (UINT k = 0; k < active_mask.size(); k++) {
                        valid &= expect(
                            (r[match + k] & ~active_mask[k]) == 0,
                            "case %u lane %u IEEE WaveMatch word %u includes inactive lanes", c, lane, k
                        );
                        ignore[match + k] = true;
                      }
                      if (!valid) {
                        readback->Unmap(0, nullptr);
                        return verdict();
                      }
                    }
                    if (type.real && std::isnan(decoded(v)))
                      ignore[equal] = true;
                    if (!movements_only) {
                      want[first] = lane == live.begin()->first;
                      want[any] = 0, want[all] = 1, want[equal] = 1;
                      want[sum] = zero, want[product] = one;
                      want[minimum] = want[maximum] = v;
                      want[prefix_sum] = want[multi_sum] = zero;
                      want[prefix_product] = want[multi_product] = one;
                      if (!type.real)
                        want[multi_and] = mask, want[multi_or] = want[multi_xor] = 0;
                      if (!type.real && (type.width == 16 || (!type.signed_integer && type.width >= 32)))
                        want[bit_and] = mask, want[bit_or] = want[bit_xor] = 0;
                      for (UINT k = 0; k < 4; k++)
                        want[ballot + k] = want[match + k] = 0;
                      want[count] = want[prefix_count] = want[multi_count] = 0;
                      for (auto [other, u] : live) {
                        auto x = values[c * group + u];
                        bool p = (u & 1) != 0;
                        want[any] |= p, want[all] &= p;
                        want[equal] &= type.real ? numeric(x) == numeric(v) : x == v;
                        if (p)
                          want[ballot + other / 32] |= uint64_t(1) << (other % 32);
                        if (x == v)
                          want[match + other / 32] |= uint64_t(1) << (other % 32);
                        want[count] += p;
                        for (UINT k = 0; k < 4; k++)
                          want[sum + k] = combine(k, want[sum + k], x);
                        if (!type.real && (type.width == 16 || (!type.signed_integer && type.width >= 32)))
                          for (UINT k = 0; k < 3; k++)
                            want[bit_and + k] = combine(4 + k, want[bit_and + k], x);
                        if (other >= lane)
                          continue;
                        want[prefix_count] += p;
                        want[prefix_sum] = combine(0, want[prefix_sum], x);
                        want[prefix_product] = combine(1, want[prefix_product], x);
                        if (u % 3 != t % 3)
                          continue;
                        want[multi_count] += p;
                        want[multi_sum] = combine(0, want[multi_sum], x);
                        want[multi_product] = combine(1, want[multi_product], x);
                        if (!type.real)
                          for (UINT k = 0; k < 3; k++)
                            want[multi_and + k] = combine(4 + k, want[multi_and + k], x);
                      }
                    }
                  }
                  for (UINT f = 0; f < fields; f++) {
                    if (ignore[f])
                      continue;
                    bool same = r[f] == want[f];
                    const bool arithmetic = (f >= sum && f <= prefix_product) || f == multi_sum || f == multi_product;
                    if (type.real && arithmetic && live.count(lane) && !movements_only) {
                      const double expected = decoded(want[f]), actual = decoded(r[f]);
                      if (std::isnan(expected))
                        same = std::isnan(actual);
                      if (expected == 0 && (f == sum || f == prefix_sum || f == multi_sum))
                        same = actual == 0;
                      if (expected == 0 && (f == minimum || f == maximum)) {
                        same = false;
                        for (auto [other, u] : live) {
                          auto input = values[c * group + u];
                          if (numeric(input) == 0)
                            same |= r[f] == input || (type.width == 32 && r[f] == (input & sign));
                        }
                      }
                      same &= (r[f] & ~mask) == 0;
                    }
                    if (!expect(
                            same, "case %u wave %u lane %u field %u: %#llx, want %#llx", c, wave, lane, f, r[f], want[f]
                        )) {
                      readback->Unmap(0, nullptr);
                      return verdict();
                    }
                  }
                }
              }
              for (UINT t = n; t < group; t++)
                for (UINT f = 0; f < fields; f++)
                  if (!expect(row(t)[f] == UINT64_MAX, "case %u inactive thread %u wrote field %u", c, t, f)) {
                    readback->Unmap(0, nullptr);
                    return verdict();
                  }
            }
            readback->Unmap(0, nullptr);
          }
        }
      }
    }
  }
  return verdict();
}
