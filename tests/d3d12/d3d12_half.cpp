// contract: native 16-bit shader values (-enable-16bit-types) keep their bits on every path. a compute shader reads a
// constant buffer row packed with 16-bit members, a structured buffer of halves (stride 2) and a half texture, and
// writes one 16-bit element per thread next to its neighbours', halves at 2-byte raw offsets and a negative 16-bit
// texel, and takes 16-bit bit counts; a draw carries a half vector, a 16-bit integer and an interpolated half, which
// the pixel shader evaluates, from vertex to pixel shader into 16-bit targets. the
// expectations are the same values in C++'s _Float16 and int16_t.
#include "d3d12_test.hpp"
#include <bit>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float16_t4 h4; uint16_t u0; int16_t s0; float16_t f0; uint16_t u1; };
StructuredBuffer<float16_t> halves : register(t0);
Texture2D<float16_t4> texels : register(t1);
RWStructuredBuffer<uint16_t> shorts : register(u0);
RWByteAddressBuffer raw : register(u1);
RWTexture2D<int16_t> signed_texels : register(u2);
RWStructuredBuffer<uint> o : register(u3);
[numthreads(N, 1, 1)]
void cs(uint t : SV_GroupIndex) {
  float16_t h = halves[t] * f0;
  float16_t4 texel = texels.Load(int3(t, 0, 0));
  shorts[t] = (uint16_t)(t * 3 + u0);
  raw.Store<float16_t>(2 * t, h);
  signed_texels[uint2(t, 0)] = s0 - (int16_t)t;
  uint b = t * FIELDS;
  uint16_t u = (uint16_t)(t * 3 + u0);
  int16_t i = s0 - (int16_t)t;
  o[b + 0] = asuint16(h4.x) | (uint)asuint16(h4.w) << 16;
  o[b + 1] = u1 | (uint)asuint16(h) << 16;
  o[b + 2] = asuint16(texel.x) | (uint)asuint16(texel.w) << 16;
  o[b + 3] = asuint(int(signed_texels[uint2(t, 0)]));
  o[b + 4] = asuint16(raw.Load<float16_t>(2 * t));
  o[b + 5] = countbits(u);
  o[b + 6] = firstbitlow(u);
  o[b + 7] = firstbithigh(u);
  o[b + 8] = firstbithigh(i);
}

struct V {
  float4 pos : SV_Position;
  nointerpolation float16_t4 c : COLOR;
  nointerpolation int16_t s : SIGNED;
  float16_t e : EVALUATED;
};
V vs(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  V v;
  v.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  v.c = h4 * f0;
  v.s = s0;
  v.e = f0;
  return v;
}
struct Targets { float16_t4 c : SV_Target0; int16_t s : SV_Target1; };
Targets ps(V v) {
  Targets t;
  // an interpolated half, the same at every vertex, evaluated back to itself
  t.c = v.c * (EvaluateAttributeCentroid(v.e) / f0);
  t.s = v.s * (int16_t)2;
  return t;
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
    printf("skipped: native 16-bit types are shader model 6.2\n");
    return 77;
  }
  const UINT n = 16, fields = 9, size = 4;
  auto compile = [&](const char *entry, const char *profile) {
    return compiler.compile(
        hlsl, entry, profile, {"N=" + std::to_string(n), "FIELDS=" + std::to_string(fields)}, {L"-enable-16bit-types"}
    );
  };
  auto cs = compile("cs", "cs_6_2"), vs = compile("vs", "vs_6_2"), ps = compile("ps", "ps_6_2");
  if (cs.empty() || vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options, sizeof(options)));
  if (!options.Native16BitShaderOpsSupported) {
    printf("failed: native 16-bit shader operations not reported\n");
    return 1;
  }

  // the constant buffer: h4 (8 bytes), then u0, s0, f0 and u1 in the same 16-byte row
  using half = _Float16;
  const half h4[4] = {1.5, -2.25, 0.125, 65504}, f0 = -0.75;
  const uint16_t u0 = 1000, u1 = 0xbeef;
  const int16_t s0 = -300;
  auto bits = [](half h) { return (UINT)std::bit_cast<uint16_t>(h); };
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, h4, 8);
  memcpy(mapped + 8, &u0, 2);
  memcpy(mapped + 10, &s0, 2);
  memcpy(mapped + 12, &f0, 2);
  memcpy(mapped + 14, &u1, 2);
  // the structured buffer's halves, at byte 256
  auto input = [](UINT i) { return (half)(i / 4.0f - 1); };
  for (UINT i = 0; i < n; i++)
    ((half *)(mapped + 256))[i] = input(i);
  upload->Unmap(0, nullptr);

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    return res;
  };
  // the half texture's texel i is (h4.x + i, ..., h4.w), filled by a clear
  auto texels = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, n, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto signed_texels = texture(DXGI_FORMAT_R16_SINT, n, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, size, size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto integer = texture(DXGI_FORMAT_R16_SINT, size, size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto uav = [&](UINT64 bytes) {
    return buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  };
  auto shorts = uav(n * 2), raw = uav(n * 2), out = uav(n * fields * 4);

  ComPtr<ID3D12DescriptorHeap> views, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto cpu = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    auto handle = h->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += i * device->GetDescriptorHandleIncrementSize(type);
    return handle;
  };
  auto view = [&](UINT i) { return cpu(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, i); };
  auto rtv = [&](UINT i) { return cpu(rtvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, i); };
  D3D12_SHADER_RESOURCE_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  structured.Buffer = {128, n, 2}; // byte 256
  device->CreateShaderResourceView(upload.Get(), &structured, view(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC texel_view{DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  texel_view.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(texels.Get(), &texel_view, view(1));
  D3D12_UNORDERED_ACCESS_VIEW_DESC shorts_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  shorts_view.Buffer = {0, n, 2};
  device->CreateUnorderedAccessView(shorts.Get(), nullptr, &shorts_view, view(2));
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw_view{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
  raw_view.Buffer = {0, n / 2, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW};
  device->CreateUnorderedAccessView(raw.Get(), nullptr, &raw_view, view(3));
  device->CreateUnorderedAccessView(signed_texels.Get(), nullptr, nullptr, view(4));
  device->CreateRenderTargetView(texels.Get(), nullptr, rtv(0));
  device->CreateRenderTargetView(color.Get(), nullptr, rtv(1));
  device->CreateRenderTargetView(integer.Get(), nullptr, rtv(2));

  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3}};
  ranges[1].OffsetInDescriptorsFromTableStart = 2;
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  params[1].DescriptorTable = {2, ranges};
  params[2].Descriptor.ShaderRegister = 3;
  auto rs = root_signature(device.Get(), {3, params});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> cs_pso, draw_pso;
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&cs_pso)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC draw_desc{rs.Get(), bytecode(vs), bytecode(ps)};
  for (int i = 0; i < 2; i++)
    draw_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  draw_desc.SampleMask = ~0u;
  draw_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  draw_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  draw_desc.NumRenderTargets = 2;
  draw_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
  draw_desc.RTVFormats[1] = DXGI_FORMAT_R16_SINT;
  draw_desc.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&draw_desc, IID_PPV_ARGS(&draw_pso)));

  // the targets' texels follow the buffers at placement boundaries
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, out_bytes = n * fields * 4,
               place = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, color_at = (out_bytes + 4 * n + place - 1) & ~(place - 1),
               integer_at = color_at + (row * size + place - 1) / place * place;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, integer_at + row * size, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), cs_pso.Get(), IID_PPV_ARGS(&list)));
  for (UINT i = 0; i < n; i++) {
    const float value[4] = {(float)h4[0] + i, (float)h4[1], (float)h4[2], (float)h4[3]};
    D3D12_RECT texel{(LONG)i, 0, (LONG)i + 1, 1};
    list->ClearRenderTargetView(rtv(0), value, 1, &texel);
  }
  transition(list.Get(), texels.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootConstantBufferView(0, upload->GetGPUVirtualAddress());
  list->SetComputeRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  list->SetPipelineState(draw_pso.Get());
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {rtv(1), rtv(2)};
  list->OMSetRenderTargets(2, targets, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  for (auto res : {out.Get(), shorts.Get()})
    transition(list.Get(), res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (auto res : {color.Get(), integer.Get()})
    transition(list.Get(), res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, out_bytes);
  list->CopyBufferRegion(readback.Get(), out_bytes, shorts.Get(), 0, n * 2);
  for (auto [res, format, at] : {std::tuple{color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, color_at},
                                 std::tuple{integer.Get(), DXGI_FORMAT_R16_SINT, integer_at}}) {
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {at, {format, size, size, 1, (UINT)row}};
    D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  unsigned failures = 0;
  auto expect = [&](const char *what, UINT i, UINT value, UINT want) {
    if (value != want && failures++ < 8)
      printf("%s %u: %#x, want %#x\n", what, i, value, want);
  };
  auto o = (UINT *)got;
  auto short_at = (uint16_t *)(got + out_bytes);
  for (UINT t = 0; t < n; t++) {
    half h = input(t) * f0;
    auto r = o + t * fields;
    expect("constant buffer half4", t, r[0], bits(h4[0]) | bits(h4[3]) << 16);
    expect("constant buffer u1 and structured half", t, r[1], u1 | bits(h) << 16);
    expect("texture half4", t, r[2], bits((half)((float)h4[0] + t)) | bits(h4[3]) << 16);
    expect("signed 16-bit texel", t, r[3], (UINT)(int)(int16_t)(s0 - (int16_t)t));
    expect("raw half at 2 bytes", t, r[4], bits(h));
    // HLSL's bit positions count from the lowest bit; a signed value's high one is the first unlike its sign
    uint16_t u = t * 3 + u0;
    int16_t i = s0 - (int16_t)t;
    expect("countbits of 16 bits", t, r[5], std::popcount(u));
    expect("firstbitlow of 16 bits", t, r[6], std::countr_zero(u));
    expect("firstbithigh of 16 bits", t, r[7], 15 - std::countl_zero(u));
    expect("signed firstbithigh of 16 bits", t, r[8], 15 - std::countl_zero(uint16_t(i < 0 ? ~i : i)));
    expect("adjacent 16-bit store", t, short_at[t], (uint16_t)(t * 3 + u0));
  }
  auto color_row = (uint16_t *)(got + color_at);
  auto integer_row = (int16_t *)(got + integer_at);
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      UINT i = y * size + x;
      for (int c = 0; c < 4; c++)
        expect("half target", i, color_row[y * row / 2 + x * 4 + c], bits(h4[c] * f0));
      expect("16-bit integer target", i, (uint16_t)integer_row[y * row / 2 + x], (uint16_t)(s0 * 2));
    }
  readback->Unmap(0, nullptr);
  printf("%s: %u mismatches\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
