// contract: XR_BIAS is a Texture2D in the R10G10B10A2_TYPELESS family, copied without changing its bits and rendered
// through UNORM or UINT views. "Scan-out hardware must be able to apply the bias and scale" (Microsoft Learn,
// DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM); XR_BIAS to Float Conversion Rules gives
// "return (float)( (XRComponent & 0x3ff) - 0x180 ) / 510.f;". alpha remains 2-bit UNORM (XR Format Alpha Content).
// https://learn.microsoft.com/en-us/windows-hardware/drivers/display/casting-ability-of-xr-formats
// https://learn.microsoft.com/en-us/windows-hardware/drivers/display/xr-bias-to-float-conversion-rules
// "The only supported swap effects are DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL and DXGI_SWAP_EFFECT_FLIP_DISCARD"
// (Direct3D 12 Swap Chains); DXGI_SWAP_CHAIN_DESC1's flip format list excludes XR. thus D3D12 XR swap chains must
// be refused, not presented. GetBuffer and a refused XR resize of a legal chain preserve its ordinary format.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
// every RGB code and both alpha bits are read back after rendering and copies;
// no D3D12 display result is claimed for a forbidden swap chain.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <cmath>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
  uint r = uint(pos.x) & COLOR_MAX;
  uint3 q = uint3(r, (r + BIAS) & COLOR_MAX, COLOR_MAX - r);
  float3 colour = (float3(q) - BIAS) / SCALE;
  return float4((colour * SCALE + BIAS) / COLOR_MAX, float(uint(pos.y) & ALPHA_MAX) / ALPHA_MAX);
}
uint4 ps_uint(float4 pos : SV_Position) : SV_Target {
  uint r = uint(pos.x) & COLOR_MAX;
  return uint4(r, (r + BIAS) & COLOR_MAX, COLOR_MAX - r, uint(pos.y) & ALPHA_MAX) ^
         uint4(COLOR_MAX, COLOR_MAX, COLOR_MAX, ALPHA_MAX);
}
Texture2D<float4> source : register(t0);
Texture2D<uint4> integer_source : register(t0);
SamplerState nearest : register(s0);
float4 ps_sample(float4 pos : SV_Position) : SV_Target {
  uint width, height; source.GetDimensions(width, height);
  return source.SampleLevel(nearest, pos.xy / float2(width, height), 0);
}
float4 ps_load(float4 pos : SV_Position) : SV_Target { return integer_source.Load(int3(uint2(pos.xy), 0)); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  constexpr UINT colour_bits = 10, alpha_bits = 2;
  constexpr UINT colour_max = (1u << colour_bits) - 1, alpha_max = (1u << alpha_bits) - 1, bias = 0x180;
  constexpr float scale = 510;
  const std::vector<std::string> defines = {"COLOR_MAX=" + std::to_string(colour_max),
                                            "ALPHA_MAX=" + std::to_string(alpha_max), "BIAS=" + std::to_string(bias),
                                            "SCALE=" + std::to_string(scale)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       ps_uint = compiler.compile(hlsl, "ps_uint", "ps", defines),
       ps_sample = compiler.compile(hlsl, "ps_sample", "ps", defines),
       ps_load = compiler.compile(hlsl, "ps_load", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty() && !ps_uint.empty() && !ps_sample.empty() && !ps_load.empty(),
              "HLSL did not compile"))
    return verdict();
  constexpr DXGI_FORMAT xr = DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_12_1};
  ComPtr<ID3D12Device> probe;
  CHECK(make_device(nullptr, levels[0], IID_PPV_ARGS(&probe)));
  D3D12_FEATURE_DATA_FEATURE_LEVELS maximum{ARRAYSIZE(levels), levels};
  CHECK(probe->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &maximum, sizeof(maximum)));
  probe.Reset();
  HWND window = CreateWindowA("static", "d3d12_xrbias", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              128, 128, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!expect(window != nullptr, "CreateWindow: %lu", GetLastError()))
    return verdict();
  for (auto level : levels) {
    if (level > maximum.MaxSupportedFeatureLevel) {
      printf("not exercised: feature level %#x exceeds device maximum %#x\n", level, maximum.MaxSupportedFeatureLevel);
      continue;
    }
    step("feature level %#x: XR support", level);
    ComPtr<ID3D12Device> device;
    CHECK(make_device(nullptr, level, IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{xr};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    const UINT required =
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_DISPLAY | D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
    expect((support.Support1 & required) == required && !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE),
           "XR storage and sampling support %#x", support.Support1);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    for (auto effect : {DXGI_SWAP_EFFECT_DISCARD, DXGI_SWAP_EFFECT_SEQUENTIAL, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                        DXGI_SWAP_EFFECT_FLIP_DISCARD})
      for (auto format : {xr, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_UNKNOWN,
                          DXGI_FORMAT_R8G8B8A8_UNORM})
        for (bool legacy : {true, false}) {
          step("effect %u format %u %s: D3D12 swap-chain format", effect, format,
               legacy ? "CreateSwapChain" : "CreateSwapChainForHwnd");
          DXGI_SWAP_CHAIN_DESC1 desc{
              7, 4, format, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_STRETCH, effect};
          ComPtr<IDXGISwapChain> chain;
          HRESULT hr;
          if (legacy) {
            DXGI_SWAP_CHAIN_DESC old{{desc.Width, desc.Height, {}, format},
                                     desc.SampleDesc,
                                     desc.BufferUsage,
                                     desc.BufferCount,
                                     window,
                                     TRUE,
                                     effect};
            hr = factory->CreateSwapChain(queue.Get(), &old, &chain);
          } else {
            ComPtr<IDXGISwapChain1> made;
            hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &made);
            if (made)
              CHECK(made.As(&chain));
          }
          const bool allowed = format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                               (effect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
          expect(hr == (allowed ? S_OK : DXGI_ERROR_INVALID_CALL) && bool(chain) == allowed,
                 "creation returned %#lx", hr);
        }
    DXGI_SWAP_CHAIN_DESC1 control{7,
                                  4,
                                  DXGI_FORMAT_R10G10B10A2_UNORM,
                                  FALSE,
                                  {1, 0},
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                  2,
                                  DXGI_SCALING_STRETCH,
                                  DXGI_SWAP_EFFECT_FLIP_DISCARD};
    ComPtr<IDXGISwapChain1> chain;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &control, nullptr, nullptr, &chain));
    ComPtr<IDXGISwapChain3> chain3;
    CHECK(chain.As(&chain3));
    for (auto refused : {xr, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
      expect(chain->ResizeBuffers(0, 9, 5, refused, 0) == DXGI_ERROR_INVALID_CALL,
             "flip resize accepted format %u", refused);
      expect(chain3->ResizeBuffers1(0, 9, 5, refused, 0, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
             "flip ResizeBuffers1 accepted format %u", refused);
    }
    DXGI_SWAP_CHAIN_DESC1 got{};
    CHECK(chain->GetDesc1(&got));
    expect(got.Format == control.Format && got.Width == control.Width && got.Height == control.Height,
           "refused resize changed the chain");
    ComPtr<ID3D12Resource> back;
    CHECK(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
    expect(back->GetDesc().Format == control.Format, "GetBuffer changed after refused resize");
    back.Reset();
    chain3.Reset();
    chain.Reset();

    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
    D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
    param.DescriptorTable = {1, &range};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    auto rs = root_signature(device.Get(), {1, &param, 1, &sampler});
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.Get();
    pd.VS = bytecode(vs);
    pd.PS = bytecode(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    pd.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso[2];
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso[0])));
    pd.PS = bytecode(ps_uint);
    pd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UINT;
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso[1])));
    ComPtr<ID3D12PipelineState> sample_pso[2];
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.PS = bytecode(ps_sample);
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&sample_pso[0])));
    pd.PS = bytecode(ps_load);
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&sample_pso[1])));
    D3D12_DESCRIPTOR_HEAP_DESC srv_hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    CHECK(device->CreateDescriptorHeap(&srv_hd, IID_PPV_ARGS(&srv_heap)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12DescriptorHeap> heap;
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    CHECK(list->Close());
    // flip buffers remain UNORM while an sRGB view encodes the linear clear (D3D11.3 3.2.3.8 and 19.1.3.4)
    for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
      auto desc = control;
      desc.Format = format;
      ComPtr<IDXGISwapChain1> control_chain;
      CHECK(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &control_chain));
      ComPtr<ID3D12Resource> control_back;
      CHECK(control_chain->GetBuffer(0, IID_PPV_ARGS(&control_back)));
      D3D12_RENDER_TARGET_VIEW_DESC view{format == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                          : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                        D3D12_RTV_DIMENSION_TEXTURE2D};
      auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(control_back.Get(), &view, rtv);
      auto td = control_back->GetDesc();
      expect(td.Format == format, "sRGB view changed flip buffer format");
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 size;
      device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &size);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      transition(list.Get(), control_back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float colour[] = {0.25f, 0.25f, 0.25f, 1};
      list->ClearRenderTargetView(rtv, colour, 0, nullptr);
      transition(list.Get(), control_back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = footprint;
      D3D12_TEXTURE_COPY_LOCATION src{control_back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      transition(list.Get(), control_back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      void *pixels;
      CHECK(readback->Map(0, nullptr, &pixels));
      UINT encoded = UINT(std::round((1.055f * std::pow(colour[0], 1.f / 2.4f) - 0.055f) * UINT8_MAX));
      for (UINT y = 0; y < td.Height; y++)
        for (UINT x = 0; x < td.Width; x++)
          for (UINT c = 0; c < 4; c++)
            expect(((const uint8_t *)pixels)[y * footprint.Footprint.RowPitch + x * 4 + c] ==
                       (c == 3 ? UINT8_MAX : encoded),
                   "sRGB flip clear differs at %u,%u channel %u", x, y, c);
      readback->Unmap(0, nullptr);
      CHECK(control_chain->Present(0, 0));
    }
    for (UINT width : {7u, colour_max + 2}) {
      const UINT height = alpha_max + 1;
      D3D12_RESOURCE_DESC td{
          D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, width, height, 1, 1, xr, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> texture;
      CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            nullptr, IID_PPV_ARGS(&texture)));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 size;
      device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &size);
      expect(footprint.Footprint.Format == xr && footprint.Footprint.Width == width &&
                 footprint.Footprint.RowPitch >= width * sizeof(uint32_t),
             "XR footprint differs");
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(bool(readback), "readback allocation failed"))
        return verdict();
      std::vector<uint32_t> words(width * height);
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          UINT r = x & colour_max;
          words[y * width + x] = r | (((r + bias) & colour_max) << colour_bits) |
                                 ((colour_max - r) << (2 * colour_bits)) | ((y & alpha_max) << (3 * colour_bits));
        }
      auto bytes_are = [&]() -> HRESULT {
        void *pixels;
        HRESULT hr = readback->Map(0, nullptr, &pixels);
        if (FAILED(hr))
          return hr;
        for (UINT y = 0; y < height; y++)
          for (UINT x = 0; x < width; x++) {
            auto value = ((const uint32_t *)((const char *)pixels + y * footprint.Footprint.RowPitch))[x];
            expect(value == words[y * width + x], "pixel %u,%u: %#x, expected %#x", x, y, value, words[y * width + x]);
          }
        readback->Unmap(0, nullptr);
        return S_OK;
      };
      D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = footprint;
      D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_RENDER_TARGET_VIEW_DESC view{DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RTV_DIMENSION_TEXTURE2D};
      auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
      for (UINT i = 0; i < ARRAYSIZE(pso); i++) {
        step("width %u: render through %s view", width, i ? "UINT" : "UNORM");
        if (i)
          for (auto &word : words)
            word = ~word;
        view.Format = i ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
        device->CreateRenderTargetView(texture.Get(), &view, rtv);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso[i].Get()));
        if (i)
          transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->SetGraphicsRootSignature(rs.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT area{0, 0, LONG(width), LONG(height)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &area);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(bytes_are());
      }
      for (UINT i = 0; i < ARRAYSIZE(sample_pso); i++) {
        step("width %u: shader read through %s view", width, i ? "UINT" : "UNORM");
        auto float_desc = td;
        float_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ComPtr<ID3D12Resource> sampled;
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &float_desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&sampled)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sample_footprint;
        UINT64 sample_size;
        device->GetCopyableFootprints(&float_desc, 0, 1, 0, &sample_footprint, nullptr, nullptr, &sample_size);
        auto sample_readback =
            buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sample_size, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = i ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
        view.Format = float_desc.Format;
        device->CreateRenderTargetView(sampled.Get(), &view, rtv);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), sample_pso[i].Get()));
        transition(
            list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
        list->SetGraphicsRootSignature(rs.Get());
        auto descriptors = srv_heap.Get();
        list->SetDescriptorHeaps(1, &descriptors);
        list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT area{0, 0, LONG(width), LONG(height)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &area);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), sampled.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(
            list.Get(), texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        D3D12_TEXTURE_COPY_LOCATION sample_dst{sample_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        sample_dst.PlacedFootprint = sample_footprint;
        D3D12_TEXTURE_COPY_LOCATION sample_src{sampled.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&sample_dst, 0, 0, 0, &sample_src, nullptr);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        void *pixels;
        CHECK(sample_readback->Map(0, nullptr, &pixels));
        for (UINT y = 0; y < height; y++)
          for (UINT x = 0; x < width; x++)
            for (UINT c = 0; c < 4; c++) {
              UINT mask = c == 3 ? alpha_max : colour_max;
              float wanted = float((words[y * width + x] >> (c * colour_bits)) & mask);
              if (!i)
                wanted /= mask;
              auto got = ((const float *)((const char *)pixels + y * sample_footprint.Footprint.RowPitch))[x * 4 + c];
              expect(std::fabs(got - wanted) <= 1.f / (colour_max * colour_max),
                     "typed sample %u,%u channel %u: %g, expected %g", x, y, c, got, wanted);
            }
        sample_readback->Unmap(0, nullptr);
      }
      view.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      device->CreateRenderTargetView(texture.Get(), &view, rtv);
      for (auto format :
           {xr, DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UINT}) {
        step("width %u: copies to and from format %u", width, format);
        auto copy_desc = td;
        copy_desc.Format = format;
        ComPtr<ID3D12Resource> copy;
        CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&copy)));
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        list->CopyResource(copy.Get(), texture.Get());
        transition(list.Get(), copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION copied{copy.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&dst, 0, 0, 0, &copied, nullptr);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(bytes_are());
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float empty[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(rtv, empty, 0, nullptr);
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(texture.Get(), copy.Get());
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(bytes_are());
      }
    }
  }
  DestroyWindow(window);
  return verdict();
}
