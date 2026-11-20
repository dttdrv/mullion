// contract: D3D11.3 16.2: "the Pixel Shader is invoked once for each pixel covered by the primitive (pixel-frequency),
// or once per sample (sample-frequency)". 16.13: "After the discard is issued, further operations on UAVs do not
// change the UAV memory". 3.5.6.4: UAV-only rendering rasterizes at ForcedSampleCount. source:
// D3D11_3_FunctionalSpec.htm, which D3D12 keeps for these rules. each run counts its pixel before a discard of every
// third pixel and after it, and adds to one total past it; a pixel is covered when any sample of the standard
// pattern (../raster_samples.hpp) is, by ../raster.hpp's coverage. the triangle lies inside the viewport, so no
// clipping splits it.
#include "d3d12_test.hpp"
#include "../raster_samples.hpp"

static const char writes[] = R"hlsl(
StructuredBuffer<float4> positions : register(t0);
RWStructuredBuffer<uint> counts : register(u1);
float4 vs(uint id : SV_VertexID) : SV_Position { return positions[id]; }
uint run(float4 p) {
  uint pixel = uint(p.y) * WIDTH + uint(p.x);
  InterlockedAdd(counts[pixel], 1);
  if (pixel % 3 == 0)
    discard;
  InterlockedAdd(counts[WIDTH * HEIGHT + pixel], 1);
  InterlockedAdd(counts[2 * WIDTH * HEIGHT], 1);
  return pixel;
}
float4 ps_pixel(float4 p : SV_Position) : SV_Target { return run(p); }
float4 ps_sample(float4 p : SV_Position, uint sample : SV_SampleIndex) : SV_Target { return run(p) + sample; }
)hlsl";

int
main(int argc, char **argv) {
  using namespace raster;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines{"WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height)};
  auto vs = compiler.compile(writes, "vs", "vs", defines);
  std::string ps[2] = {compiler.compile(writes, "ps_pixel", "ps", defines),
                       compiler.compile(writes, "ps_sample", "ps", defines)};
  if (!expect(!vs.empty() && !ps[0].empty() && !ps[1].empty(), "shaders did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[1].Descriptor.ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  auto c = base("inner triangle with partly covered edge pixels");
  c.vertices = {vertex(1.3f, 1.1f), vertex(width - 1.3f, 2.2f), vertex(2.1f, height - 1.4f)};
  auto positions = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, c.vertices.size() * sizeof(Color),
                          D3D12_RESOURCE_STATE_GENERIC_READ);
  Color *mapped;
  CHECK(positions->Map(0, nullptr, (void **)&mapped));
  for (size_t i = 0; i < c.vertices.size(); i++)
    mapped[i] = c.vertices[i].position;
  const UINT pixels = width * height, words = 2 * pixels + 1;
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, words * 4, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, words * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, words * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  void *zero;
  CHECK(zeros->Map(0, nullptr, &zero));
  memset(zero, 0, words * 4);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const DXGI_FORMAT colour = DXGI_FORMAT_R8G8B8A8_UNORM;
  struct Mode {
    UINT samples;
    bool uav_only, per_sample;
  };
  std::vector<Mode> modes;
  for (UINT samples : {1u, 4u, 8u})
    for (bool per_sample : {false, true})
      modes.push_back({samples, false, per_sample});
  // sample-frequency runs are invalid without render targets (3.5.6.4)
  for (UINT samples : {0u, 4u, 8u})
    modes.push_back({samples, true, false});
  for (auto mode : modes) {
    step("%u samples, %s, %s-frequency", mode.samples, mode.uav_only ? "UAVs alone" : "render target",
         mode.per_sample ? "sample" : "pixel");
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{colour, std::max(mode.samples, 1u)};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)));
    // 4x is required of every render target format, 8x is optional (format support for feature level 11_0
    // hardware): Metal rasterizes 8 samples on Apple10 alone (Metal feature set tables)
    if (!levels.NumQualityLevels && mode.samples > 4)
      continue;
    if (!expect(levels.NumQualityLevels, "%ux is not supported", mode.samples))
      continue;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs), desc.PS = bytecode(ps[mode.per_sample]);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ForcedSampleCount = mode.uav_only ? mode.samples : 0;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = !mode.uav_only, desc.RTVFormats[0] = mode.uav_only ? DXGI_FORMAT_UNKNOWN : colour;
    desc.SampleDesc = {mode.uav_only ? 1 : mode.samples, 0};
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    ComPtr<ID3D12Resource> target;
    if (!mode.uav_only) {
      const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, colour, {mode.samples, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            nullptr, IID_PPV_ARGS(&target)));
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    }
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipeline.Get()));
    list->CopyResource(counts.Get(), zeros.Get());
    transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootShaderResourceView(0, positions->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(1, counts->GetGPUVirtualAddress());
    list->OMSetRenderTargets(!mode.uav_only, mode.uav_only ? nullptr : &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(UINT(c.vertices.size()), 1, 0, 0);
    transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), counts.Get());
    transition(list.Get(), counts.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
    const UINT *got;
    if (!expect(ran == S_OK, "the device after the draw: %08lx", ran) ||
        FAILED(readback->Map(0, nullptr, (void **)&got)))
      return verdict();

    std::vector<std::vector<Color>> coverage;
    for (auto p : raster_samples::pattern(std::max(mode.samples, 1u)))
      coverage.push_back(expected(c, double(p[0]) / sample_grid, double(p[1]) / sample_grid));
    unsigned wrong = 0, kept = 0, partial = 0;
    for (UINT p = 0; p < pixels; p++) {
      unsigned covered = 0;
      for (auto &samples : coverage)
        covered += samples[p][0] != 0;
      partial += covered && covered < coverage.size();
      unsigned runs = mode.per_sample ? covered : covered != 0, after = p % 3 ? runs : 0;
      kept += after;
      if ((got[p] != runs || got[pixels + p] != after) && wrong++ < 4)
        expect(false, "pixel %u,%u: %u runs and %u after the discard, want %u and %u", p % width, p / width, got[p],
               got[pixels + p], runs, after);
    }
    expect(!wrong, "%u pixels ran or wrote a wrong number of times", wrong);
    expect(got[2 * pixels] == kept, "%u added past the discard, want %u", got[2 * pixels], kept);
    expect(mode.samples < 2 || partial, "no pixel is partly covered at %u samples", mode.samples);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
