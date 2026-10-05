// contract: a vertex and pixel shader pair, bound through a root signature with a root CBV, an SRV table and a
// sampler table, renders a sampled value * tint at every pixel. the value comes from the mip that D3D's LOD rule
// selects (D3D11.3 7.18.11, 5.8.2): the LOD from the screen footprint, the shader or its gradients, plus the
// sampler's MipLODBias, clamped by the view's ResourceMinLODClamp and the sampler's MaxLOD. SampleCmpLevelZero starts
// from LOD 0. GatherRed returns the red channels of the four texels of the view's first mip around the sample point,
// in D3D's component order, moved by a programmable offset's low 6 bits, signed (22.4.4); GatherCmp compares them.
// A view min-LOD clamp that removes that mip makes gathers read 0 (5.8.6.1). CalculateLevelOfDetail returns the
// biased LOD clamped and unclamped (22.5.6).
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float4 tint; float lod; float grad; float ref; int offset; };
Texture2D t : register(t0);
Texture2D<float> depth : register(t1);
SamplerState s : register(s0);
SamplerComparisonState sc : register(s1);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 ps(V i) : SV_Target { return t.Sample(s, i.uv) * tint; }
float4 ps_level(V i) : SV_Target { return t.SampleLevel(s, i.uv, lod) * tint; }
float4 ps_grad(V i) : SV_Target { return t.SampleGrad(s, i.uv, float2(grad, 0), float2(0, grad)) * tint; }
float4 ps_cmp(V i) : SV_Target { return depth.SampleCmpLevelZero(sc, i.uv, ref) * tint; }
float4 ps_gather(V i) : SV_Target { return t.GatherRed(s, i.uv) * tint; }
float4 ps_gather_po(V i) : SV_Target { return t.GatherRed(s, i.uv, int2(offset, offset)) * tint; }
float4 ps_gather_cmp(V i) : SV_Target { return depth.GatherCmp(sc, i.uv, ref) * tint; }
float4 ps_lod(V i) : SV_Target {
  // LODs mapped into unorm range: clamped / 4, (unclamped + 8) / 16
  return float4(t.CalculateLevelOfDetail(s, i.uv) / 4, (t.CalculateLevelOfDetailUnclamped(s, i.uv) + 8) / 16, 0, 1) * tint;
}
)hlsl";

static ComPtr<ID3D12Resource>
texture(
    ID3D12Device *device, DXGI_FORMAT format, UINT size, UINT16 mips, D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state
) {
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,    size, size, 1, mips, format, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN,       flags};
  ComPtr<ID3D12Resource> res;
  device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
  return res;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  enum { Sample, Level, Grad, Cmp, Gather, Lod, GatherPo, GatherCmp, Shaders };
  const char *entries[] = {"ps", "ps_level", "ps_grad", "ps_cmp", "ps_gather", "ps_lod", "ps_gather_po", "ps_gather_cmp"};
  auto vs = compiler.compile(hlsl, "vs", "vs");
  std::string ps[Shaders];
  for (int i = 0; i < Shaders; i++)
    ps[i] = compiler.compile(hlsl, entries[i], "ps");
  if (vs.empty() || std::any_of(ps, ps + Shaders, [](auto &p) { return p.empty(); })) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE srvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2}, samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2};
  D3D12_ROOT_PARAMETER params[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_CBV},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}
  };
  params[1].DescriptorTable = {1, &srvs};
  params[2].DescriptorTable = {1, &samplers};
  auto rs = root_signature(device.Get(), {3, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  ComPtr<ID3D12PipelineState> pso[Shaders];
  for (int i = 0; i < Shaders; i++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs);
    desc.PS = bytecode(ps[i]);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[i])));
  }

  // inputs: a 2x2 color texture with distinct texels and a 1x1 second mip, a depth texture whose two mips sit on
  // either side of the comparison reference, and a tint that scales every channel differently
  const UINT texels[2][4] = {{0xff2040c0, 0x80ff0010, 0x40a0ff70, 0xc0608020}, {0x3070b0f0}};
  const float depths[2] = {0.25f, 0.75f};
  const float tint[4] = {1.0f, 0.5f, 0.25f, 0.75f};
  const UINT size = 64, tex_size = 2, mips = 2;
  // `lod` is the shader's LOD for SampleLevel, and the LOD its gradients express for SampleGrad. `ref` sits between
  // the depth mips for SampleCmp, and between 0 and the first mip for GatherCmp, so both outcomes differ
  struct Case {
    int ps;
    float lod, bias, min_lod_clamp, max_lod = D3D12_FLOAT32_MAX;
    int offset = 0;
    float ref = 0.5f;
    D3D12_COMPARISON_FUNC func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    UINT view_mips = mips;
  } cases[] = {{Sample, 0, 0, 0}, {Sample, 0, 0, 1}, {Level, 0, 1, 0},  {Level, 0, 0, 1}, {Level, 1, -1, 0},
               {Grad, 0, 1, 0},   {Grad, 1, -1, 0},  {Grad, 0, 0, 1},   {Cmp, 0, 0, 0},   {Cmp, 0, 1, 0},
               {Cmp, 0, 0, 1},    {Gather, 0, 0, 0}, {Gather, 0, 0, 1}, {Lod, 0, 0, 0},   {Lod, 0, 0, 1},
               {Lod, 0, 5.5f, 0}, {Lod, 0, 6, 0, 0.25f}, {Lod, 0, 0, 1, 0.25f},
               {.ps = GatherPo, .offset = 119}, {.ps = GatherPo, .offset = -9},
               {.ps = GatherCmp, .ref = 0.125f}, {.ps = GatherCmp, .min_lod_clamp = 1, .ref = 0.125f},
               {.ps = GatherCmp, .min_lod_clamp = 1, .ref = NAN, .func = D3D12_COMPARISON_FUNC_GREATER_EQUAL},
               {.ps = GatherCmp, .min_lod_clamp = 1, .ref = NAN, .func = D3D12_COMPARISON_FUNC_NOT_EQUAL},
               {.ps = Gather, .min_lod_clamp = 0.5f, .view_mips = 1}};
  const UINT n = std::size(cases);

  auto tex = texture(
      device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, tex_size, mips, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST
  );
  auto ds = texture(
      device.Get(), DXGI_FORMAT_R32_TYPELESS, tex_size, mips, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
      D3D12_RESOURCE_STATE_DEPTH_WRITE
  );
  auto rt = texture(
      device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, size, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
      D3D12_RESOURCE_STATE_RENDER_TARGET
  );
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_fp[mips], rt_fp;
  UINT64 tex_bytes, rt_bytes;
  auto tex_desc = tex->GetDesc(), rt_desc = rt->GetDesc();
  device->GetCopyableFootprints(&tex_desc, 0, mips, 0, tex_fp, nullptr, nullptr, &tex_bytes);
  device->GetCopyableFootprints(&rt_desc, 0, 1, 0, &rt_fp, nullptr, nullptr, &rt_bytes);
  const UINT cb_stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  UINT64 cb_offset = (tex_bytes + cb_stride - 1) / cb_stride * cb_stride;
  auto upload =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cb_offset + cb_stride * n, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, rt_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT m = 0; m < mips; m++)
    for (UINT y = 0; y < tex_fp[m].Footprint.Height; y++)
      memcpy(
          mapped + tex_fp[m].Offset + y * tex_fp[m].Footprint.RowPitch, &texels[m][y * tex_fp[m].Footprint.Width],
          tex_fp[m].Footprint.Width * 4
      );
  for (UINT c = 0; c < n; c++) {
    float constants[7] = {tint[0],      tint[1], tint[2], tint[3], cases[c].lod, std::exp2(cases[c].lod) / tex_size,
                          cases[c].ref};
    memcpy(mapped + cb_offset + cb_stride * c, constants, sizeof(constants));
    memcpy(mapped + cb_offset + cb_stride * c + sizeof(constants), &cases[c].offset, sizeof(int));
  }

  // per case: SRVs (color, depth) and samplers (color, comparison) carry the case's clamp and bias
  ComPtr<ID3D12DescriptorHeap> srv_heap, sampler_heap, rtv_heap, dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * n, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap)));
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&sampler_heap)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, mips};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap)));
  auto srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  auto dsv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  auto cpu = [](ID3D12DescriptorHeap *heap, UINT step, UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + step * i};
  };
  auto gpu = [](ID3D12DescriptorHeap *heap, UINT step, UINT i) {
    return D3D12_GPU_DESCRIPTOR_HANDLE{heap->GetGPUDescriptorHandleForHeapStart().ptr + step * i};
  };
  for (UINT c = 0; c < n; c++) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    srv.Texture2D = {0, cases[c].view_mips, 0, cases[c].min_lod_clamp};
    device->CreateShaderResourceView(tex.Get(), &srv, cpu(srv_heap.Get(), srv_step, 2 * c));
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(ds.Get(), &srv, cpu(srv_heap.Get(), srv_step, 2 * c + 1));
    // lod is undefined with point mip filtering (D3D11.3 22.5.6)
    D3D12_SAMPLER_DESC sampler{
        cases[c].ps == Lod ? D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        cases[c].bias
    };
    sampler.MaxLOD = cases[c].max_lod;
    device->CreateSampler(&sampler, cpu(sampler_heap.Get(), sampler_step, 2 * c));
    sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    sampler.ComparisonFunc = cases[c].func;
    device->CreateSampler(&sampler, cpu(sampler_heap.Get(), sampler_step, 2 * c + 1));
  }
  device->CreateRenderTargetView(rt.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT m = 0; m < mips; m++) {
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = m}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = tex_fp[m]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
    dsv.Texture2D.MipSlice = m;
    device->CreateDepthStencilView(ds.Get(), &dsv, cpu(dsv_heap.Get(), dsv_step, m));
    list->ClearDepthStencilView(cpu(dsv_heap.Get(), dsv_step, m), D3D12_CLEAR_FLAG_DEPTH, depths[m], 0, 0, nullptr);
  }
  transition(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  unsigned failures = 0;
  for (UINT c = 0; c < n; c++) {
    auto &k = cases[c];
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso[k.ps].Get()));
    ID3D12DescriptorHeap *heaps[] = {srv_heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + cb_offset + cb_stride * c);
    list->SetGraphicsRootDescriptorTable(1, gpu(srv_heap.Get(), srv_step, 2 * c));
    list->SetGraphicsRootDescriptorTable(2, gpu(sampler_heap.Get(), sampler_step, 2 * c));
    D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{rt.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = rt_fp}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    // Sample takes its LOD from one texel's screen footprint, the others as stated above; point mip filtering
    // picks the nearest mip of the biased, clamped LOD
    float footprint_lod = std::log2((float)tex_size / size);
    bool gather = k.ps == Gather || k.ps == GatherPo || k.ps == GatherCmp;
    float lod = k.ps == Sample || k.ps == Lod ? footprint_lod : k.ps == Cmp || gather ? 0 : k.lod;
    // the sampler's clamp (MinLOD 0) first, then the view's min-LOD clamp (5.8.6.1)
    float clamped = std::max(std::clamp(lod + k.bias, 0.f, std::min((float)mips - 1, k.max_lod)), k.min_lod_clamp);
    UINT mip = gather ? 0 : (UINT)std::lround(clamped);
    // gathers read the view's first mip, which a min-LOD clamp of 1 or more removes; offsets keep their low 6 bits
    // the view's first mip is gone once the clamp reaches the next mip or passes the view's last (5.8.5)
    bool removed = gather && (k.min_lod_clamp >= 1 || k.min_lod_clamp > k.view_mips - 1.0f);
    // the reference against a texel with D3D's comparison, in C++'s IEEE operators, so NaN fails all but NOT_EQUAL
    auto passes = [&](float ref, float texel) {
      switch (k.func) {
      case D3D12_COMPARISON_FUNC_LESS: return ref < texel;
      case D3D12_COMPARISON_FUNC_EQUAL: return ref == texel;
      case D3D12_COMPARISON_FUNC_LESS_EQUAL: return ref <= texel;
      case D3D12_COMPARISON_FUNC_GREATER: return ref > texel;
      case D3D12_COMPARISON_FUNC_NOT_EQUAL: return ref != texel;
      case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return ref >= texel;
      default: return k.func == D3D12_COMPARISON_FUNC_ALWAYS;
      }
    };
    int offset = (int)((unsigned)k.offset << 26) >> 26;
    UINT mip_size = tex_size >> mip;
    unsigned char *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    unsigned mismatches = 0;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto texel = texels[mip][(y * mip_size / size) * mip_size + x * mip_size / size];
        auto got = pixels + y * rt_fp.Footprint.RowPitch + x * 4;
        // unorm results may round either way (1 lsb)
        // the gather footprint: the texels on either side of the sample point, clamped to the texture
        auto side = [&](UINT p, int next) {
          int i = (int)std::floor((p + 0.5f) / size * tex_size - 0.5f) + next;
          return (UINT)std::clamp(i, 0, (int)tex_size - 1);
        };
        const int gather_order[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}}; // (u, v) of x, y, z, w
        for (int ch = 0; ch < 4; ch++) {
          auto gathered = removed ? 0.0f
                                  : (float)(texels[0][side(y, gather_order[ch][1] + offset) * tex_size +
                                                      side(x, gather_order[ch][0] + offset)] &
                                            0xff);
          float values[] = {
              (float)((texel >> (8 * ch)) & 0xff),
              (float)((texel >> (8 * ch)) & 0xff),
              (float)((texel >> (8 * ch)) & 0xff),
              passes(k.ref, depths[mip]) * 255.0f,
              gathered,
              ch == 0   ? clamped / 4 * 255
              : ch == 1 ? (lod + k.bias + 8) / 16 * 255
              : ch == 2 ? 0
                        : 255,
              gathered,
              passes(k.ref, removed ? 0 : depths[0]) * 255.0f,
          };
          float value = values[k.ps];
          float want = value * tint[ch];
          if (std::fabs(got[ch] - want) > 1.0f && mismatches++ < 4)
            printf("case %u pixel %u,%u channel %d: got %u, want %.2f\n", c, x, y, ch, got[ch], want);
        }
      }
    readback->Unmap(0, nullptr);
    printf(
        "case %u (%s, lod %g, bias %g, min lod %g, max lod %g, offset %d, ref %g, mip %u): %u mismatching channels of %u\n",
        c, entries[k.ps], k.lod, k.bias, k.min_lod_clamp, k.max_lod, k.offset, k.ref, mip, mismatches, size * size * 4
    );
    failures += mismatches != 0;
  }
  printf("%s: %u of %u cases failed\n", failures ? "failed" : "passed", failures, n);
  return failures != 0;
}
