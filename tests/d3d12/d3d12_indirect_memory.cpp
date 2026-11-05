// contract: an ExecuteIndirect's memory is that of what its commands set. Metal reserves room in each command of an
// indirect command buffer for the buffers its descriptor says a command may set ("Metal needs to reserve enough memory
// in each command to store this many arguments. Use the smallest value that works for all commands",
// MTLIndirectCommandBufferDescriptor maxKernelBufferBindCount and maxVertexBufferBindCount). a command that changes a
// root constant changes no more than the root arguments, at most D3D12_MAX_ROOT_COST dwords (Microsoft Learn, Root
// signature limits), so what an allocator keeps for each draw that changes a root constant, as Metal counts it (DXGI
// CurrentUsage, "current video memory usage, in bytes"), is less than those dwords more than for a draw that changes
// nothing. as many draws as a power of two, the sizes Mullion's pools of command buffers come in; and as many
// dispatches that each change a root constant run with their own: dispatch n adds n + 1 to word n
#include "d3d12_test.hpp"
#include <dxgi1_4.h>

static const char hlsl[] = R"hlsl(
cbuffer Command : register(b0) { uint n; };
RWByteAddressBuffer words : register(u0);
[numthreads(1, 1, 1)] void cs() { words.InterlockedAdd(n * 4, n + 1); }
float4 vs(uint v : SV_VertexID) : SV_Position { return float4(v == 1 ? 1 : -1, v == 2 ? 1 : -1, n * 1e-9, 1); }
float4 ps() : SV_Target { return float4(n, 0, 0, 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto cs = compiler.compile(hlsl, "cs", "cs"), vs = compiler.compile(hlsl, "vs", "vs"),
       ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!cs.empty() && !vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter3> adapter;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  CHECK(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
  auto usage = [&]() -> INT64 {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    return info.CurrentUsage;
  };
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 1};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  auto rs = root_signature(device.Get(), {2, parameters});
  if (!expect(rs != nullptr, "the root signature is refused"))
    return verdict();
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> compute;
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC render_desc{rs.Get(), bytecode(vs), bytecode(ps)};
  render_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  render_desc.SampleMask = ~0u;
  render_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  render_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  render_desc.NumRenderTargets = 1;
  render_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  render_desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> render;
  CHECK(device->CreateGraphicsPipelineState(&render_desc, IID_PPV_ARGS(&render)));

  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                        {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  CHECK(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  const UINT count = 1u << 14;
  struct Dispatch {
    UINT n;
    D3D12_DISPATCH_ARGUMENTS dispatch;
  };
  struct Draw {
    UINT n;
    D3D12_DRAW_ARGUMENTS draw;
  };
  auto dispatches = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(Dispatch) * count,
                           D3D12_RESOURCE_STATE_GENERIC_READ),
       draws = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(Draw) * count, D3D12_RESOURCE_STATE_GENERIC_READ),
       words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
       readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, count * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(dispatches && draws && words && readback, "the buffers are refused"))
    return verdict();
  Dispatch *dispatch;
  Draw *draw;
  CHECK(dispatches->Map(0, nullptr, (void **)&dispatch));
  CHECK(draws->Map(0, nullptr, (void **)&draw));
  for (UINT n = 0; n < count; n++)
    dispatch[n] = {n, {1, 1, 1}}, draw[n] = {n, {3, 1, 0, 0}};

  D3D12_INDIRECT_ARGUMENT_DESC constant{D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT};
  constant.Constant = {0, 0, 1};
  const D3D12_INDIRECT_ARGUMENT_DESC dispatch_arguments[] = {constant, {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}},
                                     draw_arguments[] = {constant, {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}};
  const D3D12_COMMAND_SIGNATURE_DESC dispatch_desc{sizeof(Dispatch), 2, dispatch_arguments},
      rooted_desc{sizeof(Draw), 2, draw_arguments}, plain_desc{sizeof(Draw), 1, draw_arguments + 1};
  ComPtr<ID3D12CommandSignature> dispatch_signature, rooted_signature, plain_signature;
  CHECK(device->CreateCommandSignature(&dispatch_desc, rs.Get(), IID_PPV_ARGS(&dispatch_signature)));
  CHECK(device->CreateCommandSignature(&rooted_desc, rs.Get(), IID_PPV_ARGS(&rooted_signature)));
  CHECK(device->CreateCommandSignature(&plain_desc, nullptr, IID_PPV_ARGS(&plain_signature)));

  // a list of one ExecuteIndirect, run: what Metal counts more after it, for each command. allocators and lists stay
  // alive, since a released command buffer leaves Metal's count only when Metal frees it
  std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
  std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
  auto each = [&](ID3D12PipelineState *pso, auto record) -> INT64 {
    allocators.emplace_back();
    lists.emplace_back();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators.back()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators.back().Get(), pso,
                                         IID_PPV_ARGS(&lists.back()))))
      return expect(false, "a command list is refused"), 0;
    INT64 before = usage();
    record(lists.back().Get());
    expect(SUCCEEDED(submit(device.Get(), queue.Get(), lists.back().Get())), "a command list fails");
    return (usage() - before) / count;
  };

  step("dispatches");
  each(compute.Get(), [&](ID3D12GraphicsCommandList *list) {
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(1, words->GetGPUVirtualAddress());
    list->ExecuteIndirect(dispatch_signature.Get(), count, dispatches.Get(), 0, nullptr, 0);
    transition(list, words.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, words.Get(), 0, count * 4);
  });
  UINT *word;
  CHECK(readback->Map(0, nullptr, (void **)&word));
  UINT wrong = 0;
  for (UINT n = 0; n < count; n++)
    wrong += word[n] != n + 1;
  expect(!wrong, "%u of %u dispatches did not run with their own constant", wrong, count);
  readback->Unmap(0, nullptr);

  step("draws");
  const D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
  const D3D12_RECT scissor{0, 0, 4, 4};
  auto drawing = [&](ID3D12CommandSignature *signature, UINT64 offset) {
    return each(render.Get(), [&](ID3D12GraphicsCommandList *list) {
      list->SetGraphicsRootSignature(rs.Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->ExecuteIndirect(signature, count, draws.Get(), offset, nullptr, 0);
    });
  };
  INT64 plain = drawing(plain_signature.Get(), offsetof(Draw, draw)), rooted = drawing(rooted_signature.Get(), 0),
        most = D3D12_MAX_ROOT_COST * 4;
  expect(rooted - plain < most,
         "a draw that changes a root constant keeps %lld bytes more than one that changes nothing, %lld or more", rooted - plain,
         most);
  return verdict();
}
