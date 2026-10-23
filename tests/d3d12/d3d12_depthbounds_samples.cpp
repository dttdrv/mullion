// contract: the depth bounds test, by sample and as depth changes (D3D12 Depth Bounds Test spec: a sample passes when
// the depth buffer's stored value for it is inside [min, max]). a 4x depth buffer whose samples hold different depths
// keeps, of a draw tested against it, only the samples whose depth is in the range; a draw that writes depth between
// two tested draws changes what the second passes; and a new range does, with the depth unchanged. on a GPU without
// the test (Apple9 and before) the pixel shader tests what a kernel found before the draw's pass, which these cover.
#include "d3d12_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float depth; float value; };
// the whole viewport, at `depth`; each draw's viewport is the columns it covers
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}
float ps() : SV_Target { return value; }
// each sample's own depth: a tenth more for each sample
struct Out {
  float color : SV_Target;
  float depth : SV_Depth;
};
Out ps_samples(float4 position : SV_Position, uint sample : SV_SampleIndex) {
  Out o = {value, 0.1 * (sample + 1)};
  return o;
}
)hlsl";

// a pipeline stream entry: its type, then its data, at pointer alignment
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Data> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Data data;
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2)));
  if (!options2.DepthBoundsTestSupported) {
    printf("failed: no depth bounds test\n");
    return 1;
  }
  const UINT size = 16;
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_samples = compiler.compile(hlsl, "ps_samples", "ps");
  if (vs.empty() || ps.empty() || ps_samples.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {1, &param});

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
  } stream{};
  // a pipeline that writes depth always, or one that tests the bounds and writes nothing but color
  auto pipeline = [&](UINT samples, bool bounded, const std::string &shader, ComPtr<ID3D12PipelineState> &out) {
    stream.rs.data = rs.Get();
    stream.vs.data = bytecode(vs);
    stream.ps.data = bytecode(shader);
    stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    stream.raster.data.DepthClipEnable = TRUE;
    stream.depth.data = {};
    stream.depth.data.DepthEnable = TRUE;
    stream.depth.data.DepthWriteMask = bounded ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
    stream.depth.data.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    stream.depth.data.DepthBoundsTestEnable = bounded;
    stream.targets.data = {};
    stream.targets.data.NumRenderTargets = 1;
    stream.targets.data.RTFormats[0] = DXGI_FORMAT_R32_FLOAT;
    stream.depth_format.data = DXGI_FORMAT_D32_FLOAT;
    stream.samples.data = {samples, 0};
    stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
    return device->CreatePipelineState(&desc, IID_PPV_ARGS(&out));
  };
  ComPtr<ID3D12PipelineState> write_samples, tested_ms, write, tested;
  CHECK(pipeline(4, false, ps_samples, write_samples));
  CHECK(pipeline(4, true, ps, tested_ms));
  CHECK(pipeline(1, false, ps, write));
  CHECK(pipeline(1, true, ps, tested));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> targets[2], depths[2], resolved;
  for (UINT ms = 0; ms < 2; ms++) {
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        0,
        size,
        size,
        1,
        1,
        DXGI_FORMAT_R32_FLOAT,
        {ms ? 4u : 1u, 0},
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[ms])
    ));
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depths[ms])
    ));
  }
  {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}};
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)
    ));
  }
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtv[2], dsv[2];
  for (UINT ms = 0; ms < 2; ms++) {
    rtv[ms] = {
        rtvs->GetCPUDescriptorHandleForHeapStart().ptr +
        ms * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
    };
    dsv[ms] = {
        dsvs->GetCPUDescriptorHandleForHeapStart().ptr +
        ms * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
    };
    device->CreateRenderTargetView(targets[ms].Get(), nullptr, rtv[ms]);
    device->CreateDepthStencilView(depths[ms].Get(), nullptr, dsv[ms]);
  }

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList1> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  // a quad over columns [left, right) of the target, a fraction each
  auto draw = [&](ID3D12PipelineState *pso, float depth, float value, float left, float right) {
    list->SetPipelineState(pso);
    float constants[2] = {depth, value};
    list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    D3D12_VIEWPORT part{left * size, 0, (right - left) * size, (float)size, 0, 1};
    list->RSSetViewports(1, &part);
    list->DrawInstanced(3, 1, 0, 0);
  };
  auto begin = [&](UINT ms) {
    list->SetGraphicsRootSignature(rs.Get());
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv[ms], FALSE, &dsv[ms]);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv[ms], zero, 0, nullptr);
    list->ClearDepthStencilView(dsv[ms], D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
  };
  // the target's first row, resolved where it has samples
  auto finish = [&](UINT ms, float *out) -> HRESULT {
    ID3D12Resource *read = targets[ms].Get();
    if (ms) {
      transition(list.Get(), targets[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
      list->ResolveSubresource(resolved.Get(), 0, targets[1].Get(), 0, DXGI_FORMAT_R32_FLOAT);
      transition(list.Get(), targets[1].Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      read = resolved.Get();
    } else {
      transition(list.Get(), read, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R32_FLOAT, size, size, 1, (UINT)row}};
    D3D12_TEXTURE_COPY_LOCATION src{read, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(
        list.Get(), read, D3D12_RESOURCE_STATE_COPY_SOURCE,
        ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET
    );
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (FAILED(hr))
      return hr;
    float *mapped;
    if (FAILED(hr = readback->Map(0, nullptr, (void **)&mapped)))
      return hr;
    memcpy(out, mapped, size * sizeof(float));
    readback->Unmap(0, nullptr);
    allocator->Reset();
    return list->Reset(allocator.Get(), nullptr);
  };
  unsigned failures = 0;
  float got[size];
  auto expect_row = [&](const char *what, auto &&want) {
    for (UINT x = 0; x < size; x++)
      if (std::fabs(got[x] - want(x)) > 1e-4f && failures++ < 12)
        printf("%s: pixel %u is %g, want %g\n", what, x, got[x], want(x));
  };

  step("a 4x depth buffer of 0.1, 0.2, 0.3 and 0.4 by sample keeps the samples in [0.15, 0.35]");
  begin(1);
  draw(write_samples.Get(), 0.5f, 0, 0, 1);
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv[1], zero, 0, nullptr);
  list->OMSetDepthBounds(0.15f, 0.35f);
  draw(tested_ms.Get(), 0.5f, 1, 0, 1);
  CHECK(finish(1, got));
  expect_row("4x, two samples of four in the bounds", [](UINT) { return 0.5f; });

  step("a draw that writes depth between two tested draws changes what the second passes");
  begin(0);
  list->OMSetDepthBounds(0.4f, 0.6f);
  draw(tested.Get(), 0.5f, 1, 0, 1);
  draw(write.Get(), 0.9f, 3, 0, 0.5f);
  draw(tested.Get(), 0.5f, 2, 0, 1);
  CHECK(finish(0, got));
  // the left half's depth is 0.9 and out of the bounds: its pixels keep the depth draw's 3
  expect_row("the second tested draw after depth changed", [&](UINT x) { return x < size / 2 ? 3.0f : 2.0f; });

  step("a new range, the depth the same, changes what passes");
  begin(0);
  draw(write.Get(), 0.2f, 0, 0, 0.5f);
  list->OMSetDepthBounds(0.4f, 0.6f);
  draw(tested.Get(), 0.5f, 1, 0, 1);
  list->OMSetDepthBounds(0.1f, 0.3f);
  draw(tested.Get(), 0.5f, 2, 0, 1);
  CHECK(finish(0, got));
  // the left half's depth is 0.2, in the second range; the right half's 0.5, in the first
  expect_row("two ranges over the same depth", [&](UINT x) { return x < size / 2 ? 2.0f : 1.0f; });

  printf("%s: %u wrong values\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
