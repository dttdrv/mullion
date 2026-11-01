// contract: a sample outside the depth bounds fails the test, and "cannot affect the render target, depth, or stencil
// buffer" (D3D12 Depth Bounds Test spec); an occlusion query counts none of them, as d3d12_depthbounds checks, however
// the query and the draws interleave. a pipeline with the test and a pixel shader forced early draws over a depth
// buffer holding 0.9 with bounds [0, 0.5]: a query begun after a draw of the same pipeline in the same pass, a binary
// one begun so, and a query begun again after one ended all count 0, and bounds of [0, 1] count every sample. GPUs
// before Apple10 have no depth bounds test, and Mullion's pixel shader discards the samples outside, which a query
// counts unless the depth and stencil tests run after it. Reset returns a list to its initial state (Microsoft Learn,
// ID3D12GraphicsCommandList::Reset): a recording after one that tested the bounds counts every sample of [0, 1].
// https://microsoft.github.io/DirectX-Specs/d3d/DepthBoundsTest.html
#include "d3d12_test.hpp"

// a pipeline stream entry: its type, then its data, at pointer alignment
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Data> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Data data;
};

static const char hlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.25, 1);
}
[earlydepthstencil] float4 ps() : SV_Target { return 1; }
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)));
  if (!options.DepthBoundsTestSupported) {
    printf("skipped: no depth bounds test\n");
    return 77;
  }
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList1> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  auto rs = root_signature(device.Get(), {});
  if (!expect(bool(rs), "the root signature is refused"))
    return verdict();

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
  } stream{};
  stream.rs.data = rs.Get(), stream.vs.data = bytecode(vs), stream.ps.data = bytecode(ps);
  stream.depth.data = {TRUE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS};
  stream.depth.data.DepthBoundsTestEnable = TRUE;
  stream.depth_format.data = DXGI_FORMAT_D32_FLOAT;
  stream.targets.data = {{DXGI_FORMAT_R8G8B8A8_UNORM}, 1};
  stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  stream.rasterizer.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.rasterizer.data.DepthClipEnable = TRUE;
  D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
  ComPtr<ID3D12PipelineState> pipeline, plain;
  CHECK(device->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
  stream.depth.data.DepthBoundsTestEnable = FALSE;
  CHECK(device->CreatePipelineState(&desc, IID_PPV_ARGS(&plain)));

  constexpr UINT size = 4;
  const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1,
                                        DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  auto depth_desc = target_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT, depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  ComPtr<ID3D12Resource> target, depth;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        nullptr, IID_PPV_ARGS(&depth)));
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

  constexpr UINT queries = 4;
  ComPtr<ID3D12QueryHeap> occlusion;
  const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, queries};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&occlusion)));
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, queries * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "the readback buffer is refused"))
    return verdict();

  auto start = [&](float low, float high) {
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    const D3D12_RECT scissor{0, 0, size, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetDepthBounds(low, high);
  };
  auto query = [&](D3D12_QUERY_TYPE type, UINT index) {
    list->BeginQuery(occlusion.Get(), type, index);
    list->DrawInstanced(3, 1, 0, 0);
    list->EndQuery(occlusion.Get(), type, index);
  };

  // the depth buffer holds 0.9
  step("queries begun in a pass after a draw");
  CHECK(forget(readback.Get()));
  const float zero[4] = {};
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.9f, 0, 0, nullptr);
  start(0, 0.5f);
  // each query begins in a pass that drew with the pipeline before
  list->DrawInstanced(3, 1, 0, 0);
  query(D3D12_QUERY_TYPE_OCCLUSION, 0);
  list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
  start(0, 0.5f);
  list->DrawInstanced(3, 1, 0, 0);
  query(D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
  query(D3D12_QUERY_TYPE_OCCLUSION, 2);
  list->OMSetDepthBounds(0, 1);
  query(D3D12_QUERY_TYPE_OCCLUSION, 3);
  list->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, readback.Get(), 0);
  list->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1, 1, readback.Get(), sizeof(UINT64));
  list->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 2, 2, readback.Get(), 2 * sizeof(UINT64));
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT64 *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  const UINT64 want[queries] = {0, 0, 0, size * size};
  const char *names[queries] = {"begun after a draw in the pass", "binary, begun after a draw in the pass",
                                "begun again after one ended", "bounds of [0, 1]"};
  for (UINT i = 0; i < queries; i++)
    expect(got[i] == want[i], "query %u (%s) counts %llu, want %llu", i, names[i], got[i], want[i]);
  readback->Unmap(0, nullptr);

  // a recording that finds the samples in [0, 0.5], none, then one reset after it that tests [0, 1], the default, in a
  // pass of its own opened by a draw without the test. the padding before that pass, copies of two sizes, moves the
  // pass through the addresses the allocator gives, among them the one the first recording's pass had (17 buffer
  // copies when this was written), where the bug skips finding the samples and binds the first recording's. the API
  // shows no address, so nothing here tells whether a padding reached it: if the passes' or commands' sizes change so
  // that none does, this part passes without testing the bug, and the range of paddings needs widening
  auto scratch = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 64 * 1024, D3D12_RESOURCE_STATE_COPY_DEST),
       source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(scratch && source, "the padding buffers are refused"))
    return verdict();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{
      0, {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}
  };
  UINT wrong = 0;
  for (UINT copies = 0; copies < 48; copies++)
    for (UINT texels = 0; texels < 4; texels++) {
      step("a reset after a recording that tested the bounds, %u buffer and %u texel copies before the pass", copies,
           texels);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.9f, 0, 0, nullptr);
      start(0, 0.5f);
      list->DrawInstanced(3, 1, 0, 0);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));

      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      for (UINT i = 0; i < copies; i++)
        list->CopyBufferRegion(scratch.Get(), i * 512, source.Get(), 0, 4);
      for (UINT i = 0; i < texels; i++) {
        D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            dst{scratch.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprint;
        dst.PlacedFootprint.Offset = 32768 + i * 1024;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      start(0, 1);
      list->SetPipelineState(plain.Get());
      list->DrawInstanced(3, 1, 0, 0);
      list->SetPipelineState(pipeline.Get());
      list->BeginQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      list->DrawInstanced(3, 1, 0, 0);
      list->EndQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      list->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, readback.Get(), 0);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, (void **)&got));
      if (got[0] != size * size && !wrong++)
        printf("%u buffer and %u texel copies: the query counts %llu, want %u\n", copies, texels, got[0], size * size);
      readback->Unmap(0, nullptr);
    }
  expect(!wrong, "%u wrong counts after a reset", wrong);
  return verdict();
}
