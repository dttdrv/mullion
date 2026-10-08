// contract: each completed execution of a closed list counts only that execution's visible samples, and draws
// predicated on resolved query results obey all 64 bits at the given buffer offset, in this list and the next.
// "a command list can be executed multiple times (as long as the application ensures that the previous executions
// have completed before submitting new executions)" (Microsoft Learn, Creating and recording command lists and
// bundles). D3D11.3 20.4.6 counts the difference between Begin and End: "the difference between two requests (one
// request for Issue( BEGIN ), and one request for Issue( END ))". With depth/stencil disabled and one sample per
// pixel, the precise count is the sum of the scissor rectangles' areas.
// D3D12_QUERY_TYPE_OCCLUSION: "Indicates the query is for depth/stencil occlusion counts"; BINARY_OCCLUSION "acts
// like D3D12_QUERY_TYPE_OCCLUSION except that it returns simply a binary 0/1 result: 0 indicates that no samples
// passed depth and stencil testing, 1 indicates that at least one sample passed depth and stencil testing".
// D3D12_PREDICATION_OP: EQUAL_ZERO "Enables predication if all 64-bits are zero", NOT_EQUAL_ZERO "Enables predication
// if at least one of the 64-bits are not zero". SetPredication: "subsequent rendering and resource manipulation
// commands are not actually performed if the resulting predicate data of the predicate is equal to the operation
// specified". Additive pixels count every draw that runs, including both executions of an unchanged list.
// sources: https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_query_type
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_predication_op
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication
// Metal's setVisibilityResultMode(_:offset:) stores a count "at offset, which needs to be a multiple of 8", and
// "You can set a specific offset value only once per render pass". A query spanning repeated, nested draws crosses
// "Maximum visibility query offset" (Metal Feature Set Tables: 256 KB from Apple7), with counts on both sides.
// sources: https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setvisibilityresultmode(_:offset:)
// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
#include "d3d12_test.hpp"
#include <algorithm>
#include <iterator>
#include <limits>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps() : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  const D3D12_QUERY_TYPE types[] = {D3D12_QUERY_TYPE_OCCLUSION, D3D12_QUERY_TYPE_BINARY_OCCLUSION};
  const D3D12_PREDICATION_OP ops[] = {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO};
  const bool mixed[] = {false, true}, visible[] = {false, true};
  const UINT predicate_lists = 2;
  const UINT replay_queries = std::size(types) * std::size(ops) * std::size(mixed), window = replay_queries,
             scratch = window + std::size(types), queries = scratch + 1;
  const UINT source_queries = std::size(types) * std::size(visible),
             cells = source_queries * (1 + predicate_lists * std::size(ops));
  const UINT cell_width = 4, height = 3, width = cell_width * cells;
  const UINT64 result_bytes = (queries + 2) * sizeof(UINT64), predicate_offset = sizeof(UINT64);
  // a supplied high-only predicate checks its width without inventing a large query result
  const UINT64 high = UINT64(1) << std::numeric_limits<UINT>::digits;
  const UINT64 runs[][2] = {{high, 0}, {0, high}, {high, high}};
  // two offsets per nested query cross the Metal feature table's Apple7+ visibility window
  const UINT visibility_bytes = 256 * 1024, brackets = visibility_bytes / sizeof(UINT64) / 2 + 1;
  const UINT window_draws = brackets + 2;

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(bool(rs), "root signature could not be made"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
                                    D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD,
                                    D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  const auto RT = D3D12_RESOURCE_STATE_RENDER_TARGET, DEST = D3D12_RESOURCE_STATE_COPY_DEST,
             SOURCE = D3D12_RESOURCE_STATE_COPY_SOURCE, PRED = D3D12_RESOURCE_STATE_PREDICATION;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, RT, nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12QueryHeap> occlusion;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&occlusion)));
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, DEST);
  auto predicate = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * sizeof(UINT64), PRED);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, result_bytes + 2 * sizeof(UINT64),
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 pixel_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &pixel_bytes);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixel_bytes, DEST);
  if (!expect(results && predicate && upload && counts && pixels, "buffers could not be made"))
    return verdict();
  UINT64 *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  std::fill(mapped, mapped + queries + 2, ~UINT64(0));
  mapped[0] = 0;
  mapped[queries + 2] = 0;

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocators[2];
  ComPtr<ID3D12GraphicsCommandList> list, other;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  for (auto &allocator : allocators)
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), pso.Get(), IID_PPV_ARGS(&list)
  ));
  CHECK(device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[1].Get(), pso.Get(), IID_PPV_ARGS(&other)
  ));
  CHECK(other->Close());
  const float black[4] = {};
  D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  auto bind = [&](ID3D12GraphicsCommandList *l) {
    l->SetPipelineState(pso.Get());
    l->SetGraphicsRootSignature(rs.Get());
    l->RSSetViewports(1, &viewport);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  };
  auto rect = [&](UINT cell, bool fixed = false) {
    LONG x = cell * cell_width;
    return fixed ? D3D12_RECT{x + 3, 0, x + 4, 3} : D3D12_RECT{x, 0, x + 3, 2};
  };
  auto area = [](D3D12_RECT r) { return UINT64(r.right - r.left) * (r.bottom - r.top); };
  auto draw = [](ID3D12GraphicsCommandList *l, D3D12_RECT r) {
    l->RSSetScissorRects(1, &r);
    l->DrawInstanced(3, 1, 0, 0);
  };
  auto snapshot = [&](ID3D12GraphicsCommandList *l, D3D12_RESOURCE_STATES state) {
    transition(l, results.Get(), state, SOURCE);
    l->CopyBufferRegion(counts.Get(), 0, results.Get(), 0, result_bytes);
    transition(l, results.Get(), SOURCE, state);
    transition(l, target.Get(), RT, SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = footprint;
    l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(l, target.Get(), SOURCE, RT);
  };
  std::vector<float> want_pixels(width * height);
  auto add = [&](D3D12_RECT r, UINT times = 1) {
    for (LONG y = r.top; y < r.bottom; y++)
      for (LONG x = r.left; x < r.right; x++)
        want_pixels[y * width + x] += times;
  };
  auto inspect = [&](const std::vector<UINT64> &want_counts) {
    UINT64 *got;
    CHECK(counts->Map(0, nullptr, (void **)&got));
    expect(got[0] == 0 && got[queries + 1] == ~UINT64(0), "resolve changed a guard word");
    for (UINT q = 0; q < want_counts.size(); q++)
      expect(got[q + 1] == want_counts[q], "query %u: %llu, want %llu", q, got[q + 1], want_counts[q]);
    counts->Unmap(0, nullptr);
    const char *bytes;
    CHECK(pixels->Map(0, nullptr, (void **)&bytes));
    for (UINT y = 0; y < height; y++) {
      auto row = (const float *)(bytes + footprint.Offset + y * footprint.Footprint.RowPitch);
      for (UINT x = 0; x < width; x++)
        expect(
            row[x] == want_pixels[y * width + x], "pixel (%u,%u): %g, want %g", x, y, row[x], want_pixels[y * width + x]
        );
    }
    pixels->Unmap(0, nullptr);
    return 0;
  };

  step("record precise and binary replay, mixed draws, and a query spanning a visibility window");
  bind(list.Get());
  for (UINT q = 0; q < replay_queries; q++) {
    auto type = types[q / (std::size(ops) * std::size(mixed))];
    list->BeginQuery(occlusion.Get(), type, q);
    if (mixed[q % std::size(mixed)])
      draw(list.Get(), rect(q, true));
    list->SetPredication(predicate.Get(), predicate_offset, ops[q / std::size(mixed) % std::size(ops)]);
    draw(list.Get(), rect(q));
    list->SetPredication(nullptr, 0, ops[0]);
    list->EndQuery(occlusion.Get(), type, q);
  }
  for (UINT t = 0; t < std::size(types); t++)
    list->BeginQuery(occlusion.Get(), types[t], window + t);
  list->SetPredication(predicate.Get(), predicate_offset, ops[0]);
  draw(list.Get(), rect(replay_queries));
  for (UINT i = 0; i < brackets; i++) {
    list->BeginQuery(occlusion.Get(), types[0], scratch);
    draw(list.Get(), rect(replay_queries));
    list->EndQuery(occlusion.Get(), types[0], scratch);
  }
  draw(list.Get(), rect(replay_queries));
  list->SetPredication(nullptr, 0, ops[0]);
  for (UINT t = 0; t < std::size(types); t++)
    list->EndQuery(occlusion.Get(), types[t], window + t);
  for (UINT q = 0; q < queries; q++) {
    auto type = q < replay_queries ? types[q / (std::size(ops) * std::size(mixed))] : types[q == window + 1];
    list->ResolveQueryData(occlusion.Get(), type, q, 1, results.Get(), (q + 1) * sizeof(UINT64));
  }
  snapshot(list.Get(), DEST);
  CHECK(list->Close());
  for (auto &values : runs) {
    std::fill(want_pixels.begin(), want_pixels.end(), 0);
    for (UINT execution = 0; execution < std::size(values); execution++) {
      step("replay %llu then %llu, execution %u", values[0], values[1], execution);
      if (!execution || values[execution] != values[execution - 1]) {
        CHECK(allocators[1]->Reset());
        CHECK(other->Reset(allocators[1].Get(), pso.Get()));
        mapped[queries + 3] = values[execution];
        transition(other.Get(), predicate.Get(), PRED, DEST);
        other->CopyBufferRegion(predicate.Get(), 0, upload.Get(), result_bytes, 2 * sizeof(UINT64));
        transition(other.Get(), predicate.Get(), DEST, PRED);
        if (!execution) {
          other->ClearRenderTargetView(rtv, black, 0, nullptr);
          other->CopyBufferRegion(results.Get(), 0, upload.Get(), 0, result_bytes);
        }
        CHECK(submit(device.Get(), queue.Get(), other.Get()));
      }
      CHECK(forget(counts.Get()));
      CHECK(forget(pixels.Get()));
      CHECK(execute(device.Get(), queue.Get(), list.Get()));
      std::vector<UINT64> want_counts(queries);
      for (UINT q = 0; q < replay_queries; q++) {
        auto type = types[q / (std::size(ops) * std::size(mixed))];
        bool ran = (values[execution] == 0) !=
                   (ops[q / std::size(mixed) % std::size(ops)] == D3D12_PREDICATION_OP_EQUAL_ZERO);
        UINT64 samples = ran ? area(rect(q)) : 0;
        if (ran)
          add(rect(q));
        if (mixed[q % std::size(mixed)]) {
          samples += area(rect(q, true));
          add(rect(q, true));
        }
        want_counts[q] = type == D3D12_QUERY_TYPE_BINARY_OCCLUSION ? samples != 0 : samples;
      }
      bool ran = values[execution] != 0;
      want_counts[window] = ran ? window_draws * area(rect(replay_queries)) : 0;
      want_counts[window + 1] = ran;
      want_counts[scratch] = ran ? area(rect(replay_queries)) : 0;
      if (ran)
        add(rect(replay_queries), window_draws);
      if (inspect(want_counts))
        return 1;
    }
  }

  step("resolve precise and binary visible and empty queries, then predicate in the same list");
  CHECK(allocators[1]->Reset());
  CHECK(other->Reset(allocators[1].Get(), pso.Get()));
  other->ClearRenderTargetView(rtv, black, 0, nullptr);
  other->CopyBufferRegion(results.Get(), 0, upload.Get(), 0, result_bytes);
  CHECK(submit(device.Get(), queue.Get(), other.Get()));
  CHECK(allocators[0]->Reset());
  CHECK(list->Reset(allocators[0].Get(), pso.Get()));
  bind(list.Get());
  std::fill(want_pixels.begin(), want_pixels.end(), 0);
  std::vector<UINT64> want_counts(source_queries);
  for (UINT q = 0; q < source_queries; q++) {
    auto type = types[q / std::size(visible)];
    auto r = rect(q);
    if (!visible[q % std::size(visible)])
      r.right = r.left;
    list->BeginQuery(occlusion.Get(), type, q);
    draw(list.Get(), r);
    list->EndQuery(occlusion.Get(), type, q);
    list->ResolveQueryData(occlusion.Get(), type, q, 1, results.Get(), (q + 1) * sizeof(UINT64));
    want_counts[q] = type == D3D12_QUERY_TYPE_BINARY_OCCLUSION ? area(r) != 0 : area(r);
    add(r);
    transition(list.Get(), results.Get(), DEST, PRED);
    for (UINT op = 0; op < std::size(ops); op++) {
      auto marker = rect(source_queries + q * std::size(ops) + op);
      list->SetPredication(results.Get(), (q + 1) * sizeof(UINT64), ops[op]);
      draw(list.Get(), marker);
      list->SetPredication(nullptr, 0, ops[0]);
      if ((want_counts[q] == 0) != (ops[op] == D3D12_PREDICATION_OP_EQUAL_ZERO))
        add(marker);
    }
    if (q + 1 < source_queries)
      transition(list.Get(), results.Get(), PRED, DEST);
  }
  snapshot(list.Get(), PRED);
  CHECK(forget(counts.Get()));
  CHECK(forget(pixels.Get()));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  if (inspect(want_counts))
    return 1;

  step("predicate the next list on the same resolved results with both operations");
  CHECK(allocators[1]->Reset());
  CHECK(other->Reset(allocators[1].Get(), pso.Get()));
  bind(other.Get());
  for (UINT q = 0; q < source_queries; q++)
    for (UINT op = 0; op < std::size(ops); op++) {
      auto marker = rect(source_queries * (1 + std::size(ops)) + q * std::size(ops) + op);
      other->SetPredication(results.Get(), (q + 1) * sizeof(UINT64), ops[op]);
      draw(other.Get(), marker);
      other->SetPredication(nullptr, 0, ops[0]);
      if ((want_counts[q] == 0) != (ops[op] == D3D12_PREDICATION_OP_EQUAL_ZERO))
        add(marker);
    }
  snapshot(other.Get(), PRED);
  CHECK(forget(counts.Get()));
  CHECK(forget(pixels.Get()));
  CHECK(submit(device.Get(), queue.Get(), other.Get()));
  if (inspect(want_counts))
    return 1;
  upload->Unmap(0, nullptr);
  return verdict();
}
