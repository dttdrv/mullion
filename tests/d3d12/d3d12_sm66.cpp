// contract: the operations shader models 6.4 to 6.6 require. a compute wave, a third of its lanes sitting out,
// computes packed 8-bit dot products, dot2add, the pack and unpack intrinsics, WaveMatch and the WaveMultiPrefix
// operations over the groups WaveMatch finds; the expectations simulate them over the recorded lanes on the CPU. a
// triangle covering one pixel center then runs one live lane per quad: the other three are helpers, and IsHelperLane
// must say so on each. a shader that requires a wave size runs only at the device's, and one with 64-bit atomics builds
// like its 32-bit twin (d3d12_atomic64 checks what they do). float exchanges swap the bits, and a bitwise compare
// exchange swaps only on equal bits; each returns what memory held.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <map>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> o : register(u0);
[numthreads(LANES, 1, 1)]
void cs(uint t : SV_GroupIndex) {
  uint b = t * FIELDS, x = t * 0x9e3779b1u, y = t * 0x85ebca6bu + 7;
  int4 v = int4(t, t, t, t) * int4(37, -41, 3, -1) - 64;
  o[b + 0] = WaveGetLaneIndex();
  o[b + 1] = asuint(dot4add_i8packed(x, y, int(t)));
  o[b + 2] = dot4add_u8packed(x, y, t);
  float16_t2 h0 = float16_t2(t * 0.5, 1.5), h1 = float16_t2(0.25, -float(t));
  o[b + 3] = asuint(dot2add(h0, h1, 2.0f));
  o[b + 4] = pack_u8(uint4(v));
  o[b + 5] = pack_clamp_u8(v);
  o[b + 6] = pack_clamp_s8(v);
  int4 s = unpack_s8s32(x);
  uint4 u = unpack_u8u32(x);
  o[b + 7] = asuint(s.x + s.y * 3 + s.z * 5 + s.w * 7);
  o[b + 8] = u.x + u.y * 3 + u.z * 5 + u.w * 7;
  o[b + 9] = IsHelperLane();
  if (t % 3 != 1) {
    uint4 m = WaveMatch(t % 5 < 2 ? 7 : t / 3);
    o[b + 10] = m.x;
    o[b + 11] = m.y;
    o[b + 12] = WaveMultiPrefixSum(t + 1, m);
    o[b + 13] = WaveMultiPrefixProduct(t % 3 + 1, m);
    o[b + 14] = WaveMultiPrefixBitAnd(~(1u << (t % 32)), m);
    o[b + 15] = WaveMultiPrefixBitOr(1u << (t % 32), m);
    o[b + 16] = WaveMultiPrefixBitXor(x, m);
    o[b + 17] = WaveMultiPrefixCountBits((t & 1) != 0, m);
    o[b + 18] = asuint(WaveMultiPrefixSum(t * 0.5, m));
  }
}

[WaveSize(WAVE)] [numthreads(WAVE, 1, 1)] void sized(uint t : SV_GroupIndex) { o[t] = WaveActiveSum(1); }

#ifdef EXCHANGE
RWByteAddressBuffer raw : register(u0);
[numthreads(1, 1, 1)] void exchange() {
  float was[3];
  raw.InterlockedExchangeFloat(0, SWAPPED, was[0]);
  raw.InterlockedCompareExchangeFloatBitwise(4, HELD, SWAPPED, was[1]);
  raw.InterlockedCompareExchangeFloatBitwise(8, -HELD, SWAPPED, was[2]);
  raw.Store3(12, asuint(float3(was[0], was[1], was[2])));
}
#endif
#ifdef T
RWByteAddressBuffer raw : register(u0);
[numthreads(1, 1, 1)] void atomic() { T r; raw.ATOMIC(0, 1, r); raw.Store<T>(8, r); }
#endif

// one pixel center, (1.5, 1.5) of a SIZE x SIZE target, inside a small triangle
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2(1.4 + 0.2 * (id == 1) + 0.1 * (id == 2), 1.4 + 0.2 * (id == 2));
  return float4(p / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
}
// this lane's IsHelperLane in bit 0, then the quad's across x, across y and diagonally
uint ps() : SV_Target {
  uint h = IsHelperLane();
  return h | QuadReadAcrossX(h) << 1 | QuadReadAcrossY(h) << 2 | QuadReadAcrossDiagonal(h) << 3;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: shader model 6.4 to 6.6 operations are DXIL only\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  const UINT lanes = options.WaveLaneCountMin, fields = 19, size = 4;
  std::vector<std::string> defines = {
      "LANES=" + std::to_string(lanes), "FIELDS=" + std::to_string(fields), "SIZE=" + std::to_string(size),
      "WAVE=" + std::to_string(lanes)
  };
  // dot2add takes 16-bit halves
  auto compile = [&](const char *entry, const char *profile) {
    return compiler.compile(hlsl, entry, profile, defines, {L"-enable-16bit-types"});
  };
  auto cs = compile("cs", "cs_6_6"), vs = compile("vs", "vs_6_6"), ps = compile("ps", "ps_6_6");
  if (cs.empty() || vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  auto draw_rs = root_signature(device.Get(), {});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> cs_pso, draw_pso, sized_pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&cs_pso)));
  // the device's wave size, and another the shader model allows (4 to 128)
  unsigned failures = 0;
  for (UINT wave : {lanes, lanes == 128 ? 64 : lanes * 2}) {
    auto d = defines;
    d.back() = "WAVE=" + std::to_string(wave);
    auto sized = compiler.compile(hlsl, "sized", "cs_6_6", d, {L"-enable-16bit-types"});
    D3D12_COMPUTE_PIPELINE_STATE_DESC sized_desc{rs.Get(), bytecode(sized)};
    ComPtr<ID3D12PipelineState> pso;
    bool created = SUCCEEDED(device->CreateComputePipelineState(&sized_desc, IID_PPV_ARGS(&pso)));
    if (created != (wave == lanes) && ++failures)
      printf("a shader requiring waves of %u was %s\n", wave, created ? "accepted" : "refused");
    if (wave == lanes)
      sized_pso = pso;
  }
  for (bool wide : {false, true}) {
    auto d = defines;
    d.insert(d.end(), {wide ? "T=uint64_t" : "T=uint", wide ? "ATOMIC=InterlockedAdd64" : "ATOMIC=InterlockedAdd"});
    auto atomic = compiler.compile(hlsl, "atomic", "cs_6_6", d, {L"-enable-16bit-types"});
    D3D12_COMPUTE_PIPELINE_STATE_DESC atomic_desc{rs.Get(), bytecode(atomic)};
    ComPtr<ID3D12PipelineState> pso;
    if (atomic.empty() || FAILED(device->CreateComputePipelineState(&atomic_desc, IID_PPV_ARGS(&pso))))
      printf("the %u-bit atomic add was %s\n", wide ? 64 : 32, pso ? "accepted" : "refused"), failures++;
  }
  auto exchange_defines = defines;
  exchange_defines.insert(exchange_defines.end(), {"EXCHANGE", "SWAPPED=2.5", "HELD=1.5"});
  auto exchange = compiler.compile(hlsl, "exchange", "cs_6_6", exchange_defines, {L"-enable-16bit-types"});
  D3D12_COMPUTE_PIPELINE_STATE_DESC exchange_desc{rs.Get(), bytecode(exchange)};
  ComPtr<ID3D12PipelineState> exchange_pso;
  if (exchange.empty() || FAILED(device->CreateComputePipelineState(&exchange_desc, IID_PPV_ARGS(&exchange_pso))))
    printf("the float exchanges were refused\n"), failures++;
  if (!sized_pso || failures) {
    printf("failed: %u wrong pipelines\n", failures);
    return 1;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC draw_desc{draw_rs.Get(), bytecode(vs), bytecode(ps)};
  draw_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  draw_desc.SampleMask = ~0u;
  draw_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  draw_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  draw_desc.NumRenderTargets = 1;
  draw_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  draw_desc.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&draw_desc, IID_PPV_ARGS(&draw_pso)));

  const UINT64 bytes = lanes * fields * 4, row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  // the target's texels follow at the next placement boundary
  const UINT64 pixels_at = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1),
               sums_at = pixels_at + row * size, floats_at = sums_at + lanes * 4;
  const float held = 1.5f, swapped = 2.5f;
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, floats_at + 24, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 12, D3D12_RESOURCE_STATE_GENERIC_READ);
  float *start;
  CHECK(upload->Map(0, nullptr, (void **)&start));
  start[0] = start[1] = start[2] = held;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), cs_pso.Get(), IID_PPV_ARGS(&list)));
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, bytes);
  // the sized shader's wave sums, after the first output
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetPipelineState(sized_pso.Get());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), sums_at, out.Get(), 0, lanes * 4);
  // the exchanges, on three words that hold `held`
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(out.Get(), 0, upload.Get(), 0, 12);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetPipelineState(exchange_pso.Get());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), floats_at, out.Get(), 0, 24);
  // an integer target clears to the color's values
  const UINT untouched = 7;
  const float clear_value[4] = {untouched, untouched, untouched, untouched};
  list->ClearRenderTargetView(rtv, clear_value, 0, nullptr);
  list->SetPipelineState(draw_pso.Get());
  list->SetGraphicsRootSignature(draw_rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = {pixels_at, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  UINT *o;
  CHECK(readback->Map(0, nullptr, (void **)&o));

  for (UINT t = 0; t < lanes; t++)
    if (o[sums_at / 4 + t] != lanes && failures++ < 8)
      printf("thread %u of a sized wave counts %u lanes, want %u\n", t, o[sums_at / 4 + t], lanes);
  // memory: swapped, swapped on equal bits, kept on unequal ones; each returns held
  const float floats_want[6] = {swapped, swapped, held, held, held, held};
  for (UINT i = 0; i < 6; i++)
    if (reinterpret_cast<float *>(o)[floats_at / 4 + i] != floats_want[i] && failures++ < 8)
      printf("float exchange word %u: %g, want %g\n", i, reinterpret_cast<float *>(o)[floats_at / 4 + i], floats_want[i]);
  auto expect = [&](UINT t, UINT field, UINT want, const char *what) {
    if (o[t * fields + field] != want && failures++ < 8)
      printf("thread %u %s: %#x, want %#x\n", t, what, o[t * fields + field], want);
  };
  std::map<UINT, UINT> thread_of;
  for (UINT t = 0; t < lanes; t++)
    thread_of[o[t * fields]] = t;
  if (thread_of.size() != lanes)
    printf("failed: %zu distinct lanes of %u\n", thread_of.size(), lanes), failures++;
  auto active = [](UINT t) { return t % 3 != 1; };
  auto key = [](UINT t) { return t % 5 < 2 ? 7 : t / 3; };
  auto byte = [](UINT v, int i) { return (v >> 8 * i) & 0xff; };
  for (auto [lane, t] : thread_of) {
    UINT x = t * 0x9e3779b1u, y = t * 0x85ebca6bu + 7, dot_s = t, dot_u = t, pack = 0, clamp_u = 0, clamp_s = 0;
    int unpacked_s = 0;
    UINT unpacked_u = 0;
    const int scale[4] = {37, -41, 3, -1};
    for (int i = 0; i < 4; i++) {
      dot_s += (int8_t)byte(x, i) * (int8_t)byte(y, i);
      dot_u += byte(x, i) * byte(y, i);
      int v = (int)t * scale[i] - 64;
      pack |= ((UINT)v & 0xff) << 8 * i;
      clamp_u |= (UINT)std::clamp(v, 0, 255) << 8 * i;
      clamp_s |= ((UINT)std::clamp(v, -128, 127) & 0xff) << 8 * i;
      unpacked_s += (int8_t)byte(x, i) * (2 * i + 1);
      unpacked_u += byte(x, i) * (2 * i + 1);
    }
    expect(t, 1, dot_s, "dot4add_i8packed");
    expect(t, 2, dot_u, "dot4add_u8packed");
    expect(t, 3, std::bit_cast<UINT>(2.0f + t * 0.5f * 0.25f - 1.5f * t), "dot2add");
    expect(t, 4, pack, "pack_u8");
    expect(t, 5, clamp_u, "pack_clamp_u8");
    expect(t, 6, clamp_s, "pack_clamp_s8");
    expect(t, 7, (UINT)unpacked_s, "unpack_s8s32");
    expect(t, 8, unpacked_u, "unpack_u8u32");
    expect(t, 9, 0, "IsHelperLane in compute");
    if (!active(t))
      continue;
    // this lane's group: the active lanes with its key; the prefixes run over the group's lanes before this one
    uint64_t group = 0;
    UINT sum = 0, product = 1, and_ = ~0u, or_ = 0, xor_ = 0, odd = 0;
    float fsum = 0;
    for (auto [l, u] : thread_of)
      if (active(u) && key(u) == key(t)) {
        group |= 1ull << l;
        if (l < lane)
          sum += u + 1, product *= u % 3 + 1, and_ &= ~(1u << u % 32), or_ |= 1u << u % 32,
              xor_ ^= u * 0x9e3779b1u, odd += u & 1, fsum += u * 0.5f;
      }
    expect(t, 10, (UINT)group, "WaveMatch low");
    expect(t, 11, (UINT)(group >> 32), "WaveMatch high");
    expect(t, 12, sum, "WaveMultiPrefixSum");
    expect(t, 13, product, "WaveMultiPrefixProduct");
    expect(t, 14, and_, "WaveMultiPrefixBitAnd");
    expect(t, 15, or_, "WaveMultiPrefixBitOr");
    expect(t, 16, xor_, "WaveMultiPrefixBitXor");
    expect(t, 17, odd, "WaveMultiPrefixCountBits");
    expect(t, 18, std::bit_cast<UINT>(fsum), "WaveMultiPrefixSum float");
  }
  // the pixel at (1, 1) is live; its quad's other three are helpers, and nothing else runs
  auto pixels = o + pixels_at / 4;
  for (UINT py = 0; py < size; py++)
    for (UINT px = 0; px < size; px++) {
      UINT got = pixels[py * row / 4 + px], want = px == 1 && py == 1 ? 0b1110 : untouched;
      if (got != want && failures++ < 8)
        printf("pixel (%u, %u) helper lanes: %#x, want %#x\n", px, py, got, want);
    }
  readback->Unmap(0, nullptr);
  printf("%s: %u mismatches over %u lanes and %u pixels\n", failures ? "failed" : "passed", failures, lanes, size * size);
  return failures != 0;
}
