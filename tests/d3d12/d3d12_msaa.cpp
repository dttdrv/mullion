// contract: pixel shaders on a 4x target get and give per-sample state as D3D defines it (D3D11.3 16.4, 16.5, 17):
// - at sample frequency, SV_SampleIndex names the sample, SV_Coverage still holds the primitive's coverage of the
//   whole pixel (16.3.2), and attributes evaluated at that sample land where a sample-interpolated SV_Position puts
//   it; snapped offsets land at center + offset / 16, and the centroid of a fully covered pixel is its center. the
//   render target's and a Texture2DMS's sample positions and count are where and how many samples the rasterizer uses
// - at pixel frequency, fine and coarse derivatives are the quad's differences, the SV_Coverage output and discard
//   pick the samples written, and SV_Depth is what a later depth test compares against
// a compute shader reads every sample back through Texture2DMS, whose GetDimensions must report the sample count.
#include "d3d12_test.hpp"
#include <bit>
#include <cfloat>
#include <cmath>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; float2 tc : TEXCOORD0; };
// a full-screen triangle whose tc is the screen position, so tc evaluated anywhere is that point; its depth rises by
// SLOPE per pixel along x
V vs(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  V o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), Z + SLOPE * SIZE * uv.x, 1); o.tc = uv * SIZE; return o;
}
static const int2 offset = int2(OFFSET_X, OFFSET_Y);
struct PerSample { float4 eval : SV_Target0; uint4 ids : SV_Target1; float4 depth : SV_Target2; float4 position : SV_Target3; };
struct SampleV { sample float4 pos : SV_Position; float2 tc : TEXCOORD0; };
PerSample ps_sample(SampleV i, uint s : SV_SampleIndex, uint coverage : SV_Coverage) {
  float2 center = floor(i.pos.xy) + 0.5;
  PerSample o;
  // depth where the position is: at the sample when SV_Position is sample-interpolated
  o.depth = float4(i.pos.z - (Z + SLOPE * i.pos.x), GetRenderTargetSampleCount(), 0, 0);
  o.position = float4(i.pos.xy - center, GetRenderTargetSamplePosition(s));
  o.eval = float4(EvaluateAttributeAtSample(i.tc, s) - i.pos.xy, EvaluateAttributeSnapped(i.tc, offset) - center - offset / 16.0);
  o.ids = uint4(s, coverage, asuint(EvaluateAttributeCentroid(i.tc) - center));
  return o;
}
struct PerPixel { float4 deriv : SV_Target0; uint4 id : SV_Target1; float depth : SV_Depth; uint coverage : SV_Coverage; };
PerPixel ps_pixel(V i) {
  uint2 p = uint2(i.pos.xy);
  // with the x*y term a fine derivative depends on the pixel's row or column, which a coarse one may not
  float2 q = i.tc * i.tc + i.tc.x * i.tc.y;
  PerPixel o;
  o.deriv = float4(ddx_fine(q.x), ddy_fine(q.y), ddx_coarse(q.x), ddy_coarse(q.y));
  o.id = FIRST;
  o.depth = i.tc.x / SIZE;
  o.coverage = p.x + p.y;
  if ((p.x + p.y * 3) % 5 == 0)
    discard;
  return o;
}
uint4 ps_second(V i) : SV_Target1 { return SECOND; }

Texture2DMS<float4> eval : register(t0);
Texture2DMS<uint4> ids : register(t1);
Texture2DMS<float4> deriv : register(t2);
Texture2DMS<uint4> id : register(t3);
Texture2DMS<float4> depth : register(t4);
Texture2DMS<float4> position : register(t5);
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(SIZE, SIZE, 1)] void cs(uint2 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLES; s++) {
    uint i = ((p.y * SIZE + p.x) * SAMPLES + s) * 6;
    result[i] = asuint(eval.Load(p, s));
    result[i + 1] = ids.Load(p, s);
    result[i + 2] = asuint(deriv.Load(p, s));
    result[i + 3] = id.Load(p, s);
    result[i + 4] = asuint(depth.Load(p, s));
    result[i + 5] = asuint(position.Load(p, s));
    result[SIZE * SIZE * SAMPLES * 6 + 1 + s] = asuint(float4(eval.GetSamplePosition(s), 0, 0));
  }
  uint w, h, n;
  eval.GetDimensions(w, h, n);
  result[SIZE * SIZE * SAMPLES * 6] = uint4(w, h, n, 0);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT size = 16, samples = 4, first = 1, second = 2, pixels = size * size, full = (1u << samples) - 1;
  const int offset[2] = {4, -8};
  // the second pass draws at depth 1/2, so it passes where the first wrote less than that; the first pass's depth
  // slopes by a quarter over the target, far more than the sample offsets' tolerance
  const float second_z = 0.5f, slope = 0.25f / size;
  // accuracy of attribute evaluation: half the 1/16 pixel grid D3D places sample and snapped positions on
  const float kPosition = 1.0f / 32;
  // derivatives are differences of squares up to size^2, each off by a few float ulps
  const float kDerivative = 4 * size * size * FLT_EPSILON;
  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size), "SAMPLES=" + std::to_string(samples), "FIRST=" + std::to_string(first),
      "SECOND=" + std::to_string(second), "OFFSET_X=" + std::to_string(offset[0]),
      "OFFSET_Y=" + std::to_string(offset[1])
  };
  auto with_z = [&](float z, float slope = 0) {
    auto d = defines;
    d.push_back("Z=" + std::to_string(z));
    d.push_back("SLOPE=" + std::to_string(slope));
    return d;
  };
  auto vs = compiler.compile(hlsl, "vs", "vs", with_z(0, slope)), vs_second = compiler.compile(hlsl, "vs", "vs", with_z(second_z)),
       ps_sample = compiler.compile(hlsl, "ps_sample", "ps", with_z(0, slope)),
       ps_pixel = compiler.compile(hlsl, "ps_pixel", "ps", with_z(0)),
       ps_second = compiler.compile(hlsl, "ps_second", "ps", with_z(0)), cs = compiler.compile(hlsl, "cs", "cs", with_z(0));
  for (auto *code : {&vs, &vs_second, &ps_sample, &ps_pixel, &ps_second, &cs})
    if (code->empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto draw_rs = root_signature(device.Get(), {});
  D3D12_DESCRIPTOR_RANGE srvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &srvs};
  auto read_rs = root_signature(device.Get(), {2, params});

  // targets: per-sample evaluation and ids (first pass), derivatives and pass ids (second and third), per-sample
  // depth and sample positions (first pass)
  const DXGI_FORMAT formats[6] = {
      DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT,  DXGI_FORMAT_R32G32B32A32_FLOAT,
      DXGI_FORMAT_R32G32B32A32_UINT,  DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT
  };
  const int target_count = std::size(formats);
  const float clear = -1; // derivatives here are positive, so -1 marks a sample no pass wrote
  ComPtr<ID3D12Resource> targets[target_count], depth;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  for (int i = 0; i < target_count; i++) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,    size, size, 1, 1, formats[i], {samples, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])
    ));
  }
  D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_D32_FLOAT,
                                 {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));

  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, target_count},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1},
      srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, target_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap)));
  auto rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV),
       srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[target_count], dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
  for (int i = 0; i < target_count; i++) {
    rtv[i] = {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step};
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv[i]);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{formats[i], D3D12_SRV_DIMENSION_TEXTURE2DMS};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(
        targets[i].Get(), &view, {srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * srv_step}
    );
  }
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  // pass: 0 per-sample, 1 per-pixel writing depth, 2 depth-tested
  ComPtr<ID3D12PipelineState> pso[3];
  for (int pass = 0; pass < 3; pass++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = draw_rs.Get();
    desc.VS = bytecode(pass == 2 ? vs_second : vs);
    desc.PS = bytecode(pass == 0 ? ps_sample : pass == 1 ? ps_pixel : ps_second);
    desc.BlendState.IndependentBlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = pass == 2 ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[3].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.MultisampleEnable = TRUE;
    if (pass) {
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthWriteMask = pass == 1 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      desc.DepthStencilState.DepthFunc = pass == 1 ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_LESS;
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    }
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = pass ? 2 : 4;
    desc.RTVFormats[0] = formats[pass ? 2 : 0];
    desc.RTVFormats[1] = formats[pass ? 3 : 1];
    desc.RTVFormats[2] = pass ? DXGI_FORMAT_UNKNOWN : formats[4];
    desc.RTVFormats[3] = pass ? DXGI_FORMAT_UNKNOWN : formats[5];
    desc.SampleDesc = {samples, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[pass])));
  }
  ComPtr<ID3D12PipelineState> read_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC read_desc{read_rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&read_desc, IID_PPV_ARGS(&read_pso)));

  const UINT records = pixels * samples, bytes = (records * target_count + 1 + samples) * 16;
  auto result = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso[0].Get(), IID_PPV_ARGS(&list)));
  const float clear_float[4] = {clear, clear, clear, clear}, clear_uint[4] = {};
  for (int i = 0; i < target_count; i++)
    list->ClearRenderTargetView(rtv[i], i & 1 ? clear_uint : clear_float, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
  list->SetGraphicsRootSignature(draw_rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_CPU_DESCRIPTOR_HANDLE first_targets[] = {rtv[0], rtv[1], rtv[4], rtv[5]};
  list->OMSetRenderTargets(4, first_targets, FALSE, nullptr);
  list->DrawInstanced(3, 1, 0, 0);
  list->OMSetRenderTargets(2, rtv + 2, FALSE, &dsv);
  for (int pass = 1; pass < 3; pass++) {
    list->SetPipelineState(pso[pass].Get());
    list->DrawInstanced(3, 1, 0, 0);
  }
  for (int i = 0; i < target_count; i++)
    transition(
        list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    );
  ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(read_rs.Get());
  list->SetPipelineState(read_pso.Get());
  list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), result.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  unsigned mismatches = 0;
  auto check = [&](bool ok, const char *what, UINT x, UINT y, UINT s, double got, double want) {
    if (!ok && mismatches++ < 12)
      printf("%s at %u,%u sample %u: got %g, want %g\n", what, x, y, s, got, want);
  };
  auto dims = out + records * target_count * 4, texture_positions = dims + 4;
  check(dims[0] == size && dims[1] == size && dims[2] == samples, "GetDimensions samples", 0, 0, 0, dims[2], samples);
  auto f = [](UINT bits) { return std::bit_cast<float>(bits); };
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      // q at the quad's pixel centers: tc is the screen position. a coarse derivative is the quad's, from either of
      // its rows or columns
      auto q = [](double u, double v) { return u * u + u * v; };
      auto c = [](UINT v) { return v + 0.5; };
      auto ddx = [&](UINT row) { return q(c(x | 1), c(row)) - q(c(x & ~1u), c(row)); };
      auto ddy = [&](UINT column) { return q(c(y | 1), c(column)) - q(c(y & ~1u), c(column)); };
      bool discarded = (x + y * 3) % 5 == 0;
      for (UINT s = 0; s < samples; s++) {
        auto r = out + ((y * size + x) * samples + s) * target_count * 4;
        for (int c = 0; c < 4; c++)
          check(std::fabs(f(r[c])) <= kPosition, c < 2 ? "sample evaluation" : "snapped evaluation", x, y, s, f(r[c]), 0);
        check(r[4] == s, "SV_SampleIndex", x, y, s, r[4], s);
        check(r[5] == full, "SV_Coverage input", x, y, s, r[5], full);
        for (int c = 6; c < 8; c++)
          check(std::fabs(f(r[c])) <= kPosition, "centroid evaluation", x, y, s, f(r[c]), 0);
        bool written = !discarded && ((x + y) >> s & 1);
        // fine: this pixel's row and column; coarse: either of the quad's
        const double want[4][2] = {
            {ddx(y), ddx(y)}, {ddy(x), ddy(x)}, {ddx(y & ~1u), ddx(y | 1)}, {ddy(x & ~1u), ddy(x | 1)}
        };
        for (int c = 0; c < 4; c++) {
          auto matches = [&](double want) { return std::fabs(f(r[8 + c]) - (written ? want : clear)) <= kDerivative; };
          check(matches(want[c][0]) || matches(want[c][1]), c < 2 ? "fine derivative" : "coarse derivative", x, y, s,
                f(r[8 + c]), written ? want[c][0] : clear);
        }
        // the depth the first pass wrote is x / size at the pixel center; unwritten samples keep the cleared 1
        bool passes = second_z < (written ? (x + 0.5) / size : 1.0);
        UINT want_id = passes ? second : written ? first : 0;
        check(r[12] == want_id, "SV_Depth", x, y, s, r[12], want_id);
        // slope * kPosition: the depth may be off by what the position may be off by
        check(std::fabs(f(r[16])) <= slope * kPosition, "SV_Position depth", x, y, s, f(r[16]), 0);
        check(f(r[17]) == samples, "GetRenderTargetSampleCount", x, y, s, f(r[17]), samples);
        // where the rasterizer put the sample, relative to the center, against both queries
        for (int c = 0; c < 2; c++) {
          check(std::fabs(f(r[22 + c]) - f(r[20 + c])) <= kPosition, "GetRenderTargetSamplePosition", x, y, s, f(r[22 + c]), f(r[20 + c]));
          check(std::fabs(f(texture_positions[s * 4 + c]) - f(r[20 + c])) <= kPosition, "Texture2DMS GetSamplePosition", x, y, s, f(texture_positions[s * 4 + c]), f(r[20 + c]));
        }
      }
    }
  printf("%s: %u mismatches over %u samples\n", mismatches ? "failed" : "passed", mismatches, records);
  return mismatches != 0;
}
