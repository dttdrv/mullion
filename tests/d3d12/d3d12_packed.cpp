// contract: pack_u8/pack_s8 "Pack lower 8 bits, drop unused bits"; clamp variants use "[0, 255]" and
// "[-128, 127]"; unpack is sign extended or non-sign extended (DirectX-Specs, Pack/Unpack Math Intrinsics,
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Pack_Unpack_Intrinsics.html).
// DXIL.rst calls dot4add a "dot product of 4 x i8 vectors packed into i32, with accumulate to i32".
// each 16-bit pattern and each byte in every position is enumerated, with 32-bit clamp and carry boundaries, and
// dot4add meets every pair of byte values in every lane.
// the CPU extracts, extends, clamps and packs independently. all eight stages write their result bytes.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <limits>

using Words = std::array<UINT, 4>;
enum Operation { pack32, pack16, unsigned32, signed32, unsigned16, signed16, dot4, operations };

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint4> inputs : register(t0);
RWStructuredBuffer<uint4> results : register(u0);
void run(uint i) {
  if (i >= COUNT) return;
  uint4 u = inputs[i];
  int4 s = asint(u);
  uint16_t4 hu = uint16_t4(u);
  int16_t4 hs = int16_t4(hu);
  uint b = i * OPS;
  results[b + PACK32] = uint4(pack_u8(u), pack_s8(s), pack_clamp_u8(s), pack_clamp_s8(s));
  results[b + PACK16] = uint4(pack_u8(hu), pack_s8(hs), pack_clamp_u8(hs), pack_clamp_s8(hs));
  results[b + UNSIGNED32] = unpack_u8u32(u.x);
  results[b + SIGNED32] = uint4(unpack_s8s32(u.x));
  results[b + UNSIGNED16] = uint4(unpack_u8u16(u.x));
  results[b + SIGNED16] = uint4(int4(unpack_s8s16(u.x)));
  results[b + DOT4] = uint4(dot4add_u8packed(u.x, u.y, u.z), asuint(dot4add_i8packed(u.x, u.y, s.z)),
                           dot4add_u8packed(u.z, u.w, u.x), asuint(dot4add_i8packed(u.z, u.w, s.x)));
}
[numthreads(GROUP, 1, 1)] void cs(uint i : SV_DispatchThreadID) { run(i); }
struct V { float4 pos : SV_Position; nointerpolation uint id : ID; };
V at(uint i) {
  V v;
  v.pos = float4((float2(i % WIDTH, i / WIDTH) + 0.5) / float2(WIDTH, HEIGHT) * float2(2, -2) + float2(-1, 1), 0, 1);
  v.id = i;
  return v;
}
V vs(uint i : SV_VertexID) {
#if STAGE == VERTEX
  run(i);
#endif
  return at(i);
}
float4 vs_fill(uint i : SV_VertexID) : SV_Position { return float4(i == 1 ? 3 : -1, i == 2 ? -3 : 1, 0, 1); }
uint ps(float4 pos : SV_Position) : SV_Target { run(uint(pos.y) * WIDTH + uint(pos.x)); return 1; }
[maxvertexcount(1)] void gs(point V v[1], inout PointStream<V> stream) { run(v[0].id); stream.Append(v[0]); }
struct PC { float edges[2] : SV_TessFactor; };
PC pc(InputPatch<V, 1> vertices) {
  PC p;
  p.edges[0] = p.edges[1] = 1;
#if STAGE == HULL
  run(vertices[0].id);
#endif
  return p;
}
[domain("isoline")] [partitioning("integer")] [outputtopology("point")] [outputcontrolpoints(1)] [patchconstantfunc("pc")]
V hs(InputPatch<V, 1> vertices) { return vertices[0]; }
[domain("isoline")] V ds(PC p, float2 location : SV_DomainLocation, const OutputPatch<V, 1> vertices) {
#if STAGE == DOMAIN
  if (location.x == 0) run(vertices[0].id);
#endif
  return vertices[0];
}
struct Payload { uint unused; };
groupshared Payload payload;
[numthreads(MESH_GROUP, 1, 1)] void amplify(uint i : SV_DispatchThreadID, uint t : SV_GroupIndex) {
  run(i);
  if (t == 0) payload.unused = 0;
  DispatchMesh(0, 1, 1, payload);
}
[numthreads(MESH_GROUP, 1, 1)] [outputtopology("triangle")]
void mesh(uint i : SV_DispatchThreadID, out vertices V vertices[3], out indices uint3 indices[1]) {
#if STAGE == MESH
  run(i);
#endif
  SetMeshOutputCounts(0, 0);
}
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE K, class T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind = K;
  T data{};
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: packed intrinsics require DXC\n");
    return 77;
  }
  const UINT byte_values = UINT8_MAX + 1, short_values = UINT16_MAX + 1;
  std::vector<Words> input;
  for (UINT i = 0; i < short_values; i++) {
    Words v;
    for (UINT c = 0; c < v.size(); c++) {
      UINT lo = (i + c * (byte_values + 1)) & UINT16_MAX;
      UINT hi = (i * (2 * c + 1) + c) & UINT16_MAX;
      v[c] = lo | hi << (sizeof(uint16_t) * CHAR_BIT);
    }
    input.push_back(v);
  }
  // dot4add's operands, words 0 and 1 and words 2 and 3: every pair of byte values meets in every lane
  for (UINT i = 0; i < short_values; i++) {
    UINT x = i & UINT8_MAX, y = i >> CHAR_BIT, nx = UINT8_MAX - x, ny = UINT8_MAX - y;
    auto word = [](UINT b0, UINT b1, UINT b2, UINT b3) {
      return b0 | b1 << CHAR_BIT | b2 << 2 * CHAR_BIT | b3 << 3 * CHAR_BIT;
    };
    input.push_back({word(x, y, nx, ny), word(y, x, y, x), word(ny, x, y, nx), word(x, ny, nx, y)});
  }
  std::vector<int32_t> edges = {
      INT32_MIN,
      INT32_MIN + 1,
      INT16_MIN - 1,
      INT16_MIN,
      INT16_MAX,
      INT16_MAX + 1,
      INT8_MIN - 1,
      INT8_MIN,
      INT8_MIN + 1,
      -1,
      0,
      1,
      INT8_MAX - 1,
      INT8_MAX,
      INT8_MAX + 1,
      UINT8_MAX - 1,
      UINT8_MAX,
      UINT8_MAX + 1,
      INT32_MAX - 1,
      INT32_MAX
  };
  for (UINT i = 0; i < edges.size(); i++)
    for (UINT c = 0; c < Words{}.size(); c++) {
      Words v{};
      v[c] = std::bit_cast<UINT>(edges[i]);
      input.push_back(v);
    }
  const UINT group = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
  // the mesh specification limits a threadgroup to 128 threads; one byte's bit patterns supply two such groups
  const UINT mesh_group = 128, width = byte_values, height = (input.size() + width - 1) / width;
  enum Stage { compute, pixel, vertex, geometry, hull, domain, mesh, amplification, stages };
  struct Shader {
    const char *name;
    std::string vs, hs, ds, gs, ps, cs, ms, as;
  };
  Shader shaders[stages] = {{"compute"}, {"pixel"},  {"vertex"}, {"geometry"},
                            {"hull"},    {"domain"}, {"mesh"},   {"amplification"}};
  std::vector<std::string> constants = {"COUNT=" + std::to_string(input.size()),    "WIDTH=" + std::to_string(width),
                                        "HEIGHT=" + std::to_string(height),         "GROUP=" + std::to_string(group),
                                        "MESH_GROUP=" + std::to_string(mesh_group), "OPS=" + std::to_string(operations),
                                        "VERTEX=" + std::to_string(vertex),         "HULL=" + std::to_string(hull),
                                        "DOMAIN=" + std::to_string(domain),         "MESH=" + std::to_string(mesh)};
  const char *operation_names[] = {"PACK32", "PACK16", "UNSIGNED32", "SIGNED32", "UNSIGNED16", "SIGNED16", "DOT4"};
  for (UINT i = 0; i < operations; i++)
    constants.push_back(std::string(operation_names[i]) + "=" + std::to_string(i));
  for (UINT stage = 0; stage < stages; stage++) {
    auto defines = constants;
    defines.push_back("STAGE=" + std::to_string(stage));
    auto compile = [&](const char *entry, const char *kind, std::string &code) {
      code = compiler.compile(hlsl, entry, kind, defines, {L"-enable-16bit-types"});
      return expect(!code.empty(), "%s %s HLSL", shaders[stage].name, entry);
    };
    auto &s = shaders[stage];
    step("compile packed operations in %s", s.name);
    if (stage == compute) {
      if (!compile("cs", "cs_6_6", s.cs))
        return verdict();
    } else if (stage >= mesh) {
      if (!compile("mesh", "ms_6_6", s.ms) || (stage == amplification && !compile("amplify", "as_6_6", s.as)))
        return verdict();
    } else {
      if (!compile(stage == pixel ? "vs_fill" : "vs", "vs_6_6", s.vs))
        return verdict();
      if (stage == pixel && !compile("ps", "ps_6_6", s.ps))
        return verdict();
      if (stage == geometry && !compile("gs", "gs_6_6", s.gs))
        return verdict();
      if ((stage == hull || stage == domain) && (!compile("hs", "hs_6_6", s.hs) || !compile("ds", "ds_6_6", s.ds)))
        return verdict();
    }
  }
  std::vector<Words> expected(input.size() * operations);
  auto byte = [](UINT x, UINT c) { return (x >> (c * CHAR_BIT)) & UINT8_MAX; };
  auto signed_byte = [&](UINT x, UINT c) { return int32_t(std::bit_cast<int8_t>(uint8_t(byte(x, c)))); };
  for (UINT i = 0; i < input.size(); i++) {
    auto &u = input[i];
    auto out = expected.data() + i * operations;
    for (UINT c = 0; c < u.size(); c++) {
      int32_t s = std::bit_cast<int32_t>(u[c]);
      int32_t h = std::bit_cast<int16_t>(uint16_t(u[c]));
      for (UINT mode = 0; mode < 4; mode++) {
        auto packed = [&](int32_t value) {
          return UINT(
                     mode == 2   ? std::clamp(value, 0, int32_t(UINT8_MAX))
                     : mode == 3 ? std::clamp(value, int32_t(INT8_MIN), int32_t(INT8_MAX))
                                 : value
                 ) &
                 UINT8_MAX;
        };
        out[pack32][mode] |= packed(s) << (c * CHAR_BIT);
        out[pack16][mode] |= packed(h) << (c * CHAR_BIT);
      }
      out[unsigned32][c] = out[unsigned16][c] = byte(u[0], c);
      out[signed32][c] = out[signed16][c] = UINT(signed_byte(u[0], c));
    }
    for (UINT k = 0; k < u.size(); k++) {
      UINT a = k < 2 ? u[0] : u[2], b = k < 2 ? u[1] : u[3], acc = k < 2 ? u[2] : u[0];
      for (UINT c = 0; c < u.size(); c++)
        acc += k & 1 ? UINT(signed_byte(a, c) * signed_byte(b, c)) : byte(a, c) * byte(b, c);
      out[dot4][k] = acc;
    }
  }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)));
  if (!expect(
          options4.Native16BitShaderOpsSupported && options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED,
          "native16 and mesh capabilities"
      ))
    return verdict();
  const UINT64 bytes = expected.size() * sizeof(Words);
  auto source =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, input.size() * sizeof(Words), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(source && initial && output && readback, "packed buffers"))
    return verdict();
  void *mapped;
  CHECK(source->Map(0, nullptr, &mapped));
  memcpy(mapped, input.data(), input.size() * sizeof(Words));
  source->Unmap(0, nullptr);
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xcd, bytes);
  initial->Unmap(0, nullptr);
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  auto rs = root_signature(device.Get(), {std::size(params), params});
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      width,
      height,
      1,
      1,
      DXGI_FORMAT_R32_UINT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT stage = 0; stage < stages; stage++) {
    auto &s = shaders[stage];
    ComPtr<ID3D12PipelineState> pso;
    if (stage == compute) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(s.cs)};
      CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    } else if (stage >= mesh) {
      struct {
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
        Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
      } stream;
      stream.rs.data = rs.Get(), stream.ms.data = bytecode(s.ms);
      if (!s.as.empty())
        stream.as.data = bytecode(s.as);
      stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
      CHECK(device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)));
    } else {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = rs.Get();
      auto code = [](const std::string &v) { return v.empty() ? D3D12_SHADER_BYTECODE{} : bytecode(v); };
      desc.VS = code(s.vs), desc.PS = code(s.ps), desc.HS = code(s.hs), desc.DS = code(s.ds), desc.GS = code(s.gs);
      desc.SampleMask = UINT_MAX;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.PrimitiveTopologyType = stage == pixel  ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                   : stage >= hull ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
                                                   : D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      if (stage == pixel)
        desc.NumRenderTargets = 1, desc.RTVFormats[0] = target_desc.Format;
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleDesc = {1, 0};
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    }
    for (UINT path = 0; path < (stage == compute ? 2u : 3u); path++) {
      step("%s packed operations, %s", s.name, path == 0 ? "direct" : path == 1 ? "indirect" : "bundle");
      CHECK(forget(readback.Get()));
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
      if (stage == compute) {
        commands->SetComputeRootSignature(rs.Get());
        commands->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
        commands->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
      } else {
        commands->SetGraphicsRootSignature(rs.Get());
        commands->SetGraphicsRootShaderResourceView(0, source->GetGPUVirtualAddress());
        commands->SetGraphicsRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        if (stage < mesh)
          commands->IASetPrimitiveTopology(
              stage == pixel  ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
              : stage >= hull ? D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
                              : D3D_PRIMITIVE_TOPOLOGY_POINTLIST
          );
        D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        if (stage == pixel)
          list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      }
      const UINT groups =
          (input.size() + (stage == compute ? group : mesh_group) - 1) / (stage == compute ? group : mesh_group);
      ComPtr<ID3D12CommandSignature> signature;
      ComPtr<ID3D12Resource> arguments;
      if (path == 1) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{
            stage == compute ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH
            : stage >= mesh  ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH
                             : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW
        };
        UINT words[] = {
            stage == compute || stage >= mesh ? groups
            : stage == pixel                  ? 3u
                                              : UINT(input.size()),
            1, stage == compute || stage >= mesh ? 1u : 0u, 0
        };
        const UINT argument_bytes =
            stage == compute || stage >= mesh ? sizeof(D3D12_DISPATCH_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS);
        D3D12_COMMAND_SIGNATURE_DESC desc{argument_bytes, 1, &argument};
        CHECK(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)));
        arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, argument_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!expect(bool(arguments), "indirect argument buffer"))
          return verdict();
        CHECK(arguments->Map(0, nullptr, &mapped));
        memcpy(mapped, words, argument_bytes);
        arguments->Unmap(0, nullptr);
        commands->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
      } else if (stage == compute)
        commands->Dispatch(groups, 1, 1);
      else if (stage >= mesh)
        commands->DispatchMesh(groups, 1, 1);
      else
        commands->DrawInstanced(stage == pixel ? 3 : input.size(), 1, 0, 0);
      if (bundle) {
        CHECK(bundle->Close());
        list->ExecuteBundle(bundle.Get());
      }
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      Words *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < expected.size(); i++)
        for (UINT c = 0; c < Words{}.size(); c++)
          if (!expect(
                  got[i][c] == expected[i][c], "input %u operation %u component %u: %#x, want %#x", i / operations,
                  i % operations, c, got[i][c], expected[i][c]
              )) {
            readback->Unmap(0, nullptr);
            return verdict();
          }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
