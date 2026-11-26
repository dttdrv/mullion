// contract: with d3d12.ignoreFailedPipelines (mullion.conf), a pipeline Mullion cannot make is created anyway, as one
// whose draws and dispatches are dropped, and the work recorded around it runs as it would without it. a shader with
// a double-precision operation is one the converter refuses (the device has no DoublePrecisionFloatShaderOps), so a
// graphics and a compute pipeline of one are made: one draw of the graphics pipeline writes nothing to its pixel
// while an ordinary draw writes the other, and the compute pipeline's dispatch leaves its word as an ordinary
// dispatch after it writes the next. without the option the two creations fail (CODE.md 2.5).
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> words : register(u0);
cbuffer C : register(b0) { uint value; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_ok(float4 p : SV_Position) : SV_Target { return 1; }
// a double made from bits the shader reads (1.0, as the buffer starts zeroed), which the compiler cannot fold away
float4 ps_double(float4 p : SV_Position) : SV_Target {
  double d = asdouble(words[2], 0x3ff00000) * double(p.x);
  return float4(float(d), 0.5, 0.5, 1);
}
[numthreads(1, 1, 1)] void cs_ok() { words[1] = 7; }
[numthreads(1, 1, 1)] void cs_double() {
  double d = asdouble(words[2], 0x3ff00000) * double(value);
  words[0] = uint(d);
}
)hlsl";

int
main(int argc, char **argv) {
  SetEnvironmentVariableA("MULLION_CONFIG", "d3d12.ignoreFailedPipelines=True");
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps_ok = compiler.compile(hlsl, "ps_ok", "ps"),
       ps_double = compiler.compile(hlsl, "ps_double", "ps"), cs_ok = compiler.compile(hlsl, "cs_ok", "cs"),
       cs_double = compiler.compile(hlsl, "cs_double", "cs");
  for (auto *code : {&vs, &ps_ok, &ps_double, &cs_ok, &cs_double})
    if (code->empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {2, params});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }

  step("the pipelines");
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ComPtr<ID3D12PipelineState> draw_ok, draw_double, dispatch_ok, dispatch_double;
  for (bool doubled : {false, true}) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vs);
    desc.PS = bytecode(doubled ? ps_double : ps_ok);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(doubled ? &draw_double : &draw_ok));
    expect(SUCCEEDED(hr), "the graphics pipeline%s was refused: %#x", doubled ? " of a double" : "", unsigned(hr));
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(doubled ? cs_double : cs_ok)};
    hr = device->CreateComputePipelineState(&compute, IID_PPV_ARGS(doubled ? &dispatch_double : &dispatch_ok));
    expect(SUCCEEDED(hr), "the compute pipeline%s was refused: %#x", doubled ? " of a double" : "", unsigned(hr));
  }
  if (!draw_ok || !draw_double || !dispatch_ok || !dispatch_double)
    return verdict();

  step("the draws and dispatches");
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 1, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  auto words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto pixels_read = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
  auto words_read = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!words || !pixels_read || !words_read) {
    printf("failed: buffers\n");
    return 1;
  }
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  const float clear[4]{};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  list->SetGraphicsRootUnorderedAccessView(0, words->GetGPUVirtualAddress());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_RECT scissor{0, 0, 2, 1};
  list->RSSetScissorRects(1, &scissor);
  for (UINT pixel = 0; pixel < 2; pixel++) {
    D3D12_VIEWPORT viewport{float(pixel), 0, 1, 1, 0, 1};
    list->RSSetViewports(1, &viewport);
    list->SetPipelineState(pixel ? draw_ok.Get() : draw_double.Get());
    list->DrawInstanced(3, 1, 0, 0);
  }
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, words->GetGPUVirtualAddress());
  list->SetComputeRoot32BitConstant(1, 5, 0);
  list->SetPipelineState(dispatch_double.Get());
  list->Dispatch(1, 1, 1);
  list->SetPipelineState(dispatch_ok.Get());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), words.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION to{pixels_read.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint.Footprint = {format, 2, 1, 1, 256};
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  list->CopyBufferRegion(words_read.Get(), 0, words.Get(), 0, 256);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  uint32_t *pixels, *out;
  CHECK(pixels_read->Map(0, nullptr, (void **)&pixels));
  CHECK(words_read->Map(0, nullptr, (void **)&out));
  expect(pixels[0] == 0, "the draw of the pipeline that could not be made wrote %#x", pixels[0]);
  expect(pixels[1] == 0xffffffff, "the ordinary draw after it wrote %#x, not white", pixels[1]);
  expect(out[0] == 0, "the dispatch of the pipeline that could not be made wrote %u", out[0]);
  expect(out[1] == 7, "the ordinary dispatch after it wrote %u, not 7", out[1]);
  return verdict();
}
