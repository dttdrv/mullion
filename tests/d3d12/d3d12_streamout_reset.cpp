// contract: Reset and ClearState return a list's stream output targets to their initial state, none bound
// (ID3D12GraphicsCommandList::Reset: "as if a new command list was just created"; ClearState: "Resets the state of a
// direct command list back to the state it was in when the command list was created"), so a draw with stream output
// after them streams to no buffer. a recording binds a buffer and its filled size and streams three points out of a
// vertex shader; the buffer and the size are then zeroed, and a recording after Reset, and one that binds the buffer
// and then calls ClearState, stream three more. both leave the buffer and the size at zero.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearstate
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
struct S { float4 pos : SV_Position; uint4 value : VALUE; };
S vs(uint id : SV_VertexID) {
  S o;
  o.pos = float4(0, 0, 0, 1);
  o.value = uint4(id + 1, 2, 3, 4);
  return o;
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs");
  if (!expect(!vs.empty(), "the shader does not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
  if (!expect(bool(rs), "the root signature is refused"))
    return verdict();

  const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 4, 0}};
  const UINT strides[] = {16};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs)};
  desc.StreamOutput = {entries, 1, strides, 1, D3D12_SO_NO_RASTERIZED_STREAM};
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pipeline;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));

  // the stream output buffer, then its filled size
  constexpr UINT64 bytes = 256, filled_at = 128;
  auto target = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(target && zeros && readback, "the buffers are refused"))
    return verdict();
  void *mapped;
  CHECK(zeros->Map(0, nullptr, &mapped));
  memset(mapped, 0, bytes);
  zeros->Unmap(0, nullptr);

  auto draw = [&] {
    list->SetGraphicsRootSignature(rs.Get());
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->DrawInstanced(3, 1, 0, 0);
  };
  // the buffer and its size zeroed, and left for stream output
  auto zero = [&] {
    list->CopyBufferRegion(target.Get(), 0, zeros.Get(), 0, bytes);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  };
  // the buffer read back, and left for copies
  auto read = [&] {
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, bytes);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  };
  auto check = [&](bool streamed) {
    UINT32 *got;
    if (FAILED(readback->Map(0, nullptr, (void **)&got)))
      return expect(false, "the readback buffer does not map");
    UINT wrong = 0;
    for (UINT i = 0; i < bytes / 4; i++) {
      UINT32 want = 0;
      if (streamed && i < 12)
        want = i % 4 ? i % 4 + 1 : i / 4 + 1;
      else if (streamed && i == filled_at / 4)
        want = 3 * strides[0];
      if (got[i] != want && !wrong++)
        printf("word %u is %u, want %u\n", i, got[i], want);
    }
    readback->Unmap(0, nullptr);
    return expect(!wrong, "%u wrong words", wrong);
  };

  step("a recording that binds a stream output buffer");
  CHECK(forget(readback.Get()));
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  zero();
  const D3D12_STREAM_OUTPUT_BUFFER_VIEW view{target->GetGPUVirtualAddress(), filled_at,
                                            target->GetGPUVirtualAddress() + filled_at};
  list->SOSetTargets(0, 1, &view);
  draw();
  read();
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  check(true);

  for (bool clear_state : {false, true}) {
    step("a draw after %s, with no buffer bound", clear_state ? "SOSetTargets and ClearState" : "Reset");
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    zero();
    if (clear_state) {
      list->SOSetTargets(0, 1, &view);
      list->ClearState(nullptr);
    }
    draw();
    read();
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    check(false);
  }
  return verdict();
}
