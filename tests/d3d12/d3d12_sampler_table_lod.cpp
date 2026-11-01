// contract: a pixel shader's Sample, SampleBias, Sample with a clamp and SampleCmp through samplers of a descriptor
// table read D3D's LOD, lane by lane, whichever samplers a quad's lanes index. D3D11.3 7.18.11: "biasedLOD =
// output.LOD + MipLODBias; biasedLOD = biasedLOD + srcLODBias;" and "clampedLOD = max(MinLOD,(min(MaxLOD,
// biasedLOD)))", where an isotropic filter's LOD is the log2 of the footprint's major length and an anisotropic one's
// that of the clamped footprint's minor length; a view's or the shader's min-LOD clamp then raises it (5.8.2). mip m
// of each texture holds m, and filters are linear between mips (7.18.10), so a sample reads its LOD; a comparison
// passes where the reference is below the texel (LESS), so references half a mip on either side of the expected LOD
// tell it apart, and comparison samplers' LODs are whole. a quad's lanes index one sampler or several
// (NonUniformResourceIndex), anisotropic or not, with biases of both signs, a fractional one, and a MinLOD; each
// color sampler's comparison sampler is anisotropic where it is not, and the other way round. quads a triangle covers
// in part have helper lanes (16.8), which take part in the derivatives and whose samplers may be anisotropic or
// another bias's; what covered lanes read is unchanged, and uncovered pixels are not written.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

// per quad, a 2x2 block of pixels: its footprint in texels of the first mip along x and y, the sampler each lane
// indexes, lane (x & 1) + 2 * (y & 1), and the lanes its triangle covers, the first 4, 3, 2 or 1: the rest are
// helpers. quads all of whose lanes are covered come first
static const struct {
  float footprint[2];
  unsigned lanes[4], covered;
} quads[] = {
    {{16, 1}, {0, 0, 0, 0}, 4}, {{4, 4}, {1, 1, 1, 1}, 4}, {{16, 1}, {0, 1, 2, 1}, 4}, {{16, 1}, {2, 3, 0, 1}, 4},
    {{16, 1}, {3, 3, 3, 3}, 4}, {{4, 4}, {2, 2, 2, 2}, 4}, {{16, 1}, {1, 2, 1, 2}, 4}, {{1, 16}, {3, 0, 2, 3}, 4},
    {{16, 1}, {0, 1, 0, 3}, 3}, {{16, 1}, {1, 1, 1, 2}, 3}, {{16, 1}, {2, 0, 3, 1}, 2}, {{4, 4}, {1, 0, 2, 0}, 1},
    {{16, 1}, {0, 0, 0, 0}, 1},
};
constexpr unsigned quad_count = std::size(quads), whole = 8;

// the color samplers s0-s3 and the comparison samplers s4-s7: anisotropy (1 for a linear filter), bias, MinLOD
static const struct {
  unsigned anisotropy;
  float bias, min_lod;
} colors[] = {{1, 1, 0}, {1, -1.5f, 0}, {1, 0.5f, 5}, {4, 1, 0}}, compares[] = {{4, 1, 0}, {1, -1, 0}, {1, 0, 5},
                                                                                  {1, 1, 0}};
constexpr unsigned sampler_count = std::size(colors);

static const char hlsl[] = R"hlsl(
Texture2D<float> t : register(t0);
Texture2D<float> v : register(t1);
Texture2D<float> d : register(t2);
SamplerState s[SAMPLERS] : register(s0);
SamplerComparisonState c[SAMPLERS] : register(s4);
cbuffer C : register(b0) {
  float4 quad[QUADS]; uint4 lanes[QUADS]; float4 lo[QUADS]; float4 hi[QUADS]; float bias; float least;
};
struct O {
  float s : SV_Target0; float b : SV_Target1; float cl : SV_Target2; float v : SV_Target3; float lo : SV_Target4;
  float hi : SV_Target5;
};
// the first two triangles cover the quads before WHOLE, the others a quad's first 3, 2 or 1 lanes each: no edge
// passes a pixel center
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 at;
  if (id < 6) {
    uint corner = (0x231210 >> (4 * id)) & 3;
    at = float2(corner & 1, corner >> 1) * float2(2 * WHOLE, 2);
  } else {
    uint q = WHOLE + (id - 6) / 3, corner = (id - 6) % 3;
    float covered = quad[q].z;
    float2 reach = covered == 3 ? float2(2.2, 2.2) : covered == 2 ? float2(3, 1.4) : float2(1.2, 1.2);
    at = float2(2 * q, 0) + (corner == 1 ? float2(reach.x, 0) : corner == 2 ? float2(0, reach.y) : 0);
  }
  return float4(at / float2(2 * QUADS, 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
O ps(float4 p : SV_Position) {
  uint q = uint(p.x) / 2, lane = (uint(p.x) & 1) + 2 * (uint(p.y) & 1), i = lanes[q][lane];
  uint width, height, mips;
  t.GetDimensions(0, width, height, mips);
  float2 uv = p.xy * quad[q].xy / float2(width, height);
  O o;
  o.s = t.Sample(s[NonUniformResourceIndex(i)], uv);
  o.b = t.SampleBias(s[NonUniformResourceIndex(i)], uv, bias);
  o.cl = t.Sample(s[NonUniformResourceIndex(i)], uv, int2(0, 0), least);
  o.v = v.Sample(s[NonUniformResourceIndex(i)], uv);
  o.lo = d.SampleCmp(c[NonUniformResourceIndex(i)], uv, lo[q][lane]);
  o.hi = d.SampleCmp(c[NonUniformResourceIndex(i)], uv, hi[q][lane]);
  return o;
}
)hlsl";

// D3D11.3 7.18.11 for an axis-aligned footprint, then the bias, the sampler's MinLOD, the mips there are and a
// min-LOD clamp
static float
lod(const float footprint[2], unsigned anisotropy, float bias, float min_lod, unsigned mips, float least = 0) {
  float major = std::max(footprint[0], footprint[1]), determinant = footprint[0] * footprint[1];
  float ratio = major * major / determinant;
  float minor = anisotropy == 1 ? major : ratio > anisotropy ? major / anisotropy : determinant / major;
  return std::clamp(std::max(std::max(std::log2(minor) + bias, min_lod), least), 0.0f, float(mips - 1));
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile the sampling shaders");
  const std::vector<std::string> defines = {"QUADS=" + std::to_string(quad_count),
                                            "SAMPLERS=" + std::to_string(sampler_count),
                                            "WHOLE=" + std::to_string(whole)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  constexpr UINT size = 256, mips = std::bit_width(size), targets = 6;
  constexpr float depth_scale = 16, sample_bias = -1, least = 3.5f, view_clamp = 3, unwritten = -1;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](DXGI_FORMAT format, UINT64 width, UINT height, UINT16 layers, UINT16 levels,
                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, layers, levels, format, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    return res;
  };
  auto color = texture(DXGI_FORMAT_R32_FLOAT, size, size, 1, mips, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
  auto depth = texture(DXGI_FORMAT_R32_TYPELESS, size, size, 1, mips, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
  auto target = texture(DXGI_FORMAT_R32_FLOAT, 2 * quad_count, 2, targets, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (!expect(color && depth && target, "textures were not created"))
    return verdict();

  auto color_desc = color->GetDesc(), target_desc = target->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp[mips], target_fp[targets];
  UINT64 color_bytes, target_bytes;
  device->GetCopyableFootprints(&color_desc, 0, mips, 0, color_fp, nullptr, nullptr, &color_bytes);
  device->GetCopyableFootprints(&target_desc, 0, targets, 0, target_fp, nullptr, nullptr, &target_bytes);
  constexpr UINT cb_stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  const UINT64 cb_offset = (color_bytes + cb_stride - 1) / cb_stride * cb_stride;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cb_offset + 4 * cb_stride,
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && readback, "buffers were not created"))
    return verdict();
  CHECK(forget(readback.Get()));
  char *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT m = 0; m < mips; m++)
    for (UINT y = 0; y < color_fp[m].Footprint.Height; y++)
      std::fill_n((float *)(mapped + color_fp[m].Offset + y * color_fp[m].Footprint.RowPitch),
                  color_fp[m].Footprint.Width, float(m));

  // per quad its footprint, its lanes' samplers and the references half a mip below and above the LOD each lane's
  // comparison expects
  struct {
    float quad[quad_count][4];
    unsigned lanes[quad_count][4];
    float lo[quad_count][4], hi[quad_count][4];
    float bias, least;
  } constants{};
  for (UINT q = 0; q < quad_count; q++) {
    constants.quad[q][0] = quads[q].footprint[0];
    constants.quad[q][1] = quads[q].footprint[1];
    constants.quad[q][2] = float(quads[q].covered);
    for (UINT l = 0; l < 4; l++) {
      auto &c = compares[quads[q].lanes[l]];
      float want = lod(quads[q].footprint, c.anisotropy, c.bias, c.min_lod, mips);
      constants.lanes[q][l] = quads[q].lanes[l];
      constants.lo[q][l] = (want - 0.5f) / depth_scale;
      constants.hi[q][l] = (want + 0.5f) / depth_scale;
    }
  }
  constants.bias = sample_bias;
  constants.least = least;
  static_assert(sizeof(constants) <= 4 * cb_stride);
  memcpy(mapped + cb_offset, &constants, sizeof(constants));
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12DescriptorHeap> views, samplers, rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2 * sampler_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&samplers)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targets};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, mips};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsvs)));
  auto at = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{h->GetCPUDescriptorHandleForHeapStart().ptr +
                                       i * device->GetDescriptorHandleIncrementSize(type)};
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv.Texture2D.MipLevels = mips;
  device->CreateShaderResourceView(color.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
  device->CreateShaderResourceView(depth.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2));
  srv.Texture2D.ResourceMinLODClamp = view_clamp;
  device->CreateShaderResourceView(color.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1));
  for (UINT i = 0; i < targets; i++) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{DXGI_FORMAT_R32_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
    rtv.Texture2DArray = {0, i, 1};
    device->CreateRenderTargetView(target.Get(), &rtv, at(rtvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, i));
  }
  for (UINT k = 0; k < 2 * sampler_count; k++) {
    bool compare = k >= sampler_count;
    auto &state = compare ? compares[k - sampler_count] : colors[k];
    D3D12_FILTER filter =
        state.anisotropy > 1 ? (compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC)
                             : (compare ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    D3D12_SAMPLER_DESC desc{filter, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP, state.bias, state.anisotropy, D3D12_COMPARISON_FUNC_LESS};
    desc.MinLOD = state.min_lod;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&desc, at(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, k));
  }

  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2 * sampler_count}};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].DescriptorTable = {1, &ranges[0]};
  params[2].DescriptorTable = {1, &ranges[1]};
  auto root = root_signature(device.Get(), {3, params});
  if (!expect(bool(root), "root signature was not created"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = root.Get();
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
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  step("upload the mips and clear the depth mips");
  for (UINT m = 0; m < mips; m++) {
    D3D12_TEXTURE_COPY_LOCATION dst{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = m}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = color_fp[m]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
    dsv.Texture2D.MipSlice = m;
    device->CreateDepthStencilView(depth.Get(), &dsv, at(dsvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, m));
    list->ClearDepthStencilView(at(dsvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, m), D3D12_CLEAR_FLAG_DEPTH,
                                m / depth_scale, 0, 0, nullptr);
  }
  transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  step("draw through the descriptor table's samplers, to whole quads and quads in part");
  CHECK(allocator->Reset());
  CHECK(list->Reset(allocator.Get(), pso.Get()));
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(root.Get());
  list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + cb_offset);
  list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootDescriptorTable(2, samplers->GetGPUDescriptorHandleForHeapStart());
  D3D12_VIEWPORT viewport{0, 0, float(target_desc.Width), float(target_desc.Height), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(target_desc.Width), LONG(target_desc.Height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  auto first = rtvs->GetCPUDescriptorHandleForHeapStart();
  const float clear[4] = {unwritten, unwritten, unwritten, unwritten};
  for (UINT k = 0; k < targets; k++)
    list->ClearRenderTargetView(at(rtvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, k), clear, 0, nullptr);
  list->OMSetRenderTargets(targets, &first, TRUE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(6 + 3 * (quad_count - whole), 1, 0, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT k = 0; k < targets; k++) {
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = k}},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = target_fp[k]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  char *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  // D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT: the LOD's precision, and so a mip blend's
  const float tolerance = std::ldexp(1.0f, -D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT);
  const char *names[targets] = {"Sample",      "SampleBias", "Sample with a clamp", "Sample of a clamped view",
                                "SampleCmp below", "SampleCmp above"};
  for (UINT q = 0; q < quad_count; q++)
    for (UINT y = 0; y < 2; y++)
      for (UINT x = 2 * q; x < 2 * q + 2; x++) {
        unsigned index = quads[q].lanes[(x & 1) + 2 * y];
        auto &s = colors[index];
        auto &footprint = quads[q].footprint;
        bool covered = (x & 1) + 2 * y < quads[q].covered;
        step("read quad %u (footprint %gx%g, %u lanes covered), lane %u,%u: sampler %u%s", q, footprint[0],
             footprint[1], quads[q].covered, x & 1, y, index, covered ? "" : ", a helper");
        float wants[targets] = {lod(footprint, s.anisotropy, s.bias, s.min_lod, mips),
                                lod(footprint, s.anisotropy, s.bias + sample_bias, s.min_lod, mips),
                                lod(footprint, s.anisotropy, s.bias, s.min_lod, mips, least),
                                lod(footprint, s.anisotropy, s.bias, s.min_lod, mips, view_clamp), 1, 0};
        if (!covered)
          std::fill_n(wants, targets, unwritten);
        for (UINT k = 0; k < targets; k++) {
          float value =
              *(float *)(got + target_fp[k].Offset + y * target_fp[k].Footprint.RowPitch + x * sizeof(float));
          expect(std::fabs(value - wants[k]) <= tolerance, "%s at pixel %u,%u read %g, expected %g", names[k], x, y,
                 value, wants[k]);
        }
      }
  readback->Unmap(0, nullptr);
  return verdict();
}
