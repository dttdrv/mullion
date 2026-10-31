// contract: the pipeline's SampleMask limits which samples a draw writes whether or not it has a pixel shader (D3D11.3
// 17.17: "SampleMask is always applied, regardless of whether Multisample rendertargets are bound or not"), depth
// included: the output merger updates only the samples left in the coverage. a depth-only draw at z = 0.5 with
// DepthFunc ALWAYS goes over a multisampled D32_FLOAT buffer cleared to 1, so the samples at 0.5 are those of the mask.
// the bit of a sample is implementation-defined, so each pixel counts its samples at 0.5, popcount(mask) of them. it
// runs without a pixel shader, with the depth bounds test on and no pixel shader (a range of [0, 1], which every
// sample passes), and with an empty pixel shader. each sample is read back through a Texture2DMS.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d12_test.hpp"
#include <bit>

// a pipeline stream entry: its type, then its data, at pointer alignment
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Data> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  Data data;
};

static const char hlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
void ps() {}
Texture2DMS<float> t : register(t0);
RWStructuredBuffer<float> o : register(u0);
[numthreads(SAMPLES, 1, 1)] void cs(uint3 pixel : SV_GroupID, uint sample : SV_GroupIndex) {
  o[(pixel.y * WIDTH + pixel.x) * SAMPLES + sample] = t.Load(int2(pixel.xy), sample);
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr UINT width = 4, height = 4;
  auto vs = compiler.compile(hlsl, "vs", "vs", {"SAMPLES=1", "WIDTH=1"}),
       ps = compiler.compile(hlsl, "ps", "ps", {"SAMPLES=1", "WIDTH=1"});
  if (!expect(!vs.empty() && !ps.empty(), "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList1> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "the root signature is refused"))
    return verdict();

  ComPtr<ID3D12DescriptorHeap> dsvs, srvs;
  const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1},
      srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs)));
  auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> depth_format;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> mask;
  } stream{};
  stream.rs.data = rs.Get(), stream.vs.data = bytecode(vs);
  stream.depth.data = {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_ALWAYS};
  stream.depth_format.data = DXGI_FORMAT_D32_FLOAT;
  stream.targets.data = {{}, 0};
  stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  stream.rasterizer.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.rasterizer.data.DepthClipEnable = TRUE;

  const UINT masks[] = {0x1, 0x5, 0x0, 0xfffffffe, UINT_MAX};
  enum Shader { none, bounds, empty };
  for (UINT samples : {2u, 4u}) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{DXGI_FORMAT_D32_FLOAT, samples};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support))) ||
        !support.NumQualityLevels) {
      printf("D32_FLOAT has no %u-sample depth buffers\n", samples);
      continue;
    }
    auto cs = compiler.compile(
        hlsl, "cs", "cs", {"SAMPLES=" + std::to_string(samples), "WIDTH=" + std::to_string(width)}
    );
    if (!expect(!cs.empty(), "the %u-sample read does not compile", samples))
      return verdict();
    D3D12_COMPUTE_PIPELINE_STATE_DESC read_desc{rs.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> read;
    CHECK(device->CreateComputePipelineState(&read_desc, IID_PPV_ARGS(&read)));
    const UINT64 bytes = UINT64(width) * height * samples * sizeof(float);
    for (Shader shader : {none, bounds, empty}) {
      if (shader == bounds && !options2.DepthBoundsTestSupported) {
        printf("no depth bounds test\n");
        continue;
      }
      for (UINT mask : masks) {
        const char *kind = shader == none     ? "no pixel shader"
                           : shader == bounds ? "depth bounds and no pixel shader"
                                              : "empty pixel shader";
        step("%u samples, %s, sample mask %#x", samples, kind, mask);
        stream.ps.data = shader == empty ? bytecode(ps) : D3D12_SHADER_BYTECODE{};
        stream.depth.data.DepthBoundsTestEnable = shader == bounds;
        stream.sample.data = {samples, 0};
        stream.mask.data = mask;
        D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> pipeline;
        CHECK(device->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline)));

        const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC depth_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
                                             DXGI_FORMAT_R32_TYPELESS, {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
        ComPtr<ID3D12Resource> depth;
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)));
        auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(result && readback, "the result buffers are refused"))
          return verdict();
        CHECK(forget(readback.Get()));
        D3D12_DEPTH_STENCIL_VIEW_DESC view{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2DMS};
        device->CreateDepthStencilView(depth.Get(), &view, dsv);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2DMS,
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
        device->CreateShaderResourceView(depth.Get(), &srv, srvs->GetCPUDescriptorHandleForHeapStart());

        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
        const D3D12_RECT scissor{0, 0, width, height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        if (shader == bounds)
          list->OMSetDepthBounds(0, 1);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetPipelineState(pipeline.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap *heaps[] = {srvs.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rs.Get());
        list->SetPipelineState(read.Get());
        list->SetComputeRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
        list->Dispatch(width, height, 1);
        transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, bytes);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));

        float *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        const UINT want = std::popcount(mask & ((1u << samples) - 1));
        UINT wrong = 0;
        for (UINT pixel = 0; pixel < width * height; pixel++) {
          UINT drawn = 0;
          for (UINT sample = 0; sample < samples; sample++) {
            float z = got[pixel * samples + sample];
            if (z != 0.5f && z != 1.0f && !wrong++)
              printf("pixel %u, sample %u holds %g\n", pixel, sample, z);
            drawn += z == 0.5f;
          }
          if (drawn != want && !wrong++)
            printf("pixel %u has %u samples at 0.5, want %u\n", pixel, drawn, want);
        }
        readback->Unmap(0, nullptr);
        expect(!wrong, "%u wrong pixels or samples", wrong);
      }
    }
  }
  return verdict();
}
