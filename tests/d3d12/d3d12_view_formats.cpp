// contract: a draw into a render target view or over a depth stencil view whose format is not the pipeline's writes
// and tests as the view says. D3D12 asks that a pipeline's RTVFormats and DSVFormat match the views bound
// (Microsoft Learn, D3D12_GRAPHICS_PIPELINE_STATE_DESC) and leaves a mismatch undefined, so these expectations are
// not the specification's (docs/TESTING.md 5.1): they are what titles that do this get from vkd3d-proton, whose
// passes take their attachments' formats from the views and which makes a pipeline again for the depth stencil view
// bound ("We are basically forced to compile PSO late with proper DSV format. This is an application bug if it
// happens.", libs/vkd3d/state.c). what a view's format means is the specification's: an sRGB view stores the
// linear color it is given encoded (D3D11.3 3.2, its conversion of linear float to sRGB), and LESS passes a nearer
// depth, which the view then holds (D3D11.3 17, the output merger's depth test).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc
// https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/state.c
// a triangle over the target writes 0.5 to a typeless R8G8B8A8 texture through its UNORM and its sRGB view, with a
// pipeline of each format: the byte stored is 128 through the UNORM view and 188 through the sRGB one, whichever the
// pipeline names. then a D32_FLOAT pipeline that tests LESS and writes depth draws over views of three depth
// formats cleared to 0.5: red at 0.25 passes, green at 0.75 fails against the clear, and blue at 0.4 fails against
// what red wrote, so every pixel is red.
#include "d3d12_test.hpp"
#include <cstdlib>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { float4 color; float z; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), z, 1);
}
float4 ps() : SV_Target { return color; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();
  constexpr UINT size = 4;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  struct Constants {
    float color[4], z;
  };
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants = {0, 0, sizeof(Constants) / sizeof(UINT)};
  auto rs = root_signature(device.Get(), {1, &param});
  if (!expect(bool(rs), "root signature was not created"))
    return verdict();

  const DXGI_FORMAT encodings[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
  const DXGI_FORMAT depths[] = {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D16_UNORM};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.SampleDesc = {1, 0};
  // a pipeline of each encoding without depth, then the UNORM one with a depth test
  ComPtr<ID3D12PipelineState> pipelines[std::size(encodings)], tested;
  for (UINT i = 0; i < std::size(encodings); i++) {
    desc.RTVFormats[0] = encodings[i];
    CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[i])));
  }
  desc.RTVFormats[0] = encodings[0];
  desc.DSVFormat = depths[0];
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&tested)));

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      0,
      size,
      size,
      1,
      1,
      DXGI_FORMAT_R8G8B8A8_TYPELESS,
      {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  };
  ComPtr<ID3D12Resource> target, depth_textures[std::size(depths)];
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "readback buffer was not created"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, UINT(std::size(encodings))},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, UINT(std::size(depths))};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[std::size(encodings)], dsvs[std::size(depths)];
  for (UINT i = 0; i < std::size(encodings); i++) {
    rtvs[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtvs[i].ptr += i * device->GetDescriptorHandleIncrementSize(rtv_desc.Type);
    D3D12_RENDER_TARGET_VIEW_DESC view{encodings[i], D3D12_RTV_DIMENSION_TEXTURE2D};
    device->CreateRenderTargetView(target.Get(), &view, rtvs[i]);
  }
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  for (UINT i = 0; i < std::size(depths); i++) {
    texture_desc.Format = depths[i];
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
        IID_PPV_ARGS(&depth_textures[i])
    ));
    dsvs[i] = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    dsvs[i].ptr += i * device->GetDescriptorHandleIncrementSize(dsv_desc.Type);
    device->CreateDepthStencilView(depth_textures[i].Get(), nullptr, dsvs[i]);
  }

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // records the draws of `constants` with `pipeline` into `rtv` over `dsv`, runs them and maps the target's bytes
  auto draw = [&](ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                  const D3D12_CPU_DESCRIPTOR_HANDLE *dsv, std::initializer_list<Constants> constants,
                  const unsigned char **pixels) -> HRESULT {
    HRESULT hr;
    if (FAILED(hr = forget(readback.Get())) || FAILED(hr = allocator->Reset()) ||
        FAILED(hr = list->Reset(allocator.Get(), pipeline)))
      return hr;
    const float black[4] = {};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    if (dsv)
      list->ClearDepthStencilView(*dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, dsv);
    list->SetGraphicsRootSignature(rs.Get());
    D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (auto &values : constants) {
      list->SetGraphicsRoot32BitConstants(0, sizeof(values) / sizeof(UINT), &values, 0);
      list->DrawInstanced(3, 1, 0, 0);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    into.PlacedFootprint = footprint;
    list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())))
      return hr;
    return readback->Map(0, nullptr, (void **)pixels);
  };
  // every pixel's channels against `want`, a unit of the format either way
  auto check = [&](const unsigned char *pixels, const int want[4]) {
    for (UINT y = 0; y < size; y++)
      for (UINT x = 0; x < size; x++)
        for (UINT channel = 0; channel < 4; channel++) {
          int got = pixels[footprint.Offset + y * footprint.Footprint.RowPitch + x * 4 + channel];
          expect(
              std::abs(got - want[channel]) <= 1, "pixel %u,%u channel %u is %d, expected %d", x, y, channel, got,
              want[channel]
          );
        }
    readback->Unmap(0, nullptr);
  };

  // 0.5 as UNORM, and as sRGB: 1.055 * 0.5^(1/2.4) - 0.055 = 0.7354
  const int stored[std::size(encodings)][4] = {{128, 128, 128, 255}, {188, 188, 188, 255}};
  for (UINT pipeline = 0; pipeline < std::size(encodings); pipeline++)
    for (UINT view = 0; view < std::size(encodings); view++) {
      step("pipeline of format %u into a view of format %u", encodings[pipeline], encodings[view]);
      const unsigned char *pixels;
      CHECK(draw(pipelines[pipeline].Get(), rtvs[view], nullptr, {{{0.5f, 0.5f, 0.5f, 1}, 0.5f}}, &pixels));
      check(pixels, stored[view]);
    }
  for (UINT view = 0; view < std::size(depths); view++) {
    step("pipeline of depth format %u over a view of format %u", depths[0], depths[view]);
    const unsigned char *pixels;
    CHECK(draw(
        tested.Get(), rtvs[0], &dsvs[view], {{{1, 0, 0, 1}, 0.25f}, {{0, 1, 0, 1}, 0.75f}, {{0, 0, 1, 1}, 0.4f}},
        &pixels
    ));
    const int red[4] = {255, 0, 0, 255};
    check(pixels, red);
  }
  return verdict();
}
