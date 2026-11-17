// contract: the lists of one ExecuteCommandLists are one stream of work (ID3D12CommandQueue::ExecuteCommandLists), so
// draws to the same target from successive lists land in submission order (D3D11.3 functional specification, 4.2
// Fixed Order of Pipeline Results), what a list clears at its start is cleared, a target only the next list draws to
// gets its pixels, depth one list wrote is what the next one tests against, and a list after them that copies the
// targets sees all of it. each case is some lists of a slow draw each, then a list that copies, in one call.
// argv[2] "child": the same in a process that encodes every run of passes on several threads
// (MULLION_D3D12_PARALLEL_COMMANDS, MULLION_D3D12_ENCODE_THREADS); the test starts it itself.
// argv[2] "bench": how long a call of lists takes that each draw once to one large target, as a game's lists recorded
// on several threads do; the median and fastest of its runs, argv[3] of them.
#include "d3d12_test.hpp"
#include <algorithm>

static constexpr UINT size = 256, lists = 4, repeats = 4;
// so many iterations a pixel that a copy which did not wait for the last draw would read before it wrote
static constexpr UINT work = 1u << 13;
static const char hlsl[] = R"hlsl(
cbuffer Parameters : register(b0) { uint value; uint work; uint mask; float depth; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, depth, 1);
}
uint slow() {
  uint v = value;
  [loop] for (uint i = 0; i < work; i++)
    v = v * 1664525u + 1013904223u;
  return value + (v & mask);
}
uint ps() : SV_Target { return slow(); }
struct Two { uint a : SV_Target0; uint b : SV_Target1; };
Two both() { Two two = {slow(), value + 1}; return two; }
)hlsl";

enum Case { Order, Clear, Targets, Depth, Cases };
static const char *names[] = {"draws in order", "a clear that begins a list", "a target only the next list has",
                              "depth a list wrote"};

static int
run(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       both = compiler.compile(hlsl, "both", "ps");
  if (!expect(!vs.empty() && !ps.empty() && !both.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants.Num32BitValues = 4;
  auto roots = root_signature(device.Get(), {1, &parameter});
  CHECK(roots ? S_OK : E_FAIL);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{roots.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = desc.BlendState.RenderTarget[1].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  // one target; one and depth, which a draw nearer than what is there writes; two targets
  ComPtr<ID3D12PipelineState> one, deep, two;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&one)));
  desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&deep)));
  desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
  desc.DepthStencilState = {};
  desc.NumRenderTargets = 2;
  desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
  desc.PS = bytecode(both);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&two)));

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  const bool bench = argc > 2 && !strcmp(argv[2], "bench");
  const UINT side = bench ? 2048 : size;
  D3D12_RESOURCE_DESC color_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> colors[2], depth;
  for (auto &color : colors)
    CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &color_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)));
  auto depth_desc = color_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2], dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < 2; i++) {
    rtvs[i] = {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
               i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    device->CreateRenderTargetView(colors[i].Get(), nullptr, rtvs[i]);
  }
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  // the lists that draw, and the one that copies
  ComPtr<ID3D12CommandAllocator> allocators[lists + 1];
  ComPtr<ID3D12GraphicsCommandList> recorded[lists + 1];
  for (UINT i = 0; i <= lists; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                    IID_PPV_ARGS(&recorded[i])));
    CHECK(recorded[i]->Close());
  }
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  UINT64 signaled = 0;
  // runs the lists in one call and waits for them: the device's reason for being removed, or S_OK
  auto execute = [&](UINT count) {
    ID3D12CommandList *ran[lists + 1];
    for (UINT i = 0; i < count; i++)
      ran[i] = recorded[i].Get();
    queue->ExecuteCommandLists(count, ran);
    HRESULT hr;
    if (FAILED(hr = queue->Signal(fence.Get(), ++signaled)) || FAILED(hr = fence->SetEventOnCompletion(signaled, nullptr)))
      return hr;
    return device->GetDeviceRemovedReason();
  };
  // records a list that draws `value` everywhere (or left of `split`) at depth `z`, after clearing the first target to
  // `cleared` if that is not 0
  auto record = [&](ID3D12CommandAllocator *allocator, ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline,
                    UINT value, UINT iterations, float z = 0, UINT split = ~0u, UINT cleared = 0) {
    HRESULT hr;
    if (FAILED(hr = allocator->Reset()) || FAILED(hr = list->Reset(allocator, pipeline)))
      return hr;
    // a UINT target takes the clear's floats as integers
    const float clear[4] = {float(cleared)};
    if (cleared)
      list->ClearRenderTargetView(rtvs[0], clear, 0, nullptr);
    list->SetGraphicsRootSignature(roots.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(pipeline == two.Get() ? 2 : 1, rtvs, TRUE, pipeline == deep.Get() ? &dsv : nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(std::min(split, side)), LONG(side)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    UINT constants[4] = {value, iterations, 0};
    memcpy(&constants[3], &z, sizeof(z));
    list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);
    return list->Close();
  };
  auto draw = [&](UINT i, auto &&...arguments) {
    return record(allocators[i].Get(), recorded[i].Get(), arguments...);
  };

  if (bench) {
    constexpr UINT passes = 32;
    const UINT runs = argc > 3 ? atoi(argv[3]) : 21;
    ComPtr<ID3D12CommandAllocator> bench_allocators[passes];
    ComPtr<ID3D12GraphicsCommandList> bench_lists[passes];
    ID3D12CommandList *ran[passes];
    for (UINT i = 0; i < passes; i++) {
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&bench_allocators[i])));
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, bench_allocators[i].Get(), one.Get(),
                                      IID_PPV_ARGS(&bench_lists[i])));
      CHECK(bench_lists[i]->Close());
      // a draw of a little of the target, whose pass costs what Metal loads and stores of the whole
      CHECK(record(bench_allocators[i].Get(), bench_lists[i].Get(), one.Get(), i, 0, 0, side / 32));
      ran[i] = bench_lists[i].Get();
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::vector<double> took;
    for (UINT r = 0; r < runs; r++) {
      LARGE_INTEGER began, ended;
      QueryPerformanceCounter(&began);
      queue->ExecuteCommandLists(passes, ran);
      CHECK(queue->Signal(fence.Get(), ++signaled));
      CHECK(fence->SetEventOnCompletion(signaled, nullptr));
      QueryPerformanceCounter(&ended);
      took.push_back(double(ended.QuadPart - began.QuadPart) * 1000 / frequency.QuadPart);
    }
    // the first run makes the pipeline's Metal objects
    took.erase(took.begin());
    std::sort(took.begin(), took.end());
    printf("bench: %u lists of a draw to %ux%u, median %.3f ms, fastest %.3f ms\n", passes, side, side,
           took[took.size() / 2], took[0]);
    return verdict();
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 image_bytes;
  device->GetCopyableFootprints(&color_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  ComPtr<ID3D12Resource> readbacks[2];
  for (auto &readback : readbacks) {
    readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, image_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(readback ? S_OK : E_FAIL);
  }
  for (UINT c = 0; c < Cases; c++) {
    step("%s", names[c]);
    for (UINT r = 0; r < repeats; r++) {
      // values of each repeat's own, which what an earlier one left cannot match
      const UINT base = 16 * (c * repeats + r) + 16;
      // what each target holds at each pixel when the lists are done
      UINT want[2][2]; // [target][left or right of the split]
      const UINT split = side / 2;
      {
        auto list = recorded[0].Get();
        CHECK(allocators[0]->Reset());
        CHECK(list->Reset(allocators[0].Get(), nullptr));
        for (UINT t = 0; t < 2; t++) {
          const float clear[4] = {float(base + 9 + t)};
          list->ClearRenderTargetView(rtvs[t], clear, 0, nullptr);
        }
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        CHECK(list->Close());
        CHECK(execute(1));
      }
      if (c == Order) {
        // each list draws over the one before
        for (UINT i = 0; i < lists; i++)
          CHECK(draw(i, one.Get(), base + i, work));
        want[0][0] = want[0][1] = base + lists - 1;
        want[1][0] = want[1][1] = base + 10;
      } else if (c == Clear) {
        // the second list clears the target, then draws left of the split
        CHECK(draw(0, one.Get(), base, work));
        CHECK(draw(1, one.Get(), base + 1, work, 0.0f, split, base + 7));
        for (UINT i = 2; i < lists; i++)
          CHECK(draw(i, one.Get(), base + i, work, 0, split));
        want[0][0] = base + lists - 1, want[0][1] = base + 7;
        want[1][0] = want[1][1] = base + 10;
      } else if (c == Targets) {
        // the last list draws to both targets, the ones before to the first
        for (UINT i = 0; i + 1 < lists; i++)
          CHECK(draw(i, one.Get(), base + i, work));
        CHECK(draw(lists - 1, two.Get(), base + lists - 1, work, 0, split));
        want[0][0] = base + lists - 1, want[0][1] = base + lists - 2;
        want[1][0] = base + lists, want[1][1] = base + 10;
      } else {
        // the first list draws at the middle depth; the next ones farther, which fail the test, then nearer
        CHECK(draw(0, deep.Get(), base, work, 0.5f));
        CHECK(draw(1, deep.Get(), base + 1, work, 0.75f));
        CHECK(draw(2, deep.Get(), base + 2, work, 0.25f, split));
        CHECK(draw(3, deep.Get(), base + 3, work, 0.375f));
        want[0][0] = base + 2, want[0][1] = base + 3;
        want[1][0] = want[1][1] = base + 10;
      }
      // the copies in the same call, after a barrier
      auto back = recorded[lists].Get();
      CHECK(allocators[lists]->Reset());
      CHECK(back->Reset(allocators[lists].Get(), nullptr));
      for (UINT t = 0; t < 2; t++) {
        CHECK(forget(readbacks[t].Get()));
        transition(back, colors[t].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{colors[t].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{readbacks[t].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprint;
        back->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(back, colors[t].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      }
      CHECK(back->Close());
      CHECK(execute(lists + 1));
      for (UINT t = 0; t < 2; t++) {
        BYTE *mapped;
        CHECK(readbacks[t]->Map(0, nullptr, (void **)&mapped));
        UINT wrong = 0, got_first = 0, want_first = 0;
        for (UINT y = 0; y < side; y++)
          for (UINT x = 0; x < side; x++) {
            UINT got, expected = want[t][x >= split];
            memcpy(&got, mapped + y * footprint.Footprint.RowPitch + x * sizeof(UINT), sizeof(UINT));
            if (got != expected && !wrong++)
              got_first = got, want_first = expected;
          }
        readbacks[t]->Unmap(0, nullptr);
        expect(!wrong, "repeat %u, target %u: %u of %u wrong, the first %u, want %u", r, t, wrong, side * side,
               got_first, want_first);
      }
    }
  }
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 2)
    return run(argc, argv);
  int result = run(argc, argv);
  if (result != 0)
    return result;
  step("every run of passes encoded on several threads");
  char exe[MAX_PATH];
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);
  SetEnvironmentVariableA("MULLION_D3D12_PARALLEL_COMMANDS", "1");
  SetEnvironmentVariableA("MULLION_D3D12_ENCODE_THREADS", "4");
  auto command = std::string("\"") + exe + "\" " + (argc > 1 ? argv[1] : "dxil") + " child";
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) ? S_OK
                                                                                                             : E_FAIL);
  DWORD waited = WaitForSingleObject(process.hProcess, 300000), code = 1;
  if (waited != WAIT_OBJECT_0)
    TerminateProcess(process.hProcess, 1);
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  expect(waited == WAIT_OBJECT_0 && code == 0, "the child's result %lu, wait %lu", code, waited);
  return verdict();
}
