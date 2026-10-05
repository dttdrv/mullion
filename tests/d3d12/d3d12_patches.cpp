// contract: a draw of many triangle patches, each with control points of its own and factors of its own (D3D11.3 11:
// the tessellator cuts each patch at the factors its patch-constant function writes, and the domain shader runs with
// that patch's control points). every patch is a triangle in a cell of its own; a domain point is the control
// points weighted by its location, so whatever the factors, the patch's triangles cover its triangle exactly once and
// nothing else, and carry its cell's number. the target adds up what is drawn: 1 and the cell's number plus one
// inside each patch, 0 outside. pixels whose centre is on a patch's long edge are left out.
// the factors differ from edge to edge and patch to patch, over the whole range to MAX_FACTOR.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
struct CP { float2 p : P; uint cell : CELL; };
CP vs(CP v) { return v; }
struct PC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
float factor_of(uint n) { return 1 + (n * 7) % MAX_FACTOR; }
PC pc(InputPatch<CP, 3> ip) {
  PC o;
  uint c = ip[0].cell;
  o.edges[0] = factor_of(c), o.edges[1] = factor_of(c + 1), o.edges[2] = factor_of(c + 2);
  o.inside = factor_of(c + 3);
  return o;
}
[domain("tri")] [partitioning(PARTITIONING)] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("pc")] [maxtessfactor(MAX_FACTOR)]
CP hs(InputPatch<CP, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }
struct D { float4 pos : SV_Position; float cell : CELL; };
[domain("tri")]
D ds(PC pc, float3 at : SV_DomainLocation, const OutputPatch<CP, 3> p) {
  D o;
  o.pos = float4(p[0].p * at.x + p[1].p * at.y + p[2].p * at.z, 0, 1);
  o.cell = p[0].cell * at.x + p[1].cell * at.y + p[2].cell * at.z;
  return o;
}
float2 ps(D i) : SV_Target { return float2(1, i.cell + 1); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const UINT side = 16, across = 8, patches = across * across, size = side * across;
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32_FLOAT;

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
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

  // a patch's control points: its cell's top left, top right and bottom left corners; the indexed draw takes the
  // patches last to first
  struct Vertex {
    float x, y;
    UINT cell;
  };
  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 3 * patches * sizeof(Vertex), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 3 * patches * sizeof(UINT16), D3D12_RESOURCE_STATE_GENERIC_READ);
  Vertex *corners;
  UINT16 *order;
  CHECK(vertices->Map(0, nullptr, (void **)&corners));
  CHECK(indices->Map(0, nullptr, (void **)&order));
  auto clip = [&](UINT px, UINT py, UINT cell) { return Vertex{px * 2.0f / size - 1, 1 - py * 2.0f / size, cell}; };
  for (UINT cell = 0; cell < patches; cell++) {
    UINT x = cell % across * side, y = cell / across * side;
    corners[3 * cell] = clip(x, y, cell), corners[3 * cell + 1] = clip(x + side, y, cell), corners[3 * cell + 2] = clip(x, y + side, cell);
    for (UINT i = 0; i < 3; i++)
      order[3 * cell + i] = 3 * (patches - 1 - cell) + i;
  }
  D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), UINT(3 * patches * sizeof(Vertex)), sizeof(Vertex)};
  D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), UINT(3 * patches * sizeof(UINT16)), DXGI_FORMAT_R16_UINT};

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  unsigned failures = 0, draws = 0;
  for (const char *partitioning : {"integer", "fractional_odd", "fractional_even", "pow2"})
    for (UINT max_factor : {64u, 7u})
      for (bool indexed : {false, true}) {
        const std::vector<std::string> defines = {std::string("PARTITIONING=\"") + partitioning + "\"",
                                                  "MAX_FACTOR=" + std::to_string(max_factor)};
        auto vs = compiler.compile(hlsl, "vs", "vs", defines), hs = compiler.compile(hlsl, "hs", "hs", defines),
             ds = compiler.compile(hlsl, "ds", "ds", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
        if (vs.empty() || hs.empty() || ds.empty() || ps.empty()) {
          printf("failed: HLSL did not compile\n");
          return 1;
        }
        const D3D12_INPUT_ELEMENT_DESC elements[] = {{"P", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0}, {"CELL", 0, DXGI_FORMAT_R32_UINT, 0, 8}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rs.Get();
        desc.VS = bytecode(vs), desc.HS = bytecode(hs), desc.DS = bytecode(ds), desc.PS = bytecode(ps);
        desc.InputLayout = {elements, 2};
        // what is drawn adds up
        desc.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE,
                                           D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
        desc.SampleMask = ~0u;
        desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
        desc.SampleDesc = {1, 0};
        ComPtr<ID3D12PipelineState> pso;
        CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        const float clear[4] = {};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
        list->SetGraphicsRootSignature(rs.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        list->IASetVertexBuffers(0, 1, &vbv);
        list->IASetIndexBuffer(&ibv);
        if (indexed)
          list->DrawIndexedInstanced(3 * patches, 1, 0, 0, 0);
        else
          list->DrawInstanced(3 * patches, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));

        const char *pixels;
        CHECK(readback->Map(0, nullptr, (void **)&pixels));
        unsigned wrong = 0;
        for (UINT y = 0; y < size; y++)
          for (UINT x = 0; x < size; x++) {
            // a pixel's centre is half a pixel into its cell: inside the patch before the long edge, on it, or past it
            UINT in = x % side + y % side, cell = y / side * across + x / side;
            if (in == side - 1)
              continue;
            auto got = (const float *)(pixels + y * footprint.Footprint.RowPitch) + 2 * x;
            float covered = in < side - 1, number = covered ? cell + 1 : 0;
            if ((got[0] != covered || !(std::abs(got[1] - number) <= 1.0f / 64)) && wrong++ < 3)
              printf("%s, factors to %u%s: pixel %u,%u of cell %u is covered %g times with number %g, want %g and %g\n",
                     partitioning, max_factor, indexed ? ", indexed" : "", x, y, cell, got[0], got[1], covered, number);
          }
        readback->Unmap(0, nullptr);
        failures += wrong;
        draws++;
      }
  printf("%s: %u wrong pixels over %u draws of %u patches\n", failures ? "failed" : "passed", failures, draws, patches);
  return failures != 0;
}
