// contract: one draw reaches the layers its shaders name, in every kind of layered target, and what was drawn there
// is what a shader then reads from the layer. this is how an engine fills a shadow atlas' pages, a cube map's six
// faces and the slices of a volume in one pass each (Unreal's virtual shadow maps and Lumen's volumes, Unity's
// point light shadows and light probes):
// - SV_RenderTargetArrayIndex "specifies which render target array slice the primitive goes to" (D3D11.3 4.4.6,
//   and 10.4.x for the geometry shader that writes it); a vertex shader may write it where the device reports
//   VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, otherwise that half is named as not
//   run. instance i of a triangle that covers the target goes to layer i, and its pixels hold the layer and their
//   own place; half of every layer is then cleared by rectangle;
// - the target is a 2D array, the six faces of a cube (a 2D array of six, D3D12_RESOURCE_DESC), or a 3D texture,
//   whose render target view takes its depth slices as layers (D3D12_TEX3D_RTV: FirstWSlice, WSize);
// - a cube sampled along an axis gives the face of that axis: +x, -x, +y, -y, +z, -z are faces 0 to 5
//   (D3D12_TEXCUBE_SRV; D3D11.3 7.18.11), and a volume sampled linearly on the plane between two slices gives
//   their mean (D3D11.3 7.18.8 linear filtering, weights of one half);
// - a compute shader writes a volume through RWTexture3D, each texel its own place.
#include "d3d12_test.hpp"
#include <cmath>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; nointerpolation uint named : NAMED; uint layer : SV_RenderTargetArrayIndex; };
float4 corner(uint id) { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
V vs_names(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  V v;
  v.pos = corner(id), v.named = instance, v.layer = instance;
  return v;
}
struct P { float4 pos : SV_Position; nointerpolation uint named : NAMED; };
P vs_passes(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  P p;
  p.pos = corner(id), p.named = instance;
  return p;
}
[maxvertexcount(3)] void gs_names(triangle P p[3], inout TriangleStream<V> stream) {
  for (uint i = 0; i < 3; i++) {
    V v;
    v.pos = p[i].pos, v.named = p[i].named, v.layer = p[i].named;
    stream.Append(v);
  }
}
// the layer the pixel is in, and its place
uint4 ps_places(V v) : SV_Target { return uint4(v.named + 1, v.pos.x, v.pos.y, v.layer + 1); }
// a shade of the layer, for the targets that are sampled afterwards
float4 ps_shades(V v) : SV_Target { return (v.named + 1) / float(LAYERS + 1); }

TextureCube<float4> cube : register(t0);
Texture3D<float4> volume : register(t1);
SamplerState point_clamp : register(s0);
SamplerState linear_clamp : register(s1);
RWStructuredBuffer<float> sampled : register(u0);
RWTexture3D<uint4> written : register(u1);
[numthreads(1, 1, 1)] void cs_samples() {
  const float3 axes[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
  for (uint face = 0; face < 6; face++)
    sampled[face] = cube.SampleLevel(point_clamp, axes[face], 0).x;
  // the planes between slices z and z + 1
  for (uint z = 0; z + 1 < LAYERS; z++)
    sampled[6 + z] = volume.SampleLevel(linear_clamp, float3(0.5, 0.5, (z + 1.0) / LAYERS), 0).x;
}
[numthreads(4, 4, 4)] void cs_writes(uint3 id : SV_DispatchThreadID) { written[id] = uint4(id + 1, 9); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT side = 16, layers = 8, faces = 6;
  const std::vector<std::string> defines = {"LAYERS=" + std::to_string(layers)};
  auto vs_passes = compiler.compile(hlsl, "vs_passes", "vs", defines),
       gs_names = compiler.compile(hlsl, "gs_names", "gs", defines), ps_places = compiler.compile(hlsl, "ps_places", "ps", defines),
       ps_shades = compiler.compile(hlsl, "ps_shades", "ps", defines), cs_samples = compiler.compile(hlsl, "cs_samples", "cs", defines),
       cs_writes = compiler.compile(hlsl, "cs_writes", "cs", defines);
  if (vs_passes.empty() || gs_names.empty() || ps_places.empty() || ps_shades.empty() || cs_samples.empty() ||
      cs_writes.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  const bool from_vertex = options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;
  // a compiler may refuse a vertex shader that names a layer for a device that has none
  auto vs_names = from_vertex ? compiler.compile(hlsl, "vs_names", "vs", defines) : std::string();
  if (from_vertex && vs_names.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1}};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &ranges[0]}, params[1].DescriptorTable = {1, &ranges[1]};
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {
      {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP},
      {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP}};
  for (UINT i = 0; i < 2; i++)
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER, samplers[i].MaxLOD = D3D12_FLOAT32_MAX, samplers[i].ShaderRegister = i;
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params, 2, samplers});

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> rtv_heap, views;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto view = [&](UINT i) {
    auto handle = views->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += i * increment;
    return handle;
  };

  // a layered target: a 2D array of `count` slices, or a 3D texture `count` deep
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto target = [&](bool deep, UINT count, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{deep ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side,
                             (UINT16)count, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> texture;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    expect(hr == S_OK, "a %s of %u layers: %08lx", deep ? "3D texture" : "2D array", count, hr);
    return texture;
  };
  auto renders_to = [&](ID3D12Resource *texture, bool deep, UINT count, DXGI_FORMAT format) {
    D3D12_RENDER_TARGET_VIEW_DESC desc{format, deep ? D3D12_RTV_DIMENSION_TEXTURE3D : D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
    if (deep)
      desc.Texture3D = {0, 0, count};
    else
      desc.Texture2DArray = {0, 0, count, 0};
    device->CreateRenderTargetView(texture, &desc, rtv);
  };
  auto pipeline = [&](bool vertex_names, const std::string &ps, DXGI_FORMAT format) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(vertex_names ? vs_names : vs_passes), desc.PS = bytecode(ps);
    if (!vertex_names)
      desc.GS = bytecode(gs_names);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
    desc.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(hr == S_OK, "the pipeline: %08lx", hr);
    return pso;
  };
  // a triangle over the whole target for each layer, each to its own
  auto draws = [&](ID3D12PipelineState *pso, UINT count) {
    const float clear[4] = {};
    D3D12_VIEWPORT viewport{0, 0, (float)side, (float)side, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)side, (LONG)side};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(rs.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, count, 0, 0);
  };
  // every layer of a texture, one after another in a buffer: a 2D array's are its subresources, a 3D texture's are
  // the slices of its one subresource
  auto reads = [&](ID3D12Resource *texture, bool deep, UINT count, D3D12_RESOURCE_STATES state, auto each) {
    auto desc = texture->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(deep ? 1 : count);
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, footprints.size(), 0, footprints.data(), nullptr, nullptr, &bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list.Get(), texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < footprints.size(); i++) {
      D3D12_TEXTURE_COPY_LOCATION from{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = i}},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[i]}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (FAILED(submit(device.Get(), queue.Get(), list.Get())))
      return expect(false, "the list was not run");
    const char *out;
    if (FAILED(readback->Map(0, nullptr, (void **)&out)))
      return expect(false, "the layers could not be read");
    for (UINT layer = 0; layer < count; layer++) {
      auto &footprint = footprints[deep ? 0 : layer];
      const char *start = out + footprint.Offset + (deep ? layer * footprint.Footprint.RowPitch * side : 0);
      for (UINT y = 0; y < side; y++)
        for (UINT x = 0; x < side; x++)
          each(layer, x, y, (const UINT *)(start + y * footprint.Footprint.RowPitch) + 4 * x);
    }
    readback->Unmap(0, nullptr);
    return true;
  };

  struct Kind {
    const char *name;
    bool deep;
    UINT count;
  };
  const Kind kinds[] = {{"a 2D array", false, layers}, {"a cube's six faces", false, faces}, {"a 3D texture", true, layers}};
  bool vertex_ran = false;
  for (auto &kind : kinds)
    for (bool vertex_names : {false, true}) {
      if (vertex_names && !from_vertex)
        continue;
      vertex_ran |= vertex_names;
      step("%s, each layer named by the %s shader", kind.name, vertex_names ? "vertex" : "geometry");
      const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT;
      auto texture = target(kind.deep, kind.count, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
      auto pso = pipeline(vertex_names, ps_places, format);
      if (!texture || !pso)
        continue;
      renders_to(texture.Get(), kind.deep, kind.count, format);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      draws(pso.Get(), kind.count);
      // then the left half of every layer is cleared: a clear of a view clears what the view has, and a clear of
      // a rectangle is a pass of its own (ID3D12GraphicsCommandList::ClearRenderTargetView, pRects)
      const float erased[4] = {5, 6, 7, 8};
      const D3D12_RECT left{0, 0, (LONG)side / 2, (LONG)side};
      list->ClearRenderTargetView(rtv, erased, 1, &left);
      unsigned wrong = 0;
      reads(texture.Get(), kind.deep, kind.count, D3D12_RESOURCE_STATE_RENDER_TARGET, [&](UINT layer, UINT x, UINT y, const UINT *got) {
        const UINT want[4] = {x < side / 2 ? 5 : layer + 1, x < side / 2 ? 6 : x, x < side / 2 ? 7 : y, x < side / 2 ? 8 : layer + 1};
        if (memcmp(got, want, sizeof(want)) && wrong++ < 3)
          expect(false, "layer %u, pixel %u,%u holds %u %u %u %u, want %u %u %u %u", layer, x, y, got[0], got[1], got[2], got[3],
                 want[0], want[1], want[2], want[3]);
      });
      expect(wrong <= 3, "and %u more pixels", wrong - 3);
    }
  if (!vertex_ran)
    printf("not run, the device has no layers named by the vertex shader\n");

  step("a cube sampled along its axes and a volume sampled between its slices, after a draw into each");
  const DXGI_FORMAT shade = DXGI_FORMAT_R8G8B8A8_UNORM;
  auto cube = target(false, faces, shade, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET),
       volume = target(true, layers, shade, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET),
       written = target(true, layers, DXGI_FORMAT_R32G32B32A32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto shades = pipeline(false, ps_shades, shade);
  ComPtr<ID3D12PipelineState> samples, writes;
  D3D12_COMPUTE_PIPELINE_STATE_DESC samples_desc{rs.Get(), bytecode(cs_samples)}, writes_desc{rs.Get(), bytecode(cs_writes)};
  CHECK(device->CreateComputePipelineState(&samples_desc, IID_PPV_ARGS(&samples)));
  CHECK(device->CreateComputePipelineState(&writes_desc, IID_PPV_ARGS(&writes)));
  if (!cube || !volume || !written || !shades)
    return verdict();
  const UINT results = faces + layers - 1;
  auto sampled = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, results * sizeof(float), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, results * sizeof(float), D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_SHADER_RESOURCE_VIEW_DESC cube_view{shade, D3D12_SRV_DIMENSION_TEXTURECUBE, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING},
      volume_view{shade, D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  cube_view.TextureCube = {0, 1}, volume_view.Texture3D = {0, 1};
  D3D12_UNORDERED_ACCESS_VIEW_DESC written_view{DXGI_FORMAT_R32G32B32A32_UINT, D3D12_UAV_DIMENSION_TEXTURE3D};
  written_view.Texture3D = {0, 0, layers};
  device->CreateShaderResourceView(cube.Get(), &cube_view, view(0));
  device->CreateShaderResourceView(volume.Get(), &volume_view, view(1));
  device->CreateUnorderedAccessView(written.Get(), nullptr, &written_view, view(2));

  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), nullptr));
  renders_to(cube.Get(), false, faces, shade);
  draws(shades.Get(), faces);
  renders_to(volume.Get(), true, layers, shade);
  draws(shades.Get(), layers);
  transition(list.Get(), cube.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), volume.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  auto first = views->GetGPUDescriptorHandleForHeapStart(), third = first;
  third.ptr += 2 * increment;
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, first);
  list->SetComputeRootDescriptorTable(1, third);
  list->SetComputeRootUnorderedAccessView(2, sampled->GetGPUVirtualAddress());
  list->SetPipelineState(samples.Get());
  list->Dispatch(1, 1, 1);
  list->SetPipelineState(writes.Get());
  list->Dispatch(side / 4, side / 4, layers / 4);
  transition(list.Get(), sampled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), sampled.Get());
  unsigned wrong = 0;
  if (reads(written.Get(), true, layers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, [&](UINT z, UINT x, UINT y, const UINT *got) {
        if ((got[0] != x + 1 || got[1] != y + 1 || got[2] != z + 1 || got[3] != 9) && wrong++ < 3)
          expect(false, "the volume a compute shader wrote: texel %u,%u,%u holds %u %u %u %u, want %u %u %u 9", x, y, z, got[0],
                 got[1], got[2], got[3], x + 1, y + 1, z + 1);
      })) {
    expect(wrong <= 3, "and %u more texels", wrong - 3);
    const float *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    // what an 8-bit target keeps of a layer's shade (D3D11.3 3.2.3.6: float to unorm rounds to the nearest)
    auto shade_of = [&](UINT layer) { return std::round((layer + 1) * 255.0f / (layers + 1)) / 255.0f; };
    for (UINT face = 0; face < faces; face++)
      expect(std::abs(got[face] - shade_of(face)) <= 1e-6f, "the cube along axis %u gives %g, want face %u's %g", face, got[face], face,
             shade_of(face));
    // linear filtering has 8 bits of weight (D3D11.3 7.18.8): a half is exact, the mean is held to one step of them
    for (UINT z = 0; z + 1 < layers; z++) {
      float want = (shade_of(z) + shade_of(z + 1)) / 2;
      expect(std::abs(got[faces + z] - want) <= 1.0f / 256 / (layers + 1), "the volume between slices %u and %u gives %g, want %g", z,
             z + 1, got[faces + z], want);
    }
  }
  return verdict();
}
