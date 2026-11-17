// contract: a pixel shader's Sample, SampleBias and SampleCmp through an anisotropic sampler add the LOD bias to the
// LOD of the clamped footprint. D3D11.3 7.18.11: "if ( output.ratioOfAnisotropy > input.maxAniso )" then
// "lengthMinor = lengthMajor/output.ratioOfAnisotropy", else "lengthMinor = determinant/lengthMajor";
// "output.LOD = log2(lengthMinor)", then "biasedLOD = output.LOD + MipLODBias; biasedLOD = biasedLOD + srcLODBias;"
// and "clampedLOD = max(MinLOD,(min(MaxLOD, biasedLOD)))". mip m of each texture holds m, and anisotropic filters
// are linear between mips (7.18.10), so a sample reads its LOD; a comparison passes where the reference is below the
// texel (LESS), so two references half a mip on either side of the expected LOD tell it apart. footprints are
// axis-aligned, most of them powers of two, whose expected LODs are whole mips. 12x4 and 4x12 have a ratio of 3,
// just past the limit of MaxAnisotropy 4 that a MipLODBias of -1 halves, where Metal, which biases before it clamps,
// would read log2(3) and Direct3D reads 1; under MaxAnisotropy 1 their LOD is fractional, and a comparison reads the
// blend of what it compared in the two mips around it. MaxAnisotropy 1 and a bias of 0 are controls; footprints longer
// and shorter than MaxAnisotropy on both axes, a 2D and a 2D array texture, descriptor and static samplers, and
// biases of both signs vary what the conversion of each instruction depends on. the static samplers are listed out of
// register order after one of another stage, so each is told apart by its register. the textures are twice as wide
// as they are high, so a footprint's ratio in texels is not its ratio in texture coordinates.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

// the footprints in texels of the first mip per pixel along x and y, one 2x2 quad each
static const float footprints[][2] = {{16, 1}, {1, 16}, {4, 4}, {64, 4}, {2, 1}, {12, 4}, {4, 12}};
constexpr unsigned quads = std::size(footprints);

static const char hlsl[] = R"hlsl(
Texture2D<float> t : register(t0);
Texture2DArray<float> a : register(t1);
Texture2D<float> d : register(t2);
SamplerState s : register(s0);
SamplerComparisonState c : register(s1);
cbuffer C : register(b0) { float4 quad[QUADS]; float bias; };
struct O { float t : SV_Target0; float a : SV_Target1; float b : SV_Target2; float lo : SV_Target3; float hi : SV_Target4; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
O ps(float4 p : SV_Position) {
  // xy: the quad's footprint, zw: the comparison references
  float4 q = quad[uint(p.x) / 2];
  uint width, height, mips;
  t.GetDimensions(0, width, height, mips);
  float2 uv = p.xy * q.xy / float2(width, height);
  O o;
  o.t = t.Sample(s, uv);
  o.a = a.Sample(s, float3(uv, 1));
  o.b = t.SampleBias(s, uv, bias);
  o.lo = d.SampleCmp(c, uv, q.z);
  o.hi = d.SampleCmp(c, uv, q.w);
  return o;
}
)hlsl";

// D3D11.3 7.18.11 for an axis-aligned footprint, then the bias and the clamp to the mips there are
static float
lod(const float footprint[2], float anisotropy, float bias, unsigned mips) {
  float major = std::max(footprint[0], footprint[1]), determinant = footprint[0] * footprint[1];
  float ratio = major * major / determinant;
  float minor = ratio > anisotropy ? major / anisotropy : determinant / major;
  return std::clamp(std::log2(minor) + bias, 0.0f, float(mips - 1));
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile the sampling shaders");
  const std::string define = "QUADS=" + std::to_string(quads);
  auto vs = compiler.compile(hlsl, "vs", "vs", {define}), ps = compiler.compile(hlsl, "ps", "ps", {define});
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  // the largest footprint at the largest bias stays inside the mips
  constexpr UINT size = 256, mips = std::bit_width(size), slices = 2, targets = 5;
  constexpr float depth_scale = 16;
  constexpr float sample_bias = -1, biases[] = {-1, 0, 1};
  constexpr UINT anisotropies[] = {1, 4, D3D12_MAX_MAXANISOTROPY};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](DXGI_FORMAT format, UINT64 width, UINT height, UINT16 layers, UINT16 levels,
                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, layers, levels, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    return res;
  };
  auto color = texture(DXGI_FORMAT_R32_FLOAT, size, size / 2, slices, mips, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
  auto depth = texture(DXGI_FORMAT_R32_TYPELESS, size, size / 2, 1, mips, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
  auto target = texture(DXGI_FORMAT_R32_FLOAT, 2 * quads, 2, targets, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (!expect(color && depth && target, "textures were not created"))
    return verdict();

  auto color_desc = color->GetDesc(), target_desc = target->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp[mips * slices], target_fp[targets];
  UINT64 color_bytes, target_bytes;
  device->GetCopyableFootprints(&color_desc, 0, mips * slices, 0, color_fp, nullptr, nullptr, &color_bytes);
  device->GetCopyableFootprints(&target_desc, 0, targets, 0, target_fp, nullptr, nullptr, &target_bytes);
  constexpr UINT cb_stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  constexpr UINT draws = 2 * std::size(anisotropies) * std::size(biases);
  const UINT64 cb_offset = (color_bytes + cb_stride - 1) / cb_stride * cb_stride;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cb_offset + cb_stride * draws,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes * draws, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "buffers were not created"))
    return verdict();
  CHECK(forget(readback.Get()));
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT sub = 0; sub < mips * slices; sub++)
    for (UINT y = 0; y < color_fp[sub].Footprint.Height; y++)
      std::fill_n((float *)(mapped + color_fp[sub].Offset + y * color_fp[sub].Footprint.RowPitch),
                  color_fp[sub].Footprint.Width, float(sub % mips));

  ComPtr<ID3D12DescriptorHeap> views, samplers, rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, draws, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&samplers)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targets};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, mips};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsvs)));
  auto at = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{h->GetCPUDescriptorHandleForHeapStart().ptr +
                                       i * device->GetDescriptorHandleIncrementSize(type)};
  };
  const UINT sampler_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv.Texture2D.MipLevels = mips;
  device->CreateShaderResourceView(color.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
  device->CreateShaderResourceView(depth.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2));
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Texture2DArray = {0, mips, 0, slices};
  device->CreateShaderResourceView(color.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1));
  for (UINT i = 0; i < targets; i++) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{DXGI_FORMAT_R32_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
    rtv.Texture2DArray = {0, i, 1};
    device->CreateRenderTargetView(target.Get(), &rtv, at(rtvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, i));
  }

  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3}, {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2}};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].DescriptorTable = {1, &ranges[0]};
  params[2].DescriptorTable = {1, &ranges[1]};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.VS = bytecode(vs);
  pso_desc.PS = bytecode(ps);
  for (UINT i = 0; i < targets; i++) {
    pso_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.RTVFormats[i] = DXGI_FORMAT_R32_FLOAT;
  }
  pso_desc.SampleMask = ~0u;
  pso_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = targets;
  pso_desc.SampleDesc = {1, 0};

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  step("upload the mips and clear the depth mips");
  for (UINT sub = 0; sub < mips * slices; sub++) {
    D3D12_TEXTURE_COPY_LOCATION dst{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = color_fp[sub]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  for (UINT m = 0; m < mips; m++) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
    dsv.Texture2D.MipSlice = m;
    device->CreateDepthStencilView(depth.Get(), &dsv, at(dsvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, m));
    list->ClearDepthStencilView(at(dsvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, m), D3D12_CLEAR_FLAG_DEPTH,
                                m / depth_scale, 0, 0, nullptr);
  }
  transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  struct Draw {
    bool fixed;
    UINT anisotropy;
    float bias;
  } plan[draws];
  UINT n = 0;
  for (bool fixed : {false, true})
    for (UINT anisotropy : anisotropies)
      for (float bias : biases)
        plan[n++] = {fixed, anisotropy, bias};
  for (UINT i = 0; i < draws; i++) {
    auto &draw = plan[i];
    step("%s samplers, MaxAnisotropy %u, MipLODBias %g", draw.fixed ? "static" : "descriptor", draw.anisotropy,
         draw.bias);
    D3D12_SAMPLER_DESC desc{D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP, draw.bias, draw.anisotropy, D3D12_COMPARISON_FUNC_LESS};
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_STATIC_SAMPLER_DESC fixed[2];
    for (UINT k = 0; k < 2; k++) {
      desc.Filter = k ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
      fixed[k] = {desc.Filter, desc.AddressU, desc.AddressV, desc.AddressW, desc.MipLODBias, desc.MaxAnisotropy,
                  desc.ComparisonFunc, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, desc.MinLOD, desc.MaxLOD, k};
      fixed[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      if (!draw.fixed)
        device->CreateSampler(&desc, {samplers->GetCPUDescriptorHandleForHeapStart().ptr + (2 * i + k) * sampler_step});
    }
    // the static samplers out of register order, after one the shader does not take: a point sampler of a vertex
    // shader's register, whose state is not theirs
    D3D12_STATIC_SAMPLER_DESC listed[3] = {
        {D3D12_FILTER_MIN_MAG_MIP_POINT, desc.AddressU, desc.AddressV, desc.AddressW, 0, 1, desc.ComparisonFunc,
         D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0, D3D12_FLOAT32_MAX, 7, 0, D3D12_SHADER_VISIBILITY_VERTEX},
        fixed[1], fixed[0]};
    auto root = draw.fixed ? root_signature(device.Get(), {2, params, 3, listed})
                           : root_signature(device.Get(), {3, params});
    if (!expect(bool(root), "root signature was not created"))
      return verdict();
    pso_desc.pRootSignature = root.Get();
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

    // per quad its footprint and the references half a mip below and above the LOD a comparison expects
    float constants[quads * 4 + 1];
    for (UINT q = 0; q < quads; q++) {
      float want = lod(footprints[q], float(draw.anisotropy), draw.bias, mips);
      float values[] = {footprints[q][0], footprints[q][1], (want - 0.5f) / depth_scale, (want + 0.5f) / depth_scale};
      std::copy_n(values, 4, constants + 4 * q);
    }
    constants[quads * 4] = sample_bias;
    memcpy(mapped + cb_offset + cb_stride * i, constants, sizeof(constants));

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootSignature(root.Get());
    list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + cb_offset + cb_stride * i);
    list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
    if (!draw.fixed)
      list->SetGraphicsRootDescriptorTable(
          2, {samplers->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(2 * i) * sampler_step});
    D3D12_VIEWPORT viewport{0, 0, float(target_desc.Width), float(target_desc.Height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(target_desc.Width), LONG(target_desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    auto first = rtvs->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(targets, &first, TRUE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT k = 0; k < targets; k++) {
      auto fp = target_fp[k];
      fp.Offset += target_bytes * i;
      D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = k}},
          dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
  }
  upload->Unmap(0, nullptr);

  char *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  // D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT: the LOD's precision, and so a mip blend's
  const float tolerance = std::ldexp(1.0f, -D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT);
  const char *names[targets] = {"Sample", "Sample of an array", "SampleBias", "SampleCmp below", "SampleCmp above"};
  for (UINT i = 0; i < draws; i++)
    for (UINT q = 0; q < quads; q++) {
      auto &draw = plan[i];
      step("read %s samplers, MaxAnisotropy %u, MipLODBias %g, footprint %gx%g", draw.fixed ? "static" : "descriptor",
           draw.anisotropy, draw.bias, footprints[q][0], footprints[q][1]);
      float want = lod(footprints[q], float(draw.anisotropy), draw.bias, mips);
      // a comparison compares each texel, then filters what it compared (sample_c, 22.4.19): between two mips it
      // reads the blend of theirs
      auto compared = [&](float reference) {
        float low = std::floor(want), part = want - low;
        return (1 - part) * (low > reference) + part * (low + 1 > reference);
      };
      float wants[targets] = {want, want, lod(footprints[q], float(draw.anisotropy), draw.bias + sample_bias, mips),
                              compared(want - 0.5f), compared(want + 0.5f)};
      for (UINT k = 0; k < targets; k++)
        for (UINT y = 0; y < 2; y++)
          for (UINT x = 2 * q; x < 2 * q + 2; x++) {
            float value = *(float *)(got + target_bytes * i + target_fp[k].Offset + y * target_fp[k].Footprint.RowPitch +
                                     x * sizeof(float));
            expect(std::fabs(value - wants[k]) <= tolerance, "%s at pixel %u,%u read %g, expected %g", names[k], x, y,
                   value, wants[k]);
          }
    }
  readback->Unmap(0, nullptr);
  return verdict();
}
