// contract: an SV_ViewportArrayIndex of 16 or more selects viewport 0 for everything a viewport decides: "Values
// outside [0..n-1] are treated as 0 for indexing viewports", n being "the maximum number of viewports allowed", 16
// (D3D11.3 functional spec, 15.8.1). an index below 16 past the viewports set selects a viewport that is not set,
// which is of no size and has the empty scissor (15.6, 15.7), so nothing is drawn through it.
// - the depth a pixel shader writes (SV_Depth) is clamped to the selected viewport's MinDepth and MaxDepth (17.10).
//   viewport 0's range is [0.25, 0.5] and viewport 1's [0.6, 0.7]; a pixel shader writes 0.9 or 0.1 through
//   indices 0, 1, 16, 17 and 0xffffffff, each to a pixel of its own. the blend factor is set to values no viewport
//   has, so a range read from past the viewports' cannot pass for viewport 0's.
// - conservative rasterization grows a triangle in the selected viewport's window (15.17): a sliver inside one pixel,
//   over no pixel center, covers that pixel and no other through indices 0, 16 and 0xffffffff, with one viewport set,
//   and nothing through index 5.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.8.1%20Viewport%20Selection
#include "d3d12_test.hpp"
#include <cmath>

static constexpr UINT depth_width = 8, size = 8;

static const char hlsl[] = R"hlsl(
cbuffer Draw : register(b0) { uint viewport; float depth; float2 low; float2 high; };
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }
struct G { float4 position : SV_Position; uint viewport : SV_ViewportArrayIndex; };
// a triangle over the window rectangle from `low` to `high` (in pixels of a WIDTH x HEIGHT target), its corners
// the rectangle's top left, top right and bottom left
[maxvertexcount(3)]
void gs(point float4 p[1] : SV_Position, inout TriangleStream<G> stream) {
  float2 corners[3] = {low, float2(high.x, low.y), float2(low.x, high.y)};
  for (int i = 0; i < 3; i++) {
    G g;
    g.position = float4(corners[i] / float2(WIDTH, HEIGHT) * float2(2, -2) + float2(-1, 1), 0.5, 1);
    g.viewport = viewport;
    stream.Append(g);
  }
}
float ps_depth(G input) : SV_Depth { return depth; }
uint ps_cover(G input) : SV_Target { return 1; }
)hlsl";

struct Constants {
  UINT viewport;
  float depth, low[2], high[2];
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> depth_defines = {"WIDTH=" + std::to_string(depth_width), "HEIGHT=1"},
                                 cover_defines = {"WIDTH=" + std::to_string(size), "HEIGHT=" + std::to_string(size)};
  auto vs = compiler.compile(hlsl, "vs", "vs", depth_defines), gs_depth = compiler.compile(hlsl, "gs", "gs", depth_defines),
       gs_cover = compiler.compile(hlsl, "gs", "gs", cover_defines),
       ps_depth = compiler.compile(hlsl, "ps_depth", "ps", depth_defines),
       ps_cover = compiler.compile(hlsl, "ps_cover", "ps", depth_defines);
  if (!expect(!vs.empty() && !gs_depth.empty() && !gs_cover.empty() && !ps_depth.empty() && !ps_cover.empty(),
              "HLSL did not compile"))
    return verdict();

  step("create the device");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, sizeof(Constants) / 4};
  auto rs = root_signature(device.Get(), {1, &parameter});
  CHECK(rs ? S_OK : E_FAIL);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

  {
    step("written depth: create the pipeline and the depth target");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps_depth)};
    desc.GS = bytecode(gs_depth);
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, depth_width, 1, 1, 1, DXGI_FORMAT_D32_FLOAT,
                                    {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    ComPtr<ID3D12Resource> target;
    D3D12_CLEAR_VALUE clear{DXGI_FORMAT_D32_FLOAT};
    clear.DepthStencil.Depth = 1;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&target)
    ));
    ComPtr<ID3D12DescriptorHeap> dsvs;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
    auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(target.Get(), nullptr, dsv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(forget(readback.Get()));

    // each pixel's viewport index and written depth, and the depth it must hold
    struct Pixel {
      UINT viewport;
      float written, want;
    };
    const Pixel pixels[depth_width] = {
        {0, 0.9f, 0.5f},  {0, 0.1f, 0.25f},  {16, 0.9f, 0.5f},          {16, 0.1f, 0.25f},
        {17, 0.9f, 0.5f}, {1, 0.9f, 0.7f}, {0xffffffff, 0.9f, 0.5f}, {0xffffffff, 0.1f, 0.25f},
    };
    step("written depth: draw a pixel through each index");
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    const D3D12_VIEWPORT viewports[] = {{0, 0, depth_width, 1, 0.25f, 0.5f}, {0, 0, depth_width, 1, 0.6f, 0.7f}};
    const D3D12_RECT scissors[] = {{0, 0, depth_width, 1}, {0, 0, depth_width, 1}};
    list->RSSetViewports(2, viewports);
    list->RSSetScissorRects(2, scissors);
    const float factor[4] = {0.75f, 0.8f, 0.05f, 0.95f};
    list->OMSetBlendFactor(factor);
    list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    for (UINT x = 0; x < depth_width; x++) {
      Constants c{pixels[x].viewport, pixels[x].written, {x + 0.25f, 0.25f}, {x + 1.0f, 1.0f}};
      list->SetGraphicsRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
      list->DrawInstanced(1, 1, 0, 0);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION placed{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        texture{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    placed.PlacedFootprint = footprint;
    list->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    float *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT x = 0; x < depth_width; x++)
      expect(got[x] == pixels[x].want, "pixel %u (viewport index %#x, depth %g written) holds %g, want %g", x,
             pixels[x].viewport, pixels[x].written, got[x], pixels[x].want);
    readback->Unmap(0, nullptr);
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.ConservativeRasterizationTier == D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED) {
    printf("conservative rasterization is not supported\n");
    return verdict();
  }
  for (UINT index : {0u, 16u, 0xffffffffu, 5u}) {
    step("conservative sliver through viewport index %#x", index);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps_cover)};
    desc.GS = bytecode(gs_cover);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT,
                                    {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
    ));
    ComPtr<ID3D12DescriptorHeap> rtvs;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(forget(readback.Get()));

    CHECK(allocator->Reset());
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    const D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    const D3D12_RECT scissor{0, 0, size, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    // inside pixel (3, 5), clear of its center (3.5, 5.5)
    Constants c{index, 0, {3.2f, 5.1f}, {3.8f, 5.3f}};
    list->SetGraphicsRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    list->DrawInstanced(1, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION placed{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        texture{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    placed.PlacedFootprint = footprint;
    list->CopyTextureRegion(&placed, 0, 0, 0, &texture, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    uint8_t *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        UINT value = reinterpret_cast<const UINT *>(got + y * footprint.Footprint.RowPitch)[x],
             want = x == 3 && y == 5 && index != 5;
        expect(value == want, "viewport index %#x: pixel %u,%u holds %u, want %u", index, x, y, value, want);
      }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
