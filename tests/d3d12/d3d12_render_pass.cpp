// contract: what a render pass binds, clears and leaves (DirectX-Specs, Render Passes).
// - the targets bound for raster are those whose beginning access is not NO_ACCESS, and a pipeline's render targets
//   are these in their order: "entries [0] and [1] map to BeginPass surfaces [2] and [4] respectively - the order the
//   raster bound surfaces appear in the render pass RTV list without any gaps" (Surfaces That BeginRenderPass Binds
//   For Raster). a target that is not bound keeps what it held;
// - a beginning access of CLEAR clears "whether or not the resource is interacted with any further in the Render
//   Pass" (D3D12_RENDER_PASS_BEGINNING_ACCESS_CLEAR);
// - "The RTVs/DSV set in BeginRenderPass the Render Pass are not propagated out to the Command List" (Render Pass
//   output bindings): a draw after EndRenderPass reaches none of them, as vkd3d-proton's test_renderpass_rendering
//   records of Windows;
// - a pass that resumes one in the next list goes on with what that one left: suspending and resuming "results in no
//   rendering difference (versus ENDING_PRESERVE/BEGINNING_PRESERVE)" (HLK Testing), also where the resuming pass is
//   given the suspending one's description with its clear, which the same test of vkd3d-proton's records.
// the draws are a triangle over the whole target, cut to a scissor: red to the pipeline's first target and green to
// its second, so each texel says which target it was and whether a clear or a draw came last.
// https://microsoft.github.io/DirectX-Specs/d3d/RenderPasses.html
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct Targets {
  float4 first : SV_Target0;
  float4 second : SV_Target1;
};
Targets ps() {
  Targets to;
  to.first = float4(1, 0, 0, 1);
  to.second = float4(0, 1, 0, 1);
  return to;
}
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
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  for (auto &target : desc.BlendState.RenderTarget)
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 2;
  desc.RTVFormats[0] = desc.RTVFormats[1] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  // four targets of 8 by 8, and a fifth for the passes that suspend and resume
  enum { Targets = 5 };
  const UINT size = 8, half = size / 2;
  ComPtr<ID3D12Resource> target[Targets];
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, Targets};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[Targets];
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  for (UINT i = 0; i < Targets; i++) {
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target[i])
    ));
    rtv[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
    rtv[i].ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(target[i].Get(), nullptr, rtv[i]);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, Targets * bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator[2];
  ComPtr<ID3D12GraphicsCommandList4> list[2];
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  for (UINT i = 0; i < 2; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator[i])));
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator[i].Get(), pso.Get(), IID_PPV_ARGS(&list[i])
    ));
  }

  // what a target holds before the passes, what a pass clears one to, and the pipeline's two colors
  const float before[4] = {0.2f, 0.4f, 0.6f, 0.8f}, cleared[4] = {0, 0, 1, 1};
  const UINT32 held = 0xcc996633, blue = 0xffff0000, red = 0xff0000ff, green = 0xff00ff00;
  auto access = [&](UINT i, D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE begin, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE end) {
    D3D12_RENDER_PASS_RENDER_TARGET_DESC pass{rtv[i], {begin}, {end}};
    pass.BeginningAccess.Clear.ClearValue.Format = format;
    memcpy(pass.BeginningAccess.Clear.ClearValue.Color, cleared, sizeof(cleared));
    return pass;
  };
  const auto no_access = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS,
             preserve = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE,
             clear = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
  const auto keep = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE,
             none = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
  const D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  // a triangle over the target, cut to the columns from `left` to `right`
  auto draw = [&](ID3D12GraphicsCommandList4 *to, LONG left, LONG right) {
    const D3D12_RECT scissor{left, 0, right, (LONG)size};
    to->SetGraphicsRootSignature(rs.Get());
    to->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    to->RSSetViewports(1, &viewport);
    to->RSSetScissorRects(1, &scissor);
    to->DrawInstanced(3, 1, 0, 0);
  };

  step("a pass of four targets, the first and third not bound; a draw in it and one after it");
  for (UINT i = 0; i < Targets; i++)
    list[0]->ClearRenderTargetView(rtv[i], before, 0, nullptr);
  const D3D12_RENDER_PASS_RENDER_TARGET_DESC four[] = {
      access(0, no_access, none), access(1, preserve, keep), access(2, no_access, none), access(3, clear, keep)
  };
  list[0]->BeginRenderPass(std::size(four), four, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  draw(list[0].Get(), 0, half);
  list[0]->EndRenderPass();
  // the right half, after the pass: no target has it
  draw(list[0].Get(), half, size);
  CHECK(submit(device.Get(), queue.Get(), list[0].Get()));

  step("a pass that clears and draws and is suspended, and the pass of the next list that resumes it");
  CHECK(allocator[0]->Reset());
  CHECK(list[0]->Reset(allocator[0].Get(), pso.Get()));
  const D3D12_RENDER_PASS_RENDER_TARGET_DESC one[] = {access(4, clear, keep)};
  list[0]->BeginRenderPass(1, one, nullptr, D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS);
  draw(list[0].Get(), 0, half);
  list[0]->EndRenderPass();
  CHECK(list[0]->Close());
  list[1]->BeginRenderPass(1, one, nullptr, D3D12_RENDER_PASS_FLAG_RESUMING_PASS);
  list[1]->EndRenderPass();
  for (UINT i = 0; i < Targets; i++) {
    transition(list[1].Get(), target[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target[i].Get()}, to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    to.PlacedFootprint = footprint;
    to.PlacedFootprint.Offset = i * bytes;
    list[1]->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  CHECK(list[1]->Close());
  ID3D12CommandList *both[] = {list[0].Get(), list[1].Get()};
  queue->ExecuteCommandLists(2, both);
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(queue->Signal(fence.Get(), 1));
  auto done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  CHECK(fence->SetEventOnCompletion(1, done));
  WaitForSingleObject(done, INFINITE);
  CloseHandle(done);

  // each target's left and right halves
  const struct {
    UINT32 left, right;
    const char *what;
  } want[Targets] = {
      {held, held, "the first target, not bound"},
      {red, held, "the second target, the pipeline's first"},
      {held, held, "the third target, not bound"},
      {green, blue, "the fourth target, the pipeline's second, cleared"},
      {red, blue, "the target of the suspended and resumed passes"},
  };
  const UINT8 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < Targets; i++)
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        UINT32 texel, wanted = x < half ? want[i].left : want[i].right;
        memcpy(&texel, got + i * bytes + y * footprint.Footprint.RowPitch + x * 4, sizeof(texel));
        expect(texel == wanted, "%s: texel %u,%u is %08x, want %08x", want[i].what, x, y, texel, wanted);
      }
  readback->Unmap(0, nullptr);
  return verdict();
}
