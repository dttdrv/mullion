// contract: variable rate shading, tier 2 (D3D12 Variable Rate Shading spec). at a coarse rate the pixel shader runs
// once for each coarse pixel, a 2x2 block of the target at 2x2 (2x1 two pixels wide, 1x2 two high), with SV_Position
// and its inputs at the coarse pixel's center (perspective or not), derivatives across coarse pixels, and SV_Coverage
// holding each pixel's samples in turn, left to right then top to bottom; its outputs go to all of the coarse pixel's
// pixels, an output coverage's bits to each pixel's samples, and a discard drops the coarse pixel. the rate is
// RSSetShadingRate's combined with the primitive's (SV_ShadingRate, from the vertex shader) and then the shading rate
// image's (a D3D12_SHADING_RATE for each 16x16 tile; 1x1 beyond it), by passthrough, override, min, max or sum. a
// shader that outputs depth shades every pixel at any rate.
#include "d3d12_test.hpp"
#include <cmath>
#include <functional>
#include <tuple>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint mode; };
RWByteAddressBuffer counts : register(u0);
struct V {
  float4 position : SV_Position;
  float a : A;
  noperspective float n : N;
};
// a triangle over the 32x32 target, its corners at (0, 0), (64, 0) and (0, 64) of a 64x64 viewport, of w 1, or 1, 4 and
// 16 (mode 1). Metal clips a triangle that reaches past its guard band, a quarter beyond the viewport, into several.
// mode 3's corners are (40, 40), (8.25, 40) and (40, 8.25): its long edge crosses quads whose first pixel it leaves out
V vs(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  float w = mode == 1 ? (id == 0 ? 1 : id == 1 ? 4 : 16) : 1;
  float2 p = mode == 3 ? 40 - uv * 15.875 : uv * 32;
  V v;
  v.position = float4(p.x / 32 - 1, 1 - p.y / 32, 0.5, 1) * w;
  v.a = uv.x * 8 + uv.y * 3;
  v.n = uv.x * 5 + uv.y * 7;
  return v;
}
struct Out {
  float4 color : SV_Target0;
  uint2 info : SV_Target1;
};
// once for each run, by where it is: a coarse pixel's center is a pixel's corner, its right and lower pixel's index
void count(float4 position) {
  counts.InterlockedAdd(4 * ((uint)position.y * 32 + (uint)position.x), 1);
}
Out ps(V v, uint coverage : SV_Coverage) {
  count(v.position);
  // the coverage, there, for a triangle's edge and at 4x
  if (mode >= 3)
    counts.InterlockedOr(4 * (1024 + (uint)v.position.y * 32 + (uint)v.position.x), coverage);
  // a coarse pixel whose center is left of 15 is dropped: at 2x2, the target's columns 0 to 13
  if (mode == 2 && v.position.x < 15)
    discard;
  Out o;
  o.color = float4(v.position.xy, v.a, v.n);
  o.info = uint2(coverage, (uint)round(ddx(v.n) * 64));
  return o;
}
// coarse shading is off for a shader that outputs depth
struct DepthOut {
  float4 color : SV_Target0;
  uint2 info : SV_Target1;
  float depth : SV_Depth;
};
DepthOut ps_depth(V v) {
  count(v.position);
  DepthOut o = {float4(v.position.xy, v.a, v.n), uint2(0, 0), 0.5};
  return o;
}
// an output coverage of the coarse pixel's first pixel's sample
struct MaskOut {
  float4 color : SV_Target0;
  uint2 info : SV_Target1;
  uint mask : SV_Coverage;
};
MaskOut ps_mask(V v) {
  MaskOut o = {float4(v.position.xy, v.a, v.n), uint2(0, 0), 1};
  return o;
}
)hlsl";

// SM 6.4: the primitive's rate from the vertex shader, and the coarse pixel's in the pixel shader
static const char hlsl_rate[] = R"hlsl(
struct V {
  float4 position : SV_Position;
  float a : A;
  noperspective float n : N;
  uint rate : SV_ShadingRate;
};
V vs_rate(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  V v;
  v.position = float4(uv * float2(1, -1) + float2(-1, 1), 0.5, 1);
  v.a = uv.x * 8 + uv.y * 3;
  v.n = uv.x * 5 + uv.y * 7;
  v.rate = 0x5; // D3D12_SHADING_RATE_2X2
  return v;
}
// the geometry shader's rate instead: 2x1
[maxvertexcount(3)]
void gs_rate(triangle V input[3], inout TriangleStream<V> output) {
  for (uint i = 0; i < 3; i++) {
    V v = input[i];
    v.rate = 0x4; // D3D12_SHADING_RATE_2X1
    output.Append(v);
  }
}
struct Out {
  float4 color : SV_Target0;
  uint2 info : SV_Target1;
};
Out ps_rate(float4 position : SV_Position, float a : A, noperspective float n : N, uint rate : SV_ShadingRate) {
  Out o = {float4(position.xy, a, n), uint2(rate, 0)};
  return o;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // SV_ShadingRate is SM 6.4's
  bool dxil = compiler.dxc;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  unsigned failures = 0, failures_before = 0;

  step("the device reports tier 2, 16x16 tiles and the sum combiner");
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS10 options10{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)));
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS10, &options10, sizeof(options10)));
  if (options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_2 ||
      options6.ShadingRateImageTileSize != 16 || !options10.VariableRateShadingSumCombinerSupported) {
    printf(
        "failed: tier %d, tiles of %u, sum combiner %d\n", options6.VariableShadingRateTier,
        options6.ShadingRateImageTileSize, options10.VariableRateShadingSumCombinerSupported
    );
    return 1;
  }

  const UINT size = 32, tile = 16;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_depth = compiler.compile(hlsl, "ps_depth", "ps"), ps_mask = compiler.compile(hlsl, "ps_mask", "ps");
  std::string vs_rate, gs_rate, ps_rate;
  if (dxil) {
    vs_rate = compiler.compile(hlsl_rate, "vs_rate", "vs_6_4");
    gs_rate = compiler.compile(hlsl_rate, "gs_rate", "gs_6_4");
    ps_rate = compiler.compile(hlsl_rate, "ps_rate", "ps_6_4");
  }
  if (vs.empty() || ps.empty() || ps_depth.empty() || ps_mask.empty() ||
      (dxil && (vs_rate.empty() || gs_rate.empty() || ps_rate.empty()))) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  params[1].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {2, params});

  auto pipeline = [&](const std::string &vertex, const std::string &pixel, UINT samples = 1,
                      const std::string &geometry = {}) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vertex);
    desc.PS = bytecode(pixel);
    if (!geometry.empty())
      desc.GS = bytecode(geometry);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.RTVFormats[1] = DXGI_FORMAT_R32G32_UINT;
    desc.SampleDesc = {samples, 0};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))))
      printf("failed: a pipeline was not made\n");
    return pso;
  };
  auto shaded = pipeline(vs, ps), depth = pipeline(vs, ps_depth), masked = pipeline(vs, ps_mask),
       shaded4 = pipeline(vs, ps, 4);
  ComPtr<ID3D12PipelineState> primitive_rate = dxil ? pipeline(vs_rate, ps_rate) : nullptr,
                              geometry_rate = dxil ? pipeline(vs_rate, ps_rate, 1, gs_rate) : nullptr;
  if (!shaded || !depth || !masked || !shaded4 || (dxil && (!primitive_rate || !geometry_rate)))
    return 1;

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  // [2] and [3] are 4x
  ComPtr<ID3D12Resource> targets[4], image;
  const DXGI_FORMAT formats[2] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32_UINT};
  const UINT texel[2] = {16, 8};
  for (UINT i = 0; i < 4; i++) {
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        size,
        size,
        1,
        1,
        formats[i % 2],
        {i < 2 ? 1u : 4u, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])
    ));
  }
  // two tiles wide, one high: the target's lower half is beyond it
  const UINT image_width = 2, image_height = 1;
  {
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, image_width, image_height, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image)
    ));
  }
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < 4; i++)
    device->CreateRenderTargetView(
        targets[i].Get(), nullptr,
        {rtv.ptr + i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)}
    );
  // the runs at each place, then the coverages there
  const UINT64 counts_size = 2 * size * size * 4;
  auto counts = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, counts_size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, counts_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto image_upload = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_RESOURCE_STATE_GENERIC_READ
  );
  {
    void *mapped;
    CHECK(zeros->Map(0, nullptr, &mapped));
    memset(mapped, 0, counts_size);
    zeros->Unmap(0, nullptr);
  }
  const UINT64 row = size * 16;
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size * 2 + counts_size, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList5> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the image's tiles, as D3D12_SHADING_RATEs
  auto set_image = [&](uint8_t left, uint8_t right) -> HRESULT {
    uint8_t *mapped;
    HRESULT hr = image_upload->Map(0, nullptr, (void **)&mapped);
    if (FAILED(hr))
      return hr;
    mapped[0] = left;
    mapped[1] = right;
    image_upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{image.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{image_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = {0, {DXGI_FORMAT_R8_UINT, image_width, image_height, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
    hr = submit(device.Get(), queue.Get(), list.Get());
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
    transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    return hr;
  };

  struct Pixel {
    float color[4];
    uint32_t info[2];
  };
  std::vector<Pixel> got(size * size);
  std::vector<uint32_t> ran(size * size), covered(size * size);
  // one draw of the whole target, at a rate, with combiners and maybe the image
  auto draw = [&](ID3D12PipelineState *pso, UINT mode, D3D12_SHADING_RATE rate, D3D12_SHADING_RATE_COMBINER first,
                  D3D12_SHADING_RATE_COMBINER second, bool with_image, UINT samples = 1) -> HRESULT {
    // 4x targets are not read back
    auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE first_rtv{rtv.ptr + (samples > 1 ? 2 : 0) * increment};
    list->CopyBufferRegion(counts.Get(), 0, zeros.Get(), 0, counts_size);
    D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
    uav.UAV.pResource = counts.Get();
    list->ResourceBarrier(1, &uav);
    if (with_image)
      transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
    const float clear[4] = {-1, -1, -1, -1};
    for (UINT i = 0; i < 2; i++)
      list->ClearRenderTargetView({first_rtv.ptr + i * increment}, clear, 0, nullptr);
    list->OMSetRenderTargets(2, &first_rtv, TRUE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, 1, &mode, 0);
    list->SetGraphicsRootUnorderedAccessView(1, counts->GetGPUVirtualAddress());
    D3D12_VIEWPORT viewport{0, 0, 2.0f * size, 2.0f * size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_SHADING_RATE_COMBINER combiners[2] = {first, second};
    list->RSSetShadingRate(rate, combiners);
    list->RSSetShadingRateImage(with_image ? image.Get() : nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    // as a list starts
    list->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
    list->RSSetShadingRateImage(nullptr);
    if (with_image)
      transition(list.Get(), image.Get(), D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT i = 0; i < 2 && samples == 1; i++) {
      transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = {i * row * size, {formats[i], size, size, 1, (UINT)row}};
      D3D12_TEXTURE_COPY_LOCATION src{targets[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), row * size * 2, counts.Get(), 0, counts_size);
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
      for (UINT x = 0; x < size; x++) {
        auto &p = got[y * size + x];
        memcpy(p.color, mapped + y * row + x * texel[0], sizeof(p.color));
        memcpy(p.info, mapped + row * size + y * row + x * texel[1], sizeof(p.info));
      }
    memcpy(ran.data(), mapped + row * size * 2, counts_size / 2);
    memcpy(covered.data(), mapped + row * size * 2 + counts_size / 2, counts_size / 2);
    readback->Unmap(0, nullptr);
    allocator->Reset();
    return list->Reset(allocator.Get(), nullptr);
  };

  // the triangle's attributes at a point of the target, perspective correct over w of 1, 4 and 16 (mode 1), and the
  // noperspective one
  auto attribute = [](float x, float y, bool perspective) {
    float b1 = x / 64, b2 = y / 64, b0 = 1 - b1 - b2;
    float w[3] = {1, perspective ? 4.0f : 1.0f, perspective ? 16.0f : 1.0f};
    float over = b0 / w[0] + b1 / w[1] + b2 / w[2];
    float a = (b1 * 16 / w[1] + b2 * 6 / w[2]) / over;
    return std::pair{a, b1 * 10 + b2 * 14};
  };
  // a pixel's coarse pixel at a rate (log2 of its width, then height, D3D12_SHADING_RATE's way), and its center
  struct Coarse {
    UINT width, height;
    float cx, cy;
  };
  auto coarse = [](UINT x, UINT y, UINT rate) {
    UINT w = 1u << (rate >> 2), h = 1u << (rate & 3);
    return Coarse{w, h, w == 1 ? x + 0.5f : float(x & ~1u) + 1, h == 1 ? y + 0.5f : float(y & ~1u) + 1};
  };
  auto report = [&](const char *what, UINT x, UINT y, const char *field, double value, double want) {
    // each step's first few
    if (failures++ - failures_before < 8)
      printf("%s: pixel (%u, %u)'s %s is %g, want %g\n", what, x, y, field, value, want);
  };
  // the target and the runs, against the rate each pixel has (`rate_at`) for ps (or ps_rate)
  auto expect = [&](const char *what, const std::function<UINT(UINT, UINT)> &rate_at, bool perspective,
                    bool rate_input) {
    std::vector<uint32_t> want_ran(size * size);
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto rate = rate_at(x, y);
        auto c = coarse(x, y, rate);
        auto &p = got[y * size + x];
        auto [a, n] = attribute(c.cx, c.cy, perspective);
        if (p.color[0] != c.cx || p.color[1] != c.cy) {
          report(what, x, y, "SV_Position.x", p.color[0], c.cx);
          report(what, x, y, "SV_Position.y", p.color[1], c.cy);
        }
        if (std::fabs(p.color[2] - a) > 1e-4f * std::max(1.0f, std::fabs(a)))
          report(what, x, y, "perspective input", p.color[2], a);
        if (std::fabs(p.color[3] - n) > 1e-4f * std::max(1.0f, std::fabs(n)))
          report(what, x, y, "noperspective input", p.color[3], n);
        if (rate_input) {
          if (p.info[0] != rate)
            report(what, x, y, "SV_ShadingRate", p.info[0], rate);
          continue;
        }
        // every pixel of the coarse pixel covered, a sample each
        uint32_t coverage = (1u << (c.width * c.height)) - 1;
        if (p.info[0] != coverage)
          report(what, x, y, "SV_Coverage", p.info[0], coverage);
        if (p.info[1] != 10 * c.width)
          report(what, x, y, "ddx(n) * 64", p.info[1], 10 * c.width);
        want_ran[UINT(c.cy) * size + UINT(c.cx)] = 1;
      }
    if (rate_input)
      return;
    for (UINT i = 0; i < size * size; i++)
      if (ran[i] != want_ran[i])
        report(what, i % size, i / size, "runs there", ran[i], want_ran[i]);
  };
  auto everywhere = [](UINT rate) { return [=](UINT, UINT) { return rate; }; };
  const auto pass = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, override = D3D12_SHADING_RATE_COMBINER_OVERRIDE;

  const D3D12_SHADING_RATE rates[] = {
      D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_2X2
  };
  const char *names[] = {"1x1", "1x2", "2x1", "2x2"};
  for (UINT perspective = 0; perspective < 2; perspective++)
    for (UINT i = 0; i < 4; i++) {
      failures_before = failures;
      step("RSSetShadingRate(%s), %s", names[i], perspective ? "in perspective" : "w 1");
      CHECK(draw(shaded.Get(), perspective, rates[i], pass, pass, false));
      expect(names[i], everywhere(rates[i]), perspective, false);
    }

  failures_before = failures;

  step("the rates the image's tiles override: 2x2 and 2x1 above, 1x1 below, beyond the image");
  CHECK(set_image(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X1));
  CHECK(draw(shaded.Get(), 0, D3D12_SHADING_RATE_1X2, pass, override, true));
  expect(
      "the image",
      [&](UINT x, UINT y) -> UINT {
        return y >= tile ? D3D12_SHADING_RATE_1X1 : x < tile ? D3D12_SHADING_RATE_2X2 : D3D12_SHADING_RATE_2X1;
      },
      false, false
  );

  failures_before = failures;

  step("the combiners of a 2x1 rate and an image of 1x2: min, max, sum and passthrough");
  CHECK(set_image(D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_1X2));
  // in the image, and below it, where a tile is 1x1
  const std::tuple<D3D12_SHADING_RATE_COMBINER, D3D12_SHADING_RATE, D3D12_SHADING_RATE> combined[] = {
      {D3D12_SHADING_RATE_COMBINER_MIN, D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_1X1},
      {D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X1},
      {D3D12_SHADING_RATE_COMBINER_SUM, D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X1},
      {pass, D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_2X1},
  };
  const char *combiner_names[] = {"min", "max", "sum", "passthrough"};
  for (UINT i = 0; i < 4; i++) {
    auto [combiner, inside, below] = combined[i];
    CHECK(draw(shaded.Get(), 0, D3D12_SHADING_RATE_2X1, pass, combiner, true));
    expect(combiner_names[i], [&](UINT, UINT y) -> UINT { return y < tile ? inside : below; }, false, false);
  }
  failures_before = failures;
  step("a sum past 2x2 is 2x2");
  CHECK(set_image(D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X2));
  CHECK(draw(shaded.Get(), 0, D3D12_SHADING_RATE_2X2, pass, D3D12_SHADING_RATE_COMBINER_SUM, true));
  expect("sum of 2x2 and 2x2", everywhere(D3D12_SHADING_RATE_2X2), false, false);

  failures_before = failures;

  step("a discard drops the whole coarse pixel: at 2x2, columns 0 to 13");
  CHECK(draw(shaded.Get(), 2, D3D12_SHADING_RATE_2X2, pass, pass, false));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      float want = x < 14 ? -1.0f : float(x & ~1u) + 1;
      if (got[y * size + x].color[0] != want)
        report("discard", x, y, "SV_Position.x", got[y * size + x].color[0], want);
    }

  failures_before = failures;

  step("a shader that outputs depth shades every pixel at 2x2");
  CHECK(draw(depth.Get(), 0, D3D12_SHADING_RATE_2X2, pass, pass, false));
  for (UINT i = 0; i < size * size; i++) {
    if (got[i].color[0] != i % size + 0.5f)
      report("depth output", i % size, i / size, "SV_Position.x", got[i].color[0], i % size + 0.5f);
    if (ran[i] != 1)
      report("depth output", i % size, i / size, "runs there", ran[i], 1);
  }

  failures_before = failures;

  step("an output coverage of 1 is the first pixel's of each coarse pixel");
  CHECK(draw(masked.Get(), 0, D3D12_SHADING_RATE_2X2, pass, pass, false));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      float want = (x & 1) || (y & 1) ? -1.0f : float(x) + 1;
      if (got[y * size + x].color[0] != want)
        report("output coverage", x, y, "SV_Position.x", got[y * size + x].color[0], want);
    }

  // a triangle's edge: a coarse pixel that is partly covered runs once, its coverage its covered pixels'
  for (UINT i = 1; i < 4; i++) {
    failures_before = failures;
    step("%s over the edge from (8.25, 40) to (40, 8.25)", names[i]);
    CHECK(draw(shaded.Get(), 3, rates[i], pass, pass, false));
    std::vector<uint32_t> want_ran(size * size), want_covered(size * size);
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto c = coarse(x, y, rates[i]);
        bool inside = x + y + 1 > 48.25f;
        float want = inside ? c.cx : -1.0f;
        if (got[y * size + x].color[0] != want)
          report(names[i], x, y, "SV_Position.x", got[y * size + x].color[0], want);
        if (!inside)
          continue;
        UINT at = UINT(c.cy) * size + UINT(c.cx);
        want_ran[at] = 1;
        UINT fx = c.width == 2 ? x & 1 : 0, fy = c.height == 2 ? y & 1 : 0;
        want_covered[at] |= 1u << (fy * c.width + fx);
      }
    for (UINT j = 0; j < size * size; j++) {
      if (ran[j] != want_ran[j])
        report(names[i], j % size, j / size, "runs there", ran[j], want_ran[j]);
      if (covered[j] != want_covered[j])
        report(names[i], j % size, j / size, "SV_Coverage there", covered[j], want_covered[j]);
    }
  }

  // 4x: SV_Coverage's bits are the samples of the coarse pixel's first pixel, then its second's
  for (UINT i = 1; i < 4; i++) {
    failures_before = failures;
    step("%s at 4x", names[i]);
    CHECK(draw(shaded4.Get(), 4, rates[i], pass, pass, false, 4));
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        auto c = coarse(x, y, rates[i]);
        if (x != UINT(c.cx) || y != UINT(c.cy))
          continue;
        uint32_t want = (1u << (4 * c.width * c.height)) - 1;
        if (covered[y * size + x] != want || ran[y * size + x] < 1)
          report(names[i], x, y, "SV_Coverage there", covered[y * size + x], want);
      }
  }

  if (dxil) {
    failures_before = failures;
    step("the vertex shader's SV_ShadingRate of 2x2, overriding, and the pixel shader's SV_ShadingRate");
    CHECK(draw(primitive_rate.Get(), 0, D3D12_SHADING_RATE_1X1, override, pass, false));
    expect("SV_ShadingRate, overriding", everywhere(D3D12_SHADING_RATE_2X2), false, true);
    failures_before = failures;
    step("the vertex shader's SV_ShadingRate passed through: 1x1");
    CHECK(draw(primitive_rate.Get(), 0, D3D12_SHADING_RATE_1X1, pass, override, false));
    expect("SV_ShadingRate, passed through", everywhere(D3D12_SHADING_RATE_1X1), false, true);
    failures_before = failures;
    step("the geometry shader's SV_ShadingRate of 2x1, overriding");
    CHECK(draw(geometry_rate.Get(), 0, D3D12_SHADING_RATE_1X1, override, pass, false));
    expect("the geometry shader's SV_ShadingRate", everywhere(D3D12_SHADING_RATE_2X1), false, true);
  }

  printf("%s: %u wrong values\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
