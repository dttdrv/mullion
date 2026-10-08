// contract: an occlusion result counts the samples passing between Begin and End, across every pass and counter
// window; beginning the same query again replaces its result. whole-pixel rectangles in a single-sample target
// make each count their area, and overlapping queries each count the draws inside their own span.
// "number of multisamples which passed depth and stencil testing"; the difference is taken for
// "(one request for Issue( BEGIN ), and one request for Issue( END ))." (D3D11.3 20.4.6).
// D3D12_QUERY_TYPE_OCCLUSION: "Indicates the query is for depth/stencil occlusion counts"; BINARY_OCCLUSION
// "acts like D3D12_QUERY_TYPE_OCCLUSION except that it returns simply a binary 0/1 result: 0 indicates that no
// samples passed depth and stencil testing, 1 indicates that at least one sample passed depth and stencil testing"
// (Microsoft Learn, https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_query_type).
// Metal stores a visibility result "at offset, which needs to be a multiple of 8", and "You can set a specific
// offset value only once per render pass" (Apple, MTLRenderCommandEncoder::setVisibilityResultMode(_:offset:)).
// "Maximum visibility query offset": 65,528 B through Apple6, 256 KB from Apple7 (Apple, Metal Feature Set Tables,
// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf). offset zero is reserved by VisibilitySlot;
// each short query's Begin and End takes another slot while Across stays active, exceeding a window by a margin.
#include "d3d12_test.hpp"

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
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(rs.Get() != nullptr, "root signature could not be made"))
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

  const LONG width = 8, height = 5;
  const UINT visibility_window_bytes = 256 * 1024, window_slots = visibility_window_bytes / sizeof(UINT64);
  const UINT shorts = window_slots / 2 + width * height;
  enum {
    Across, Before, After, Reused, Empty, BinaryBefore, BinaryAfter, EmptyBinaryBefore, EmptyBinaryAfter, Controls
  };
  const char *names[] = {
      "across windows", "before rollover", "after rollover", "second span", "no render pass",
      "binary before rollover", "binary after rollover", "empty binary before", "empty binary after"
  };
  const UINT counts[] = {Controls, shorts}, stride = Controls + shorts + 2;
  std::vector<UINT64> want[2];
  ComPtr<ID3D12QueryHeap> heaps[2];
  for (UINT i = 0; i < std::size(heaps); i++) {
    D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, counts[i]};
    CHECK(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heaps[i])));
  }
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, std::size(want) * stride * sizeof(UINT64),
                        D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(results.Get() != nullptr, "readback could not be made"))
    return verdict();
  CHECK(forget(results.Get()));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, (UINT)height, 1, 1,
                                DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  ComPtr<ID3D12Fence> held, completed;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)));
  CHECK(queue->Wait(held.Get(), 1));

  for (UINT recording = 0; recording < std::size(want); recording++) {
    step("recording %u: %u short queries inside a query across visibility windows", recording, shorts);
    if (recording && !expect(SUCCEEDED(list->Reset(allocator.Get(), pso.Get())), "pending list could not be reset"))
      ExitProcess(verdict());
    want[recording].resize(Controls + shorts);
    auto &expected = want[recording];
    bool active[Controls] = {};
    auto type = [&](UINT q) {
      return q >= BinaryBefore ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION;
    };
    auto begin = [&](UINT q) {
      expected[q] = 0;
      active[q] = true;
      list->BeginQuery(heaps[0].Get(), type(q), q);
    };
    auto end = [&](UINT q) {
      list->EndQuery(heaps[0].Get(), type(q), q);
      active[q] = false;
      if (type(q) == D3D12_QUERY_TYPE_BINARY_OCCLUSION)
        expected[q] = expected[q] != 0;
    };
    auto draw = [&](LONG w, LONG h) {
      D3D12_RECT scissor{0, 0, w, h};
      list->RSSetScissorRects(1, &scissor);
      list->DrawInstanced(3, 1, 0, 0);
      const UINT64 samples = UINT64(scissor.right - scissor.left) * (scissor.bottom - scissor.top);
      for (UINT q = 0; q < Controls; q++)
        if (active[q])
          expected[q] += samples;
      return samples;
    };
    D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
    list->SetGraphicsRootSignature(rs.Get());
    list->RSSetViewports(1, &viewport);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    begin(Empty);
    end(Empty);
    begin(Across);
    begin(Reused);
    begin(Before);
    begin(BinaryBefore);
    draw(2 + recording, 2);
    end(BinaryBefore);
    end(Before);
    begin(EmptyBinaryBefore);
    end(EmptyBinaryBefore);
    draw(1 + recording, 1);
    const float black[4] = {};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    draw(2, 1 + recording);

    for (UINT q = 0; q < shorts; q++) {
      list->BeginQuery(heaps[1].Get(), D3D12_QUERY_TYPE_OCCLUSION, q);
      expected[Controls + q] = draw(1 + (q + recording) % width, 1 + (q / width + recording) % height);
      list->EndQuery(heaps[1].Get(), D3D12_QUERY_TYPE_OCCLUSION, q);
    }
    end(Reused);
    begin(Reused);
    draw(1 + recording, 2 + recording);
    end(Reused);
    begin(After);
    begin(BinaryAfter);
    draw(width - recording, height);
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    draw(2 + recording, 1);
    end(BinaryAfter);
    end(After);
    begin(EmptyBinaryAfter);
    draw(0, height);
    end(EmptyBinaryAfter);
    end(Across);
    draw(width, height);

    step("recording %u: two heaps resolved in parts and out of order", recording);
    auto resolve = [&](UINT heap_index, D3D12_QUERY_TYPE query_type, UINT first, UINT count) {
      const UINT at = recording * stride + 1 + (heap_index ? Controls : 0) + first;
      list->ResolveQueryData(
          heaps[heap_index].Get(), query_type, first, count, results.Get(), UINT64(at) * sizeof(UINT64)
      );
    };
    const UINT split = shorts / 2;
    resolve(1, D3D12_QUERY_TYPE_OCCLUSION, split, shorts - split);
    resolve(0, D3D12_QUERY_TYPE_BINARY_OCCLUSION, BinaryAfter, Controls - BinaryAfter);
    resolve(0, D3D12_QUERY_TYPE_OCCLUSION, Reused, BinaryBefore - Reused);
    resolve(0, D3D12_QUERY_TYPE_OCCLUSION, Across, 1);
    resolve(1, D3D12_QUERY_TYPE_OCCLUSION, 0, split);
    resolve(0, D3D12_QUERY_TYPE_BINARY_OCCLUSION, BinaryBefore, 1);
    resolve(0, D3D12_QUERY_TYPE_OCCLUSION, Before, After - Before + 1);
    if (!expect(SUCCEEDED(list->Close()), "recording %u did not close", recording))
      ExitProcess(verdict());
    ID3D12CommandList *one[] = {list.Get()};
    queue->ExecuteCommandLists(std::size(one), one);
    if (!expect(SUCCEEDED(queue->Signal(completed.Get(), recording + 1)), "completion could not be queued"))
      ExitProcess(verdict());
  }

  step("both recordings pending on one allocator, then completed");
  if (!expect(held->GetCompletedValue() == 0 && completed->GetCompletedValue() == 0, "the queue did not stay held") ||
      !expect(SUCCEEDED(held->Signal(1)), "the hold could not be released"))
    ExitProcess(verdict());
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  bool done = event && SUCCEEDED(completed->SetEventOnCompletion(std::size(want), event)) &&
              WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0;
  if (event)
    CloseHandle(event);
  const HRESULT reason = device->GetDeviceRemovedReason();
  if (!expect(done, "recordings did not finish, device status %08lx", reason) ||
      !expect(reason == S_OK, "device was removed: %08lx", reason))
    ExitProcess(verdict());
  UINT64 *got;
  if (!expect(SUCCEEDED(results->Map(0, nullptr, (void **)&got)), "results could not be read"))
    return verdict();
  for (UINT recording = 0; recording < std::size(want); recording++) {
    step("recording %u: every count and the guards of its resolve region", recording);
    const UINT base = recording * stride;
    expect(got[base] == ~UINT64(0) && got[base + stride - 1] == ~UINT64(0), "resolve wrote beyond its region");
    for (UINT q = 0; q < Controls; q++)
      expect(got[base + 1 + q] == want[recording][q], "%s: %llu samples, want %llu", names[q],
             got[base + 1 + q], want[recording][q]);
    UINT wrong = 0;
    for (UINT q = 0; q < shorts; q++)
      if (got[base + 1 + Controls + q] != want[recording][Controls + q] && wrong++ == 0)
        expect(false, "short query %u: %llu samples, want %llu", q, got[base + 1 + Controls + q],
               want[recording][Controls + q]);
    expect(!wrong, "%u short queries had wrong counts", wrong);
  }
  results->Unmap(0, nullptr);
  return verdict();
}
