// contract: a barrier orders the work after it after the work before it for the stages its states (or SyncAfter)
// name, and a later barrier for pixel shading alone leaves that order standing. "ResourceBarrier: ... all prior
// accesses to the resource are complete before the transition" (D3D12 Resource Barriers), and an enhanced barrier
// "blocks the SyncAfter scope from executing until the SyncBefore scope completes" (D3D12 Enhanced Barriers, Barrier
// Sync). a slow draw writes a buffer a later draw's vertex shader reads, and other work writes a buffer its pixel
// shader reads:
// - Older: a slow dispatch, after the vertex barrier and before a barrier for pixel shading;
// - Together: a quick draw before the slow one, both barriers in one call after them;
// - Enhanced: as Together, in an enhanced barrier whose SyncAfter names vertex shading;
// - InPass: as Together, the barrier for pixel shading before the reading pass's first draw, which reads only the
//   pixel shader's buffer, and the vertex barrier before its second, which reads both.
// the reading draw must see both writes, though Mullion lets its vertices start before the passes just before it end.
// the GPU runs render passes' vertex stages in order, and their fragment stages, so what would run early is a vertex
// stage beside a slow fragment stage, or a fragment stage beside a dispatch
#include "d3d12_test.hpp"

static constexpr UINT size = 512, repeats = 4, scale = 1000, group = 64;
// so many iterations a pixel that a stage that did not wait for the slow draw would run first
static constexpr UINT work = 1u << 15;
static const char hlsl[] = R"hlsl(
cbuffer Parameters : register(b0) { uint value; uint work; uint mask; };
RWStructuredBuffer<uint> written : register(u0);
StructuredBuffer<uint> vertices_read : register(t0);
StructuredBuffer<uint> pixels_read : register(t1);
struct Vertex { float4 position : SV_Position; nointerpolation uint read : READ; };
Vertex vs(uint id : SV_VertexID) {
  Vertex v;
  v.position = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
  v.read = READS ? vertices_read[0] : 0;
  return v;
}
uint ps_write(Vertex v) : SV_Target {
  uint x = uint(v.position.x) ^ uint(v.position.y);
  [loop] for (uint i = 0; i < work; i++)
    x = x * 1664525u + 1013904223u;
  written[0] = value + (x & mask);
  return value;
}
[numthreads(GROUP, 1, 1)] void cs(uint3 tid : SV_DispatchThreadID) {
  uint x = tid.x;
  [loop] for (uint i = 0; i < work; i++)
    x = x * 1664525u + 1013904223u;
  // every thread writes the one value; a write that depends on the loop keeps the loop
  if (!(x & mask))
    written[0] = value;
}
uint ps_read(Vertex v) : SV_Target {
  return v.read * SCALE + pixels_read[0];
}
)hlsl";

enum Case { Older, Together, Enhanced, InPass, Cases };
static const char *names[] = {
    "the vertex barrier before the draw between", "both in one barrier", "both in one enhanced barrier",
    "the vertex barrier inside the reading pass"
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const std::string scaled = "SCALE=" + std::to_string(scale), grouped = "GROUP=" + std::to_string(group);
  auto vs_plain = compiler.compile(hlsl, "vs", "vs", {"READS=0", scaled, grouped}),
       vs_read = compiler.compile(hlsl, "vs", "vs", {"READS=1", scaled, grouped}),
       ps_write = compiler.compile(hlsl, "ps_write", "ps", {"READS=0", scaled, grouped}),
       ps_read = compiler.compile(hlsl, "ps_read", "ps", {"READS=1", scaled, grouped}),
       cs = compiler.compile(hlsl, "cs", "cs", {"READS=0", scaled, grouped});
  if (!expect(!vs_plain.empty() && !vs_read.empty() && !ps_write.empty() && !ps_read.empty() && !cs.empty(),
              "HLSL did not compile"))
    return verdict();
  if (argc > 2 && !strcmp(argv[2], "shaders"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)));
  if (!expect(options.EnhancedBarriersSupported, "enhanced barriers not supported"))
    return verdict();
  D3D12_ROOT_PARAMETER parameters[4] = {
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_SRV},
      {D3D12_ROOT_PARAMETER_TYPE_SRV}
  };
  parameters[0].Constants.Num32BitValues = 3;
  parameters[3].Descriptor.ShaderRegister = 1;
  auto roots = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  CHECK(roots ? S_OK : E_FAIL);
  // the writing draws' pipeline, the reading draw's, and one that reads only in its pixel shader
  const std::string *stages[][2] = {{&vs_plain, &ps_write}, {&vs_read, &ps_read}, {&vs_plain, &ps_read}};
  ComPtr<ID3D12PipelineState> pipelines[std::size(stages)];
  for (UINT p = 0; p < std::size(stages); p++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{roots.Get(), bytecode(*stages[p][0]), bytecode(*stages[p][1])};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[p])));
  }
  auto writes = pipelines[0].Get(), reads = pipelines[1].Get(), pixels_only = pipelines[2].Get();
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{roots.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> compute;
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList7> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the reading draw's target for each repeat of each case, then the two writing draws' own
  constexpr UINT reading = Cases * repeats, targets = reading + 2;
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> colors[targets];
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targets};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT step_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto rtv = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step_size}; };
  for (UINT i = 0; i < targets; i++) {
    CHECK(device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
        IID_PPV_ARGS(&colors[i])
    ));
    device->CreateRenderTargetView(colors[i].Get(), nullptr, rtv(i));
  }
  // what the vertex shader reads, and what the pixel shader reads
  ComPtr<ID3D12Resource> read[2];
  for (auto &b : read) {
    b = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CHECK(b ? S_OK : E_FAIL);
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 image_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, image_bytes * reading, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? S_OK : E_FAIL);
  CHECK(forget(readback.Get()));

  list->SetGraphicsRootSignature(roots.Get());
  list->SetComputeRootSignature(roots.Get());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
  auto draw = [&](ID3D12PipelineState *pipeline, UINT target, UINT value, UINT iterations, ID3D12Resource *into) {
    list->SetPipelineState(pipeline);
    auto view = rtv(target);
    list->OMSetRenderTargets(1, &view, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    UINT constants[] = {value, iterations, 0};
    list->SetGraphicsRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
    list->SetGraphicsRootUnorderedAccessView(1, into->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(2, read[0]->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(3, read[1]->GetGPUVirtualAddress());
    list->DrawInstanced(3, 1, 0, 0);
  };
  // the barriers from the writes to the reads (`to_read`) or back, for the buffers `which` names, in one call
  auto order = [&](bool enhanced, bool to_read, std::initializer_list<UINT> which) {
    std::vector<D3D12_RESOURCE_BARRIER> legacy;
    std::vector<D3D12_BUFFER_BARRIER> buffers;
    for (UINT b : which) {
      const D3D12_RESOURCE_STATES state =
          b ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      const D3D12_BARRIER_SYNC sync = b ? D3D12_BARRIER_SYNC_PIXEL_SHADING : D3D12_BARRIER_SYNC_VERTEX_SHADING;
      D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
      barrier.Transition = {read[b].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            to_read ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : state,
                            to_read ? state : D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
      legacy.push_back(barrier);
      buffers.push_back({to_read ? D3D12_BARRIER_SYNC_PIXEL_SHADING : sync, to_read ? sync : D3D12_BARRIER_SYNC_PIXEL_SHADING,
                         to_read ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS : D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                         to_read ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                         read[b].Get(), 0, UINT64_MAX});
    }
    if (!enhanced)
      return list->ResourceBarrier(UINT(legacy.size()), legacy.data());
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, UINT32(buffers.size())};
    group.pBufferBarriers = buffers.data();
    list->Barrier(1, &group);
  };
  // the writing draws' values for a repeat: a value no other repeat writes, which a read too early cannot give
  auto vertex_value = [](UINT n) { return n + 1; };
  auto pixel_value = [](UINT n) { return 100 + n; };
  for (UINT c = 0; c < Cases; c++)
    for (UINT r = 0; r < repeats; r++) {
      const UINT n = c * repeats + r;
      const bool enhanced = c == Enhanced;
      if (c == Older) {
        draw(writes, reading, vertex_value(n), work, read[0].Get());
        order(false, true, {0});
        list->SetPipelineState(compute.Get());
        UINT constants[] = {pixel_value(n), work, 0};
        list->SetComputeRoot32BitConstants(0, UINT(std::size(constants)), constants, 0);
        list->SetComputeRootUnorderedAccessView(1, read[1]->GetGPUVirtualAddress());
        list->Dispatch(size * size / group, 1, 1);
        order(false, true, {1});
      } else {
        draw(writes, reading + 1, pixel_value(n), 0, read[1].Get());
        draw(writes, reading, vertex_value(n), work, read[0].Get());
        if (c == InPass) {
          order(false, true, {1});
          draw(pixels_only, n, 0, 0, read[1].Get());
          order(false, true, {0});
        } else {
          order(enhanced, true, {0, 1});
        }
      }
      draw(reads, n, 0, 0, read[1].Get());
      order(enhanced, false, {0, 1});
    }
  for (UINT n = 0; n < reading; n++) {
    transition(list.Get(), colors[n].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{colors[n].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprint;
    dst.PlacedFootprint.Offset = n * image_bytes;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  BYTE *bytes;
  CHECK(readback->Map(0, nullptr, (void **)&bytes));
  for (UINT c = 0; c < Cases; c++) {
    step("%s", names[c]);
    for (UINT r = 0; r < repeats; r++) {
      const UINT n = c * repeats + r, want = vertex_value(n) * scale + pixel_value(n);
      UINT wrong = 0, first = 0;
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          UINT got;
          memcpy(&got, bytes + n * image_bytes + y * footprint.Footprint.RowPitch + x * sizeof(UINT), sizeof(UINT));
          if (got != want && !wrong++)
            first = got;
        }
      expect(!wrong, "repeat %u: %u pixels wrong, the first %u, want %u", r, wrong, first, want);
    }
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
