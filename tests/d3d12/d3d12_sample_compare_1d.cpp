// contract: SampleCmp and SampleCmpLevelZero on a Texture1D and a Texture1DArray compare the reference with the texel
// the coordinate selects, as on a 2D texture: sample_c and sample_c_lz take "Texture1D, Texture1DArray, Texture2D,
// Texture2DArray, TextureCube or TextureCubeArray" (D3D11.3 functional spec, 22.4.19 and 22.4.20), and a comparison
// passes, giving 1, when "the source data is less than the destination data" for D3D12_COMPARISON_FUNC_LESS (the
// reference is the source, the texel the destination). a pixel shader samples a texture of 4 texels with a point
// comparison sampler at each texel's center, slice 0's depths {0.1, 0.4, 0.6, 0.9} and slice 1's {0.7, 0.2, 0.8, 0.3},
// so each texel and slice gives a result of its own. the textures are views of an R32_TYPELESS depth texture, and
// R32_FLOAT views of an R32_FLOAT texture. each pixel starts at a value no comparison gives.
// the shader also reads the same views without a comparison, as a 1D view may be both compared and read: Load and
// Sample (a point sampler) of the Texture1D, Load and SampleLevel of slice 1 of the array give the texels themselves
// (22.4.10 ld, 22.4.15 sample), on the target's second row.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#22.4.19%20sample_c
#include "d3d12_test.hpp"

static constexpr UINT width = 4;

static const char hlsl[] = R"hlsl(
Texture1D<float> plain : register(t0);
Texture1DArray<float> layers : register(t1);
SamplerComparisonState less : register(s0);
SamplerState nearest : register(s1);
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
// row 0 the comparisons, row 1 the reads, all of them made on every row
float4 ps(float4 position : SV_Position) : SV_Target {
  float u = position.x / WIDTH;
  int x = int(position.x);
  float4 compared = float4(plain.SampleCmp(less, u, 0.5), plain.SampleCmpLevelZero(less, u, 0.5),
                           layers.SampleCmp(less, float2(u, 1), 0.5),
                           layers.SampleCmpLevelZero(less, float2(u, 0), 0.35));
  float4 read = float4(plain.Load(int2(x, 0)), plain.Sample(nearest, u), layers.Load(int3(x, 1, 0)),
                       layers.SampleLevel(nearest, float2(u, 1), 0));
  return position.y < 1 ? compared : read;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines = {"WIDTH=" + std::to_string(width)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();

  step("create the device and the pipeline");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  parameter.DescriptorTable = {1, &range};
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {sampler, sampler};
  samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  samplers[1].ShaderRegister = 1;
  auto rs = root_signature(device.Get(), {1, &parameter, 2, samplers});
  CHECK(rs ? S_OK : E_FAIL);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  HRESULT created = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  if (!expect(SUCCEEDED(created), "CreateGraphicsPipelineState returned %#lx", created))
    return verdict();

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 2, 1, 1,
                                  DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
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
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT target_footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &target_footprint, nullptr, nullptr, &target_bytes);

  const float depths[2][width] = {{0.1f, 0.4f, 0.6f, 0.9f}, {0.7f, 0.2f, 0.8f, 0.3f}};
  struct Kind {
    const char *name;
    DXGI_FORMAT format;
    D3D12_RESOURCE_FLAGS flags;
  };
  for (auto &kind : {Kind{"depth texture", DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL},
                     Kind{"R32_FLOAT texture", DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE}}) {
    step("%s: upload the depths", kind.name);
    D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE1D, 0, width, 1, 2, 1, kind.format, {1, 0},
                                     D3D12_TEXTURE_LAYOUT_UNKNOWN, kind.flags};
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
    ));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2];
    UINT64 bytes;
    device->GetCopyableFootprints(&texture_desc, 0, 2, 0, footprints, nullptr, nullptr, &bytes);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *mapped;
    CHECK(upload->Map(0, nullptr, (void **)&mapped));
    for (UINT s = 0; s < 2; s++)
      memcpy(mapped + footprints[s].Offset, depths[s], sizeof(depths[s]));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> views;
    D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
    auto handle = views->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE1D,
                                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Texture1D = {0, 1, 0};
    device->CreateShaderResourceView(texture.Get(), &srv, handle);
    handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
    srv.Texture1DArray = {0, 1, 0, 2, 0};
    device->CreateShaderResourceView(texture.Get(), &srv, handle);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(forget(readback.Get()));

    step("%s: draw", kind.name);
    CHECK(allocator->Reset());
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
    for (UINT s = 0; s < 2; s++) {
      D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
          to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      from.PlacedFootprint = footprints[s];
      to.SubresourceIndex = s;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const float clear[4] = {0.25f, 0.25f, 0.25f, 0.25f};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, width, 2, 0, 1};
    const D3D12_RECT scissor{0, 0, width, 2};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION placed{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        texture_location{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    placed.PlacedFootprint = target_footprint;
    list->CopyTextureRegion(&placed, 0, 0, 0, &texture_location, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    if (!expect(SUCCEEDED(hr), "%s: the draw failed: %#lx", kind.name, hr))
      return verdict();

    step("%s: read back", kind.name);
    float *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    const char *what[4] = {"Texture1D SampleCmp, reference 0.5", "Texture1D SampleCmpLevelZero, reference 0.5",
                           "Texture1DArray slice 1 SampleCmp, reference 0.5",
                           "Texture1DArray slice 0 SampleCmpLevelZero, reference 0.35"};
    for (UINT x = 0; x < width; x++) {
      const float want[4] = {0.5f < depths[0][x] ? 1.f : 0.f, 0.5f < depths[0][x] ? 1.f : 0.f,
                             0.5f < depths[1][x] ? 1.f : 0.f, 0.35f < depths[0][x] ? 1.f : 0.f};
      for (UINT c = 0; c < 4; c++)
        expect(got[4 * x + c] == want[c], "%s: %s at texel %u gives %g, want %g", kind.name, what[c], x,
               got[4 * x + c], want[c]);
    }
    const char *read[4] = {"Texture1D Load", "Texture1D Sample", "Texture1DArray slice 1 Load",
                           "Texture1DArray slice 1 SampleLevel"};
    const float *second = got + target_footprint.Footprint.RowPitch / sizeof(float);
    for (UINT x = 0; x < width; x++) {
      const float want[4] = {depths[0][x], depths[0][x], depths[1][x], depths[1][x]};
      for (UINT c = 0; c < 4; c++)
        expect(second[4 * x + c] == want[c], "%s: %s of the compared view at texel %u gives %g, want %g", kind.name,
               read[c], x, second[4 * x + c], want[c]);
    }
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
