// contract: a pixel shader's samples through a descriptor table's samplers read what the shader asks for in the order
// it asks for it, whatever lies between two samples of one sampler: a sample whose coordinate reads the one before it
// (a dependent read), a derivative of the one before (ddx_fine, D3D11.3 16.8), a write of it to a UAV or to a local
// array a dynamic index keeps in memory and a read of that element back (a thread reads its own writes, 5.3.10 for a
// UAV), or no more than other samples of that sampler. mip m of the
// texture holds m and filters are linear between mips (7.18.10), so a sample reads its LOD (7.18.11); each quad's
// lanes index one sampler or several (NonUniformResourceIndex), isotropic with biases of both signs or anisotropic.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

// per quad, the sampler each lane indexes, lane (x & 1) + 2 * (y & 1)
static const unsigned quads[][4] = {{0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 2, 1}, {2, 0, 1, 0}, {3, 3, 3, 3}, {0, 3, 1, 2}};
constexpr unsigned quad_count = std::size(quads);

// the samplers: anisotropy (1 for a linear filter) and bias
static const struct {
  unsigned anisotropy;
  float bias;
} samplers_used[] = {{1, 1}, {1, -1.5f}, {1, 0.5f}, {4, 1}};
constexpr unsigned sampler_count = std::size(samplers_used);
// each sample's footprint is this one's, in texels of the first mip, times a power of two
constexpr float footprint[2] = {4, 1};

static const char hlsl[] = R"hlsl(
Texture2D<float> t : register(t0);
RWStructuredBuffer<float> u : register(u0);
SamplerState s[SAMPLERS] : register(s0);
cbuffer C : register(b0) { uint4 lanes[QUADS]; float zero; uint none; };
struct O {
  float a : SV_Target0; float dependent : SV_Target1; float across : SV_Target2; float after : SV_Target3;
  float kept : SV_Target4; float last : SV_Target5; float sum : SV_Target6;
};
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
O ps(float4 p : SV_Position) {
  uint q = uint(p.x) / 2, i = lanes[q][(uint(p.x) & 1) + 2 * (uint(p.y) & 1)];
  uint width, height, mips;
  t.GetDimensions(0, width, height, mips);
  float2 uv = p.xy * float2(FOOTPRINT_X, FOOTPRINT_Y) / float2(width, height);
  O o;
  // the second sample's coordinate reads the first's value
  o.a = t.Sample(s[NonUniformResourceIndex(i)], uv);
  o.dependent = t.Sample(s[NonUniformResourceIndex(i)], uv + o.a * zero);
  // a derivative of the first between two samples
  float b = t.Sample(s[NonUniformResourceIndex(i)], uv * 2);
  o.across = ddx_fine(b);
  o.after = t.Sample(s[NonUniformResourceIndex(i)], uv * 4);
  // the first written to a UAV and read back between two samples (a UAV's value may not reach a coordinate), then
  // the second written to a local array and read back before a third
  float x = t.Sample(s[NonUniformResourceIndex(i)], uv * 8);
  uint element = uint(p.y) * QUADS * 2 + uint(p.x);
  u[element] = x;
  float kept = u[element + none];
  float y = t.Sample(s[NonUniformResourceIndex(i)], uv * 16);
  float local[4] = {-1, -1, -1, -1};
  local[i % 4] = y;
  o.kept = kept + local[(i + none) % 4];
  o.last = t.Sample(s[NonUniformResourceIndex(i)], uv * 32);
  // runs of eight samples of one sampler, each run another's
  float sum = 0;
  [unroll] for (uint j = 0; j < 4; j++)
    [unroll] for (int k = 0; k < 8; k++)
      sum += t.Sample(s[NonUniformResourceIndex((i + j) % SAMPLERS)], uv * (1 << j), int2(k - 4, 0));
  o.sum = sum;
  return o;
}
)hlsl";

// D3D11.3 7.18.11 for an axis-aligned footprint scaled by `scale`, then the bias and the mips there are
static float
lod(unsigned sampler, float scale, unsigned mips) {
  auto &s = samplers_used[sampler];
  float major = std::max(footprint[0], footprint[1]) * scale, determinant = footprint[0] * footprint[1] * scale * scale;
  float ratio = major * major / determinant;
  float minor = s.anisotropy == 1 ? major : ratio > s.anisotropy ? major / s.anisotropy : determinant / major;
  return std::clamp(std::log2(minor) + s.bias, 0.0f, float(mips - 1));
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile the sampling shaders");
  const std::vector<std::string> defines = {
      "QUADS=" + std::to_string(quad_count), "SAMPLERS=" + std::to_string(sampler_count),
      "FOOTPRINT_X=" + std::to_string(footprint[0]), "FOOTPRINT_Y=" + std::to_string(footprint[1])};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !ps.empty(), "HLSL did not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  constexpr UINT size = 256, mips = std::bit_width(size), targets = 7, width = 2 * quad_count, height = 2;
  constexpr float unwritten = -1;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](UINT64 w, UINT h, UINT16 layers, UINT16 levels, D3D12_RESOURCE_FLAGS flags,
                     D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, layers, levels, DXGI_FORMAT_R32_FLOAT, {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    return res;
  };
  auto color = texture(size, size, 1, mips, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  auto uav = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, width * height * sizeof(float),
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto target = texture(width, height, targets, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (!expect(color && uav && target, "textures were not created"))
    return verdict();

  auto color_desc = color->GetDesc(), target_desc = target->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp[mips], target_fp[targets];
  UINT64 color_bytes, target_bytes;
  const UINT64 uav_bytes = width * height * sizeof(float);
  device->GetCopyableFootprints(&color_desc, 0, mips, 0, color_fp, nullptr, nullptr, &color_bytes);
  device->GetCopyableFootprints(&target_desc, 0, targets, 0, target_fp, nullptr, nullptr, &target_bytes);
  constexpr UINT align = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  const UINT64 uav_offset = (color_bytes + align - 1) / align * align;
  const UINT64 cb_offset = (uav_offset + uav_bytes + align - 1) / align * align;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cb_offset + align, D3D12_RESOURCE_STATE_GENERIC_READ);
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
  std::fill_n((float *)(mapped + uav_offset), width * height, unwritten);
  struct {
    unsigned lanes[quad_count][4];
    float zero;
    unsigned none;
  } constants{};
  for (UINT q = 0; q < quad_count; q++)
    std::copy_n(quads[q], 4, constants.lanes[q]);
  static_assert(sizeof(constants) <= align);
  memcpy(mapped + cb_offset, &constants, sizeof(constants));
  upload->Unmap(0, nullptr);

  ComPtr<ID3D12DescriptorHeap> views, samplers, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&views)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, sampler_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&samplers)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targets};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  auto at = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{h->GetCPUDescriptorHandleForHeapStart().ptr +
                                       i * device->GetDescriptorHandleIncrementSize(type)};
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv.Texture2D.MipLevels = mips;
  device->CreateShaderResourceView(color.Get(), &srv, at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  uav_view.Buffer = {0, width * height, sizeof(float)};
  device->CreateUnorderedAccessView(uav.Get(), nullptr, &uav_view,
                                    at(views.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1));
  for (UINT i = 0; i < targets; i++) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{DXGI_FORMAT_R32_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
    rtv.Texture2DArray = {0, i, 1};
    device->CreateRenderTargetView(target.Get(), &rtv, at(rtvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, i));
  }
  for (UINT k = 0; k < sampler_count; k++) {
    auto &state = samplers_used[k];
    D3D12_SAMPLER_DESC desc{state.anisotropy > 1 ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP, state.bias, state.anisotropy, D3D12_COMPARISON_FUNC_NEVER};
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&desc, at(samplers.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, k));
  }

  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, sampler_count}};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[1].DescriptorTable = {2, &ranges[0]};
  params[2].DescriptorTable = {1, &ranges[2]};
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
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  step("upload the mips and the UAV, and draw");
  for (UINT m = 0; m < mips; m++) {
    D3D12_TEXTURE_COPY_LOCATION dst{color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = m}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = color_fp[m]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  list->CopyBufferRegion(uav.Get(), 0, upload.Get(), uav_offset, uav_bytes);
  transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), uav.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(root.Get());
  list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + cb_offset);
  list->SetGraphicsRootDescriptorTable(1, views->GetGPUDescriptorHandleForHeapStart());
  list->SetGraphicsRootDescriptorTable(2, samplers->GetGPUDescriptorHandleForHeapStart());
  D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  auto first = rtvs->GetCPUDescriptorHandleForHeapStart();
  list->OMSetRenderTargets(targets, &first, TRUE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT k = 0; k < targets; k++) {
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = k}},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = target_fp[k]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  char *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  // D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT: the LOD's precision, and so a mip blend's; a difference or a sum of samples
  // has each one's
  const float tolerance = std::ldexp(1.0f, -D3D12_MIP_LOD_FRACTIONAL_BIT_COUNT);
  const char *names[targets] = {"the first sample", "a dependent read", "ddx_fine of a sample", "a sample after it",
                                "a UAV element and an array element read back", "a sample after it", "runs of samples"};
  const float tolerances[targets] = {tolerance, tolerance, 2 * tolerance, tolerance, 2 * tolerance, tolerance,
                                     32 * tolerance};
  for (UINT q = 0; q < quad_count; q++)
    for (UINT y = 0; y < 2; y++)
      for (UINT x = 2 * q; x < 2 * q + 2; x++) {
        unsigned i = quads[q][(x & 1) + 2 * y];
        step("read quad %u, lane %u,%u: sampler %u", q, x & 1, y, i);
        float sum = 0;
        for (UINT j = 0; j < 4; j++)
          sum += 8 * lod((i + j) % sampler_count, float(1 << j), mips);
        float wants[targets] = {lod(i, 1, mips),
                                lod(i, 1, mips),
                                lod(quads[q][1 + 2 * y], 2, mips) - lod(quads[q][2 * y], 2, mips),
                                lod(i, 4, mips),
                                lod(i, 8, mips) + lod(i, 16, mips),
                                lod(i, 32, mips),
                                sum};
        for (UINT k = 0; k < targets; k++) {
          float value =
              *(float *)(got + target_fp[k].Offset + y * target_fp[k].Footprint.RowPitch + x * sizeof(float));
          expect(std::fabs(value - wants[k]) <= tolerances[k], "%s at pixel %u,%u read %g, expected %g", names[k], x,
                 y, value, wants[k]);
        }
      }
  readback->Unmap(0, nullptr);
  return verdict();
}
