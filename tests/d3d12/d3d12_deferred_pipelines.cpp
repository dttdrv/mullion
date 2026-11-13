// contract: a pipeline draws as soon as it is created, whatever is still compiling. A D3D12 pipeline is ready once
// CreateGraphicsPipelineState returns (ID3D12Device::CreateGraphicsPipelineState); Mullion leaves Metal's compile of
// it to compile threads (DeferredPipeline), and a draw of it waits for that compile or does it itself.
// - pipelines created on several threads at once, each drawn at once in one command list, draw what they write;
// - pipelines released before their compile has run take nothing down with them, and later pipelines still draw;
// - a pipeline created again from the same description, while the first is still compiling, draws too.
// the target is one row with a column per pipeline: pipeline n writes its own value into column n.
#include "d3d12_test.hpp"
#include <algorithm>
#include <thread>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint column; };
float4 cover(uint id : SV_VertexID) : SV_Position {
  // a triangle over the column's pixel, which is the middle of its pixel at this column
  float2 uv = float2((id << 1) & 2, id & 2);
  float x = (column + 0.5) * 2 / WIDTH - 1;
  return float4(x + (uv.x - 0.5) * 4 / WIDTH, uv.y * -2 + 1, 0, 1);
}
uint ps() : SV_Target { return VALUE; }
)hlsl";

int
main(int argc, char **argv) {
  // no compile threads (DeferredPipeline): each pipeline is made by its draw, or never when it is released first,
  // whatever the threads would have finished by the time the list is recorded
  SetEnvironmentVariableA("MULLION_D3D12_DEFER_PIPELINES", "draw");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  const UINT drawn = 48, released = 48, threads = 4;
  const UINT width = drawn + 1;
  const std::vector<std::string> width_define = {"WIDTH=" + std::to_string(width), "VALUE=0"};
  auto cover = compiler.compile(hlsl, "cover", "vs", width_define);
  // each pipeline a pixel shader of its own, so that each is a Metal pipeline of its own to compile
  std::vector<std::string> pixel_shaders(drawn + released);
  for (UINT i = 0; i < drawn + released; i++)
    pixel_shaders[i] =
        compiler.compile(hlsl, "ps", "ps", {"WIDTH=" + std::to_string(width), "VALUE=" + std::to_string(1000 + i)});
  if (cover.empty() || std::any_of(pixel_shaders.begin(), pixel_shaders.end(), [](auto &s) { return s.empty(); })) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 1;
  auto rs = root_signature(device.Get(), {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(cover);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};

  // created on several threads at once; the ones to release are released straight away
  std::vector<ComPtr<ID3D12PipelineState>> pipelines(drawn);
  std::vector<HRESULT> results(drawn + released, S_OK);
  std::vector<std::thread> creators;
  for (UINT t = 0; t < threads; t++)
    creators.emplace_back([&, t] {
      for (UINT i = t; i < drawn + released; i += threads) {
        auto own = desc;
        own.PS = bytecode(pixel_shaders[i]);
        ComPtr<ID3D12PipelineState> pipeline;
        results[i] = device->CreateGraphicsPipelineState(&own, IID_PPV_ARGS(&pipeline));
        if (i < drawn)
          pipelines[i] = pipeline;
      }
    });
  for (auto &creator : creators)
    creator.join();
  // the same description as pipeline 0's, created again while it may still be compiling, draws in the last column
  ComPtr<ID3D12PipelineState> again;
  {
    auto own = desc;
    own.PS = bytecode(pixel_shaders[0]);
    CHECK(device->CreateGraphicsPipelineState(&own, IID_PPV_ARGS(&again)));
  }
  unsigned failures = 0;
  for (UINT i = 0; i < drawn + released; i++)
    if (FAILED(results[i]) && failures++ < 12)
      printf("pipeline %u: created with %#lx\n", i, results[i]);

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  width,
                                  1,
                                  1,
                                  1,
                                  DXGI_FORMAT_R32_UINT,
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT row;
  UINT64 size;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &row, nullptr, nullptr, &size);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float zero[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)width, 1, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)width, 1};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT i = 0; i <= drawn; i++) {
    list->SetPipelineState(i < drawn ? pipelines[i].Get() : again.Get());
    list->SetGraphicsRoot32BitConstant(0, i, 0);
    // only its own column: the triangle covers its neighbours' pixels too
    D3D12_RECT column{(LONG)i, 0, LONG(i + 1), 1};
    list->RSSetScissorRects(1, &column);
    list->DrawInstanced(3, 1, 0, 0);
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  dst.PlacedFootprint = row;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  UINT *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT i = 0; i <= drawn; i++) {
    UINT want = i < drawn ? 1000 + i : 1000;
    if (got[i] != want && failures++ < 12)
      printf("column %u is %u, want %u\n", i, got[i], want);
  }
  readback->Unmap(0, nullptr);
  printf(
      "%s: %u wrong over %u pipelines drawn as created on %u threads, %u released at once\n",
      failures ? "failed" : "passed", failures, drawn + 1, threads, released
  );
  return failures != 0;
}
