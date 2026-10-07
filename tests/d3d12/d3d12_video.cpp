// contract: frames of textured squares, presented to a window, show each texture at the level of detail Direct3D
// defines, in the back buffer and in the window's own pixels. every level of a texture is one color, so a pixel says
// which level it was sampled from, or which two and how much of each: a texture shown at a lower resolution than it
// has is a wrong color. the reference is the rule itself (D3D11.3 7.18.11 "LOD Calculations", 7.18.10 "Mipmap
// Selection"), computed here from each square's footprint:
// - the level is log2 of the longer side of a pixel's footprint in texels; with anisotropic filtering log2 of the
//   shorter side, or of the longer over MaxAnisotropy when the footprint is more stretched than that;
// - plus MipLODBias, then "max(MinLOD, min(MaxLOD, biasedLOD))", then no less than the view's ResourceMinLODClamp
//   (5.8) and inside the view's levels, which start at MostDetailedMip. D3D12_SAMPLER_DESC asks for a MaxLOD of
//   at least MinLOD; for one that is less, 7.18.11 says what a sampler does: "MinLOD takes precedence";
// - a linear mip filter blends the level and the next by the fraction, a point one takes the level of LOD + 0.5;
//   LOD has 8 bits of fraction (7.18.16.1), so a blend is within two 256ths of the two levels' difference.
// the textures are one of RGBA8 and one of BC1 with levels down to one texel (levels smaller than a block), copied
// in level by level, and a reserved one whose large levels get memory and texels between two frames
// (UpdateTileMappings), as streaming does: before that a sampler's MinLOD keeps to the levels that have memory.
// a non-anisotropic sampler leaves MaxAnisotropy at zero, as applications do.
// each frame is checked twice: the back buffer against the rule, and the window against the back buffer. the squares
// change places every frame, so a window, or a readback, that holds an older frame is wrong. the window's pixels
// are read through GDI, which has them when frames get to the window through GDI (dxgi.presentThroughGDI); the
// Metal view's own pixels are not read by this test.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <dxgi1_4.h>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float2 scale; };
Texture2D t : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o; float2 corner = float2((id << 1) & 2, id & 2);
  o.uv = corner * scale;
  o.pos = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }
)hlsl";

// a level's color in 5, 6 and 5 bits, which BC1 holds exactly, as 8 bit channels (a UNORM's value over its maximum)
struct Color {
  int r, g, b;
};
static Color
color(UINT index) {
  const UINT r5 = (index * 11 + 3) % 32, g6 = 63 - index * 7, b5 = (index * 19 + 5) % 32;
  return {(int)std::lround(r5 * 255 / 31.0), (int)std::lround(g6 * 255 / 63.0), (int)std::lround(b5 * 255 / 31.0)};
}
static UINT16
color565(UINT index) {
  return ((index * 11 + 3) % 32) << 11 | (63 - index * 7) << 5 | ((index * 19 + 5) % 32);
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  SetEnvironmentVariableA("DXMT_CONFIG", "dxgi.presentThroughGDI=True");
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  // squares of `cell` pixels, `across` a row, in a window of `client` pixels; textures of `size` texels
  const UINT client = 256, cell = 32, across = client / cell, buffers = 2, frames = 12;
  const UINT size = 256, levels = 9;
  const DXGI_FORMAT target_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  bool tiles = options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_1;

  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0}};
  D3D12_ROOT_PARAMETER parameters[3] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  parameters[0].Constants = {0, 0, 2};
  parameters[1].DescriptorTable = {1, &ranges[0]};
  parameters[2].DescriptorTable = {1, &ranges[1]};
  auto rs = root_signature(device.Get(), {3, parameters});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(vs), bytecode(ps)};
  pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso_desc.SampleMask = ~0u;
  pso_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = target_format;
  pso_desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the textures, and which color each has at a level: no two have a color at the same level
  enum Kind { Rgba, Bc1, Tiled, Kinds };
  const DXGI_FORMAT formats[Kinds] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
  auto shade = [&](Kind kind, UINT level) { return kind == Rgba ? level : kind == Bc1 ? levels - 1 - level : (level + 3) % levels; };
  ComPtr<ID3D12Resource> textures[Kinds];
  std::vector<ComPtr<ID3D12Resource>> uploads;
  // copies its color into each of the texture's levels from `first` to before `end`, by the footprints an
  // application asks for
  auto fill = [&](Kind kind, UINT first, UINT end) -> HRESULT {
    auto desc = textures[kind]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[levels];
    UINT rows[levels];
    UINT64 total;
    device->GetCopyableFootprints(&desc, 0, levels, 0, footprints, rows, nullptr, &total);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    UINT8 *data;
    HRESULT hr = upload->Map(0, nullptr, (void **)&data);
    if (FAILED(hr))
      return hr;
    for (UINT level = first; level < end; level++) {
      auto &f = footprints[level];
      auto c = color(shade(kind, level));
      for (UINT row = 0; row < rows[level]; row++) {
        auto at = data + f.Offset + row * f.Footprint.RowPitch;
        if (kind == Bc1) {
          // a block of one color: both of its colors that one, and every texel the first
          const UINT16 c565 = color565(shade(kind, level));
          const UINT8 block[8] = {UINT8(c565), UINT8(c565 >> 8), UINT8(c565), UINT8(c565 >> 8)};
          for (UINT x = 0; x < (f.Footprint.Width + 3) / 4; x++)
            memcpy(at + x * sizeof(block), block, sizeof(block));
        } else {
          for (UINT x = 0; x < f.Footprint.Width; x++)
            memcpy(at + x * 4, std::array<UINT8, 4>{UINT8(c.r), UINT8(c.g), UINT8(c.b), 255}.data(), 4);
        }
      }
      D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, to{textures[kind].Get()};
      from.PlacedFootprint = f;
      to.SubresourceIndex = level;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    uploads.push_back(upload);
    return S_OK;
  };
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  for (Kind kind : {Rgba, Bc1, Tiled}) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, (UINT16)levels, formats[kind], {1, 0}};
    if (kind != Tiled) {
      CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[kind])));
      CHECK(fill(kind, 0, levels));
      transition(list.Get(), textures[kind].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else if (tiles) {
      desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
      CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[kind])));
    }
  }
  // the reserved texture's tiles: its small levels share some (the packed ones) and have them from the start
  UINT tile_count = 0, tilings = levels;
  D3D12_PACKED_MIP_INFO packed{};
  D3D12_SUBRESOURCE_TILING tiling[levels]{};
  ComPtr<ID3D12Heap> tile_heap;
  if (tiles) {
    step("a reserved texture: memory and texels for its packed levels");
    device->GetResourceTiling(textures[Tiled].Get(), &tile_count, &packed, nullptr, &tilings, 0, tiling);
    D3D12_HEAP_DESC heap_desc{UINT64(tile_count) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, default_heap, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
    CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&tile_heap)));
    if (!expect(packed.NumStandardMips && packed.NumPackedMips && packed.NumStandardMips + packed.NumPackedMips == levels,
                "GetResourceTiling: %u standard and %u packed levels of %u", packed.NumStandardMips, packed.NumPackedMips, levels))
      return verdict();
    D3D12_TILED_RESOURCE_COORDINATE at{0, 0, 0, packed.NumStandardMips};
    D3D12_TILE_REGION_SIZE region{packed.NumTilesForPackedMips};
    UINT heap_offset = tile_count - packed.NumTilesForPackedMips, range_tiles = packed.NumTilesForPackedMips;
    queue->UpdateTileMappings(textures[Tiled].Get(), 1, &at, &region, tile_heap.Get(), 1, nullptr, &heap_offset, &range_tiles, D3D12_TILE_MAPPING_FLAG_NONE);
    CHECK(fill(Tiled, packed.NumStandardMips, levels));
    transition(list.Get(), textures[Tiled].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  // the views of the textures: all of one, some levels of it, or with a clamp
  struct View {
    Kind kind;
    UINT first, count;
    float clamp;
  };
  const View views[] = {{Rgba, 0, levels, 0}, {Bc1, 0, levels, 0}, {Tiled, 0, levels, 0}, {Rgba, 3, 2, 0}, {Rgba, 0, levels, 2.5f}};
  enum { Whole, Compressed, Streamed, Part, Clamped };
  // a square: its footprint's sides in texels of the view's first level, as powers of two, its sampler, its view,
  // and the frames it is drawn in: all, or the ones before or after the reserved texture gets all its memory
  enum When { Always, Before, After };
  const float no_limit = D3D12_FLOAT32_MAX, standard = packed.NumStandardMips;
  const D3D12_FILTER trilinear = D3D12_FILTER_MIN_MAG_MIP_LINEAR, nearest = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
                     anisotropic = D3D12_FILTER_ANISOTROPIC;
  struct Square {
    const char *what;
    float du, dv;
    D3D12_FILTER filter;
    UINT max_anisotropy;
    float bias, min_lod, max_lod;
    UINT view;
    When when;
  };
  const Square squares[] = {
      {"one texel a pixel", 0, 0, trilinear, 0, 0, 0, no_limit, Whole},
      {"two texels a pixel", 1, 1, trilinear, 0, 0, 0, no_limit, Whole},
      {"four texels a pixel", 2, 2, trilinear, 0, 0, 0, no_limit, Whole},
      {"eight texels a pixel", 3, 3, trilinear, 0, 0, 0, no_limit, Whole},
      {"between levels 1 and 2", 1.5f, 1.5f, trilinear, 0, 0, 0, no_limit, Whole},
      {"a quarter past level 2", 2.25f, 2.25f, trilinear, 0, 0, 0, no_limit, Whole},
      {"a point mip filter below the middle of two levels", 1.375f, 1.375f, nearest, 0, 0, 0, no_limit, Whole},
      {"a point mip filter above the middle of two levels", 1.625f, 1.625f, nearest, 0, 0, 0, no_limit, Whole},
      {"magnified four times", -2, -2, trilinear, 0, 0, 0, no_limit, Whole},
      {"a bias of 1", 1, 1, trilinear, 0, 1, 0, no_limit, Whole},
      {"a bias of -1", 2, 2, trilinear, 0, -1, 0, no_limit, Whole},
      {"a bias of a half", 0, 0, trilinear, 0, 0.5f, 0, no_limit, Whole},
      {"MaxLOD below the footprint's level", 3, 3, trilinear, 0, 0, 0, 1, Whole},
      {"MinLOD above the footprint's level", 0, 0, trilinear, 0, 0, 2, no_limit, Whole},
      {"MinLOD above MaxLOD", 0, 0, trilinear, 0, 0, 3, 1, Whole},
      {"8 by 1 texels, anisotropy of 16", 3, 0, anisotropic, 16, 0, 0, no_limit, Whole},
      {"1 by 8 texels, anisotropy of 16", 0, 3, anisotropic, 16, 0, 0, no_limit, Whole},
      {"8 by 1 texels, anisotropy of 4", 3, 0, anisotropic, 4, 0, 0, no_limit, Whole},
      {"8 by 1 texels, anisotropy of 2", 3, 0, anisotropic, 2, 0, 0, no_limit, Whole},
      {"8 by 2 texels, anisotropy of 16", 3, 1, anisotropic, 16, 0, 0, no_limit, Whole},
      {"8 by 1 texels, not anisotropic", 3, 0, trilinear, 0, 0, 0, no_limit, Whole},
      {"a view of two levels, its first", 0, 0, trilinear, 0, 0, 0, no_limit, Part},
      {"a view of two levels, between them", 0.5f, 0.5f, trilinear, 0, 0, 0, no_limit, Part},
      {"a view of two levels, past its last", 3, 3, trilinear, 0, 0, 0, no_limit, Part},
      {"a view's clamp above the footprint's level", 0, 0, trilinear, 0, 0, 0, no_limit, Clamped},
      {"a view's clamp below the footprint's level", 4, 4, trilinear, 0, 0, 0, no_limit, Clamped},
      {"BC1, one texel a pixel", 0, 0, trilinear, 0, 0, 0, no_limit, Compressed},
      {"BC1, its level of 8 texels", 5, 5, trilinear, 0, 0, 0, no_limit, Compressed},
      {"BC1, its level of one block", 6, 6, trilinear, 0, 0, 0, no_limit, Compressed},
      {"BC1, its level of 2 texels", 7, 7, trilinear, 0, 0, 0, no_limit, Compressed},
      {"BC1, its level of 1 texel", 8, 8, trilinear, 0, 0, 0, no_limit, Compressed},
      {"a reserved texture with memory for its packed levels", 0, 0, trilinear, 0, 0, standard, no_limit, Streamed, Before},
      {"a reserved texture, a packed level", standard + 1, standard + 1, trilinear, 0, 0, standard, no_limit, Streamed, Before},
      {"a reserved texture with all its memory", 0, 0, trilinear, 0, 0, 0, no_limit, Streamed, After},
      {"a reserved texture with all its memory, its second level", 1, 1, trilinear, 0, 0, 0, no_limit, Streamed, After},
  };
  const UINT count = std::size(squares);
  // the color the rule gives a square, and how far from it a channel may be for the fraction's 8 bits
  struct Shown {
    Color color;
    int tolerance;
  };
  auto reference = [&](const Square &square) -> Shown {
    auto &view = views[square.view];
    float longer = std::max(square.du, square.dv), shorter = std::min(square.du, square.dv);
    bool stretched = longer - shorter > std::log2((float)square.max_anisotropy);
    float lod = square.filter != anisotropic ? longer : stretched ? longer - std::log2((float)square.max_anisotropy) : shorter;
    lod = std::max(square.min_lod, std::min(square.max_lod, lod + square.bias));
    lod = std::clamp(std::max(lod, view.clamp - view.first), 0.f, float(view.count - 1));
    bool blends = D3D12_DECODE_MIP_FILTER(square.filter) == D3D12_FILTER_TYPE_LINEAR || square.filter == anisotropic;
    UINT level = blends ? (UINT)lod : (UINT)(lod + 0.5f);
    float weight = blends ? lod - level : 0;
    Color a = color(shade(view.kind, view.first + level)), b = color(shade(view.kind, view.first + std::min(level + 1, view.count - 1)));
    auto mix = [&](int x, int y) { return (int)std::lround(x + (y - x) * weight); };
    int widest = std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
    return {{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)}, weight ? (widest * 2 + 255) / 256 : 0};
  };

  ComPtr<ID3D12DescriptorHeap> view_heap, sampler_heap, rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (UINT)std::size(views), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      sampler_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, buffers};
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&view_heap)));
  CHECK(device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&sampler_heap)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  const UINT view_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
             sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER),
             rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = 0; i < std::size(views); i++) {
    if (!textures[views[i].kind])
      continue;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{formats[views[i].kind], D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    desc.Texture2D = {views[i].first, views[i].count, 0, views[i].clamp};
    device->CreateShaderResourceView(textures[views[i].kind].Get(), &desc, {view_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * view_step});
  }
  for (UINT i = 0; i < count; i++) {
    D3D12_SAMPLER_DESC desc{squares[i].filter, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            squares[i].bias, squares[i].max_anisotropy, D3D12_COMPARISON_FUNC_NEVER, {}, squares[i].min_lod, squares[i].max_lod};
    device->CreateSampler(&desc, {sampler_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * sampler_step});
  }

  step("a window of %u pixels with a swap chain of %u buffers", client, buffers);
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_video"};
  RegisterClassA(&wc);
  RECT rect{0, 0, (LONG)client, (LONG)client};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowA("d3d12_video", "d3d12_video", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, rect.right - rect.left,
                              rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 sc_desc{client, client, target_format, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers,
                                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD};
  ComPtr<IDXGISwapChain1> sc1;
  ComPtr<IDXGISwapChain3> swapchain;
  CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &sc_desc, nullptr, nullptr, &sc1));
  CHECK(sc1.As(&swapchain));
  ComPtr<ID3D12Resource> back[buffers];
  for (UINT i = 0; i < buffers; i++) {
    CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&back[i])));
    device->CreateRenderTargetView(back[i].Get(), nullptr, {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step});
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT frame_footprint;
  UINT64 frame_bytes;
  auto back_desc = back[0]->GetDesc();
  device->GetCopyableFootprints(&back_desc, 0, 1, 0, &frame_footprint, nullptr, nullptr, &frame_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, frame_bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // the places a square is looked at: its middle and near its corners
  const UINT places[][2] = {{cell / 2, cell / 2}, {3, 3}, {cell - 4, 3}, {3, cell - 4}, {cell - 4, cell - 4}};
  const float background[4] = {0, 0, 0, 1};
  for (UINT frame = 0; frame < frames; frame++) {
    bool after = frame >= frames / 2;
    if (tiles && frame == frames / 2) {
      step("frame %u: memory and texels for the reserved texture's %u standard levels", frame, packed.NumStandardMips);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      std::vector<D3D12_TILED_RESOURCE_COORDINATE> at;
      std::vector<D3D12_TILE_REGION_SIZE> regions;
      UINT heap_offset = 0, range_tiles = 0;
      for (UINT level = 0; level < packed.NumStandardMips; level++) {
        at.push_back({0, 0, 0, level});
        regions.push_back({UINT(tiling[level].WidthInTiles) * tiling[level].HeightInTiles});
        range_tiles += regions.back().NumTiles;
      }
      queue->UpdateTileMappings(textures[Tiled].Get(), at.size(), at.data(), regions.data(), tile_heap.Get(), 1, nullptr, &heap_offset, &range_tiles, D3D12_TILE_MAPPING_FLAG_NONE);
      transition(list.Get(), textures[Tiled].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(fill(Tiled, 0, packed.NumStandardMips));
      transition(list.Get(), textures[Tiled].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
    }
    step("frame %u: %u squares, each one place on from the frame before", frame, count);
    // the square a place of the window shows in this frame, or none
    auto shown = [&](UINT place) -> const Square * {
      auto &square = squares[(place + frame) % count];
      bool drawn = place < count && textures[views[square.view].kind] && (square.when == Always || (square.when == After) == after);
      return drawn ? &square : nullptr;
    };
    UINT i = swapchain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step};
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtv, background, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    ID3D12DescriptorHeap *heaps[] = {view_heap.Get(), sampler_heap.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT place = 0; place < across * across; place++) {
      auto square = shown(place);
      if (!square)
        continue;
      UINT n = square - squares, x = place % across * cell, y = place / across * cell;
      D3D12_VIEWPORT viewport{(float)x, (float)y, (float)cell, (float)cell, 0, 1};
      D3D12_RECT scissor{(LONG)x, (LONG)y, LONG(x + cell), LONG(y + cell)};
      // texture coordinates across the square that make its pixels' footprints the square's sides in texels
      const float texels = size >> views[square->view].first;
      const float scale[2] = {std::exp2(square->du) * cell / texels, std::exp2(square->dv) * cell / texels};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->SetGraphicsRoot32BitConstants(0, 2, scale, 0);
      list->SetGraphicsRootDescriptorTable(1, {view_heap->GetGPUDescriptorHandleForHeapStart().ptr + square->view * view_step});
      list->SetGraphicsRootDescriptorTable(2, {sampler_heap->GetGPUDescriptorHandleForHeapStart().ptr + n * sampler_step});
      list->DrawInstanced(3, 1, 0, 0);
    }
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{back[i].Get()}, to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = frame_footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), back[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(swapchain->Present(1, 0));

    // the frame as drawn: the back buffer's pixel at each place looked at, against the rule. a square is reported
    // once, with its first wrong place
    auto name = [&](UINT place) { return shown(place) ? shown(place)->what : "the background"; };
    auto pixel_of = [&](UINT place, UINT n) { return std::pair{place % across * cell + places[n][0], place / across * cell + places[n][1]}; };
    std::vector<std::array<int, 3>> drawn;
    UINT8 *texels;
    CHECK(readback->Map(0, nullptr, (void **)&texels));
    for (UINT place = 0; place < across * across; place++) {
      Shown want = shown(place) ? reference(*shown(place)) : Shown{{0, 0, 0}, 0};
      unsigned off_places = 0;
      for (UINT n = 0; n < std::size(places); n++) {
        auto [x, y] = pixel_of(place, n);
        auto texel = texels + frame_footprint.Offset + y * frame_footprint.Footprint.RowPitch + x * 4;
        drawn.push_back({texel[0], texel[1], texel[2]});
        auto &got = drawn.back();
        int off = std::max({std::abs(got[0] - want.color.r), std::abs(got[1] - want.color.g), std::abs(got[2] - want.color.b)});
        if (off > want.tolerance + 1 && !off_places++)
          expect(false, "the back buffer's pixel %u,%u of %s: %d %d %d, want %d %d %d within %d", x, y, name(place), got[0],
                 got[1], got[2], want.color.r, want.color.g, want.color.b, want.tolerance + 1);
      }
    }
    readback->Unmap(0, nullptr);
    // the frame as shown: the window's pixel at each place is the back buffer's, within a step for the window's own
    // rounding. a frame gets to the window some time after Present
    const UINT attempts = 250;
    for (UINT attempt = 0, differ = 1; differ && attempt < attempts; attempt++) {
      MSG msg;
      while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
      Sleep(20);
      HDC dc = GetDC(window);
      differ = 0;
      for (UINT place = 0; place < across * across; place++) {
        for (UINT n = 0; n < std::size(places); n++) {
          auto [x, y] = pixel_of(place, n);
          auto rgb = GetPixel(dc, x, y);
          auto &want = drawn[place * std::size(places) + n];
          const int got[3] = {GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)};
          if (std::max({std::abs(got[0] - want[0]), std::abs(got[1] - want[1]), std::abs(got[2] - want[2])}) > 1 && !differ++ &&
              attempt == attempts - 1)
            expect(false, "the window's pixel %u,%u of %s: %d %d %d, and the back buffer's is %d %d %d", x, y, name(place),
                   got[0], got[1], got[2], want[0], want[1], want[2]);
        }
      }
      ReleaseDC(window, dc);
    }
  }
  return verdict();
}
