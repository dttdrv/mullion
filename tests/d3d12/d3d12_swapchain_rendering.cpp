// contract: retained descriptors keep naming the same D3D12 back buffers across
// presents: "The order of buffers returned by GetBuffer never changes."
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains
// bundles inherit their caller's targets, viewport and root arguments; direct
// and indirect draws must write the same image. ExecuteIndirect permits bundles
// when "The command signature contains exactly one operation."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executebundle
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
// only exact UNORM endpoints are written. every buffer byte and local GDI frame
// pixel is checked, without assuming discarded contents survive Present.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint image_tag; uint buffer_index; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
  uint x = uint(position.x), y = uint(position.y);
  return float4((image_tag >> x) & 1, (image_tag >> y) & 1, (x + y + buffer_index) & 1, 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  const bool sm50 = argc > 1 && !strcmp(argv[1], "dxbc50");
  if (!sm50 && !front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto compile = [&](const char *entry) {
    if (!sm50)
      return compiler.compile(hlsl, entry, entry);
    ComPtr<ID3DBlob> code, errors;
    std::string profile = std::string(entry) + "_5_0";
    HRESULT hr =
        D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, entry, profile.c_str(), 0, 0, &code, &errors);
    if (FAILED(hr)) {
      if (errors)
        printf(
            "%s: %.*s\n", entry, int(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer())
        );
      else
        printf("%s: compiler HRESULT %#lx\n", entry, hr);
      return std::string{};
    }
    return kept(std::string(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize()), entry);
  };
  auto vs = compile("vs"), ps = compile("ps");
  if (vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  SetEnvironmentVariableA("MULLION_CONFIG", "dxgi.presentThroughGDI=True");
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants.Num32BitValues = 2;
  auto root = root_signature(device.Get(), {1, &parameter});
  if (!expect(root != nullptr, "root signature"))
    return verdict();
  constexpr UINT width = 17, height = 13, vertices = 3;
  constexpr DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{root.Get(), bytecode(vs), bytecode(ps)};
  pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline.SampleMask = ~0u;
  pipeline.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline.NumRenderTargets = 1;
  pipeline.RTVFormats[0] = format;
  pipeline.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  const D3D12_DRAW_ARGUMENTS arguments{vertices, 1, 0, 0};
  auto indirect = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(arguments), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(indirect != nullptr, "indirect arguments"))
    return verdict();
  void *mapped = nullptr;
  CHECK(indirect->Map(0, nullptr, &mapped));
  memcpy(mapped, &arguments, sizeof(arguments));
  indirect->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC operation{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(arguments), 1, &operation};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12GraphicsCommandList> bundles[2];
  for (UINT indirect_draw = 0; indirect_draw < ARRAYSIZE(bundles); indirect_draw++) {
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(), IID_PPV_ARGS(&bundles[indirect_draw])
    ));
    bundles[indirect_draw]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (indirect_draw)
      bundles[indirect_draw]->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
    else
      bundles[indirect_draw]->DrawInstanced(vertices, 1, 0, 0);
    CHECK(bundles[indirect_draw]->Close());
  }
  ComPtr<IDXGIFactory2> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  HWND window = CreateWindowA(
      "static", "d3d12_swapchain_rendering", WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr, nullptr,
      GetModuleHandleA(nullptr), nullptr
  );
  if (!expect(window != nullptr, "CreateWindow"))
    return verdict();
  UpdateWindow(window);
  const char *paths[] = {"direct", "bundle", "indirect", "bundle indirect"};
  UINT image_tag = 0;
  for (auto effect : {DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_DISCARD})
    for (UINT count = 2; count <= DXGI_MAX_SWAP_CHAIN_BUFFERS; count++) {
      DXGI_SWAP_CHAIN_DESC1 desc{
          width, height, format, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, count, DXGI_SCALING_STRETCH, effect
      };
      ComPtr<IDXGISwapChain1> first;
      CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &first));
      ComPtr<IDXGISwapChain3> chain;
      CHECK(first.As(&chain));
      ComPtr<ID3D12DescriptorHeap> views;
      D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, count};
      CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
      const SIZE_T stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      std::vector<ComPtr<ID3D12Resource>> back(count);
      for (UINT i = 0; i < count; i++) {
        CHECK(chain->GetBuffer(i, IID_PPV_ARGS(&back[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{views->GetCPUDescriptorHandleForHeapStart().ptr + i * stride};
        device->CreateRenderTargetView(back[i].Get(), nullptr, rtv);
        for (UINT j = 0; j < i; j++)
          expect(back[i] != back[j], "buffers %u and %u have the same resource", i, j);
      }
      auto resource_desc = back[0]->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      UINT64 size = 0;
      device->GetCopyableFootprints(&resource_desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(readback != nullptr, "readback"))
        return verdict();
      for (UINT path = 0; path < ARRAYSIZE(paths); path++)
        for (UINT frame = 0; frame <= count * 2; frame++) {
          step("effect %u count %u %s frame %u", effect, count, paths[path], frame);
          const UINT tag = ++image_tag;
          UINT index = chain->GetCurrentBackBufferIndex();
          if (!expect(index < count, "current index %u", index))
            return verdict();
          ComPtr<ID3D12Resource> again;
          CHECK(chain->GetBuffer(index, IID_PPV_ARGS(&again)));
          expect(again == back[index], "GetBuffer identity rotated");
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), pso.Get()));
          transition(list.Get(), back[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
          D3D12_CPU_DESCRIPTOR_HANDLE rtv{views->GetCPUDescriptorHandleForHeapStart().ptr + index * stride};
          const float clear[4] = {};
          list->ClearRenderTargetView(rtv, clear, 0, nullptr);
          list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
          list->SetGraphicsRootSignature(root.Get());
          const UINT constants[] = {tag, index};
          list->SetGraphicsRoot32BitConstants(0, ARRAYSIZE(constants), constants, 0);
          const D3D12_VIEWPORT viewport{0, 0, width, height, 0, 1};
          const D3D12_RECT scissor{0, 0, width, height};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          if (path & 1)
            list->ExecuteBundle(bundles[path / 2].Get());
          else if (path / 2)
            list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
          else
            list->DrawInstanced(vertices, 1, 0, 0);
          transition(
              list.Get(), back[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE
          );
          D3D12_TEXTURE_COPY_LOCATION source{back[index].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
              target{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
          list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
          transition(list.Get(), back[index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          const auto color = [&](UINT x, UINT y) {
            return RGB(((tag >> x) & 1) * 255, ((tag >> y) & 1) * 255, ((x + y + index) & 1) * 255);
          };
          BYTE *bytes = nullptr;
          CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&bytes)));
          UINT wrong = 0;
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++) {
              auto want = color(x, y);
              const BYTE expected[] = {GetRValue(want), GetGValue(want), GetBValue(want), 255};
              wrong +=
                  memcmp(bytes + y * footprint.Footprint.RowPitch + x * sizeof(expected), expected, sizeof(expected)) !=
                  0;
            }
          readback->Unmap(0, nullptr);
          expect(!wrong, "%u rendered pixels differ", wrong);
          CHECK(chain->Present(0, 0));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), nullptr));
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          HDC dc = GetDC(window);
          wrong = 0;
          for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++)
              wrong += !dc || GetPixel(dc, x, y) != color(x, y);
          if (dc)
            ReleaseDC(window, dc);
          expect(!wrong, "%u presented pixels differ", wrong);
          expect(chain->GetCurrentBackBufferIndex() == (index + 1) % count, "wrong next index");
        }
    }
  DestroyWindow(window);
  return verdict();
}
