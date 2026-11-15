// contract: every command of an ExecuteIndirect draws with the state the command list has bound, but for what its
// signature's arguments set: the call's pseudo-code runs "pCommandSignature->Interpret(Arguments);" for each command
// and sets nothing else (Microsoft Learn, ID3D12GraphicsCommandList::ExecuteIndirect, Remarks, and Indirect Drawing).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing
// three commands each draw one point at a pixel of their own, taken from the vertex buffer in slot 0 at the command's
// start vertex. the pixel holds a per-instance value from slot 1, and a root constant as the vertex shader, the pixel
// shader and, where the pipeline has one, the geometry shader each read it. one signature only draws, so all three
// commands read the list's slot 1; another names slot 1, so each command reads its own. the root constant is the
// list's in both, in every command and stage. a third signature sets the second of the root parameter's two
// constants, which the vertex shader folds into what it writes: each command has its own, over the list's first
// constant and the list's vertex buffers. pixels no command draws stay zero.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint base; uint own; };
struct V { float4 p : SV_Position; nointerpolation uint n : N; };
V vs(uint id : ID, uint extra : EXTRA) {
  V v;
  v.p = float4((float2(id % SIDE, id / SIDE) + 0.5) * float2(2, -2) / SIDE + float2(-1, 1), 0, 1);
  v.n = extra | (base ^ own) << 8;
  return v;
}
[maxvertexcount(1)] void gs(point V i[1], inout PointStream<V> o) {
  V v = i[0];
  v.n |= base << 24;
  o.Append(v);
}
uint ps(V v) : SV_Target { return v.n | base << 16; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr UINT side = 4, cells = side * side, commands = 3, base = 0x5a, own = 4;
  const UINT extras[commands] = {0x11, 0x22, 0x33}, owns[commands] = {1, 2, 3};
  const std::vector<std::string> defines = {"SIDE=" + std::to_string(side)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), gs = compiler.compile(hlsl, "gs", "gs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !gs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants = {0, 0, 2};
  auto rs = root_signature(
      device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  if (!expect(bool(rs), "root signature was not created"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"ID", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"EXTRA", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1}
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.InputLayout = {elements, UINT(std::size(elements))};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pipelines[2];
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[0])));
  desc.GS = bytecode(gs);
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[1])));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, side, side, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // vertex n of slot 0 holds n; slot 1's instance values follow in the same buffer; then each signature's commands
  struct Command {
    D3D12_VERTEX_BUFFER_VIEW view;
    D3D12_DRAW_ARGUMENTS draw;
  };
  struct Constant {
    UINT own;
    D3D12_DRAW_ARGUMENTS draw;
  };
  const UINT extras_at = cells * sizeof(UINT), plain_at = extras_at + sizeof(extras),
             named_at = plain_at + commands * sizeof(D3D12_DRAW_ARGUMENTS),
             constant_at = named_at + commands * sizeof(Command);
  auto data = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, constant_at + commands * sizeof(Constant), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  if (!expect(readback && data, "buffers were not created"))
    return verdict();
  char *mapped;
  CHECK(data->Map(0, nullptr, (void **)&mapped));
  const auto address = data->GetGPUVirtualAddress();
  for (UINT n = 0; n < cells; n++)
    memcpy(mapped + n * sizeof(UINT), &n, sizeof(n));
  memcpy(mapped + extras_at, extras, sizeof(extras));
  for (UINT n = 0; n < commands; n++) {
    const D3D12_DRAW_ARGUMENTS draw{1, 1, n, 0};
    const Command command{{address + extras_at + n * sizeof(UINT), sizeof(UINT), sizeof(UINT)}, draw};
    memcpy(mapped + plain_at + n * sizeof(draw), &draw, sizeof(draw));
    memcpy(mapped + named_at + n * sizeof(command), &command, sizeof(command));
    const Constant constant{owns[n], draw};
    memcpy(mapped + constant_at + n * sizeof(constant), &constant, sizeof(constant));
  }
  data->Unmap(0, nullptr);
  const D3D12_VERTEX_BUFFER_VIEW views[] = {
      {address, cells * sizeof(UINT), sizeof(UINT)}, {address + extras_at, sizeof(extras), sizeof(UINT)}
  };
  D3D12_INDIRECT_ARGUMENT_DESC arguments[] = {
      {D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW}, {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}
  };
  D3D12_INDIRECT_ARGUMENT_DESC constants[] = {
      {D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT}, {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}
  };
  arguments[0].VertexBuffer.Slot = 1;
  constants[0].Constant = {0, 1, 1};
  // what a signature's commands set, their description, and where they are
  enum { Plain, Named, Constants };
  const struct {
    const char *sets;
    D3D12_COMMAND_SIGNATURE_DESC desc;
    UINT at;
  } kinds[] = {
      {"only draws", {sizeof(D3D12_DRAW_ARGUMENTS), 1, arguments + 1}, plain_at},
      {"names vertex buffer slot 1", {sizeof(Command), 2, arguments}, named_at},
      {"sets a root constant", {sizeof(Constant), 2, constants}, constant_at}
  };
  ComPtr<ID3D12CommandSignature> signatures[std::size(kinds)];
  for (UINT i = 0; i < std::size(signatures); i++)
    CHECK(device->CreateCommandSignature(
        &kinds[i].desc, i == Constants ? rs.Get() : nullptr, IID_PPV_ARGS(&signatures[i])
    ));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (UINT geometry = 0; geometry < 2; geometry++)
    for (UINT kind = 0; kind < std::size(kinds); kind++) {
      step("%s pipeline, signature that %s", geometry ? "geometry" : "ordinary", kinds[kind].sets);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pipelines[geometry].Get()));
      const float zero[4] = {};
      list->ClearRenderTargetView(rtv, zero, 0, nullptr);
      list->SetGraphicsRootSignature(rs.Get());
      const UINT root[] = {base, own};
      list->SetGraphicsRoot32BitConstants(0, UINT(std::size(root)), root, 0);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(side), LONG(side)};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      list->IASetVertexBuffers(0, UINT(std::size(views)), views);
      list->ExecuteIndirect(signatures[kind].Get(), commands, data.Get(), kinds[kind].at, nullptr, 0);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      into.PlacedFootprint = footprint;
      list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const char *pixels;
      CHECK(readback->Map(0, nullptr, (void **)&pixels));
      for (UINT n = 0; n < cells; n++) {
        UINT got;
        memcpy(
            &got, pixels + footprint.Offset + (n / side) * footprint.Footprint.RowPitch + (n % side) * sizeof(UINT),
            sizeof(got)
        );
        UINT want = n < commands ? extras[kind == Named ? n : 0] | (base ^ (kind == Constants ? owns[n] : own)) << 8 |
                                       base << 16 | (geometry ? base << 24 : 0)
                                 : 0;
        expect(got == want, "pixel %u is %08x, expected %08x", n, got, want);
      }
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
