// contract: recording data before a query cannot change its depth/stencil sample count or an indirect draw's
// arguments. D3D12_QUERY_TYPE_OCCLUSION "Indicates the query is for depth/stencil occlusion counts"; binary occlusion
// "returns simply a binary 0/1 result: 0 indicates that no samples passed depth and stencil testing, 1 indicates
// that at least one sample passed depth and stencil testing" (Microsoft Learn, D3D12_QUERY_TYPE).
// D3D11.3 20.4.6 counts the "number of multisamples which passed depth and stencil testing" and asks for "the
// difference between two requests (one request for Issue( BEGIN ), and one request for Issue( END ))". with one
// sample per pixel, a scissor rectangle's visible area is its count; LESS rejects pixels behind the cleared depth.
// Metal stores a count "at offset, which needs to be a multiple of 8" and says "You can set a specific offset
// value only once per render pass" (MTLRenderCommandEncoder::setVisibilityResultMode(_:offset:)). its "Maximum
// visibility query offset" is 65,528 B through Apple6 and 256 KB from Apple7 (Metal Feature Set Tables).
// sources: https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_query_type
// https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setvisibilityresultmode(_:offset:)
// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
// changed root constants upload at least their own bytes for every draw (d3d12_command_list.cpp, EncodeRootArgument).
// more than four visibility limits of uploads cross the recording arena's system-sized blocks; a query active
// during part of them distinguishes counters before that limit from the later queries' counters past it.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float depth; float value; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}
float2 ps() : SV_Target { return float2(value, 1); }
)hlsl";

int
main(int argc, char **argv) {
  step("compile shaders and create the device");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  float constants[D3D12_MAX_ROOT_COST]{};
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = std::size(constants);
  auto rs = root_signature(device.Get(), {1, &param});
  if (!expect(bool(rs), "root signature could not be made"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32_FLOAT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
                                    D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP,
                                    D3D12_COLOR_WRITE_ENABLE_ALL};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.DepthStencilState = {TRUE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_LESS};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  const UINT width = 16, height = 8;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  auto depth_desc = target_desc;
  depth_desc.Format = desc.DSVFormat;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        nullptr, IID_PPV_ARGS(&depth)));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  enum { Recording, Partial, Visible, Hidden, Across, Reused, Empty, BinaryZero, BinaryOne, BinaryMany, Queries };
  const char *names[] = {
      "recording", "partial", "visible", "hidden", "across passes", "reused", "empty", "binary zero",
      "binary one sample", "binary four samples"};
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, Queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));
  auto results =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, Queries * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  const UINT snapshots = 3;
  const UINT64 snapshot_bytes = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                                D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto pixels =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, snapshots * snapshot_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DRAW_ARGUMENTS arguments[3];
  for (UINT i = 0; i < std::size(arguments); i++)
    arguments[i] = {3, i + 1, 0, 0};
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(arguments), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(results && pixels && upload, "buffers could not be made"))
    return verdict();
  void *mapped;
  CHECK(upload->Map(0, nullptr, &mapped));
  memcpy(mapped, arguments, sizeof(arguments));
  upload->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC kind{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &kind};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  // Metal Feature Set Tables, GPU implementation limits: the largest visibility offset is 256 KB.
  const size_t visibility_limit = 256 * 1024;
  const UINT filler = std::max<size_t>(4 * visibility_limit, system.dwAllocationGranularity) / sizeof(constants) + 1;
  const UINT counted = filler / 2;
  const D3D12_RECT a{0, 0, 3, 2}, b{3, 0, 8, 3}, c{0, 3, 4, 7}, d{4, 3, 7, 5};
  const D3D12_RECT blocked{a.left, a.top, a.right - 1, a.bottom};
  const D3D12_RECT one{0, 0, 1, 1}, single{a.right - 1, a.top, a.right, a.top + 1},
      many{c.left, c.top, c.left + 2, c.top + 2};
  const D3D12_RECT indirect[] = {{10, 0, 12, 2}, {12, 0, 14, 3}, {14, 0, 16, 4}};
  auto area = [](D3D12_RECT rect) { return UINT64(rect.right - rect.left) * (rect.bottom - rect.top); };
  const UINT64 partial = area(a) - area(blocked);
  UINT64 indirect_samples = 0;
  for (UINT i = 0; i < std::size(arguments); i++)
    indirect_samples += area(indirect[i]) * arguments[i].InstanceCount;
  const UINT64 wanted[] = {
      counted * area(one), partial, area(b), 0,
      partial + area(b) + area(single) + area(many) + 2 * area(c) + area(d) + indirect_samples,
      area(d), 0, 0, UINT64(area(single) != 0), UINT64(area(many) != 0)};
  const float black[4] = {};
  D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  struct Pixel {
    float value, draws;
  };
  for (UINT round = 0; round < 2; round++) {
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)
    ));
    CHECK(forget(results.Get()));
    CHECK(forget(pixels.Get()));
    std::array<Pixel, width * height> picture{}, pictures[snapshots];
    auto bind = [&] {
      list->SetGraphicsRootSignature(rs.Get());
      list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      list->RSSetViewports(1, &viewport);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto type = [](UINT q) { return q >= BinaryZero ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION; };
    auto begin = [&](UINT q) { list->BeginQuery(queries.Get(), type(q), q); };
    auto end = [&](UINT q) { list->EndQuery(queries.Get(), type(q), q); };
    auto snapshot = [&](UINT index) {
      pictures[index] = picture;
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          dst{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = footprint;
      dst.PlacedFootprint.Offset = index * snapshot_bytes;
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    };
    step("list %u: %u draws upload at least %zu bytes, first %u counted", round, filler,
         filler * sizeof(constants), counted);
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    bind();
    list->RSSetScissorRects(1, &one);
    constants[0] = 0.25f, constants[1] = 1;
    begin(Recording);
    for (UINT i = 0; i < filler; i++) {
      if (i + 1 == counted)
        bind();
      constants[std::size(constants) - 1] = float(i);
      list->SetGraphicsRoot32BitConstants(0, std::size(constants), constants, 0);
      list->DrawInstanced(3, 1, 0, 0);
      if (i + 1 == counted)
        end(Recording);
    }
    picture[0] = {float(filler), float(filler)};
    snapshot(0);
    picture = {};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 1, &blocked);
    bind();
    auto draw = [&](D3D12_RECT rect, float z, float value, int command = -1) {
      constants[0] = z, constants[1] = value;
      list->SetGraphicsRoot32BitConstants(0, std::size(constants), constants, 0);
      list->RSSetScissorRects(1, &rect);
      const UINT instances = command < 0 ? 1 : arguments[command].InstanceCount;
      if (command < 0)
        list->DrawInstanced(3, instances, 0, 0);
      else
        list->ExecuteIndirect(signature.Get(), 1, upload.Get(), command * sizeof(D3D12_DRAW_ARGUMENTS), nullptr, 0);
      for (LONG y = rect.top; y < rect.bottom; y++)
        for (LONG x = rect.left; x < rect.right; x++) {
          const bool covered = x >= blocked.left && x < blocked.right && y >= blocked.top && y < blocked.bottom;
          if (z < (covered ? 0 : 0.5f)) {
            auto &pixel = picture[y * width + x];
            pixel.value += value * instances;
            pixel.draws += instances;
          }
        }
    };
    step("list %u: far precise, binary, empty and reused queries with indirect draws in the same passes", round);
    begin(Across);
    begin(Partial);
    draw(a, 0.25f, 1);
    end(Partial);
    draw(indirect[0], 0.25f, 10, 0);
    begin(Visible);
    draw(b, 0.25f, 2);
    end(Visible);
    begin(Hidden);
    draw(b, 0.75f, 3);
    end(Hidden);
    begin(BinaryZero);
    draw(blocked, 0.25f, 4);
    end(BinaryZero);
    begin(BinaryOne);
    draw(single, 0.25f, 3);
    end(BinaryOne);
    begin(BinaryMany);
    draw(many, 0.25f, 7);
    end(BinaryMany);
    begin(Empty);
    end(Empty);
    begin(Reused);
    draw(c, 0.25f, 4);
    end(Reused);
    draw(indirect[1], 0.25f, 11, 1);
    begin(Reused);
    draw(d, 0.25f, 5);
    end(Reused);
    snapshot(1);
    bind();
    draw(c, 0.25f, 6);
    draw(indirect[2], 0.25f, 12, 2);
    end(Across);
    snapshot(2);
    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, BinaryZero, results.Get(), 0);
    list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, BinaryZero, Queries - BinaryZero,
                           results.Get(), BinaryZero * sizeof(UINT64));
    step("list %u: submit and wait for its fence", round);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    step("list %u: read every query and every pixel of all snapshots", round);
    const UINT64 *counts;
    CHECK(results->Map(0, nullptr, (void **)&counts));
    for (UINT q = 0; q < Queries; q++)
      expect(counts[q] == wanted[q], "%s: %llu samples, want %llu", names[q], counts[q], wanted[q]);
    results->Unmap(0, nullptr);
    const char *got;
    CHECK(pixels->Map(0, nullptr, (void **)&got));
    for (UINT pass = 0; pass < snapshots; pass++)
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          Pixel pixel;
          memcpy(
              &pixel, got + pass * snapshot_bytes + y * footprint.Footprint.RowPitch + x * sizeof(Pixel), sizeof(pixel)
          );
          auto want = pictures[pass][y * width + x];
          expect(pixel.value == want.value && pixel.draws == want.draws,
                  "pass %u pixel (%u,%u): (%g,%g), want (%g,%g)", pass, x, y, pixel.value, pixel.draws,
                  want.value, want.draws);
        }
    pixels->Unmap(0, nullptr);
    CHECK(device->GetDeviceRemovedReason());
  }
  return verdict();
}
