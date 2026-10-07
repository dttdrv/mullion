// contract: pipeline states made by many threads at once, while another thread draws, are each the pipeline its
// description says. ID3D12Device's methods are free-threaded ("the ID3D12Device methods can be safely called from
// multiple threads", Direct3D 12 multithreading), and an engine that fills its pipeline cache ahead of use does
// exactly this: Unreal's PSO precaching and Unity's graphics jobs create pipelines on worker threads, many of them
// of one shader with different state, and the same description on two threads at once, while the render thread
// draws with the ones that are ready.
// every variant is a pixel shader that writes its own number and a compute shader that stores it; variant v's
// graphics pipeline also has a state of its own that the picture shows (its write mask keeps one channel out). each
// thread makes every variant, so each description is made THREADS times at once; the first thread's pipelines are
// drawn with as they arrive, and the others' once all are made. each pixel and each stored number has to be its
// variant's, from every thread's pipeline.
// a pipeline library gives back what was stored under a name, and refuses a description that is not the stored one
// with E_INVALIDARG (ID3D12PipelineLibrary::LoadGraphicsPipeline: "E_INVALIDARG if the name doesn't exist, or if the
// input description doesn't match the data in the library"; StorePipeline: "E_INVALIDARG if the name already
// exists"). a device whose D3D12_FEATURE_SHADER_CACHE has no D3D12_SHADER_CACHE_SUPPORT_LIBRARY always answers
// DXGI_ERROR_UNSUPPORTED (ID3D12Device1::CreatePipelineLibrary), and that part is named as not run.
#include "d3d12_test.hpp"
#include <atomic>
#include <thread>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
uint4 ps() : SV_Target { return uint4(VARIANT + 1, VARIANT * 3 + 1, VARIANT * 5 + 1, VARIANT * 7 + 1); }
RWStructuredBuffer<uint> stored : register(u0);
[numthreads(1, 1, 1)] void cs() { stored[SLOT] = VARIANT * 11 + 1; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT variants = 48, threads = 8, seconds = 120;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER stored_at{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &stored_at});

  // the compilers are not ours to call from many threads: every variant's code first
  auto vs = compiler.compile(hlsl, "vs", "vs", {"VARIANT=0", "SLOT=0"});
  std::vector<std::string> ps(variants), cs(variants);
  for (UINT v = 0; v < variants; v++) {
    const std::vector<std::string> defines = {"VARIANT=" + std::to_string(v), "SLOT=" + std::to_string(v)};
    ps[v] = compiler.compile(hlsl, "ps", "ps", defines), cs[v] = compiler.compile(hlsl, "cs", "cs", defines);
    if (vs.empty() || ps[v].empty() || cs[v].empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT;
  // the channel variant v's pipeline does not write
  auto kept_out = [](UINT v) { return v % 4; };
  auto graphics = [&](UINT v) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs), desc.PS = bytecode(ps[v]);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL & ~(1 << kept_out(v));
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    return desc;
  };

  // one pixel a pipeline: a thread's row, a variant's column
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, variants, threads, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  const UINT64 stored_bytes = threads * variants * sizeof(UINT);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes + stored_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto stored = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, stored_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float clear[4] = {};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  // a pipeline in use: its pixel, and its number through the offset of the buffer it is given
  auto uses = [&](UINT thread, UINT v, ID3D12PipelineState *drawn, ID3D12PipelineState *computed) {
    D3D12_VIEWPORT viewport{0, 0, (float)variants, (float)threads, 0, 1};
    D3D12_RECT pixel{(LONG)v, (LONG)thread, (LONG)v + 1, (LONG)thread + 1};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &pixel);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(drawn);
    list->DrawInstanced(3, 1, 0, 0);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(0, stored->GetGPUVirtualAddress() + thread * variants * sizeof(UINT));
    list->SetPipelineState(computed);
    list->Dispatch(1, 1, 1);
  };

  step("%u threads make %u graphics and %u compute pipelines each, while one thread draws with the first thread's", threads,
       variants, variants);
  std::vector<std::atomic<ID3D12PipelineState *>> made(2 * threads * variants);
  std::atomic<UINT> failed = 0, done = 0;
  std::vector<std::thread> pool;
  for (UINT t = 0; t < threads; t++)
    pool.emplace_back([&, t] {
      // each thread starts at a variant of its own, so the threads meet on every variant at some time
      for (UINT i = 0; i < variants; i++) {
        UINT v = (i + t * variants / threads) % variants;
        ID3D12PipelineState *drawn = nullptr, *computed = nullptr;
        auto desc = graphics(v);
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs[v])};
        failed += FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&drawn)));
        failed += FAILED(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&computed)));
        made[2 * (t * variants + v) + 1] = computed;
        made[2 * (t * variants + v)] = drawn;
      }
      done++;
    });
  // the drawing thread: a list for each of the first thread's pipelines as it arrives
  std::vector<bool> used(variants);
  for (UINT waited = 0, left = variants; left && waited < seconds * 1000; waited++) {
    for (UINT v = 0; v < variants; v++)
      if (auto drawn = made[2 * v].load(); drawn && !used[v]) {
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        uses(0, v, drawn, made[2 * v + 1]);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        used[v] = true, left--;
      }
    if (done == threads && left) {
      expect(false, "%u of the first thread's pipelines were not made", left);
      break;
    }
    Sleep(1);
  }
  for (UINT waited = 0; done < threads && waited < seconds * 10; waited++)
    Sleep(100);
  if (!expect(done == threads, "%u of %u threads have not returned from the creations after %u s", threads - done, threads, seconds))
    ExitProcess(verdict());
  for (auto &thread : pool)
    thread.join();
  expect(!failed, "%u creations failed", failed.load());

  step("the other threads' pipelines, in one list");
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  for (UINT t = 1; t < threads; t++)
    for (UINT v = 0; v < variants; v++)
      if (made[2 * (t * variants + v)] && made[2 * (t * variants + v) + 1])
        uses(t, v, made[2 * (t * variants + v)], made[2 * (t * variants + v) + 1]);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), stored.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  list->CopyBufferRegion(readback.Get(), bytes, stored.Get(), 0, stored_bytes);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  for (UINT t = 0; t < threads; t++)
    for (UINT v = 0; v < variants; v++) {
      auto pixel = (const UINT *)(out + t * footprint.Footprint.RowPitch) + 4 * v;
      UINT number = ((const UINT *)(out + bytes))[t * variants + v];
      for (UINT channel = 0; channel < 4; channel++) {
        UINT want = channel == kept_out(v) ? 0 : v * (2 * channel + 1) + 1;
        expect(pixel[channel] == want, "thread %u's graphics pipeline of variant %u: channel %u is %u, want %u", t, v, channel,
               pixel[channel], want);
      }
      expect(number == v * 11 + 1, "thread %u's compute pipeline of variant %u stored %u, want %u", t, v, number, v * 11 + 1);
    }
  readback->Unmap(0, nullptr);
  for (auto &pipeline : made)
    if (pipeline)
      pipeline.load()->Release();

  step("a pipeline library: stored, serialized, opened again, loaded");
  ComPtr<ID3D12Device1> device1;
  ComPtr<ID3D12PipelineLibrary> library;
  CHECK(device.As(&device1));
  D3D12_FEATURE_DATA_SHADER_CACHE cache{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache)));
  HRESULT hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library));
  if (!(cache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
    expect(hr == DXGI_ERROR_UNSUPPORTED, "CreatePipelineLibrary on a device that reports no libraries: %08lx, want DXGI_ERROR_UNSUPPORTED", hr);
    printf("not run, the device reports no pipeline libraries\n");
    return verdict();
  }
  if (!expect(hr == S_OK, "CreatePipelineLibrary of nothing, on a device that reports libraries: %08lx", hr))
    return verdict();
  auto first = graphics(0), second = graphics(1);
  ComPtr<ID3D12PipelineState> pipeline, loaded;
  CHECK(device->CreateGraphicsPipelineState(&first, IID_PPV_ARGS(&pipeline)));
  CHECK(library->StorePipeline(L"first", pipeline.Get()));
  hr = library->StorePipeline(L"first", pipeline.Get());
  expect(hr == E_INVALIDARG, "StorePipeline under a name in use: %08lx, want E_INVALIDARG", hr);
  std::vector<char> serialized(library->GetSerializedSize());
  CHECK(library->Serialize(serialized.data(), serialized.size()));
  ComPtr<ID3D12PipelineLibrary> reopened;
  hr = device1->CreatePipelineLibrary(serialized.data(), serialized.size(), IID_PPV_ARGS(&reopened));
  if (expect(hr == S_OK, "CreatePipelineLibrary of what Serialize wrote: %08lx", hr)) {
    hr = reopened->LoadGraphicsPipeline(L"first", &first, IID_PPV_ARGS(&loaded));
    expect(hr == S_OK && loaded, "LoadGraphicsPipeline of the stored pipeline: %08lx", hr);
    ComPtr<ID3D12PipelineState> other;
    hr = reopened->LoadGraphicsPipeline(L"first", &second, IID_PPV_ARGS(&other));
    expect(hr == E_INVALIDARG, "LoadGraphicsPipeline with another description: %08lx, want E_INVALIDARG", hr);
    hr = reopened->LoadGraphicsPipeline(L"absent", &first, IID_PPV_ARGS(&other));
    expect(hr == E_INVALIDARG, "LoadGraphicsPipeline of a name not stored: %08lx, want E_INVALIDARG", hr);
  }
  if (loaded) {
    // the loaded pipeline draws what its description says
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, (float)variants, (float)threads, 0, 1};
    D3D12_RECT all{0, 0, (LONG)variants, (LONG)threads};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &all);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(loaded.Get());
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(readback->Map(0, nullptr, (void **)&out));
    auto pixel = (const UINT *)out;
    expect(pixel[0] == 0 && pixel[1] == 1 && pixel[2] == 1 && pixel[3] == 1, "the loaded pipeline drew %u %u %u %u, want 0 1 1 1",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
