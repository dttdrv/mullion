// contract: the lists of one ExecuteCommandLists are one stream of work: "the second command list (D) may begin
// executing work before all work from the first (C) has finished", while two calls are ordered: "the first workload (A)
// finishes before the second workload (B)" (ID3D12CommandQueue::ExecuteCommandLists). so across lists of a call draws
// keep their submission order (D3D11.3 functional specification, 4.2 Fixed Order of Pipeline Results) and a barrier in
// either list orders what it names; across calls everything is ordered. a slow draw ends one list, and a quick one to
// the same target, or a dispatch that reads what the slow one's pixels wrote, begins the next.
// argv[2] "bench": how long a call of lists that nothing orders takes, each a draw to its own target, as a game's
// lists recorded on several threads are; the median and fastest of its runs, argv[3] of them.
#include "d3d12_test.hpp"
#include <algorithm>
#include <functional>

static constexpr UINT size = 512, group = 64, repeats = 4;
// so many iterations a pixel or thread that work after the slow one which did not wait for it would run first
static constexpr UINT work = 1u << 15;
static const char hlsl[] = R"hlsl(
cbuffer Parameters : register(b0) { uint value; uint work; uint mask; };
RWStructuredBuffer<uint> written : register(u0);
RWStructuredBuffer<uint> read : register(u1);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint slow(uint v) {
  [loop] for (uint i = 0; i < work; i++)
    v = v * 1664525u + 1013904223u;
  return value + (v & mask);
}
uint ps(float4 position : SV_Position) : SV_Target {
  return slow(uint(position.x) ^ uint(position.y));
}
uint write(float4 position : SV_Position) : SV_Target {
  return written[uint(position.y) * SIZE + uint(position.x)] = slow(uint(position.x) ^ uint(position.y));
}
[numthreads(GROUP, 1, 1)] void copy(uint3 tid : SV_DispatchThreadID) {
  read[tid.x] = written[tid.x] + 1;
}
)hlsl";

enum Case { Draw, BarrierBefore, BarrierAfter, Calls, Cases };
static const char *names[] = {"a draw in the next list", "a barrier ending the list", "a barrier beginning the next list",
                              "the next call, without a barrier"};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const std::vector<std::string> defines = {"GROUP=" + std::to_string(group), "SIZE=" + std::to_string(size)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       write = compiler.compile(hlsl, "write", "ps", defines), copy = compiler.compile(hlsl, "copy", "cs", defines);
  if (!expect(!vs.empty() && !ps.empty() && !write.empty() && !copy.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}
  };
  parameters[0].Constants.Num32BitValues = 3;
  parameters[2].Descriptor.ShaderRegister = 1;
  auto roots = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  CHECK(roots ? S_OK : E_FAIL);
  ComPtr<ID3D12PipelineState> writing, copying, drawing;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{roots.Get(), bytecode(copy)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&copying)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{roots.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&drawing)));
  desc.PS = bytecode(write);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&writing)));

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  // the first list, the second, and the one that reads back
  ComPtr<ID3D12CommandAllocator> allocators[3];
  ComPtr<ID3D12GraphicsCommandList> lists[3];
  for (UINT i = 0; i < 3; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                    IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 signaled = 0;
  // runs lists in one call and waits for them: the device's reason for being removed, or S_OK
  auto run = [&](UINT count, ID3D12CommandList *const *ran) {
    queue->ExecuteCommandLists(count, ran);
    HRESULT hr;
    if (FAILED(hr = queue->Signal(fence.Get(), ++signaled)) || FAILED(hr = fence->SetEventOnCompletion(signaled, nullptr)))
      return hr;
    return device->GetDeviceRemovedReason();
  };
  auto begin = [&](UINT i) {
    HRESULT hr;
    if (FAILED(hr = allocators[i]->Reset()) || FAILED(hr = lists[i]->Reset(allocators[i].Get(), nullptr)))
      return hr;
    lists[i]->SetGraphicsRootSignature(roots.Get());
    lists[i]->SetComputeRootSignature(roots.Get());
    lists[i]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    return S_OK;
  };
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};

  if (argc > 2 && !strcmp(argv[2], "bench")) {
    constexpr UINT passes = 32, side = 64, bench_work = 1u << 12;
    const UINT runs = argc > 3 ? atoi(argv[3]) : 21;
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32_UINT,
                                    {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> targets[passes];
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passes};
    CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    const UINT step_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12CommandAllocator> bench_allocators[passes];
    ComPtr<ID3D12GraphicsCommandList> bench_lists[passes];
    ID3D12CommandList *ran[passes];
    for (UINT i = 0; i < passes; i++) {
      CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &target_desc,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])));
      device->CreateRenderTargetView(targets[i].Get(), nullptr,
                                     {heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step_size});
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&bench_allocators[i])));
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, bench_allocators[i].Get(), drawing.Get(),
                                      IID_PPV_ARGS(&bench_lists[i])));
      auto list = bench_lists[i].Get();
      list->SetGraphicsRootSignature(roots.Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv{heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step_size};
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(side), LONG(side)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      UINT constants[] = {i, bench_work, 0};
      list->SetGraphicsRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
      list->DrawInstanced(3, 1, 0, 0);
      CHECK(list->Close());
      ran[i] = list;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::vector<double> took;
    for (UINT r = 0; r < runs; r++) {
      LARGE_INTEGER began, ended;
      QueryPerformanceCounter(&began);
      CHECK(run(passes, ran));
      QueryPerformanceCounter(&ended);
      took.push_back(double(ended.QuadPart - began.QuadPart) * 1000 / frequency.QuadPart);
    }
    // the first run makes the pipeline's Metal objects
    took.erase(took.begin());
    std::sort(took.begin(), took.end());
    printf("bench: %u lists of a pass of %ux%u, median %.3f ms, fastest %.3f ms\n", passes, side, side,
           took[took.size() / 2], took[0]);
    return verdict();
  }

  D3D12_RESOURCE_DESC color_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> color;
  CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &color_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(color.Get(), nullptr, rtv);
  const UINT elements = size * size;
  const UINT64 bytes = UINT64(elements) * sizeof(UINT);
  ComPtr<ID3D12Resource> written, read;
  for (auto uav : {std::ref(written), std::ref(read)}) {
    uav.get() = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CHECK(uav.get() ? S_OK : E_FAIL);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 image_bytes;
  device->GetCopyableFootprints(&color_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, std::max(image_bytes, bytes), D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);

  auto constants = [&](ID3D12GraphicsCommandList *list, bool graphics, UINT value, UINT iterations) {
    UINT values[] = {value, iterations, 0};
    if (graphics)
      list->SetGraphicsRoot32BitConstants(0, UINT(std::size(values)), values, 0);
    else
      list->SetComputeRoot32BitConstants(0, UINT(std::size(values)), values, 0);
  };
  auto draw = [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline, UINT value, UINT iterations) {
    list->SetPipelineState(pipeline);
    list->SetGraphicsRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    constants(list, true, value, iterations);
    list->DrawInstanced(3, 1, 0, 0);
  };
  auto copy_written = [&](ID3D12GraphicsCommandList *list) {
    list->SetPipelineState(copying.Get());
    list->SetComputeRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2, read->GetGPUVirtualAddress());
    list->Dispatch(elements / group, 1, 1);
  };
  const UINT slow = 1, quick = 2;
  for (UINT c = 0; c < Cases; c++) {
    step("%s", names[c]);
    for (UINT r = 0; r < repeats; r++) {
      // a value of each repeat's own, which what an earlier one left cannot match
      const UINT value = 16 * (c * repeats + r) + 16;
      CHECK(begin(0));
      CHECK(begin(1));
      auto first = lists[0].Get(), second = lists[1].Get();
      D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
      if (c == Draw) {
        const float zero[4] = {};
        first->ClearRenderTargetView(rtv, zero, 0, nullptr);
        draw(first, drawing.Get(), slow, work);
        draw(second, drawing.Get(), quick, 0);
      } else {
        // a dispatch ignores the pixel waits of the draws before it: only an ordered pass waits for their pixels' UAV
        // writes. the barrier ends the draws' pass
        draw(first, writing.Get(), value, work);
        if (c == BarrierBefore)
          first->ResourceBarrier(1, &uav);
        if (c == BarrierAfter)
          second->ResourceBarrier(1, &uav);
        copy_written(second);
      }
      CHECK(first->Close());
      CHECK(second->Close());
      ID3D12CommandList *ran[] = {first, second};
      // the calls follow each other, the queue's thread given the second before the GPU ends the first
      if (c == Calls) {
        queue->ExecuteCommandLists(1, ran);
        CHECK(run(1, ran + 1));
      } else {
        CHECK(run(2, ran));
      }
      CHECK(forget(readback.Get()));
      CHECK(begin(2));
      auto back = lists[2].Get();
      if (c == Draw) {
        transition(back, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprint;
        back->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(back, color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      } else {
        transition(back, read.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        back->CopyBufferRegion(readback.Get(), 0, read.Get(), 0, bytes);
        transition(back, read.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      CHECK(submit(device.Get(), queue.Get(), back));
      BYTE *mapped;
      CHECK(readback->Map(0, nullptr, (void **)&mapped));
      UINT wrong = 0, got_first = 0, want = c == Draw ? quick : value + 1;
      for (UINT e = 0; e < elements; e++) {
        UINT got;
        auto offset = c == Draw ? (e / size) * footprint.Footprint.RowPitch + (e % size) * sizeof(UINT) : e * sizeof(UINT);
        memcpy(&got, mapped + offset, sizeof(UINT));
        if (got != want && !wrong++)
          got_first = got;
      }
      readback->Unmap(0, nullptr);
      expect(!wrong, "repeat %u: %u of %u wrong, the first %u, want %u", r, wrong, elements, got_first, want);
    }
  }
  return verdict();
}
