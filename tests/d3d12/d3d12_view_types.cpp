// contract: single-layer 1D and 2D views accept plain or array declarations, while array views retain every slice
// relative to FirstArraySlice. typed UAV stores write that slice and mip, leaving other subresources unchanged.
// D3D11.3 22.3.12 permits "resource 'Texture1D' with array length >= 1" and "resource 'Texture2D' with array length
// >= 1" for array declarations; 5.2.1 says "the view properties (number of mipmaps, number of array elements, type,
// etc.) are always used in place of the properties of the original resource." 22.4.9 specifies "Random-access write
// of an element into a typed UAV." each source layer and mip has a distinct value; writes have distinct texels.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
#if ONE_DIMENSION
#define PLAIN Texture1D
#define ARRAY Texture1DArray
#define RWPLAIN RWTexture1D
#define RWARRAY RWTexture1DArray
#define AT(x, y) (x)
#define LAYER_AT(x, y, l) uint2(x, l)
#define LOAD(m) int2(0, m)
#define LAYER_LOAD(l, m) int3(0, l, m)
#else
#define PLAIN Texture2D
#define ARRAY Texture2DArray
#define RWPLAIN RWTexture2D
#define RWARRAY RWTexture2DArray
#define AT(x, y) uint2(x, y)
#define LAYER_AT(x, y, l) uint3(x, y, l)
#define LOAD(m) int3(0, 0, m)
#define LAYER_LOAD(l, m) int4(0, 0, l, m)
#endif
PLAIN<uint> array_as_plain : register(t1);
ARRAY<uint> plain_as_array : register(t0);
ARRAY<uint> layers : register(t2);
RWPLAIN<uint> plain_write : register(u1);
RWARRAY<uint> layers_write : register(u2);
RWStructuredBuffer<uint> results : register(u0);
[numthreads(1, 1, 1)]
void cs() {
  for (uint m = 0; m < MIPS; m++) {
    results[m] = plain_as_array.Load(LAYER_LOAD(0, m));
    results[MIPS + m] = array_as_plain.Load(LOAD(m));
    for (uint l = 0; l < LAYERS; l++)
      results[(2 + l) * MIPS + m] = layers.Load(LAYER_LOAD(l, m));
  }
}
[numthreads(1, 1, 1)]
void cs_writes() {
  for (uint y = 0; y < (ONE_DIMENSION ? 1 : SIDE); y++)
    for (uint x = 0; x < SIDE; x++) {
      uint value = WRITTEN + y * SIDE + x;
      plain_write[AT(x, y)] = value;
      if (x < (SIDE >> ARRAY_MIP) && y < (ONE_DIMENSION ? 1 : SIDE >> ARRAY_MIP))
        for (uint l = 0; l < LAYERS; l++)
          layers_write[LAYER_AT(x, y, l)] = value + (1 + l) * SIDE * SIDE;
    }
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT mips = 4, side = 1 << (mips - 1), base = 10, array_mip = 1;
  const UINT first_slice = 1, view_layers = 3, layers = first_slice + view_layers + 1;
  const UINT written = base + layers * mips;
  std::string cs[2], cs_writes[2];
  for (UINT d = 0; d < std::size(cs); d++) {
    std::vector<std::string> defines = {"MIPS=" + std::to_string(mips), "SIDE=" + std::to_string(side),
                                      "LAYERS=" + std::to_string(view_layers), "WRITTEN=" + std::to_string(written),
                                      "ARRAY_MIP=" + std::to_string(array_mip),
                                      "ONE_DIMENSION=" + std::to_string(d == 0)};
    cs[d] = compiler.compile(hlsl, "cs", "cs", defines);
    cs_writes[d] = compiler.compile(hlsl, "cs_writes", "cs", defines);
    if (cs[d].empty() || cs_writes[d].empty()) {
      printf("failed: HLSL did not compile\n");
      return 1;
    }
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0},
                                      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1}};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                   {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                   {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].DescriptorTable = {1, &ranges[0]};
  params[2].DescriptorTable = {1, &ranges[1]};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
  if (!expect(!!rs, "root signature"))
    return verdict();
  for (UINT d = 0; d < std::size(cs); d++) {
    step("%uD SRV reads and UAV stores, %u layers starting at slice %u", d + 1, view_layers, first_slice);
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs[d])};
    ComPtr<ID3D12PipelineState> pso, writes;
    CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&pso)));
    cs_desc.CS = bytecode(cs_writes[d]);
    CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&writes)));

    ComPtr<ID3D12Resource> texture[2], upload[2], tex_readback[2];
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints[2];
    for (UINT t = 0; t < std::size(texture); t++) {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC texture_desc{
          d == 0 ? D3D12_RESOURCE_DIMENSION_TEXTURE1D : D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side,
          d == 0 ? 1u : side, (UINT16)(t == 0 ? 1 : layers), (UINT16)mips, DXGI_FORMAT_R32_UINT,
          {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
      CHECK(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&texture[t])
      ));
      footprints[t].resize(mips * texture_desc.DepthOrArraySize);
      UINT64 upload_bytes;
      device->GetCopyableFootprints(
          &texture_desc, 0, footprints[t].size(), 0, footprints[t].data(), nullptr, nullptr, &upload_bytes
      );
      upload[t] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      tex_readback[t] = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, upload_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(!!upload[t] && !!tex_readback[t], "texture upload and readback buffers"))
        return verdict();
      CHECK(forget(tex_readback[t].Get()));
      char *mapped;
      CHECK(upload[t]->Map(0, nullptr, (void **)&mapped));
      for (UINT sub = 0; sub < footprints[t].size(); sub++) {
        auto &fp = footprints[t][sub];
        for (UINT y = 0; y < fp.Footprint.Height; y++)
          for (UINT x = 0; x < fp.Footprint.Width; x++)
            reinterpret_cast<UINT *>(mapped + fp.Offset + y * fp.Footprint.RowPitch)[x] = base + sub;
      }
      upload[t]->Unmap(0, nullptr);
    }

    ComPtr<ID3D12DescriptorHeap> srvs;
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ranges[0].NumDescriptors + ranges[1].NumDescriptors,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs)));
    auto handle = srvs->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC plain{
        DXGI_FORMAT_R32_UINT, d == 0 ? D3D12_SRV_DIMENSION_TEXTURE1D : D3D12_SRV_DIMENSION_TEXTURE2D,
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    if (d == 0)
      plain.Texture1D = {0, mips};
    else
      plain.Texture2D = {0, mips};
    device->CreateShaderResourceView(texture[0].Get(), &plain, handle);
    handle.ptr += increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC array{
        DXGI_FORMAT_R32_UINT, d == 0 ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    for (UINT v = 0; v < 2; v++) {
      if (d == 0)
        array.Texture1DArray = {0, mips, first_slice, v == 0 ? 1u : view_layers};
      else
        array.Texture2DArray = {0, mips, first_slice, v == 0 ? 1u : view_layers};
      device->CreateShaderResourceView(texture[1].Get(), &array, handle);
      handle.ptr += increment;
    }
    for (UINT v = 0; v < ranges[1].NumDescriptors; v++) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{DXGI_FORMAT_R32_UINT};
      if (v == 0) {
        uav.ViewDimension = d == 0 ? D3D12_UAV_DIMENSION_TEXTURE1D : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (d == 0)
          uav.Texture1D = {0};
        else
          uav.Texture2D = {0};
      } else {
        uav.ViewDimension = d == 0 ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        if (d == 0)
          uav.Texture1DArray = {array_mip, first_slice, view_layers};
        else
          uav.Texture2DArray = {array_mip, first_slice, view_layers};
      }
      device->CreateUnorderedAccessView(texture[v].Get(), nullptr, &uav, handle);
      handle.ptr += increment;
    }

    const UINT64 results_bytes = (2 + view_layers) * mips * sizeof(UINT);
    auto results = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, results_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, results_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(!!results && !!readback, "shader results and readback buffers"))
      return verdict();
    CHECK(forget(readback.Get()));

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)
    ));
    for (UINT t = 0; t < std::size(texture); t++) {
      for (UINT sub = 0; sub < footprints[t].size(); sub++) {
        D3D12_TEXTURE_COPY_LOCATION dst{
            texture[t].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}},
            src{upload[t].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[t][sub]}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      transition(list.Get(), texture[t].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    list->SetDescriptorHeaps(1, srvs.GetAddressOf());
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, results->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, results_bytes);
    for (auto &tex : texture)
      transition(list.Get(), tex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto gpu_handle = srvs->GetGPUDescriptorHandleForHeapStart();
    gpu_handle.ptr += ranges[0].NumDescriptors * increment;
    list->SetComputeRootDescriptorTable(2, gpu_handle);
    list->SetPipelineState(writes.Get());
    list->Dispatch(1, 1, 1);
    for (UINT t = 0; t < std::size(texture); t++) {
      transition(list.Get(), texture[t].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT sub = 0; sub < footprints[t].size(); sub++) {
        D3D12_TEXTURE_COPY_LOCATION src{
            texture[t].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}},
            dst{tex_readback[t].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                {.PlacedFootprint = footprints[t][sub]}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
    }
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    step("%uD SRV results, slice offsets and mips", d + 1);
    UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT v = 0; v < 2 + view_layers; v++)
      for (UINT m = 0; m < mips; m++) {
        UINT layer = v == 0 ? 0 : first_slice + (v == 1 ? 0 : v - 2);
        UINT want = base + layer * mips + m;
        expect(got[v * mips + m] == want, "SRV %u, layer %u, mip %u: %u, want %u", v, layer, m,
               got[v * mips + m], want);
      }
    readback->Unmap(0, nullptr);
    step("%uD UAV stores and untouched layers and mips, copied without a shader", d + 1);
    for (UINT t = 0; t < std::size(texture); t++) {
      char *mapped;
      CHECK(tex_readback[t]->Map(0, nullptr, (void **)&mapped));
      for (UINT sub = 0; sub < footprints[t].size(); sub++) {
        UINT layer = sub / mips, mip = sub % mips;
        auto &fp = footprints[t][sub];
        for (UINT y = 0; y < fp.Footprint.Height; y++)
          for (UINT x = 0; x < fp.Footprint.Width; x++) {
            UINT want = base + sub;
            if (t == 0 && mip == 0)
              want = written + y * side + x;
            if (t == 1 && layer >= first_slice && layer < first_slice + view_layers && mip == array_mip)
              want = written + (1 + layer - first_slice) * side * side + y * side + x;
            UINT value = reinterpret_cast<UINT *>(mapped + fp.Offset + y * fp.Footprint.RowPitch)[x];
            expect(value == want, "texture %u, layer %u, mip %u, (%u,%u): %u, want %u", t, layer, mip, x, y,
                   value, want);
          }
      }
      tex_readback[t]->Unmap(0, nullptr);
    }
  }
  return verdict();
}
