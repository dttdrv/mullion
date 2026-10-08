// contract: a query counts only visible samples between BeginQuery and EndQuery, across passes and overlapping
// queries; an empty interval writes zero. D3D11.3 20.4.6: "the driver will be asked to calculate the difference
// between two requests (one request for Issue( BEGIN ), and one request for Issue( END ))." On a one-sample target,
// "the counter, naturally is incremented by the number of whole pixels that are \"visible\"."
// D3D12_QUERY_TYPE_OCCLUSION: "Indicates the query is for depth/stencil occlusion counts"; BINARY_OCCLUSION
// "acts like D3D12_QUERY_TYPE_OCCLUSION except that it returns simply a binary 0/1 result: 0 indicates that no samples
// passed depth and stencil testing, 1 indicates that at least one sample passed depth and stencil testing"
// (Microsoft Learn, https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_query_type).
// Metal's setVisibilityResultMode(_:offset:): "at offset, which needs to be a multiple of 8", and "You can set a
// specific offset value only once per render pass" (Apple Developer Documentation, MTLRenderCommandEncoder).
// MTLVisibilityResultType.reset: "Reset visibility result data when you create a render command encoder";
// "Maximum visibility query offset": 65,528 B through Apple6, 256 KB from Apple7 (Metal Feature Set Tables).
// scissor rectangles give the independent counts: their areas, since depth/stencil testing is disabled. Four
// passes share a query, with a target change, copy and clear between them; ended queries resolve after all four.
// stops and overlapping queries stay within one pass. Empty queries reuse nonzero results, with no render pass
// and between passes. Empty segments cross a visibility window with only two draws. Results start at all ones,
// including guards and unresolved slots, so a missing write or an overwrite differs from the expected bytes.
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return 1; }
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
  const LONG side = 16;
  const D3D12_RECT rectangles[] = {{0, 0, 2, 3}, {1, 2, 6, 6}, {2, 1, 9, 6}, {3, 3, 8, 11}};
  const D3D12_RECT full{0, 0, side, side}, empty{0, 0, 0, side};
  auto area = [](const D3D12_RECT &r) { return UINT64(r.right - r.left) * (r.bottom - r.top); };
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(rs != nullptr, "root signature could not be made"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12Resource> targets[2];
  D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, UINT(side), 1, 1, desc.RTVFormats[0],
                                 {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, UINT(std::size(targets))};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE handles[std::size(targets)];
  for (UINT i = 0; i < std::size(targets); i++) {
    CHECK(device->CreateCommittedResource(
        &gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])
    ));
    handles[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
    handles[i].ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(targets[i].Get(), nullptr, handles[i]);
  }
  enum { Across, First, Second, Third, Fourth, Stop0, Stop1, Stop2, StopZero, Overlap0, Overlap1, Empty, Before, After,
         Scratch, Queries };
  ComPtr<ID3D12QueryHeap> queries;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, Queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));
  const UINT64 sentinel = ~UINT64(0), bytes = (Queries + 2) * sizeof(UINT64);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto seed = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(sentinel), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(results && seed, "result and copy buffers could not be made"))
    return verdict();
  void *mapped;
  CHECK(seed->Map(0, nullptr, &mapped));
  memcpy(mapped, &sentinel, sizeof(sentinel));
  seed->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  const D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
  const float black[4] = {};
  for (auto type : {D3D12_QUERY_TYPE_OCCLUSION, D3D12_QUERY_TYPE_BINARY_OCCLUSION}) {
    const bool binary = type == D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    std::vector<UINT64> want(Queries + 2);
    auto start = [&](const char *name) {
      step("%s: %s", binary ? "binary" : "precise", name);
      std::fill(want.begin(), want.end(), sentinel);
      HRESULT hr = allocator->Reset();
      if (SUCCEEDED(hr))
        hr = list->Reset(allocator.Get(), pso.Get());
      if (SUCCEEDED(hr))
        hr = forget(results.Get());
      if (!expect(SUCCEEDED(hr), "list or result buffer could not be reset: %08lx", hr))
        return false;
      list->SetGraphicsRootSignature(rs.Get());
      list->RSSetViewports(1, &viewport);
      list->OMSetRenderTargets(1, &handles[0], FALSE, nullptr);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      return true;
    };
    auto draw = [&](const D3D12_RECT &rect) {
      list->RSSetScissorRects(1, &rect);
      list->DrawInstanced(3, 1, 0, 0);
    };
    auto begin = [&](UINT q) { list->BeginQuery(queries.Get(), type, q); };
    auto end = [&](UINT q, UINT64 samples) {
      list->EndQuery(queries.Get(), type, q);
      want[q + 1] = binary ? UINT64(samples != 0) : samples;
    };
    auto finish = [&] {
      for (UINT q = 0; q < Queries; q++)
        if (want[q + 1] != sentinel)
          list->ResolveQueryData(queries.Get(), type, q, 1, results.Get(), (q + 1) * sizeof(UINT64));
      HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
      if (!expect(SUCCEEDED(hr), "submission: %08lx", hr))
        return false;
      const UINT64 *got;
      hr = results->Map(0, nullptr, (void **)&got);
      if (!expect(SUCCEEDED(hr), "result map: %08lx", hr))
        return false;
      for (UINT i = 0; i < want.size(); i++)
        expect(got[i] == want[i], "word %u (query %d, guards -1 and %d): %llu, want %llu", i, int(i) - 1, Queries,
               got[i], want[i]);
      results->Unmap(0, nullptr);
      return true;
    };

    if (!start("four passes, delayed resolves"))
      return verdict();
    begin(Across);
    UINT64 total = 0;
    const UINT pass_queries[] = {First, Second, Third, Fourth};
    for (UINT pass = 0; pass < std::size(rectangles); pass++) {
      if (pass == 1)
        list->OMSetRenderTargets(1, &handles[1], FALSE, nullptr);
      if (pass == 2)
        list->CopyBufferRegion(results.Get(), 0, seed.Get(), 0, sizeof(sentinel));
      if (pass == 3)
        list->ClearRenderTargetView(handles[1], black, 0, nullptr);
      begin(pass_queries[pass]);
      if (pass == 0)
        begin(Empty);
      const auto &rect = binary && pass == std::size(rectangles) - 1 ? empty : rectangles[pass];
      draw(rect);
      end(pass_queries[pass], area(rect));
      if (pass == 0)
        end(Empty, area(rect));
      total += area(rect);
    }
    end(Across, total);
    if (!finish())
      return verdict();

    if (!start("empty query, no render pass, replacing a nonzero result"))
      return verdict();
    begin(Empty);
    end(Empty, 0);
    if (!finish())
      return verdict();

    if (!start("stop and restart in one pass, including a zero-area draw"))
      return verdict();
    draw(full);
    const UINT stopped[] = {Stop0, Stop1, Stop2, StopZero};
    for (UINT i = 0; i < std::size(stopped); i++) {
      const auto &rect = i < std::size(stopped) - 1 ? rectangles[i] : empty;
      begin(stopped[i]);
      draw(rect);
      end(stopped[i], area(rect));
      draw(full);
    }
    if (!finish())
      return verdict();

    const D3D12_RECT overlap_rectangles[] = {rectangles[0], rectangles[1], rectangles[2]};
    const UINT phases = std::size(overlap_rectangles);
    for (UINT lit = 0; lit < (binary ? phases + 1 : 1); lit++) {
      if (!start("overlapping queries, counting stays on"))
        return verdict();
      if (binary)
        step("binary overlapping queries: visible phase %u (%u: none)", lit, phases);
      D3D12_RECT rects[phases];
      for (UINT phase = 0; phase < phases; phase++)
        rects[phase] = binary && lit != phase ? empty : overlap_rectangles[phase];
      begin(Overlap0);
      draw(rects[0]);
      begin(Overlap1);
      draw(rects[1]);
      end(Overlap0, area(rects[0]) + area(rects[1]));
      draw(rects[2]);
      end(Overlap1, area(rects[1]) + area(rects[2]));
      draw(full);
      if (!finish())
        return verdict();
    }

    if (!start("empty query between passes, neighbours retain their counts"))
      return verdict();
    begin(Before);
    draw(rectangles[0]);
    end(Before, area(rectangles[0]));
    list->CopyBufferRegion(results.Get(), 0, seed.Get(), 0, sizeof(sentinel));
    begin(Second);
    end(Second, 0);
    begin(After);
    draw(rectangles[1]);
    end(After, area(rectangles[1]));
    if (!finish())
      return verdict();

    if (!start("empty segments across a visibility window"))
      return verdict();
    // Metal Feature Set Tables, Maximum visibility query offset, Apple7+: 256 KB; each offset holds a UINT64
    const UINT window_bytes = 256 * 1024, window_slots = window_bytes / sizeof(UINT64);
    begin(Across);
    draw(rectangles[0]);
    for (UINT i = 0; i < window_slots / 2 + 1; i++) {
      begin(Scratch);
      end(Scratch, 0);
    }
    const auto &last = binary ? empty : rectangles[1];
    draw(last);
    end(Across, area(rectangles[0]) + area(last));
    if (!finish())
      return verdict();
  }
  return verdict();
}
