// contract: a geometry shader takes every register the vertex shader may output, 32 of them (D3D11.3 9.1? the
// vertex shader's output registers; 13: the geometry shader's inputs), for every vertex of its primitives, however
// many a draw has. each geometry shader hashes all of its primitive's vertices' registers, in order, and covers one
// pixel with the hash, which the CPU makes of the same values. the draws are longer than what one group of threads
// takes, as lists and as strips, whose primitives share vertices.
#include "d3d12_test.hpp"
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer Case : register(b0) { uint row; };
struct W { float4 pos : SV_Position; uint4 a[REGISTERS] : A; };
W vs(uint id : SV_VertexID) {
  W o;
  o.pos = float4(0, 0, 0, 1);
  for (uint r = 0; r < REGISTERS; r++)
    o.a[r] = id * ID_STEP + r * 4 + uint4(0, 1, 2, 3);
  return o;
}
struct P { float4 pos : SV_Position; nointerpolation uint value : VALUE; };
// a triangle over the center of the primitive's pixel, with the hash
void cover(uint primitive, uint hash, inout TriangleStream<P> s) {
  float2 at = float2(primitive % WIDTH, row + primitive / WIDTH);
  for (uint c = 0; c < 3; c++) {
    P o;
    o.pos = float4((at + float2(c == 1 ? 1.4 : 0, c == 2 ? 1.4 : 0)) / float2(WIDTH, HEIGHT) * float2(2, -2) + float2(-1, 1), 0, 1);
    o.value = hash;
    s.Append(o);
  }
}
#define HASH(n)                                    \
  uint hash = 0;                                   \
  for (uint i = 0; i < n; i++)                     \
    for (uint r = 0; r < REGISTERS; r++)           \
      for (uint c = 0; c < 4; c++)                 \
        hash = hash * MULTIPLIER + v[i].a[r][c];
[maxvertexcount(3)]
void gs_point(point W v[1], uint primitive : SV_PrimitiveID, inout TriangleStream<P> s) { HASH(1) cover(primitive, hash, s); }
[maxvertexcount(3)]
void gs_line(line W v[2], uint primitive : SV_PrimitiveID, inout TriangleStream<P> s) { HASH(2) cover(primitive, hash, s); }
[maxvertexcount(3)]
void gs_triangle(triangle W v[3], uint primitive : SV_PrimitiveID, inout TriangleStream<P> s) { HASH(3) cover(primitive, hash, s); }
uint ps(P p) : SV_Target { return p.value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // every output register beside the position's, and more primitives than two groups of threads take
  const UINT registers = D3D12_VS_OUTPUT_REGISTER_COUNT - 1, primitives = 70, width = 16, id_step = 131, multiplier = 31;
  struct Case {
    const char *gs;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    D3D_PRIMITIVE_TOPOLOGY topology;
    UINT corners, vertices;
    // vertex `corner` of primitive n
    UINT (*vertex)(UINT n, UINT corner);
  };
  const Case cases[] = {
      {"gs_point", D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 1, primitives,
       [](UINT n, UINT) { return n; }},
      {"gs_line", D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, 2, primitives + 1,
       [](UINT n, UINT corner) { return n + corner; }},
      {"gs_triangle", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 3 * primitives,
       [](UINT n, UINT corner) { return 3 * n + corner; }},
      // a strip's odd triangles are (n, n + 2, n + 1)
      {"gs_triangle", D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 3, primitives + 2,
       [](UINT n, UINT corner) { return n + (n % 2 && corner ? 3 - corner : corner); }},
  };
  const UINT case_rows = (primitives + width - 1) / width, height = case_rows * std::size(cases);
  std::vector<std::string> defines = {
      "REGISTERS=" + std::to_string(registers), "WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height),
      "ID_STEP=" + std::to_string(id_step), "MULTIPLIER=" + std::to_string(multiplier),
  };
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {1, &parameter});
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  ComPtr<ID3D12PipelineState> psos[std::size(cases)];
  for (size_t i = 0; i < std::size(cases); i++) {
    auto gs = compiler.compile(hlsl, cases[i].gs, "gs", defines);
    if (vs.empty() || gs.empty() || ps.empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
    desc.GS = bytecode(gs);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = cases[i].type;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&psos[i])));
  }

  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * height, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  // no hash is the clear value's: a pixel past a case's primitives keeps it
  const UINT untouched = 0;
  const float clear[4] = {};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  for (UINT i = 0; i < std::size(cases); i++) {
    UINT first_row = case_rows * i;
    list->SetPipelineState(psos[i].Get());
    list->SetGraphicsRoot32BitConstants(0, 1, &first_row, 0);
    list->IASetPrimitiveTopology(cases[i].topology);
    list->DrawInstanced(cases[i].vertices, 1, 0, 0);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_UINT, width, height, 1, (UINT)row}};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *pixels;
  CHECK(readback->Map(0, nullptr, (void **)&pixels));

  unsigned failures = 0;
  for (UINT y = 0; y < height; y++)
    for (UINT x = 0; x < width; x++) {
      auto &c = cases[y / case_rows];
      UINT n = y % case_rows * width + x, want = untouched;
      if (n < primitives) {
        want = 0;
        for (UINT corner = 0; corner < c.corners; corner++)
          for (UINT r = 0; r < registers; r++)
            for (UINT component = 0; component < 4; component++)
              want = want * multiplier + c.vertex(n, corner) * id_step + r * 4 + component;
        if (want == untouched) {
          printf("failed: primitive %u hashes to the clear value\n", n);
          return 1;
        }
      }
      auto value = reinterpret_cast<const UINT *>(pixels + row * y)[x];
      if (value != want && failures++ < 12)
        printf("%s, topology %u, primitive %u: %#x, want %#x\n", c.gs, (UINT)c.topology, n, value, want);
    }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %u primitives of %zu topologies, %u registers each vertex\n", primitives, std::size(cases), registers + 1);
  return 0;
}
