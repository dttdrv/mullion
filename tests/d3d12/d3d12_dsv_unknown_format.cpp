// contract: a pipeline whose DSVFormat is DXGI_FORMAT_UNKNOWN but that tests depth or stencil tests against the
// depth stencil view that is bound, with the depth its pixel shader outputs. D3D12 asks that the formats match and
// leaves a mismatch undefined, so these expectations are not the specification's (docs/TESTING.md 5.1): they are
// vkd3d-proton's, which keeps the bound view for titles that rely on it ("A workaround for SottR, which creates
// pipelines with DSV_UNKNOWN, but still insists on using a depth buffer", libs/vkd3d/state.c), and which Mullion takes
// as its own. a test of ALWAYS that writes nothing and no stencil test is off there, and a pipeline without tests
// leaves the view alone. a full-target triangle at z = 0.5 writes 1 to an R8G8B8A8_UNORM target cleared to 0, over a
// depth buffer cleared to `cleared`, with a pixel shader that outputs no depth or one that outputs 0.25 (SV_Depth);
// the target and the depth plane are read back. a draw with no view bound is defined, and draws. so is one with a
// forced sample count, no targets and no view (Target Independent Rasterization, D3D11.3 3.5.6.2: "Depth/Stencil
// Views must not be bound"), which counts its pixels through a UAV: a strip 0.3 pixels wide covers some of 4 samples
// in each pixel of the first column, and none of their centers.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc
// https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/state.c
#include "d3d12_test.hpp"

static const char hlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 ps() : SV_Target { return 1; }
float4 ps_depth(out float depth : SV_Depth) : SV_Target {
  depth = 0.25;
  return 1;
}
RWByteAddressBuffer pixels : register(u0);
void ps_count() { pixels.InterlockedAdd(0, 1); }
// x from 0 to 0.3 of the first pixel column of 4: it covers samples left of that column's center, not the center
float4 vs_strip(uint id : SV_VertexID) : SV_Position {
  float x = id == 1 || id == 3 || id == 4 ? -1 + 0.3 * 2 / 4 : -1;
  return float4(x, id == 2 || id == 4 || id == 5 ? -1 : 1, 0.5, 1);
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_depth = compiler.compile(hlsl, "ps_depth", "ps"), ps_count = compiler.compile(hlsl, "ps_count", "ps"),
       vs_strip = compiler.compile(hlsl, "vs_strip", "vs");
  if (!expect(!vs.empty() && !ps.empty() && !ps_depth.empty() && !ps_count.empty() && !vs_strip.empty(),
              "the shaders do not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_ROOT_PARAMETER uav{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &uav});
  if (!expect(bool(rs), "the root signature is refused"))
    return verdict();

  constexpr UINT size = 4;
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, depth_at = pitch * size;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * depth_at, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "the readback buffer is refused"))
    return verdict();

  const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                        D3D12_COMPARISON_FUNC_ALWAYS};
  const struct Case {
    const char *name;
    DXGI_FORMAT format;
    float cleared;
    BOOL depth;
    D3D12_COMPARISON_FUNC func;
    D3D12_DEPTH_WRITE_MASK write;
    BOOL stencil;
    D3D12_COMPARISON_FUNC stencil_func;
    bool drawn;
    float z;                   // the depth after the draw
    bool shader_depth = false; // the pixel shader outputs a depth of 0.25
    bool bound = true;         // the view is bound
  } cases[] = {
      {"LESS over 0", DXGI_FORMAT_D32_FLOAT, 0, TRUE, D3D12_COMPARISON_FUNC_LESS, D3D12_DEPTH_WRITE_MASK_ALL, FALSE,
       D3D12_COMPARISON_FUNC_ALWAYS, false, 0},
      {"LESS over 1, writing", DXGI_FORMAT_D32_FLOAT, 1, TRUE, D3D12_COMPARISON_FUNC_LESS, D3D12_DEPTH_WRITE_MASK_ALL,
       FALSE, D3D12_COMPARISON_FUNC_ALWAYS, true, 0.5f},
      {"GREATER over 1, no writes", DXGI_FORMAT_D32_FLOAT, 1, TRUE, D3D12_COMPARISON_FUNC_GREATER,
       D3D12_DEPTH_WRITE_MASK_ZERO, FALSE, D3D12_COMPARISON_FUNC_ALWAYS, false, 1},
      {"ALWAYS over 0, no writes", DXGI_FORMAT_D32_FLOAT, 0, TRUE, D3D12_COMPARISON_FUNC_ALWAYS,
       D3D12_DEPTH_WRITE_MASK_ZERO, FALSE, D3D12_COMPARISON_FUNC_ALWAYS, true, 0},
      {"no tests", DXGI_FORMAT_D32_FLOAT, 0, FALSE, D3D12_COMPARISON_FUNC_LESS, D3D12_DEPTH_WRITE_MASK_ALL, FALSE,
       D3D12_COMPARISON_FUNC_ALWAYS, true, 0},
      {"stencil NEVER", DXGI_FORMAT_D24_UNORM_S8_UINT, 1, FALSE, D3D12_COMPARISON_FUNC_LESS,
       D3D12_DEPTH_WRITE_MASK_ALL, TRUE, D3D12_COMPARISON_FUNC_NEVER, false, 1},
      {"LESS over 0 with stencil", DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0, TRUE, D3D12_COMPARISON_FUNC_LESS,
       D3D12_DEPTH_WRITE_MASK_ALL, TRUE, D3D12_COMPARISON_FUNC_ALWAYS, false, 0},
      {"LESS over 0.3, SV_Depth 0.25", DXGI_FORMAT_D32_FLOAT, 0.3f, TRUE, D3D12_COMPARISON_FUNC_LESS,
       D3D12_DEPTH_WRITE_MASK_ALL, FALSE, D3D12_COMPARISON_FUNC_ALWAYS, true, 0.25f, true},
      {"GREATER over 0.3, SV_Depth 0.25", DXGI_FORMAT_D32_FLOAT, 0.3f, TRUE, D3D12_COMPARISON_FUNC_GREATER,
       D3D12_DEPTH_WRITE_MASK_ALL, FALSE, D3D12_COMPARISON_FUNC_ALWAYS, false, 0.3f, true},
      {"LESS with no view bound", DXGI_FORMAT_D32_FLOAT, 0, TRUE, D3D12_COMPARISON_FUNC_LESS,
       D3D12_DEPTH_WRITE_MASK_ALL, FALSE, D3D12_COMPARISON_FUNC_ALWAYS, true, 0, false, false},
      {"LESS with no view bound, SV_Depth 0.25", DXGI_FORMAT_D32_FLOAT, 0, TRUE, D3D12_COMPARISON_FUNC_LESS,
       D3D12_DEPTH_WRITE_MASK_ALL, FALSE, D3D12_COMPARISON_FUNC_ALWAYS, true, 0, true, false},
  };
  for (auto &c : cases) {
    step("%s", c.name);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(c.shader_depth ? ps_depth : ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState = {c.depth, c.write, c.func, c.stencil, 0xff, 0xff, keep, keep};
    desc.DepthStencilState.FrontFace.StencilFunc = desc.DepthStencilState.BackFace.StencilFunc = c.stencil_func;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));

    const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1,
                                          DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto depth_desc = target_desc;
    depth_desc.Format = c.format, depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ComPtr<ID3D12Resource> target, depth;
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                          nullptr, IID_PPV_ARGS(&depth)));
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    CHECK(forget(readback.Get()));

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, c.cleared, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, c.bound ? &dsv : nullptr);
    const D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    const D3D12_RECT scissor{0, 0, size, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, pitch}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const bool read_depth = c.format != DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (read_depth) {
      src.pResource = depth.Get();
      dst.PlacedFootprint = {depth_at, {DXGI_FORMAT_R32_FLOAT, size, size, 1, pitch}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    BYTE *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    const UINT32 want = c.drawn ? 0xffffffffu : 0;
    UINT wrong = 0;
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++) {
        UINT32 texel;
        float z;
        memcpy(&texel, got + y * pitch + x * 4, 4);
        memcpy(&z, got + depth_at + y * pitch + x * 4, 4);
        if (texel != want && !wrong++)
          printf("texel %u,%u is %08x, want %08x\n", x, y, texel, want);
        if (read_depth && z != c.z && !wrong++)
          printf("depth %u,%u is %g, want %g\n", x, y, z, c.z);
      }
    readback->Unmap(0, nullptr);
    expect(!wrong, "%u wrong texels", wrong);
  }

  step("a forced sample count of 4, no targets and no view");
  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs_strip), bytecode(ps_count)};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ForcedSampleCount = 4;
    desc.DepthStencilState = {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    auto counter = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(counter && zeros, "the counter buffers are refused"))
      return verdict();
    void *mapped;
    CHECK(zeros->Map(0, nullptr, &mapped));
    memset(mapped, 0, 256);
    zeros->Unmap(0, nullptr);
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->CopyBufferRegion(counter.Get(), 0, zeros.Get(), 0, 256);
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    const D3D12_RECT scissor{0, 0, size, size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(6, 1, 0, 0);
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, counter.Get(), 0, 4);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT32 *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    // the strip covers 4 of the pattern's samples in each row's first pixel, and no pixel center
    expect(got[0] == size, "the pixel shader ran %u times, want %u", got[0], size);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
