// contract: see ../assembly.hpp, here through Direct3D 12, where a pipeline state names only the kind of primitive
// and each draw brings the topology: the same pipeline draws lists, strips and their forms with adjacency.
#include "d3d12_test.hpp"
#include "../assembly.hpp"

using namespace assembly;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, sizeof(Constants) / sizeof(uint32_t)};
  auto rs = root_signature(device.Get(), {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});

  // a pipeline per pass and kind of primitive
  const D3D12_PRIMITIVE_TOPOLOGY_TYPE types[] = {
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
  };
  ComPtr<ID3D12PipelineState> psos[passes][std::size(types)];
  const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 1, 0}};
  const UINT stride = sizeof(uint32_t), sentinel = 0xa5a5a5a5u;
  for (UINT pass = 0; pass < passes; pass++) {
    bool cull = pass & 1, with_stream = pass & 2;
    auto vs = compiler.compile(hlsl, "vs", "vs", defines(cull)), ps = compiler.compile(hlsl, "ps", "ps", defines(cull));
    if (vs.empty() || ps.empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
    if (with_stream)
      desc.StreamOutput = {entries, 1, &stride, 1, 0};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    for (UINT type = 0; type < std::size(types); type++) {
      desc.PrimitiveTopologyType = types[type];
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&psos[pass][type])));
    }
  }

  // both passes with stream output append
  std::vector<uint32_t> streamed;
  for (int pass = 0; pass < 2; pass++)
    stream(streamed);
  const UINT64 stream_bytes = stride * (streamed.size() + 1), filled_at = (stream_bytes + 7) & ~7ull, total = filled_at + 8,
               row = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  auto stream_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total, D3D12_RESOURCE_STATE_COPY_DEST);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  // the readback: the stream with its filled size, then the pixels, where a texture's rows may start
  const UINT64 pixels_at = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixels_at + row * height, D3D12_RESOURCE_STATE_COPY_DEST);
  uint8_t *init;
  CHECK(upload->Map(0, nullptr, (void **)&init));
  std::fill_n(reinterpret_cast<uint32_t *>(init), filled_at / 4, sentinel);
  memset(init + filled_at, 0, 8);
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
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
  list->CopyBufferRegion(stream_buffer.Get(), 0, upload.Get(), 0, total);
  transition(list.Get(), stream_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
  const float clear[4] = {};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  auto base = stream_buffer->GetGPUVirtualAddress();
  D3D12_STREAM_OUTPUT_BUFFER_VIEW view{base, stream_bytes, base + filled_at};
  list->SOSetTargets(0, 1, &view);
  for (UINT pass = 0; pass < passes; pass++) {
    for (UINT t = 0; t < case_count; t++) {
      auto &c = cases[t];
      auto values = constants(c, pass_rows * pass + rows * t);
      // a pipeline's kind of primitive is that of its topologies' own vertices
      list->SetPipelineState(psos[pass][c.corners - 1].Get());
      list->SetGraphicsRoot32BitConstants(0, sizeof(values) / sizeof(uint32_t), &values, 0);
      list->IASetPrimitiveTopology(c.topology);
      list->DrawInstanced(c.vertices, 1, 0, 0);
    }
    auto values = constants(turned, pass_rows * pass + turn_row);
    list->SetPipelineState(psos[pass][turned.corners - 1].Get());
    list->SetGraphicsRoot32BitConstants(0, sizeof(values) / sizeof(uint32_t), &values, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP);
    list->DrawInstanced(turn_vertices, 1, 0, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ);
    list->DrawInstanced(turned_vertices, 1, 0, 0);
  }
  transition(list.Get(), stream_buffer.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, stream_buffer.Get(), 0, total);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = {pixels_at, {DXGI_FORMAT_R32_UINT, width, height, 1, (UINT)row}};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));

  unsigned failures = 0;
  auto expect = [&](const char *what, UINT a, UINT b, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 16)
      printf("%s %u,%u: %llu, want %llu\n", what, a, b, (unsigned long long)value, (unsigned long long)want);
  };
  auto word = [&](UINT64 at) { return *reinterpret_cast<const uint32_t *>(got + at); };
  for (UINT y = 0; y < height; y++) {
    UINT pass = y / pass_rows, t = y % pass_rows / rows;
    bool turn = t == case_count;
    auto assembled = assemble(cases[turn ? 0 : t], pass & 1);
    for (UINT x = 0; x < width; x++)
      expect(
          "pixel", x, y, word(pixels_at + row * y + 4 * x),
          turn ? turned_pixel(pass & 1, x, y % rows) : pixel(cases[t], assembled, x, y % rows)
      );
  }
  for (size_t i = 0; i < streamed.size(); i++)
    expect("streamed vertex", (UINT)i, 0, word(stride * i), streamed[i]);
  expect("past the stream", (UINT)streamed.size(), 0, word(stride * streamed.size()), sentinel);
  expect("filled size", 0, 0, *reinterpret_cast<const uint64_t *>(got + filled_at), stride * streamed.size());
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %u topologies, %zu vertices streamed\n", case_count, streamed.size());
  return 0;
}
