// contract: shader model 6.6 compute quads (HLSL_SM_6_6_Derivatives). groups with even X and Y form quads from 2x2
// blocks of SV_GroupThreadID; others, like one row of fours, from four consecutive threads. in both, the IDs stay
// consistent (SV_GroupIndex, SV_DispatchThreadID), and the quad's lanes give coarse and fine derivatives, the LOD of
// Sample, SampleBias and SampleCmp, and CalculateLevelOfDetail. each thread takes a quad position p (its group thread
// ID, or for a row, its place in the quad's 2x2), differentiates a function of p that is not linear, and samples at p
// times 2^k texels, k its group's index, from mips whose texels all hold the mip's index, so the mip read is the LOD.
// a cube's LOD must not change when each lane scales the same direction by its own length.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

static const char hlsl[] = R"hlsl(
Texture2D<float> mips : register(t0);
Texture2D<float> depth : register(t1);
TextureCube<float> cube : register(t2);
SamplerState linear_mips : register(s0);
SamplerComparisonState less : register(s1);
RWStructuredBuffer<uint> o : register(u0);
[numthreads(X, Y, Z)]
void cs(uint3 g : SV_GroupThreadID, uint3 d : SV_DispatchThreadID, uint3 group : SV_GroupID, uint i : SV_GroupIndex) {
#if LINEAR
  uint2 p = uint2((i & 1) | (i >> 2 << 1), (i >> 1) & 1);
#else
  uint2 p = g.xy;
#endif
  float f = p.x * p.x + 3 * p.y * p.y + p.x * p.y;
  float2 uv = float2(p) * exp2(group.x) / SIZE;
  uint b = (group.x * X * Y * Z + i) * FIELDS;
  o[b + 0] = g.x | g.y << 8 | g.z << 16;
  o[b + 1] = all(d == group * uint3(X, Y, Z) + g) && i == (g.z * Y + g.y) * X + g.x;
  o[b + 2] = p.x | p.y << 8;
  o[b + 3] = asuint(ddx_coarse(f));
  o[b + 4] = asuint(ddy_coarse(f));
  o[b + 5] = asuint(ddx_fine(f));
  o[b + 6] = asuint(ddy_fine(f));
  o[b + 7] = asuint(mips.Sample(linear_mips, uv));
  o[b + 8] = asuint(mips.SampleBias(linear_mips, uv, 1));
  o[b + 9] = asuint(mips.CalculateLevelOfDetail(linear_mips, uv));
  o[b + 10] = asuint(mips.CalculateLevelOfDetailUnclamped(linear_mips, uv));
  // on the +x face, s steps by CUBE_STEP / 2 per p, and each lane's direction has its own length
  float3 direction = float3(1, float2(p) * CUBE_STEP) * (1 + (i & 1));
  o[b + 13] = asuint(cube.CalculateLevelOfDetailUnclamped(linear_mips, direction));
  // the depth mips hold (m + 1) / 16: references just under and over one mip's say which mip was compared
  float m = min(group.x, MIPS - 1);
  o[b + 11] = depth.SampleCmp(less, uv, (m + 0.5) / 16) > 0.5;
  o[b + 12] = depth.SampleCmp(less, uv, (m + 1.5) / 16) > 0.5;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: compute derivatives are shader model 6.6\n");
    return 77;
  }
  const UINT size = 64, mip_count = std::bit_width(size), lods = mip_count + 1, fields = 14;
  // a cube step of 2^(k + 1) / size in the direction is 2^k texels of the face; at LOD 0 the farthest quad position,
  // 31 in a row of 64, stays on the +x face
  const UINT cube_lod = 0;
  const float cube_step = float(2 << cube_lod) / size;
  struct Layout {
    UINT x, y, z;
    bool linear;
  };
  // 2x2 quads in square, uneven and layered groups; quads of a row
  const Layout layouts[] = {{8, 8, 1, false}, {6, 4, 1, false}, {4, 2, 2, false}, {64, 1, 1, true}};

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE srvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &srvs};
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {
      // LOD is defined for linear mip filtering; at whole LODs it reads one mip
      {D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
       D3D12_TEXTURE_ADDRESS_MODE_WRAP},
      {D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
       D3D12_TEXTURE_ADDRESS_MODE_WRAP},
  };
  for (UINT s = 0; s < 2; s++) {
    samplers[s].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[s].ShaderRegister = s;
  }
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
  auto rs = root_signature(device.Get(), {2, params, 2, samplers});

  // one color and one depth mip chain, each mip filled with its index (depth: (index + 1) / 16) by clears
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> color, depth, cube;
  D3D12_RESOURCE_DESC color_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, (UINT16)mip_count,
                                 DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  auto depth_desc = color_desc;
  depth_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)
  ));
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)
  ));
  auto cube_desc = color_desc;
  cube_desc.DepthOrArraySize = 6;
  cube_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &cube_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&cube)
  ));
  ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap, dsv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, mip_count}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, mip_count};
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  auto cpu = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    auto handle = h->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += i * device->GetDescriptorHandleIncrementSize(type);
    return handle;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  srv.Texture2D.MipLevels = mip_count;
  device->CreateShaderResourceView(color.Get(), &srv, cpu(srv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0));
  device->CreateShaderResourceView(depth.Get(), &srv, cpu(srv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1));
  D3D12_SHADER_RESOURCE_VIEW_DESC cube_srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURECUBE, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  cube_srv.TextureCube.MipLevels = mip_count;
  device->CreateShaderResourceView(cube.Get(), &cube_srv, cpu(srv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  for (UINT m = 0; m < mip_count; m++) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{DXGI_FORMAT_R32_FLOAT, D3D12_RTV_DIMENSION_TEXTURE2D};
    rtv.Texture2D.MipSlice = m;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
    dsv.Texture2D.MipSlice = m;
    device->CreateRenderTargetView(color.Get(), &rtv, cpu(rtv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, m));
    device->CreateDepthStencilView(depth.Get(), &dsv, cpu(dsv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, m));
    const float value[4] = {(float)m, 0, 0, 0};
    list->ClearRenderTargetView(cpu(rtv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, m), value, 0, nullptr);
    list->ClearDepthStencilView(
        cpu(dsv_heap.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, m), D3D12_CLEAR_FLAG_DEPTH, (m + 1) / 16.0f, 0, 0, nullptr
    );
  }
  transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  unsigned failures = 0;
  for (auto &l : layouts) {
    const UINT threads = l.x * l.y * l.z, bytes = lods * threads * fields * 4;
    auto name = std::to_string(l.x) + "x" + std::to_string(l.y) + "x" + std::to_string(l.z);
    auto cs = compiler.compile(
        hlsl, "cs", "cs_6_6",
        {"X=" + std::to_string(l.x), "Y=" + std::to_string(l.y), "Z=" + std::to_string(l.z),
         "LINEAR=" + std::to_string(l.linear), "SIZE=" + std::to_string(size), "MIPS=" + std::to_string(mip_count),
         "FIELDS=" + std::to_string(fields), "CUBE_STEP=" + std::to_string(cube_step)}
    );
    if (cs.empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    auto out = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    list->Dispatch(lods, 1, 1);
    transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, bytes);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT *o;
    CHECK(readback->Map(0, nullptr, (void **)&o));

    auto f = [](UINT x, UINT y) { return (float)(x * x + 3 * y * y + x * y); };
    for (UINT k = 0; k < lods; k++) {
      std::set<UINT> ids;
      for (UINT i = 0; i < threads; i++) {
        auto r = o + (k * threads + i) * fields;
        auto expect = [&](UINT field, UINT want, const char *what) {
          if (r[field] != want && failures++ < 8)
            printf("%s, k %u, thread %u %s: %#x, want %#x\n", name.c_str(), k, i, what, r[field], want);
        };
        auto fp = [](float v) { return std::bit_cast<UINT>(v); };
        ids.insert(r[0]);
        expect(1, 1, "SV_GroupIndex and SV_DispatchThreadID match SV_GroupThreadID");
        UINT x = r[2] & 0xff, y = r[2] >> 8;
        // p's quad has its first lane at the even corner, and its lanes along x, then y
        UINT x0 = x & ~1u, y0 = y & ~1u;
        expect(3, fp(f(x0 + 1, y0) - f(x0, y0)), "ddx_coarse");
        expect(4, fp(f(x0, y0 + 1) - f(x0, y0)), "ddy_coarse");
        expect(5, fp(f(x0 + 1, y) - f(x0, y)), "ddx_fine");
        expect(6, fp(f(x, y0 + 1) - f(x, y0)), "ddy_fine");
        // one step of p is 2^k texels of mip 0: LOD k, clamped to the last mip
        UINT last = mip_count - 1;
        expect(7, fp((float)std::min(k, last)), "Sample");
        expect(8, fp((float)std::min(k + 1, last)), "SampleBias");
        expect(9, fp((float)std::min(k, last)), "CalculateLevelOfDetail");
        expect(10, fp((float)k), "CalculateLevelOfDetailUnclamped");
        expect(11, 1, "SampleCmp reads its LOD's mip, not a finer one");
        expect(12, 0, "SampleCmp reads its LOD's mip, not a coarser one");
        expect(13, fp((float)cube_lod), "cube CalculateLevelOfDetailUnclamped");
      }
      if (ids.size() != threads && failures++ < 8)
        printf("%s, k %u: %zu distinct SV_GroupThreadIDs of %u\n", name.c_str(), k, ids.size(), threads);
    }
    readback->Unmap(0, nullptr);
  }
  printf("%s: %u mismatches over %zu layouts\n", failures ? "failed" : "passed", failures, std::size(layouts));
  return failures != 0;
}
