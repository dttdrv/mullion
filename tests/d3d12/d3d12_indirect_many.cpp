// contract: ExecuteIndirect interprets every command below MaxCommandCount, capped by the count buffer when present:
// "CommandCount = min(CommandCount, MaxCommandCount)" and "pCommandSignature->Interpret(Arguments);"
// (Microsoft Learn, ID3D12GraphicsCommandList::ExecuteIndirect, Remarks:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect).
// command n draws pixel n with n + 1, or adds n + 1 to word n; all other cells stay zero. draw and indexed draw
// read vertex n from their start locations (D3D11.3 8.4.1, 8.6.1), while a dispatch takes n from its root constant.
// counts cover empty, partial and multiple threadgroups, with and without count buffers, and the final cell is
// always a guard. the maxima also lie on both sides of 24, the most commands Mullion draws one by one where their
// arguments are, and the argument stride is wider than a draw's arguments, so a command read at the wrong stride
// draws another vertex. Metal Feature Set Tables, "Maximum threads per threadgroup", gives Apple7/8/9 a ceiling of
// 1024; https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf. the actual pipeline limit may be lower.
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
struct V { float4 p : SV_Position; nointerpolation uint n : N; };
V vs(uint id : ID) {
  V v;
  v.p = float4((float2(id % SIDE, id / SIDE) + 0.5) * float2(2, -2) / SIDE + float2(-1, 1), 0, 1);
  v.n = id + 1;
  return v;
}
uint ps(V v) : SV_Target { return v.n; }
cbuffer C : register(b0) { uint command; };
RWStructuredBuffer<uint> dst : register(u0);
[numthreads(1, 1, 1)] void cs() { InterlockedAdd(dst[command], command + 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT side = 512, cells = side * side, commands = cells - 1;
  const UINT threadgroup_limit = 1024;
  const std::vector<std::string> defines = {"SIDE=" + std::to_string(side)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines);
  if (vs.empty() || ps.empty() || cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  auto rs = root_signature(
      device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  if (!expect(!!rs, "root signature"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  D3D12_INPUT_ELEMENT_DESC element{"ID", 0, DXGI_FORMAT_R32_UINT};
  desc.InputLayout = {&element, 1};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12PipelineState> compute;
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
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
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, cells * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cells * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  void *empty;
  if (!expect(!!readback && !!output && !!zeros, "output buffers"))
    return verdict();
  CHECK(zeros->Map(0, nullptr, &empty));
  memset(empty, 0, cells * sizeof(UINT));
  zeros->Unmap(0, nullptr);

  // vertex n holds n, and command n draws vertex n alone
  auto vertices = buffer(
      device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ
  );
  if (!expect(!!vertices, "vertex and index buffer"))
    return verdict();
  UINT *ids;
  CHECK(vertices->Map(0, nullptr, (void **)&ids));
  for (UINT n = 0; n < commands; n++)
    ids[n] = n;
  vertices->Unmap(0, nullptr);
  D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), UINT(commands * sizeof(UINT)), sizeof(UINT)};
  D3D12_INDEX_BUFFER_VIEW ibv{vertices->GetGPUVirtualAddress(), vbv.SizeInBytes, DXGI_FORMAT_R32_UINT};
  struct Dispatch {
    UINT constant;
    D3D12_DISPATCH_ARGUMENTS groups;
  };
  union Arguments {
    D3D12_DRAW_ARGUMENTS draw;
    D3D12_DRAW_INDEXED_ARGUMENTS indexed;
    Dispatch dispatch;
  };
  D3D12_INDIRECT_ARGUMENT_DESC operations[] = {{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
                                             {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED},
                                             {D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT},
                                             {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH}};
  operations[2].Constant = {0, 0, 1};
  ComPtr<ID3D12CommandSignature> signatures[3];
  ComPtr<ID3D12Resource> arguments[3];
  for (UINT kind = 0; kind < std::size(signatures); kind++) {
    D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(Arguments), kind == 2 ? 2u : 1u, operations + kind};
    CHECK(device->CreateCommandSignature(&sig_desc, kind == 2 ? rs.Get() : nullptr, IID_PPV_ARGS(&signatures[kind])));
    arguments[kind] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands * sizeof(Arguments),
                             D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(!!arguments[kind], "argument buffer %u", kind))
      return verdict();
    Arguments *args;
    CHECK(arguments[kind]->Map(0, nullptr, (void **)&args));
    for (UINT n = 0; n < commands; n++) {
      if (kind == 0)
        args[n].draw = {1, 1, n, 0};
      else if (kind == 1)
        args[n].indexed = {1, 1, n, 0, 0};
      else
        args[n].dispatch = {n, {1, 1, 1}};
    }
    arguments[kind]->Unmap(0, nullptr);
  }
  auto count_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!count_buffer, "count buffer"))
    return verdict();

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  std::vector<UINT> maxima{commands, 23, 24, 25, 26};
  for (UINT boundary = 1; boundary <= threadgroup_limit; boundary *= 2)
    for (UINT maximum : {boundary - 1, boundary, boundary + 1})
      maxima.push_back(maximum);
  for (UINT kind = 0; kind < std::size(signatures); kind++)
    for (UINT maximum : maxima) {
      const UINT counts[] = {0, maximum / 2, maximum, maximum + 1};
      for (UINT form = 0; form <= std::size(counts); form++) {
        bool counted = form != 0, dispatch = kind == 2;
        UINT count = counted ? counts[form - 1] : maximum;
        UINT run = std::min(maximum, count);
        step("%s: maximum %u, %s count %u", dispatch ? "dispatch" : kind ? "indexed draw" : "draw", maximum,
             counted ? "buffer" : "no buffer;", count);
        UINT *counter;
        CHECK(count_buffer->Map(0, nullptr, (void **)&counter));
        *counter = count;
        count_buffer->Unmap(0, nullptr);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), dispatch ? compute.Get() : pso.Get()));
        if (dispatch) {
          list->CopyBufferRegion(output.Get(), 0, zeros.Get(), 0, cells * sizeof(UINT));
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          list->SetComputeRootSignature(rs.Get());
          list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        } else {
          const float zero[4] = {};
          list->ClearRenderTargetView(rtv, zero, 0, nullptr);
          list->SetGraphicsRootSignature(rs.Get());
          list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
          D3D12_VIEWPORT viewport{0, 0, (float)side, (float)side, 0, 1};
          D3D12_RECT scissor{0, 0, (LONG)side, (LONG)side};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
          list->IASetVertexBuffers(0, 1, &vbv);
          list->IASetIndexBuffer(&ibv);
        }
        list->ExecuteIndirect(signatures[kind].Get(), maximum, arguments[kind].Get(), 0,
                              counted ? count_buffer.Get() : nullptr, 0);
        if (dispatch) {
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, cells * sizeof(UINT));
          transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        } else {
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
          D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
              dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const char *pixels;
        CHECK(readback->Map(0, nullptr, (void **)&pixels));
        UINT wrong = 0, first = cells;
        for (UINT n = 0; n < cells; n++) {
          UINT offset = dispatch ? n * sizeof(UINT)
                                 : (n / side) * footprint.Footprint.RowPitch + sizeof(UINT) * (n % side);
          UINT got = *(const UINT *)(pixels + offset);
          UINT want = n < run ? n + 1 : 0;
          if (got != want) {
            first = std::min(first, n);
            wrong++;
          }
        }
        readback->Unmap(0, nullptr);
        expect(!wrong, "%u wrong cells, first %u; %u commands should run", wrong, first, run);
      }
    }
  return verdict();
}
