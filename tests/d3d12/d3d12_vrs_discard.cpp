// contract: variable rate shading, tier 2 (D3D12 Variable Rate Shading spec), of pixel shaders with and without a
// discard. at a coarse rate (1x2, 2x1, 2x2 of RSSetShadingRate) the pixel shader runs once for each coarse pixel, its
// inputs at the coarse pixel's center, and its outputs, the values its UAV operations return among them, go to every
// covered pixel of the coarse pixel. a shader without a discard writes every covered pixel, and a later opaque draw
// over it replaces it there; a shader's discard drops the whole coarse pixel; and the depth bounds test, which tests
// each pixel's stored depth, drops pixels one at a time, inside a coarse pixel as elsewhere, whether the shader's tests
// come before it ([earlydepthstencil]) or not.
// https://microsoft.github.io/DirectX-Specs/d3d/VariableRateShading.html
// https://microsoft.github.io/DirectX-Specs/d3d/DepthBoundsTest.html
#include "d3d12_test.hpp"
#include <cmath>
#include <functional>

// a pipeline stream entry: its type, then its data, at pointer alignment
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Data> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Data data;
};

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint half_target; float value; uint discarding; };
RWByteAddressBuffer counts : register(u0);
struct V {
  float4 position : SV_Position;
  noperspective float n : N;
};
// in a 64x64 viewport, its corners at pixels (0, 0), (64, 0) and (0, 64): the whole 32x32 target; or (half_target) at
// (0, 0), (32.25, 0) and (0, 32.25): the pixels whose x + y is 31 or less
V vs(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  float2 p = uv * (half_target ? 16.125 : 32);
  V v;
  v.position = float4(p.x / 32 - 1, 1 - p.y / 32, 0.5, 1);
  v.n = p.x + 2 * p.y;
  return v;
}
// no discard, no UAV
float4 ps(V v) : SV_Target {
  return float4(v.position.xy, value, v.n);
}
// the atomic add, its tests (the depth bounds test's among them) before it: a pixel they drop does not run it, and a
// coarse pixel they drop whole does not
[earlydepthstencil] float4 ps_early(V v) : SV_Target {
  uint old;
  counts.InterlockedAdd(4 * (uint(v.position.y) * 32 + uint(v.position.x)), 1, old);
  return float4(v.position.xy, old, v.n);
}
// a discard (when `discarding`) of the 2x2 blocks of the target whose block coordinates' sum is odd
float4 ps_discard(V v) : SV_Target {
  if (discarding && ((uint(v.position.x) / 2 + uint(v.position.y) / 2) & 1))
    discard;
  return float4(v.position.xy, value, v.n);
}
// no discard, and the value an atomic add returns where the shader runs
float4 ps_count(V v) : SV_Target {
  uint old;
  counts.InterlockedAdd(4 * (uint(v.position.y) * 32 + uint(v.position.x)), 1, old);
  return float4(v.position.xy, old, v.n);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2)));
  if (options6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || !options2.DepthBoundsTestSupported) {
    printf("failed: VRS tier %d, depth bounds test %d\n", options6.VariableShadingRateTier,
           options2.DepthBoundsTestSupported);
    return 1;
  }
  unsigned failures = 0, failures_before = 0;

  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_discard = compiler.compile(hlsl, "ps_discard", "ps"), ps_count = compiler.compile(hlsl, "ps_count", "ps"),
       ps_early = compiler.compile(hlsl, "ps_early", "ps");
  if (vs.empty() || ps.empty() || ps_discard.empty() || ps_count.empty() || ps_early.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 3};
  params[1].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {2, params});

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
  } stream{};
  stream.rs.data = rs.Get(), stream.vs.data = bytecode(vs);
  stream.targets.data = {{DXGI_FORMAT_R32G32B32A32_FLOAT}, 1};
  stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  stream.rasterizer.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.rasterizer.data.DepthClipEnable = TRUE;
  // without a depth buffer, or with one that is tested by the depth bounds test alone (its own test always passes)
  auto pipeline = [&](const std::string &pixel, bool bounds) -> ComPtr<ID3D12PipelineState> {
    stream.ps.data = bytecode(pixel);
    stream.depth.data = {bounds, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS};
    stream.depth.data.DepthBoundsTestEnable = bounds;
    stream.depth_format.data = bounds ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso))))
      printf("failed: a pipeline was not made\n");
    return pso;
  };
  auto plain = pipeline(ps, false), discarding = pipeline(ps_discard, false), counting = pipeline(ps_count, false),
       plain_bounded = pipeline(ps, true), discarding_bounded = pipeline(ps_discard, true), early_bounded = pipeline(ps_early, true);
  if (!plain || !discarding || !counting || !plain_bounded || !discarding_bounded || !early_bounded)
    return 1;

  const UINT size = 32;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target, depth;
  {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                             {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&target)));
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                          nullptr, IID_PPV_ARGS(&depth)));
  }
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  // the atomic adds' counters, each 7 before a draw
  const UINT64 counts_size = size * size * 4;
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, counts_size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto sevens = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, counts_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    uint32_t *mapped;
    CHECK(sevens->Map(0, nullptr, (void **)&mapped));
    for (UINT i = 0; i < size * size; i++)
      mapped[i] = 7;
    sevens->Unmap(0, nullptr);
  }
  const UINT64 row = size * 16;
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size + counts_size, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList5> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  struct Pixel {
    float color[4];
  };
  std::vector<Pixel> got(size * size);
  std::vector<uint32_t> count(size * size);
  struct Draw {
    ID3D12PipelineState *pso;
    UINT half_target;
    float value;
    UINT discarding;
  };
  // draws in one pass at a rate, into the target cleared to -1 (and, `bounded`, with the depth buffer, its pixels
  // whose x + y is odd or y 16 or more at 0.9, the others at 0.1, and depth bounds of [0, 0.5]), and reads the target
  // and the counters back
  auto draw = [&](D3D12_SHADING_RATE rate, std::initializer_list<Draw> draws, bool bounded = false) -> HRESULT {
    list->CopyBufferRegion(counts.Get(), 0, sevens.Get(), 0, counts_size);
    D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    uav.UAV.pResource = counts.Get();
    list->ResourceBarrier(1, &uav);
    const float clear[4] = {-1, -1, -1, -1};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.1f, 0, 0, nullptr);
    std::vector<D3D12_RECT> odd;
    for (LONG y = 0; y < (LONG)size; y++)
      for (LONG x = 0; x < (LONG)size; x++)
        if (((x + y) & 1) || y >= 16)
          odd.push_back({x, y, x + 1, y + 1});
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.9f, 0, (UINT)odd.size(), odd.data());
    list->OMSetRenderTargets(1, &rtv, TRUE, bounded ? &dsv : nullptr);
    list->OMSetDepthBounds(0.0f, 0.5f);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootUnorderedAccessView(1, counts->GetGPUVirtualAddress());
    D3D12_VIEWPORT viewport{0, 0, 2.0f * size, 2.0f * size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetShadingRate(rate, nullptr);
    for (auto &d : draws) {
      list->SetPipelineState(d.pso);
      UINT constants[3] = {d.half_target, 0, d.discarding};
      memcpy(&constants[1], &d.value, sizeof(float));
      list->SetGraphicsRoot32BitConstants(0, 3, constants, 0);
      list->DrawInstanced(3, 1, 0, 0);
    }
    list->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R32G32B32A32_FLOAT, size, size, 1, (UINT)row}};
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), row * size, counts.Get(), 0, counts_size);
    transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HRESULT hr = forget(readback.Get());
    if (SUCCEEDED(hr))
      hr = submit(device.Get(), queue.Get(), list.Get());
    if (FAILED(hr))
      return hr;
    uint8_t *mapped;
    if (FAILED(hr = readback->Map(0, nullptr, (void **)&mapped)))
      return hr;
    for (UINT y = 0; y < size; y++)
      memcpy(&got[y * size], mapped + y * row, size * sizeof(Pixel));
    memcpy(count.data(), mapped + row * size, counts_size);
    readback->Unmap(0, nullptr);
    allocator->Reset();
    return list->Reset(allocator.Get(), nullptr);
  };

  // a pixel's coarse pixel's center at a rate (log2 of its width, then height)
  auto center = [](UINT x, UINT y, UINT rate) {
    UINT w = 1u << (rate >> 2), h = 1u << (rate & 3);
    return std::pair{w == 1 ? x + 0.5f : float(x & ~1u) + 1, h == 1 ? y + 0.5f : float(y & ~1u) + 1};
  };
  auto report = [&](const char *what, UINT x, UINT y, const char *field, double value, double want) {
    if (failures++ - failures_before < 8)
      printf("%s: pixel (%u, %u)'s %s is %g, want %g\n", what, x, y, field, value, want);
  };
  // each pixel against what `want` says it is: the value its coarse pixel's run wrote, or the clear (`kept` false)
  auto expect = [&](const char *what, UINT rate, const std::function<bool(UINT, UINT, float &)> &want) {
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto &p = got[y * size + x];
        float value;
        if (!want(x, y, value)) {
          if (p.color[0] != -1 || p.color[2] != -1)
            report(what, x, y, "SV_Target.z (kept clear)", p.color[2], -1);
          continue;
        }
        auto [cx, cy] = center(x, y, rate);
        if (p.color[0] != cx || p.color[1] != cy) {
          report(what, x, y, "SV_Position.x", p.color[0], cx);
          report(what, x, y, "SV_Position.y", p.color[1], cy);
        }
        if (p.color[2] != value)
          report(what, x, y, "SV_Target.z", p.color[2], value);
        float n = cx + 2 * cy;
        if (std::fabs(p.color[3] - n) > 1e-4f * n)
          report(what, x, y, "input N", p.color[3], n);
      }
  };
  auto everywhere = [](float v) { return [=](UINT, UINT, float &value) { return value = v, true; }; };
  auto block_kept = [](UINT x, UINT y) { return !((x / 2 + y / 2) & 1); };
  auto bounds_kept = [](UINT x, UINT y) { return !((x + y) & 1) && y < 16; };

  const D3D12_SHADING_RATE rates[] = {
      D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_2X2
  };
  const char *names[] = {"1x1", "1x2", "2x1", "2x2"};
  for (UINT i = 0; i < 4; i++) {
    UINT rate = rates[i];
    failures_before = failures;
    step("%s: a shader without a discard writes every pixel its coarse pixel's outputs", names[i]);
    CHECK(draw(rates[i], {{plain.Get(), 0, 3, 0}}));
    expect(names[i], rate, everywhere(3));

    failures_before = failures;
    step("%s: opaque draws over it, the last over half the target", names[i]);
    CHECK(draw(rates[i], {{plain.Get(), 0, 1, 0}, {plain.Get(), 0, 2, 0}, {plain.Get(), 0, 3, 0},
                          {plain.Get(), 1, 4, 0}}));
    expect(names[i], rate, [](UINT x, UINT y, float &value) { return value = x + y <= 31 ? 4 : 3, true; });

    failures_before = failures;
    step("%s: a shader whose discard is never taken writes every pixel", names[i]);
    CHECK(draw(rates[i], {{discarding.Get(), 0, 5, 0}}));
    expect(names[i], rate, everywhere(5));

    failures_before = failures;
    step("%s: a discard drops the coarse pixels of the odd 2x2 blocks, over an earlier draw", names[i]);
    CHECK(draw(rates[i], {{plain.Get(), 1, 2, 0}, {discarding.Get(), 0, 6, 1}}));
    expect(names[i], rate, [&](UINT x, UINT y, float &value) {
      value = block_kept(x, y) ? 6 : 2;
      return block_kept(x, y) || x + y <= 31;
    });

    failures_before = failures;
    step("%s: every pixel takes its coarse pixel's run's atomic result, and each coarse pixel adds once", names[i]);
    CHECK(draw(rates[i], {{counting.Get(), 0, 0, 0}}));
    expect(names[i], rate, everywhere(7));
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto [cx, cy] = center(x, y, rate);
        uint32_t want = x == UINT(cx) && y == UINT(cy) ? 8 : 7;
        if (count[y * size + x] != want)
          report(names[i], x, y, "counter", count[y * size + x], want);
      }

    failures_before = failures;
    step("%s: the depth bounds test drops each pixel whose stored depth is out of bounds", names[i]);
    CHECK(draw(rates[i], {{plain_bounded.Get(), 0, 8, 0}}, true));
    expect(names[i], rate, [&](UINT x, UINT y, float &value) { return value = 8, bounds_kept(x, y); });

    failures_before = failures;
    step("%s: the depth bounds test before a shader drops pixels, and coarse pixels it drops whole do not run", names[i]);
    CHECK(draw(rates[i], {{early_bounded.Get(), 0, 0, 0}}, true));
    expect(names[i], rate, [&](UINT x, UINT y, float &value) { return value = 7, bounds_kept(x, y); });
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto [cx, cy] = center(x, y, rate);
        // the coarse pixel's center pixel counts its run: at 1x1 a pixel's own, else one in the upper half
        bool runs = x == UINT(cx) && y == UINT(cy) && (rate == D3D12_SHADING_RATE_1X1 ? bounds_kept(x, y) : y < 16);
        uint32_t want = runs ? 8 : 7;
        if (count[y * size + x] != want)
          report(names[i], x, y, "counter", count[y * size + x], want);
      }

    failures_before = failures;
    step("%s: the depth bounds test and a discard together", names[i]);
    CHECK(draw(rates[i], {{discarding_bounded.Get(), 0, 9, 1}}, true));
    expect(names[i], rate, [&](UINT x, UINT y, float &value) {
      return value = 9, bounds_kept(x, y) && block_kept(x, y);
    });
  }

  printf("%s: %u wrong values\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
