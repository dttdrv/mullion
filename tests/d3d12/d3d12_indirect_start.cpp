// contract: index reads outside the view return zero, and unsigned 32-bit address arithmetic wraps back into it.
// "Any calculated address that would fall out of bounds for a Buffer being accessed results in out-of-bounds
// behavior being invoked, where the return is 0 in all non-missing components of the format (defined in the Input
// Layout), and the default for missing components (see Defaults for Missing Components(19.1.3.3))." (D3D11.3
// 8.19.2). "In other words, should any calculation overflow 32-bits, it would wrap - and should that result happen
// to fall back into a valid range for the scenario, so be it." (8.19.1). D3D12_INDEX_BUFFER_VIEW supplies the address
// and SizeInBytes. the byte address is "StartIndexLocation*sizeof(IndexBuffer.Format)" (8.6.1).
// positions come from vertex data: "StrideInBytes*(BaseVertexLocation + IndexValue)" (8.6.1), while VertexID
// "represents the index value" (8.16). every vertex is a point at its own pixel; additive blending counts each read,
// including repeated zeros at BaseVertexLocation. indices behind the view name other pixels. CASE=n (argv[2]) selects
// one draw; only indirect starts far past the allocation run in child processes, so a GPU fault is isolated.
#include "d3d12_test.hpp"
#include <algorithm>
#include <limits>
#include <utility>

static const char hlsl[] = R"hlsl(
struct V { float4 p : SV_Position; };
V vs(uint column : COLUMN) {
  V v;
  v.p = float4((column + 0.5) * 2 / WIDTH - 1, 0, 0, 1);
  return v;
}
float ps(V v) : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  const UINT indices[] = {3, 1, 4, 2, 5, 6, 7, 8}, there = std::size(indices) / 2;
  const INT base = 2;
  const char *modes[] = {"direct", "bound view", "view argument"};
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT};
  const UINT width = *std::max_element(std::begin(indices), std::end(indices)) + base + 2;
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const UINT last = std::numeric_limits<UINT>::max();
  struct Case {
    DXGI_FORMAT format;
    UINT mode, start, count;
  };
  std::vector<Case> cases;
  for (auto format : formats)
    for (UINT mode = 0; mode < std::size(modes); mode++) {
      const UINT index_size = format == DXGI_FORMAT_R32_UINT ? sizeof(UINT) : sizeof(UINT16);
      const UINT period = last / index_size + 1;
      const UINT zeros = system.dwAllocationGranularity / (mode ? sizeof(UINT16) : index_size);
      for (auto [start, count] : {std::pair<UINT, UINT>{0, 0}, {0, there}, {there - 1, 2},
                                 {there, 1}, {there + 1, there + 1}, {last, 1}, {period, 1},
                                 {there, zeros - 1}, {there, zeros}})
        cases.push_back({format, mode, start, count});
      if (!mode)
        for (auto [start, count] : {std::pair<UINT, UINT>{there, zeros + 1}, {last, 2}, {last, there + 2}})
          cases.push_back({format, mode, start, count});
    }
  int only = -1;
  for (int i = 2; i < argc; i++)
    if (!strncmp(argv[i], "CASE=", 5))
      only = atoi(argv[i] + 5);
  if (only >= 0 && !expect(UINT(only) < cases.size(), "CASE=%d is outside %zu cases", only, cases.size()))
    return verdict();
  if (only < 0)
    for (UINT n = 0; n < cases.size(); n++) {
      if (!cases[n].mode || cases[n].start < std::size(indices))
        continue;
      step("case %u in its own process", n);
      std::string command =
          "\"" + std::string(argv[0]) + "\" " + (argc > 1 ? argv[1] : "dxil") + " CASE=" + std::to_string(n);
      STARTUPINFOA startup{sizeof(startup)};
      PROCESS_INFORMATION process{};
      if (!expect(
              CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
              "CreateProcess failed: %lu", GetLastError()
          ))
        return verdict();
      WaitForSingleObject(process.hProcess, INFINITE);
      DWORD code;
      BOOL read = GetExitCodeProcess(process.hProcess, &code);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      if (read && code == 77)
        return 77;
      expect(read && code == 0, "case %u exited with %lu", n, read ? code : GetLastError());
    }
  step("compiling shaders");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  step("creating device and graphics pipeline");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(
      device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT}
  );
  const D3D12_INPUT_ELEMENT_DESC element{"COLUMN", 0, DXGI_FORMAT_R32_UINT};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.InputLayout = {&element, 1};
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  width,
                                  1,
                                  1,
                                  1,
                                  desc.RTVFormats[0],
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, width * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *columns;
  CHECK(vertices->Map(0, nullptr, (void **)&columns));
  for (UINT i = 0; i < width; i++)
    columns[i] = i;
  vertices->Unmap(0, nullptr);
  const D3D12_VERTEX_BUFFER_VIEW vertex_view{
      vertices->GetGPUVirtualAddress(), UINT(width * sizeof(UINT)), sizeof(UINT)
  };
  ComPtr<ID3D12CommandSignature> signatures[std::size(modes)];
  auto commands = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD,
                         sizeof(D3D12_INDEX_BUFFER_VIEW) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT n = 0; n < cases.size(); n++) {
    const auto [format, mode, start, count] = cases[n];
    if (only >= 0 ? n != UINT(only) : mode && start >= std::size(indices))
      continue;
    const bool wide = format == DXGI_FORMAT_R32_UINT;
    step("case %u: %s, %s, start %#x, count %u, base %d", n, wide ? "32 bits" : "16 bits", modes[mode], start,
         count, base);
    const UINT index_size = wide ? sizeof(UINT) : sizeof(UINT16);
    auto ib = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, std::size(indices) * index_size,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped;
    CHECK(ib->Map(0, nullptr, &mapped));
    for (UINT i = 0; i < std::size(indices); i++)
      if (wide)
        static_cast<UINT *>(mapped)[i] = indices[i];
      else
        static_cast<UINT16 *>(mapped)[i] = indices[i];
    ib->Unmap(0, nullptr);
    const D3D12_INDEX_BUFFER_VIEW view{ib->GetGPUVirtualAddress(), there * index_size, format};
    const D3D12_DRAW_INDEXED_ARGUMENTS draw{count, 1, start, base, 0};
    const UINT view_bytes = mode == 2 ? sizeof(view) : 0;
    if (mode && !signatures[mode]) {
      std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
      if (mode == 2)
        arguments.push_back({D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW});
      arguments.push_back({D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED});
      D3D12_COMMAND_SIGNATURE_DESC signature_desc{
          view_bytes + UINT(sizeof(draw)), UINT(arguments.size()), arguments.data()
      };
      CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signatures[mode])));
    }
    CHECK(commands->Map(0, nullptr, &mapped));
    memcpy(mapped, &view, view_bytes);
    memcpy(static_cast<char *>(mapped) + view_bytes, &draw, sizeof(draw));
    commands->Unmap(0, nullptr);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    CHECK(forget(readback.Get()));
    const float clear[4] = {};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), 1};
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->IASetVertexBuffers(0, 1, &vertex_view);
    const D3D12_INDEX_BUFFER_VIEW bound{
        view.BufferLocation + (mode == 2 ? view.SizeInBytes : 0), view.SizeInBytes, view.Format
    };
    list->IASetIndexBuffer(&bound);
    if (mode == 0)
      list->DrawIndexedInstanced(count, 1, start, base, 0);
    else
      list->ExecuteIndirect(signatures[mode].Get(), 1, commands.Get(), 0, nullptr, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    std::vector<float> want(width);
    for (UINT i = 0; i < count; i++) {
      const UINT address = (start + i) * index_size;
      want[base + (address < view.SizeInBytes ? indices[address / index_size] : 0)]++;
    }
    const float *pixels;
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    for (UINT x = 0; x < width; x++)
      expect(pixels[x] == want[x], "pixel %u counts %.0f indices, want %.0f", x, pixels[x], want[x]);
    readback->Unmap(0, nullptr);
    expect(device->GetDeviceRemovedReason() == S_OK, "device removed after the indexed draw");

    step("case %u: a draw after the indexed draw", n);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    CHECK(forget(readback.Get()));
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    list->IASetVertexBuffers(0, 1, &vertex_view);
    list->DrawInstanced(1, 1, width - 1, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(readback->Map(0, nullptr, (void **)&pixels));
    for (UINT x = 0; x < width; x++)
      expect(pixels[x] == float(x == width - 1), "following draw's pixel %u is %.0f", x, pixels[x]);
    readback->Unmap(0, nullptr);
    expect(device->GetDeviceRemovedReason() == S_OK, "device removed after the following draw");
  }
  return verdict();
}
