// contract: a pixel shader's InterlockedMax on a texel of a 64-bit unordered access texture leaves the largest value
// any pixel drawn there gave, however many primitives cover the texel and whether or not the pass has a render
// target. 64-bit atomics on typed resources are a Shader Model 6.6 feature a device reports
// (D3D12_FEATURE_D3D12_OPTIONS9, AtomicInt64OnTypedResourceSupported), an atomic max is an "unsigned max of
// operand src0 into dst ... performed atomically" (D3D11.3 22.17.15 atomic_umax, there for 32 bits), and a pixel
// shader runs once for each pixel a primitive covers, each pixel of a rectangle of two triangles once (the
// top-left rule, D3D11.3 3.4.2.1).
// this is the visibility buffer of Unreal's Nanite: its hardware rasterizer's pixel shader has no output, the pass
// no target, and depth and the triangle's number are one 64-bit word that the nearest pixel wins by max.
// INSTANCES rectangles of whole pixels, two triangles each, lie over one another on a target SIDE pixels a side;
// the pixel shader gives each of its pixels a key from the rectangle's own word and the pixel's place in the high
// half and the rectangle's number in the low one. every texel is held to the largest value of the rectangles
// that cover it, and is 0 where none does. the diagonals put pixels of both triangles, and pixels of neither, in
// the 2 by 2 quads a rasterizer shades together. twice: with no render target at all, and with one the shader does
// not write. ROUNDS times each: what takes turns takes them differently each time.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>

static const char hlsl[] = R"hlsl(
struct Rect { uint x, y, wide, high, key; };
StructuredBuffer<Rect> rects : register(t0);
RWTexture2D<uint64_t> seen : register(u0);
struct V { float4 pos : SV_Position; nointerpolation uint instance : INSTANCE; };
V vs(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  Rect r = rects[instance];
  // the corners of two triangles that share a diagonal: 00 10 01, 10 11 01
  uint2 corner = uint2(0x1a >> id, 0x34 >> id) & 1;
  V v;
  v.pos = float4((r.x + corner.x * r.wide) * 2.0 / SIDE - 1, 1 - (r.y + corner.y * r.high) * 2.0 / SIDE, 0, 1);
  v.instance = instance;
  return v;
}
void ps(V v) {
  uint2 at = v.pos.xy;
  InterlockedMax(seen[at], (uint64_t(rects[v.instance].key + at.x + (at.y << SHIFT)) << 32) | (v.instance + 1));
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
    printf("skipped: 64-bit typed resources are DXIL only\n");
    return 77;
  }
  // a row's place in the key lies above every column's
  const UINT side = 64, instances = 256, rounds = 8, shift = std::bit_width(side - 1);
  const std::vector<std::string> defines = {"SIDE=" + std::to_string(side), "SHIFT=" + std::to_string(shift)};
  auto vs = compiler.compile(hlsl, "vs", "vs_6_6", defines), ps = compiler.compile(hlsl, "ps", "ps_6_6", defines);
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options{};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options, sizeof(options))) || !options.AtomicInt64OnTypedResourceSupported) {
    printf("skipped: no 64-bit atomics on typed resources\n");
    return 77;
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
  const DXGI_FORMAT words = DXGI_FORMAT_R32G32_UINT, colour = DXGI_FORMAT_R8G8B8A8_UNORM;
  // without a render target and with one
  ComPtr<ID3D12PipelineState> pipelines[2];
  for (UINT targets = 0; targets < 2; targets++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs), desc.PS = bytecode(ps);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = targets, desc.RTVFormats[0] = targets ? colour : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[targets])));
  }

  // the rectangles: a corner in the first half of each side and a size up to half a side, and a word of their own
  struct Rect {
    UINT x, y, wide, high, key;
  };
  auto rects = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, instances * sizeof(Rect), D3D12_RESOURCE_STATE_GENERIC_READ);
  Rect *rect;
  CHECK(rects->Map(0, nullptr, (void **)&rect));
  // a number below a power of two
  UINT random = 1;
  auto next = [&](UINT below) { return (random = random * 1664525 + 1013904223) >> 16 & (below - 1); };
  for (UINT i = 0; i < instances; i++)
    rect[i] = {next(side / 2), next(side / 2), 1 + next(side / 2), 1 + next(side / 2), next(1u << 16) << 16};
  std::vector<UINT64> want(side * side);
  for (UINT i = 0; i < instances; i++)
    for (UINT y = rect[i].y; y < rect[i].y + rect[i].high; y++)
      for (UINT x = rect[i].x; x < rect[i].x + rect[i].wide; x++)
        want[y * side + x] = std::max(want[y * side + x], UINT64(rect[i].key + x + (y << shift)) << 32 | (i + 1));

  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC seen_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, words, {1, 0},
                                D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  D3D12_RESOURCE_DESC target_desc = seen_desc;
  target_desc.Format = colour, target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> seen, target;
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &seen_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&seen)));
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> views, rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  device->CreateUnorderedAccessView(seen.Get(), nullptr, nullptr, views->GetCPUDescriptorHandleForHeapStart());
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&seen_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  void *nothing;
  CHECK(zeros->Map(0, nullptr, &nothing));
  memset(nothing, 0, bytes);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  const D3D12_TEXTURE_COPY_LOCATION texture{seen.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      from_zeros{zeros.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}},
      to_readback{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
  for (UINT targets = 0; targets < 2; targets++)
    for (UINT round = 0; round < rounds; round++) {
      step("%u rectangles over one another, %s, round %u", instances, targets ? "a render target bound" : "no render target", round);
      if (FAILED(forget(readback.Get())) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pipelines[targets].Get()))) {
        expect(false, "the list could not be begun");
        continue;
      }
      const D3D12_VIEWPORT viewport{0, 0, (float)side, (float)side, 0, 1};
      const D3D12_RECT scissor{0, 0, (LONG)side, (LONG)side};
      ID3D12DescriptorHeap *bound[] = {views.Get()};
      list->CopyTextureRegion(&texture, 0, 0, 0, &from_zeros, nullptr);
      transition(list.Get(), seen.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list->SetDescriptorHeaps(1, bound);
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
      list->SetGraphicsRootShaderResourceView(1, rects->GetGPUVirtualAddress());
      list->OMSetRenderTargets(targets, targets ? &rtv : nullptr, FALSE, nullptr);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(6, instances, 0, 0);
      transition(list.Get(), seen.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyTextureRegion(&to_readback, 0, 0, 0, &texture, nullptr);
      transition(list.Get(), seen.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
      const char *out;
      if (!expect(ran == S_OK, "the device after the draw: %08lx", ran) || FAILED(readback->Map(0, nullptr, (void **)&out)))
        return verdict();
      unsigned wrong = 0;
      for (UINT y = 0; y < side; y++)
        for (UINT x = 0; x < side; x++) {
          UINT64 got;
          memcpy(&got, out + y * footprint.Footprint.RowPitch + x * sizeof(got), sizeof(got));
          const UINT64 value = want[y * side + x];
          if (got != value && wrong++ < 4)
            expect(false, "texel %u,%u holds key %#x of rectangle %d, want key %#x of rectangle %d (-1: none)", x, y, UINT(got >> 32),
                   int(UINT(got)) - 1, UINT(value >> 32), int(UINT(value)) - 1);
        }
      expect(wrong <= 4, "and %u more texels", wrong - 4);
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
