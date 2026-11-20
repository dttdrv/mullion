// contract: draws that nothing orders with each other still give the results of their submission order. "observable
// rendering results must match results produced by serial processing of tasks" (D3D11.3 functional specification,
// 4.2 Fixed Order of Pipeline Results), UAV accesses aside. a slow draw is followed, with no barrier, by a dispatch or a
// draw elsewhere, then by a quick draw to the slow one's color or depth target: the quick one's pixels must land last.
// argv[2] "bench": how long a list of small passes that nothing orders takes, each a draw to its own target, as a
// game's shadow cascades and post-processing chains are; the median and fastest of its runs, argv[3] of them.
// "bench-mixed" follows each draw with a dispatch, as a game's compute between its passes does.
#include "d3d12_test.hpp"
#include <algorithm>

static constexpr UINT size = 512, group = 64, repeats = 4;
// so many iterations a pixel that work after the slow draw which did not wait for it would run first
static constexpr UINT work = 1u << 15;
static const char hlsl[] = R"hlsl(
cbuffer Parameters : register(b0) { uint value; uint work; uint mask; float depth; };
RWStructuredBuffer<uint> written : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, depth, 1);
}
uint ps(float4 position : SV_Position) : SV_Target {
  uint v = uint(position.x) ^ uint(position.y);
  [loop] for (uint i = 0; i < work; i++)
    v = v * 1664525u + 1013904223u;
  return value + (v & mask);
}
[numthreads(GROUP, 1, 1)] void cs(uint3 tid : SV_DispatchThreadID) {
  uint v = tid.x;
  [loop] for (uint i = 0; i < work; i++)
    v = v * 1664525u + 1013904223u;
  written[tid.x] = value + (v & mask);
}
)hlsl";

enum Case { Dispatch, Target, Depth, Cases };
static const char *names[] = {"a dispatch between", "a draw to another target between", "a shared depth target"};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const std::vector<std::string> defines = {"GROUP=" + std::to_string(group)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines);
  if (!expect(!vs.empty() && !ps.empty() && !cs.empty(), "HLSL did not compile"))
    return verdict();
  if (argc > 2 && !strcmp(argv[2], "shaders"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameters[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].Constants.Num32BitValues = 4;
  auto roots = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  CHECK(roots ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{roots.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> compute;
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));
  // without depth, writing depth where it is less, and testing it equal without writing it
  enum { Plain, Writes, Equal };
  const D3D12_COMPARISON_FUNC funcs[] = {D3D12_COMPARISON_FUNC(0), D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL};
  ComPtr<ID3D12PipelineState> pipelines[std::size(funcs)];
  for (UINT p = 0; p < std::size(funcs); p++) {
    auto func = funcs[p];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{roots.Get(), bytecode(vs), bytecode(ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    if (func) {
      desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthFunc = func;
      desc.DepthStencilState.DepthWriteMask =
          func == D3D12_COMPARISON_FUNC_LESS ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    }
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[p])));
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  if (argc > 2 && !strncmp(argv[2], "bench", 5)) {
    constexpr UINT passes = 32, side = 64, bench_work = 1u << 12;
    const UINT runs = argc > 3 ? atoi(argv[3]) : 21;
    const bool mixed = !strcmp(argv[2], "bench-mixed");
    auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, group * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CHECK(written ? S_OK : E_FAIL);
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> targets[passes];
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, passes};
    CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
    const UINT step_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < passes; i++) {
      CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            nullptr, IID_PPV_ARGS(&targets[i])));
      device->CreateRenderTargetView(targets[i].Get(), nullptr, {heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step_size});
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::vector<double> took;
    CHECK(list->Close());
    for (UINT run = 0; run < runs; run++) {
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipelines[Plain].Get()));
      list->SetGraphicsRootSignature(roots.Get());
      list->SetComputeRootSignature(roots.Get());
      list->SetComputeRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(side), LONG(side)};
      for (UINT i = 0; i < passes; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step_size};
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        UINT constants[] = {i, bench_work, 0, 0};
        list->SetGraphicsRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
        if (mixed) {
          list->SetPipelineState(compute.Get());
          const UINT constants[] = {i, 0, 0};
          list->SetComputeRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
          list->Dispatch(1, 1, 1);
          list->SetPipelineState(pipelines[Plain].Get());
        }
      }
      LARGE_INTEGER began, ended;
      QueryPerformanceCounter(&began);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      QueryPerformanceCounter(&ended);
      took.push_back(double(ended.QuadPart - began.QuadPart) * 1000 / frequency.QuadPart);
    }
    // the first run makes the pipelines' Metal objects
    took.erase(took.begin());
    std::sort(took.begin(), took.end());
    printf("bench: %u passes of %ux%u%s, median %.3f ms, fastest %.3f ms\n", passes, side, side,
           mixed ? " each with a dispatch" : "", took[took.size() / 2], took[0]);
    return verdict();
  }

  // the slow draw's target, the target drawn between, and the quick draw's target when only depth is shared
  D3D12_RESOURCE_DESC color_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      size,
      size,
      1,
      1,
      DXGI_FORMAT_R32_UINT,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  auto depth_desc = color_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> colors[3], depth;
  for (auto &color : colors)
    CHECK(device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
        IID_PPV_ARGS(&color)
    ));
  CHECK(device->CreateCommittedResource(
      &properties, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, UINT(std::size(colors))},
      dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap)));
  const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[std::size(colors)], dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < std::size(colors); i++) {
    rtvs[i] = {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step};
    device->CreateRenderTargetView(colors[i].Get(), nullptr, rtvs[i]);
  }
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  const UINT elements = size * size;
  auto written = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, UINT64(elements) * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  CHECK(written ? S_OK : E_FAIL);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 image_bytes;
  device->GetCopyableFootprints(&color_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  // per repeat of a case: the quick draw's target, the other target, and the dispatch's buffer
  const UINT64 slot = 2 * image_bytes + UINT64(elements) * sizeof(UINT);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * Cases * repeats, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);
  CHECK(forget(readback.Get()));

  list->SetGraphicsRootSignature(roots.Get());
  list->SetComputeRootSignature(roots.Get());
  list->SetComputeRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
  const float depth_value = 0.5f;
  auto draw = [&](ID3D12PipelineState *pipeline, UINT target, bool with_depth, UINT value, UINT iterations) {
    list->SetPipelineState(pipeline);
    list->OMSetRenderTargets(1, &rtvs[target], FALSE, with_depth ? &dsv : nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    UINT constants[] = {value, iterations, 0, 0};
    memcpy(&constants[3], &depth_value, sizeof(float));
    list->SetGraphicsRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
    list->DrawInstanced(3, 1, 0, 0);
  };
  const UINT slow = 1, between = 5, quick = 2, dispatched = 7;
  for (UINT c = 0; c < Cases; c++)
    for (UINT r = 0; r < repeats; r++) {
      const float zero[4] = {};
      for (auto &rtv : rtvs)
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
      list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
      // the last repeat's dispatch is overwritten with zeros here, with no view for ClearUnorderedAccessViewUint
      const UINT none[3] = {};
      list->SetPipelineState(compute.Get());
      list->SetComputeRoot32BitConstants(0, 3, none, 0);
      list->Dispatch(elements / group, 1, 1);
      D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
      list->ResourceBarrier(1, &barrier);
      const bool depth_case = c == Depth;
      draw(pipelines[depth_case ? Writes : Plain].Get(), 0, depth_case, slow, work);
      if (c == Dispatch) {
        // longer than the slow draw, so that the copies after it which did not wait for it would read first
        const UINT constants[] = {dispatched, 2 * work, 0};
        list->SetPipelineState(compute.Get());
        list->SetComputeRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
        list->Dispatch(elements / group, 1, 1);
      } else {
        draw(pipelines[Plain].Get(), 1, false, between, 0);
      }
      draw(pipelines[depth_case ? Equal : Plain].Get(), depth_case ? 2 : 0, depth_case, quick, 0);
      const UINT64 base = slot * (c * repeats + r);
      // the quick draw's target, then the other one it reads back: with only depth shared, the slow draw's
      for (UINT i = 0; i < 2; i++) {
        auto texture = colors[c == Depth ? (i ? 0 : 2) : i].Get();
        transition(list.Get(), texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprint;
        dst.PlacedFootprint.Offset = base + i * image_bytes;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      transition(list.Get(), written.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), base + 2 * image_bytes, written.Get(), 0, UINT64(elements) * sizeof(UINT));
      transition(list.Get(), written.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  BYTE *bytes;
  CHECK(readback->Map(0, nullptr, (void **)&bytes));
  for (UINT c = 0; c < Cases; c++) {
    step("%s", names[c]);
    const UINT want[2] = {quick, c == Target ? between : c == Depth ? slow : 0};
    for (UINT r = 0; r < repeats; r++) {
      auto base = bytes + slot * (c * repeats + r);
      for (UINT i = 0; i < 2; i++) {
        UINT wrong = 0, first = 0;
        for (UINT y = 0; y < size; y++)
          for (UINT x = 0; x < size; x++) {
            UINT got;
            memcpy(&got, base + i * image_bytes + y * footprint.Footprint.RowPitch + x * sizeof(UINT), sizeof(UINT));
            if (got != want[i] && !wrong++)
              first = got;
          }
        expect(!wrong, "repeat %u target %u: %u pixels wrong, the first %u, want %u", r, i, wrong, first, want[i]);
      }
      UINT wrong = 0;
      for (UINT e = 0; e < elements; e++) {
        UINT got;
        memcpy(&got, base + 2 * image_bytes + e * sizeof(UINT), sizeof(UINT));
        wrong += got != (c == Dispatch ? dispatched : 0);
      }
      expect(!wrong, "repeat %u: %u dispatch elements wrong", r, wrong);
    }
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
