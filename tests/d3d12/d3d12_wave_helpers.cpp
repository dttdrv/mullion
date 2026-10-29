// contract: wave operations exclude helper lanes; "The bits corresponding to inactive or helper lanes will be
// zero" (DXC Wave Intrinsics, https://github.com/microsoft/DirectXShaderCompiler/wiki/Wave-Intrinsics).
// "UAV accesses and most wave intrinsics are disabled on helper lanes." triangles cover one or three pixel
// centers per quad. CPU sums and masks use only pixels that survive discard; every other UAV word stays intact.
// IsHelperLane after discard includes "pixels that were explicitly discarded" (DirectX-Specs, SM6.6).
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <map>

enum Field {
  leader,
  lane,
  width,
  helper,
  helpers,
  first,
  active_count,
  sum,
  prefix,
  match,
  ballot = match + 4,
  fields = ballot + 4
};
static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> output : register(u0);
cbuffer C : register(b0) { uint discard_some; uint coverage; }
struct V { float4 pos : SV_Position; nointerpolation uint primitive : PRIMITIVE; };
V vs(uint corner : SV_VertexID, uint primitive : SV_InstanceID) {
  uint2 quad = uint2(primitive % QUADS, primitive / QUADS);
  uint selected = primitive % 4;
  float2 center = 2 * quad + uint2(selected & 1, selected >> 1) + 0.5;
  float2 p = center + float2(corner == 1 ? 0.2 : -0.1, corner == 2 ? 0.2 : -0.1);
  if (coverage != 0) p = 2 * quad + float2(corner == 1 ? 2.25 : 0, corner == 2 ? 2.25 : 0);
  V v;
  v.pos = float4(p / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
  v.primitive = primitive;
  return v;
}
uint ps(V v) : SV_Target {
  if (discard_some != 0 && (uint(v.pos.x) & 1) != 0) discard;
  uint id = uint(v.pos.y) * SIZE + uint(v.pos.x), b = id * FIELDS;
  uint h = IsHelperLane();
  output[b + LEADER] = WaveReadLaneFirst(id);
  output[b + LANE] = WaveGetLaneIndex();
  output[b + WIDTH] = WaveGetLaneCount();
  output[b + HELPER] = h;
  output[b + HELPERS] = h | QuadReadAcrossX(h) << 1 | QuadReadAcrossY(h) << 2 | QuadReadAcrossDiagonal(h) << 3;
  output[b + FIRST] = WaveIsFirstLane();
  output[b + ACTIVE_COUNT] = WaveActiveCountBits(true);
  output[b + SUM] = WaveActiveSum(id + 1);
  output[b + PREFIX] = WavePrefixSum(id + 1);
  uint4 matched = WaveMatch(uint(1)), votes = WaveActiveBallot(true);
  for (uint k = 0; k < 4; k++) {
    output[b + MATCH + k] = matched[k];
    output[b + BALLOT + k] = votes[k];
  }
  return id + 1;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler) || !compiler.dxc) {
    printf("skipped: helper wave intrinsics require DXC\n");
    return 77;
  }
  const UINT quads = 8, size = 2 * quads, pixels = size * size;
  std::vector<std::string> defines = {
      "QUADS=" + std::to_string(quads), "SIZE=" + std::to_string(size), "FIELDS=" + std::to_string(fields)
  };
  for (auto [name, f] : std::initializer_list<std::pair<const char *, Field>>{
           {"LEADER", leader},
           {"LANE", lane},
           {"WIDTH", width},
           {"HELPER", helper},
           {"HELPERS", helpers},
           {"FIRST", first},
           {"ACTIVE_COUNT", active_count},
           {"SUM", sum},
           {"PREFIX", prefix},
           {"MATCH", match},
           {"BALLOT", ballot}
       })
    defines.push_back(std::string(name) + "=" + std::to_string(f));
  auto vs = compiler.compile(hlsl, "vs", "vs_6_6", defines), ps = compiler.compile(hlsl, "ps", "ps_6_6", defines);
  if (!expect(!vs.empty() && !ps.empty(), "helper shaders"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  if (!expect(options.WaveOps && options.WaveLaneCountMin >= 4 && options.WaveLaneCountMax <= 128, "wave capabilities"))
    return verdict();
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {std::size(params), params});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  size,
                                  size,
                                  1,
                                  1,
                                  desc.RTVFormats[0],
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &target_bytes);
  const UINT64 bytes = pixels * fields * sizeof(UINT);
  auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto image = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(initial && output && readback && image, "helper buffers"))
    return verdict();
  void *mapped;
  CHECK(initial->Map(0, nullptr, &mapped));
  memset(mapped, 0xff, bytes);
  initial->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  for (UINT coverage : {0u, 1u})
    for (UINT discarded : {0u, 1u}) {
      step("helper exclusion with coverage=%u discard=%u", coverage, discarded);
      CHECK(forget(readback.Get()));
      CHECK(forget(image.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      list->CopyResource(output.Get(), initial.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const float clear[] = {0, 0, 0, 0};
      D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
      D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      list->SetGraphicsRootSignature(rs.Get());
      list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
      list->SetGraphicsRoot32BitConstant(1, discarded, 0);
      list->SetGraphicsRoot32BitConstant(1, coverage, 1);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->DrawInstanced(3, quads * quads, 0, 0);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          to{image.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      std::array<bool, pixels> visible{};
      for (UINT primitive = 0; primitive < quads * quads; primitive++) {
        for (UINT selected = 0; selected < 4; selected++) {
          UINT x = 2 * (primitive % quads) + (selected & 1), y = 2 * (primitive / quads) + (selected >> 1);
          bool covered = coverage ? selected != 3 : selected == primitive % 4;
          visible[y * size + x] = covered && (!discarded || (x & 1) == 0);
        }
      }
      const char *texels;
      CHECK(image->Map(0, nullptr, (void **)&texels));
      for (UINT i = 0; i < pixels; i++) {
        UINT value;
        memcpy(&value, texels + i / size * footprint.Footprint.RowPitch + i % size * sizeof(UINT), sizeof(value));
        expect(value == (visible[i] ? i + 1 : 0), "pixel %u: %u, expected visibility %d", i, value, visible[i]);
      }
      image->Unmap(0, nullptr);
      UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      auto row = [&](UINT i) { return got + i * fields; };
      std::map<UINT, std::map<UINT, UINT>> waves;
      for (UINT i = 0; i < pixels; i++) {
        auto r = row(i);
        if (!visible[i]) {
          for (UINT f = 0; f < fields; f++)
            expect(r[f] == UINT_MAX, "helper or discarded pixel %u wrote field %u", i, f);
        } else if (
            expect(
                r[leader] < pixels && visible[r[leader]] && r[lane] < r[width] &&
                    r[width] >= options.WaveLaneCountMin && r[width] <= options.WaveLaneCountMax,
                "pixel %u leader/lane/width %u/%u/%u", i, r[leader], r[lane], r[width]
            )
        )
          expect(waves[r[leader]].emplace(r[lane], i).second, "duplicate lane for pixel %u", i);
      }
      for (auto &[head, lanes] : waves) {
        UINT total = 0, scanned = 0;
        std::array<UINT, 4> mask{};
        for (auto [l, i] : lanes)
          total += i + 1, mask[l / 32] |= UINT(1) << (l % 32);
        // the wave reads its leader from the lowest lane that takes part: a broadcast from lane 0 would read a helper
        // lane's value where the wave's first lanes are helpers, and that pixel reports no lane at all
        auto leader = std::find_if(lanes.begin(), lanes.end(), [&](auto &lane) { return lane.second == head; });
        expect(leader != lanes.end() && leader->first == lanes.begin()->first,
               "wave %u reads its leader from lane %u, and its lowest lane is %u",
               head, leader == lanes.end() ? ~0u : leader->first, lanes.begin()->first);
        for (auto [l, i] : lanes) {
          auto r = row(i);
          UINT helper_flags = 0;
          for (UINT neighbor = 0; neighbor < 4; neighbor++) {
            UINT x = (i % size) ^ (neighbor & 1), y = (i / size) ^ (neighbor >> 1);
            helper_flags |= UINT(!visible[y * size + x]) << neighbor;
          }
          expect(
              r[helper] == 0 && r[helpers] == helper_flags, "pixel %u helper flags %#x/%#x, want %#x", i, r[helper],
              r[helpers], helper_flags
          );
          expect(
              r[first] == (l == lanes.begin()->first) && r[active_count] == lanes.size(), "pixel %u first/count %u/%u",
              i, r[first], r[active_count]
          );
          expect(
              r[sum] == total && r[prefix] == scanned, "pixel %u sum/prefix %u/%u, want %u/%u", i, r[sum], r[prefix],
              total, scanned
          );
          for (UINT k = 0; k < mask.size(); k++)
            expect(r[match + k] == mask[k] && r[ballot + k] == mask[k], "pixel %u mask word %u", i, k);
          scanned += i + 1;
        }
      }
      readback->Unmap(0, nullptr);
    }
  return verdict();
}
