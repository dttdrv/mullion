// contract: the calls of the later device and list interfaces, which engines prefer where they find them, do what
// their documentation says. one frame goes through them and is read back:
// - ID3D12Device4::CreateCommandList1 "creates a command list in the closed state": Reset succeeds at once, where a
//   list of CreateCommandList, which records, fails it with E_FAIL (ID3D12GraphicsCommandList::Reset);
// - ID3D12Device9::CreateCommandQueue1 makes a queue of the description given, which runs the list;
// - ID3D12Device8::CreateCommittedResource2 makes the resource its D3D12_RESOURCE_DESC1 describes, and
//   GetCopyableFootprints1 and GetResourceAllocationInfo2 answer for it as GetCopyableFootprints and
//   GetResourceAllocationInfo do for the same description;
// - BeginRenderPass clears a target whose beginning access is CLEAR to that color, and EndRenderPass keeps one whose
//   ending access is PRESERVE; a device of render pass tier 0 has both, "via software emulation"
//   (D3D12_RENDER_PASS_TIER). DiscardResource before the pass and PIX events around the draws change nothing;
// - OMSetBlendFactor gives D3D12_BLEND_BLEND_FACTOR its factors, and for NULL "a blend factor equal to
//   { 1, 1, 1, 1 }";
// - WriteBufferImmediate writes each value at its address in a buffer in the COPY_DEST state, with null modes as
//   D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT.
// the target is cleared to one color and drawn white in two stripes, the first through the blend factor, the second
// through a null one, so each of the three colors comes from one of the calls.
#include "d3d12_test.hpp"
#include <cmath>

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
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12Device4> device4;
  ComPtr<ID3D12Device8> device8;
  ComPtr<ID3D12Device9> device9;
  if (FAILED(device.As(&device4)) || FAILED(device.As(&device8)) || FAILED(device.As(&device9))) {
    printf("skipped: the device has no ID3D12Device9\n");
    return 77;
  }
  const UINT size = 8, stripe = 3;
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  const float cleared[4] = {0.25f, 0.5f, 0.75f, 1}, factor[4] = {0.5f, 0.25f, 1, 0.75f};

  step("ID3D12Device9::CreateCommandQueue1");
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH}, queue_is{};
  CHECK(device9->CreateCommandQueue1(&queue_desc, __uuidof(ID3D12Device), IID_PPV_ARGS(&queue)));
  queue->GetDesc(&queue_is);
  expect(queue_is.Type == queue_desc.Type && queue_is.Priority == queue_desc.Priority,
         "the queue's description: type %u, priority %d, want %u and %d", queue_is.Type, queue_is.Priority,
         queue_desc.Type, queue_desc.Priority);

  step("ID3D12Device4::CreateCommandList1: a closed list");
  ComPtr<ID3D12CommandAllocator> allocator, recording_allocator;
  ComPtr<ID3D12GraphicsCommandList4> list;
  ComPtr<ID3D12GraphicsCommandList> recording;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&recording_allocator)));
  CHECK(device4->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&list)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, recording_allocator.Get(), nullptr, IID_PPV_ARGS(&recording)));
  HRESULT closed = list->Reset(allocator.Get(), nullptr), open = recording->Reset(recording_allocator.Get(), nullptr);
  expect(closed == S_OK && open == E_FAIL, "Reset of a new list: %08lx from CreateCommandList1 and %08lx from CreateCommandList, want S_OK and E_FAIL",
         closed, open);
  if (closed != S_OK)
    return verdict();

  step("ID3D12Device8: a resource of a D3D12_RESOURCE_DESC1, its footprint and its allocation");
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_RESOURCE_DESC1 desc1{};
  memcpy(&desc1, &desc, sizeof(desc));
  ComPtr<ID3D12Resource> target;
  CHECK(device8->CreateCommittedResource2(&heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, nullptr, IID_PPV_ARGS(&target)));
  auto made = target->GetDesc();
  expect(made.Dimension == desc.Dimension && made.Width == size && made.Height == size && made.Format == format && made.Flags == desc.Flags,
         "the resource: dimension %u, %llu x %u, format %u, flags %x", made.Dimension, made.Width, made.Height, made.Format, made.Flags);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint, footprint1{};
  UINT rows, rows1 = 0;
  UINT64 row_bytes, row_bytes1 = 0, bytes, bytes1 = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_bytes, &bytes);
  device8->GetCopyableFootprints1(&desc1, 0, 1, 0, &footprint1, &rows1, &row_bytes1, &bytes1);
  expect(!memcmp(&footprint, &footprint1, sizeof(footprint)) && rows == rows1 && row_bytes == row_bytes1 && bytes == bytes1,
         "GetCopyableFootprints1: a pitch of %u, %u rows, %llu bytes; GetCopyableFootprints has %u, %u, %llu",
         footprint1.Footprint.RowPitch, rows1, bytes1, footprint.Footprint.RowPitch, rows, bytes);
  auto allocation = device->GetResourceAllocationInfo(0, 1, &desc), allocation1 = device8->GetResourceAllocationInfo2(0, 1, &desc1, nullptr);
  expect(allocation.SizeInBytes == allocation1.SizeInBytes && allocation.Alignment == allocation1.Alignment && allocation.SizeInBytes,
         "GetResourceAllocationInfo2: %llu bytes aligned to %llu; GetResourceAllocationInfo has %llu and %llu",
         allocation1.SizeInBytes, allocation1.Alignment, allocation.SizeInBytes, allocation.Alignment);

  auto rs = root_signature(device.Get(), {});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(vs), bytecode(ps)};
  // the color drawn, times the blend factor
  pso_desc.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                         D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
                                         D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
  pso_desc.SampleMask = ~0u;
  pso_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = format;
  pso_desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  // values written at every other word of a buffer, the rest left as it is made: zero, which a committed resource
  // is unless its heap says D3D12_HEAP_FLAG_CREATE_NOT_ZEROED
  const UINT values[] = {0x1234567, 0x89abcdef, 0xfedcba9};
  const UINT words = 2 * std::size(values);
  auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, words * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes + words * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  step("a frame: DiscardResource, a render pass that clears and preserves, blend factors, events, immediate writes");
  const char event[] = "frame";
  list->SetPipelineState(pso.Get());
  list->SetGraphicsRootSignature(rs.Get());
  list->DiscardResource(target.Get(), nullptr);
  // PIX's event of an ANSI string (pix3.h: PIX_EVENT_ANSI_VERSION)
  list->BeginEvent(1, event, sizeof(event));
  D3D12_RENDER_PASS_RENDER_TARGET_DESC pass{rtv, {D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR}, {D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE}};
  pass.BeginningAccess.Clear.ClearValue.Format = format;
  memcpy(pass.BeginningAccess.Clear.ClearValue.Color, cleared, sizeof(cleared));
  list->BeginRenderPass(1, &pass, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  list->RSSetViewports(1, &viewport);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT n = 0; n < 2; n++) {
    D3D12_RECT scissor{LONG(n * stripe), 0, LONG((n + 1) * stripe), (LONG)size};
    list->RSSetScissorRects(1, &scissor);
    step("the frame: OMSetBlendFactor of %s", n ? "NULL" : "factors");
    list->OMSetBlendFactor(n ? nullptr : factor);
    list->SetMarker(1, event, sizeof(event));
    list->DrawInstanced(3, 1, 0, 0);
  }
  list->EndRenderPass();
  list->EndEvent();
  step("the frame: WriteBufferImmediate");
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameters[std::size(values)];
  for (UINT i = 0; i < std::size(values); i++)
    parameters[i] = {written->GetGPUVirtualAddress() + 2 * i * sizeof(UINT), values[i]};
  list->WriteBufferImmediate(std::size(values), parameters, nullptr);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), written.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get()}, to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  list->CopyBufferRegion(readback.Get(), bytes, written.Get(), 0, words * sizeof(UINT));
  step("the frame: on the queue");
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const UINT8 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      // the stripe drawn through the factors, the one drawn through none, and what the pass cleared
      const float *want = x < stripe ? factor : x < 2 * stripe ? nullptr : cleared;
      auto texel = got + y * footprint.Footprint.RowPitch + x * 4;
      for (int c = 0; c < 4; c++)
        if (float channel = want ? want[c] * 255 : 255; std::fabs(texel[c] - channel) > 1)
          expect(false, "pixel %u,%u channel %d: %u, want %.1f, from %s", x, y, c, texel[c], channel,
                 x < stripe ? "OMSetBlendFactor's factors" : x < 2 * stripe ? "OMSetBlendFactor(NULL)" : "the render pass's clear");
    }
  UINT word[words];
  memcpy(word, got + bytes, sizeof(word));
  for (UINT i = 0; i < words; i++)
    expect(word[i] == (i % 2 ? 0 : values[i / 2]), "word %u after WriteBufferImmediate: %x, want %x", i, word[i], i % 2 ? 0 : values[i / 2]);
  return verdict();
}
