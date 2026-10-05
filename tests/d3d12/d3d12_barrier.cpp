// contract: a UAV barrier orders a draw's pixel-shader writes before a later draw's vertex-shader reads in the same
// render pass (D3D12_RESOURCE_UAV_BARRIER). on a tile-based GPU a pass shades every draw's vertices before any
// fragments, so without the ordering the second draw's vertices read the flag before the first draw wrote it. the same
// holds with enhanced barriers, on resources created in barrier layouts (ID3D12Device10, ID3D12GraphicsCommandList7).
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
// SM 5 numbers pixel-shader UAVs after the render targets
RWStructuredBuffer<uint> flag : register(u1);
RWStructuredBuffer<uint> seen : register(u2);
float4 position(uint id) {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 vs_write(uint id : SV_VertexID) : SV_Position { return position(id); }
float4 ps_write(float4 p : SV_Position) : SV_Target {
  flag[0] = WRITTEN;
  return 0;
}
struct V { float4 p : SV_Position; nointerpolation uint flag : FLAG; };
V vs_read(uint id : SV_VertexID) {
  V v;
  v.p = position(id);
  v.flag = flag[0];
  return v;
}
float4 ps_read(V v) : SV_Target {
  seen[uint(v.p.y) * SIZE + uint(v.p.x)] = v.flag;
  return 0;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT size = 4, flag = 0x5a5a, pixels = size * size;
  std::vector<std::string> defines = {"WRITTEN=" + std::to_string(flag), "SIZE=" + std::to_string(size)};
  auto vs_write = compiler.compile(hlsl, "vs_write", "vs", defines),
       ps_write = compiler.compile(hlsl, "ps_write", "ps", defines),
       vs_read = compiler.compile(hlsl, "vs_read", "vs", defines),
       ps_read = compiler.compile(hlsl, "ps_read", "ps", defines);
  for (auto *code : {&vs_write, &ps_write, &vs_read, &ps_read})
    if (code->empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Descriptor.ShaderRegister = 1;
  params[1].Descriptor.ShaderRegister = 2;
  auto rs = root_signature(device.Get(), {2, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ComPtr<ID3D12PipelineState> pso[2];
  for (int draw = 0; draw < 2; draw++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(draw ? vs_read : vs_write);
    desc.PS = bytecode(draw ? ps_read : ps_write);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[draw])));
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options)));
  ComPtr<ID3D12Device10> device10;
  if (!options.EnhancedBarriersSupported || FAILED(device.As(&device10))) {
    printf("failed: enhanced barriers not supported\n");
    return 1;
  }
  unsigned mismatches = 0;
  for (bool enhanced : {false, true}) {
    // a resource created the legacy way, or in a barrier layout
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    auto create = [&](const D3D12_RESOURCE_DESC &desc, D3D12_RESOURCE_STATES state, D3D12_BARRIER_LAYOUT layout) {
      ComPtr<ID3D12Resource> res;
      D3D12_RESOURCE_DESC1 desc1{desc.Dimension, desc.Alignment, desc.Width,      desc.Height, desc.DepthOrArraySize,
                                 desc.MipLevels, desc.Format,    desc.SampleDesc, desc.Layout, desc.Flags};
      if (enhanced)
        device10->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &desc1, layout, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&res)
        );
      else
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
      return res;
    };
    D3D12_RESOURCE_DESC target_desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, size, size, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    auto target = create(target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    // the flag word, then what each pixel of the second draw saw
    const UINT64 bytes = 4 + pixels * 4;
    D3D12_RESOURCE_DESC uav_desc{
        D3D12_RESOURCE_DIMENSION_BUFFER,
        0,
        bytes,
        1,
        1,
        1,
        DXGI_FORMAT_UNKNOWN,
        {1, 0},
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    };
    auto uav = create(uav_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED);
    if (!target || !uav) {
      printf("failed: resources not created\n");
      return 1;
    }
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList7> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso[0].Get(), IID_PPV_ARGS(&list))
    );
    D3D12_VIEWPORT viewport{0, 0, size, size, 0, 1};
    D3D12_RECT scissor{0, 0, size, size};
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootUnorderedAccessView(0, uav->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(1, uav->GetGPUVirtualAddress() + 4);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    if (enhanced) {
      D3D12_BUFFER_BARRIER barrier{
          D3D12_BARRIER_SYNC_PIXEL_SHADING,
          D3D12_BARRIER_SYNC_VERTEX_SHADING,
          D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
          D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
          uav.Get(),
          0,
          UINT64_MAX
      };
      D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
      group.pBufferBarriers = &barrier;
      list->Barrier(1, &group);
    } else {
      D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
      barrier.UAV.pResource = uav.Get();
      list->ResourceBarrier(1, &barrier);
    }
    list->SetPipelineState(pso[1].Get());
    list->DrawInstanced(3, 1, 0, 0);
    if (!enhanced)
      transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), uav.Get());
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    UINT *out;
    CHECK(readback->Map(0, nullptr, (void **)&out));
    for (UINT i = 0; i < pixels; i++)
      if (out[1 + i] != flag && mismatches++ < 4)
        printf("%s barrier: pixel %u saw %#x, want %#x\n", enhanced ? "enhanced" : "legacy", i, out[1 + i], flag);
  }
  printf(
      "%s: %u of %u pixels saw the flag before it was written\n", mismatches ? "failed" : "passed", mismatches,
      2 * pixels
  );
  return mismatches != 0;
}
