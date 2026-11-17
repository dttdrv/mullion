// contract: a pipeline keeps what it needs of its shaders once the application has freed their bytecode ("the
// application can free the memory [of the shader bytecode] after the call", D3D12_SHADER_BYTECODE as the runtime
// copies nothing it needs later), and pipelines made of the same shaders keep it once between them. 64 pipelines of
// one vertex and one pixel shader, each shader carrying 1 MB of the application's private data (the PRIV part,
// D3D_BLOB_PRIVATE_DATA), add less to what the process has resident (WorkingSetSize) than a quarter of one copy of
// the shaders a pipeline: Mullion kept a copy of each pipeline's pixel shader, for variants of it, and of its vertex
// shader, for draws of topologies with adjacency. after the bytecode is gone, a draw makes both kinds of variant of
// two of the pipelines: one without the depth its pixel shader outputs, as no depth stencil view is bound, and one
// of a triangle list with adjacency, which draws the triangles of vertices 0, 2 and 4 (D3D11.3 8.15); every pixel of
// each target is the value its draw's pixel shader was given
#include "d3d12_test.hpp"
#include <psapi.h>
#include <algorithm>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; };
// a triangle over the whole target from vertices 0, 1, 2 of a list, and 0, 2, 4 of a list with adjacency, whose
// adjacent vertices 1, 3, 5 lie outside it
float4 vs(uint id : SV_VertexID) : SV_Position {
  static const float2 corners[6] = {float2(-1, -1), float2(-1, 3), float2(3, -1), float2(-1, 3), float2(3, -1), float2(9, 9)};
  uint corner = id;
#if ADJACENT
  corner = id % 2 ? 5 : id / 2;
#endif
  return float4(corners[corner], 0, 1);
}
uint ps(float4 pos : SV_Position, out float depth : SV_Depth) : SV_Target {
  depth = 0.5;
  return value;
}
)hlsl";

// `code` with `bytes` of private data in a part of its own (DxilContainer.h: a part's name and size, then its data),
// signed again
static std::string
with_private_data(const std::string &code, size_t bytes, uint8_t seed) {
  constexpr size_t size_at = 24, count_at = 28, offsets_at = 32;
  uint32_t count, size;
  memcpy(&count, code.data() + count_at, 4);
  std::string out = code.substr(0, offsets_at + 4 * count);
  std::string parts = code.substr(offsets_at + 4 * count);
  // the offsets move by the one added
  for (uint32_t i = 0; i < count; i++) {
    uint32_t offset;
    memcpy(&offset, out.data() + offsets_at + 4 * i, 4);
    offset += 4;
    memcpy(out.data() + offsets_at + 4 * i, &offset, 4);
  }
  uint32_t added = uint32_t(out.size() + 4 + parts.size());
  out.append(reinterpret_cast<const char *>(&added), 4);
  out += parts;
  out.append("PRIV", 4);
  uint32_t length = uint32_t(bytes);
  out.append(reinterpret_cast<const char *>(&length), 4);
  for (size_t i = 0; i < bytes; i++)
    out.push_back(char(uint8_t(seed + i * 131)));
  count++, size = uint32_t(out.size());
  memcpy(out.data() + count_at, &count, 4);
  memcpy(out.data() + size_at, &size, 4);
  dxbc::sign(out);
  return out;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT width = 16, height = 16, pipelines = 64;
  const size_t private_bytes = 1 << 20;
  auto vs = compiler.compile(hlsl, "vs", "vs"), vs_adjacent = compiler.compile(hlsl, "vs", "vs", {"ADJACENT=1"}),
       ps = compiler.compile(hlsl, "ps", "ps");
  CHECK(!vs.empty() && !vs_adjacent.empty() && !ps.empty() ? S_OK : E_FAIL);
  vs = with_private_data(vs, private_bytes, 1);
  vs_adjacent = with_private_data(vs_adjacent, private_bytes, 2);
  ps = with_private_data(ps, private_bytes, 3);

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto resident = [] {
    PROCESS_MEMORY_COUNTERS counters{sizeof(counters)};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return (INT64)counters.WorkingSetSize;
  };
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {1, &parameter});
  CHECK(rs ? S_OK : E_FAIL);
  // pipelines that differ only in their depth bias, a triangle's: of one shader each, in every stage they have
  auto make = [&](const std::string &vertex, INT bias, ComPtr<ID3D12PipelineState> &pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vertex), bytecode(ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.DepthBias = bias;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  };
  // the first of each converts the shaders and makes what every pipeline of them shares
  ComPtr<ID3D12PipelineState> list_pso, adjacent_pso;
  CHECK(make(vs, 0, list_pso));
  CHECK(make(vs_adjacent, 0, adjacent_pso));
  std::vector<ComPtr<ID3D12PipelineState>> psos(pipelines);
  INT64 before = resident();
  for (UINT i = 0; i < pipelines; i++)
    CHECK(make(vs, INT(i + 1), psos[i]));
  INT64 grown = resident() - before;
  // the application's bytecode is gone: what the pipelines need of it from here on is their own
  for (auto code : {&vs, &vs_adjacent, &ps}) {
    std::fill(code->begin(), code->end(), '\xcd');
    std::string().swap(*code);
  }
  unsigned failures = 0;
  printf("%u pipelines of two shaders of %zu KB each: %lld KB more resident\n", pipelines,
         (private_bytes >> 10) + 1, (long long)grown >> 10);
  if (grown >= INT64(pipelines * private_bytes / 4)) {
    printf("pipelines of the same shaders keep a copy each of their bytecode\n");
    failures++;
  }

  // a target for each draw
  ComPtr<ID3D12Resource> targets[2];
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  for (auto &target : targets)
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
    ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = {rtvs->GetCPUDescriptorHandleForHeapStart(), rtvs->GetCPUDescriptorHandleForHeapStart()};
  rtv[1].ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = 0; i < 2; i++)
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv[i]);
  const UINT64 row = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * row * height, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(readback ? forget(readback.Get()) : E_FAIL);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float clear[4] = {};
  D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
  const UINT values[2] = {0x1234, 0x5678};
  ID3D12PipelineState *drawn[2] = {psos[pipelines - 1].Get(), adjacent_pso.Get()};
  const D3D_PRIMITIVE_TOPOLOGY topologies[2] = {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ};
  for (UINT i = 0; i < 2; i++) {
    list->ClearRenderTargetView(rtv[i], clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv[i], FALSE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetPipelineState(drawn[i]);
    list->SetGraphicsRoot32BitConstant(0, values[i], 0);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(topologies[i]);
    list->DrawInstanced(i ? 6 : 3, 1, 0, 0);
    transition(list.Get(), targets[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{targets[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = {i * row * height, {DXGI_FORMAT_R32_UINT, width, height, 1, (UINT)row}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i < 2; i++)
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        uint32_t value;
        memcpy(&value, got + i * row * height + y * row + 4 * x, 4);
        if (value != values[i] && failures++ < 16)
          printf("%s draw, pixel %u,%u: %#x, want %#x\n", i ? "adjacent" : "list", x, y, value, values[i]);
      }
  readback->Unmap(0, nullptr);
  if (failures) {
    printf("failed: %u\n", failures);
    return 1;
  }
  printf("passed\n");
  return 0;
}
