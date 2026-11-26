// contract: shader model 6.6 compute quads (HLSL_SM_6_6_Derivatives). groups with even X and Y form quads from 2x2
// blocks of SV_GroupThreadID; others, like one row of fours, from four consecutive threads. in both, the IDs stay
// consistent (SV_GroupIndex, SV_DispatchThreadID), and the quad's lanes give coarse and fine derivatives, the LOD of
// Sample, SampleBias and SampleCmp, and CalculateLevelOfDetail. each thread takes a quad position p (its group thread
// ID, or for a row, its place in the quad's 2x2), differentiates a function of p that is not linear, and samples at p
// times 2^k texels, k its group's index, from mips whose texels all hold the mip's index, so the mip read is the LOD.
// a cube's LOD must not change when each lane scales the same direction by its own length.
// DXIL.rst DerivCoarseX/Y: "The exact calculation is up to the hardware vendor." either row or column is
// allowed, but its result is shared by the quad. DerivFineX/Y: "The data in the current Pixel Shader invocation
// always participates" (https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst).
// isolated derivative shaders test the SM6.6 layout without another operation enabling it. native half and
// minimum precision use exactly representable inputs; all shaders are compiled before device creation.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>

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
  T x = T(p.x % TILE), y = T(p.y % TILE);
  T f = T(x * x - 3 * y * y + x * y);
  float2 uv = float2(p) * exp2(group.x) / SIZE;
  uint b = (group.x * X * Y * Z + i) * FIELDS;
  o[b + 0] = g.x | g.y << ID_BITS | g.z << (2 * ID_BITS);
  o[b + 1] = all(d == group * uint3(X, Y, Z) + g) && i == (g.z * Y + g.y) * X + g.x;
  o[b + 2] = p.x | p.y << HALF_WORD;
#if ONLY
  o[b + 3] = asuint(float(OP(f)));
#else
  o[b + 14] = WaveGetLaneIndex() % 4;
  o[b + 3] = asuint(float(ddx_coarse(f)));
  o[b + 4] = asuint(float(ddy_coarse(f)));
  o[b + 5] = asuint(float(ddx_fine(f)));
  o[b + 6] = asuint(float(ddy_fine(f)));
  o[b + 7] = asuint(mips.Sample(linear_mips, uv));
  o[b + 8] = asuint(mips.SampleBias(linear_mips, uv, 1));
  o[b + 9] = asuint(mips.CalculateLevelOfDetail(linear_mips, uv));
  o[b + 10] = asuint(mips.CalculateLevelOfDetailUnclamped(linear_mips, uv));
  // on the +x face, s steps by CUBE_STEP / 2 per p, and each lane's direction has its own length
  float3 direction = float3(1, float2(p % TILE) * CUBE_STEP) * (1 + (i & 1));
  o[b + 13] = asuint(cube.CalculateLevelOfDetailUnclamped(linear_mips, direction));
  // the depth mips hold (m + 1) / 16: references just under and over one mip's say which mip was compared
  float m = min(group.x, MIPS - 1);
  o[b + 11] = depth.SampleCmp(less, uv, (m + 0.5) / 16) > 0.5;
  o[b + 12] = depth.SampleCmp(less, uv, (m + 1.5) / 16) > 0.5;
#endif
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
  const UINT size = 64, mip_count = std::bit_width(size), lods = mip_count + 1, fields = 15, tile = 8;
  const UINT id_bits = std::bit_width(D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP - 1u),
             half_word = sizeof(UINT16) * CHAR_BIT;
  // a cube step of 2^(k + 1) / size is 2^k texels of the face; the tiled coordinates keep every quad on +x
  const UINT cube_lod = 0;
  const float cube_step = float(2 << cube_lod) / size;
  struct Layout {
    UINT x, y, z;
    bool linear;
  };
  // 2x2 quads in square, uneven and layered groups; quads of a row
  const Layout layouts[] = {
      {8, 8, 1, false},
      {6, 4, 1, false},
      {4, 2, 2, false},
      {64, 1, 1, true},
      {4, 1, 1, true},
      {12, 1, 1, true},
      {2, 2, 1, false},
      {2, 2, 3, false},
      {D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP, 1, 1, true},
      {D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP / 2, 2, 1, false}
  };
  const char *operations[] = {"all", "ddx_coarse", "ddy_coarse", "ddx_fine", "ddy_fine"};
  struct Variant {
    UINT layout, operation;
    const char *type;
    std::string code;
  };
  std::vector<Variant> variants;
  for (UINT l = 0; l < std::size(layouts); l++)
    for (const char *type : {"float", "float16_t", "min16float"})
      for (UINT op = 0; op < std::size(operations); op++) {
        if (op == 0 && strcmp(type, "float"))
          continue;
        auto &shape = layouts[l];
        step("compile %s %s in %ux%ux%u", type, operations[op], shape.x, shape.y, shape.z);
        auto code = compiler.compile(
            hlsl, "cs", "cs_6_6",
            {"X=" + std::to_string(shape.x), "Y=" + std::to_string(shape.y), "Z=" + std::to_string(shape.z),
             "LINEAR=" + std::to_string(shape.linear), "SIZE=" + std::to_string(size),
             "MIPS=" + std::to_string(mip_count), "FIELDS=" + std::to_string(fields),
             "CUBE_STEP=" + std::to_string(cube_step), "T=" + std::string(type), "TILE=" + std::to_string(tile),
             "ID_BITS=" + std::to_string(id_bits), "HALF_WORD=" + std::to_string(half_word),
             "ONLY=" + std::to_string(op != 0), "OP=" + std::string(op ? operations[op] : "ddx_coarse")},
            !strcmp(type, "float16_t") ? std::vector<std::wstring>{L"-enable-16bit-types"} : std::vector<std::wstring>{}
        );
        if (!expect(!code.empty(), "%s %s HLSL", type, operations[op]))
          return verdict();
        variants.push_back({l, op, type, std::move(code)});
      }

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
  if (!expect(options4.Native16BitShaderOpsSupported, "native half capability"))
    return verdict();
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

  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect dispatch buffer"))
    return verdict();
  void *mapped;
  CHECK(indirect->Map(0, nullptr, &mapped));
  const D3D12_DISPATCH_ARGUMENTS dispatch{lods, 1, 1};
  memcpy(mapped, &dispatch, sizeof(dispatch));
  indirect->Unmap(0, nullptr);
  for (auto &variant : variants) {
    auto &l = layouts[variant.layout];
    const UINT threads = l.x * l.y * l.z, bytes = lods * threads * fields * sizeof(UINT);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(variant.code)};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    auto out = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    auto initial = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(out && initial && readback, "quad buffers"))
      return verdict();
    CHECK(initial->Map(0, nullptr, &mapped));
    memset(mapped, 0xcd, bytes);
    initial->Unmap(0, nullptr);
    for (bool indirect_path : {false, true}) {
      step("%s %s, %ux%ux%u, indirect=%d", variant.type, operations[variant.operation], l.x, l.y, l.z, indirect_path);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      list->CopyResource(out.Get(), initial.Get());
      transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootSignature(rs.Get());
      list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
      list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
      if (indirect_path)
        list->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
      else
        list->Dispatch(lods, 1, 1);
      transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), out.Get());
      transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const UINT *o;
      CHECK(readback->Map(0, nullptr, (void **)&o));
      auto f = [&](UINT x, UINT y) {
        x %= tile;
        y %= tile;
        return float(int(x * x) - 3 * int(y * y) + int(x * y));
      };
      auto fp = [](float v) { return std::bit_cast<UINT>(v); };
      for (UINT k = 0; k < lods; k++)
        for (UINT i = 0; i < threads; i++) {
          auto r = o + (k * threads + i) * fields;
          UINT gx = i % l.x, gy = i / l.x % l.y, gz = i / (l.x * l.y);
          UINT x = l.linear ? ((i & 1) | (i >> 2 << 1)) : gx, y = l.linear ? ((i >> 1) & 1) : gy;
          UINT x0 = x & ~1u, y0 = y & ~1u;
          UINT base = l.linear ? (i & ~3u) : (gz * l.y + (gy & ~1u)) * l.x + (gx & ~1u);
          auto head = o + (k * threads + base) * fields;
          bool valid = true;
          auto same = [&](UINT field, UINT want, const char *what) {
            valid &= expect(r[field] == want, "group %u thread %u %s: %#x, want %#x", k, i, what, r[field], want);
          };
          auto derivative = [&](UINT field, bool vertical, bool fine) {
            float a = vertical ? f(x0, y0 + 1) - f(x0, y0) : f(x0 + 1, y0) - f(x0, y0);
            float b = vertical ? f(x0 + 1, y0 + 1) - f(x0 + 1, y0) : f(x0 + 1, y0 + 1) - f(x0, y0 + 1);
            if (fine)
              same(field, fp(vertical ? f(x, y0 + 1) - f(x, y0) : f(x0 + 1, y) - f(x0, y)), "fine derivative");
            else {
              valid &= expect(
                  r[field] == fp(a) || r[field] == fp(b), "group %u thread %u coarse derivative %#x, want %#x or %#x",
                  k, i, r[field], fp(a), fp(b)
              );
              same(field, head[field], "coarse derivative shared by quad");
            }
          };
          same(0, gx | gy << id_bits | gz << (2 * id_bits), "SV_GroupThreadID");
          same(1, 1, "SV_GroupIndex and SV_DispatchThreadID");
          same(2, x | y << half_word, "quad position");
          same(14, variant.operation ? 0xcdcdcdcdu : (x & 1) | (y & 1) << 1, "wave lane within quad");
          if (variant.operation) {
            derivative(3, variant.operation == 2 || variant.operation == 4, variant.operation >= 3);
            for (UINT field = 4; field < 14; field++)
              same(field, 0xcdcdcdcdu, "untouched field");
          } else {
            derivative(3, false, false);
            derivative(4, true, false);
            derivative(5, false, true);
            derivative(6, true, true);
            UINT last = mip_count - 1;
            same(7, fp(float(std::min(k, last))), "Sample");
            same(8, fp(float(std::min(k + 1, last))), "SampleBias");
            same(9, fp(float(std::min(k, last))), "CalculateLevelOfDetail");
            same(10, fp(float(k)), "CalculateLevelOfDetailUnclamped");
            same(11, 1, "SampleCmp lower reference");
            same(12, 0, "SampleCmp upper reference");
            same(13, fp(float(cube_lod)), "cube CalculateLevelOfDetailUnclamped");
          }
          if (!valid) {
            readback->Unmap(0, nullptr);
            return verdict();
          }
        }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
