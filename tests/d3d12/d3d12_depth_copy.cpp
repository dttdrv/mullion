// contract: a D24 depth footprint contains UNORM24 values, not float32 encodings; stencil is a separate plane.
// D3D11.3 3.2.3.5: "The starting n-bit value is converted to float (0.0f, 1.0f, 2.0f, etc.) and then divided by (2^n-1)."
// 3.2.3.2 requires "FULL IEEE-754 precision"; 5.6.2: "No stretch, color key, blend, nor format conversion."
// CopyTextureRegion requires the entire depth/stencil subresource and separates its planes (Microsoft Learn).
// full copies cross row-pitch boundaries, mips and arrays; shader loads distinguish a wrong physical encoding.
// planar staging requires zero X8 bits on input and output; Microsoft's footprint helper aligns planar rows to512.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion
// https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format
// https://microsoft.github.io/DirectX-Specs/d3d/PlanarDepthStencilDDISpec.html
// https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3dx12_resource_helpers.h
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>

static const char hlsl[] = R"hlsl(
Texture2DArray<float> depth_data : register(t0);
Texture2DArray<uint4> stencil_data : register(t1);
RWStructuredBuffer<uint2> observed : register(u0);
cbuffer parameters : register(b0) { uint width, height, mip, layer; };
[numthreads(THREADS,THREADS,1)] void cs(uint3 id : SV_DispatchThreadID) {
  if (id.x < width && id.y < height)
    observed[id.y * width + id.x] = uint2(asuint(depth_data.Load(int4(id.xy,layer,mip))),
                                         stencil_data.Load(int4(id.xy,layer,mip)).y);
}
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  const UINT threads = 8;
  auto shader = compiler.compile(hlsl, "cs", "cs", {"THREADS=" + std::to_string(threads)});
  if (!expect(!shader.empty(), "depth-copy shader did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2};
  D3D12_ROOT_PARAMETER params[3]{{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                 {D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[0].DescriptorTable = {1, &range};
  params[2].Constants = {0, 0, 4};
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(root != nullptr, "depth-copy root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> pipeline;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(shader)};
  CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC vd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, range.NumDescriptors,
                                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&vd, IID_PPV_ARGS(&views)));
  const UINT depth_bits = 24, maximum = (1u << depth_bits) - 1;
  const UINT mips = 3, layers = 2, subresources = mips * layers, planes = 2;
  const UINT row_boundary = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT / sizeof(UINT);
  auto value = [=](UINT width, UINT subresource, UINT x, UINT y) {
    const UINT id = x + y * width + subresource * (row_boundary + 1) * (row_boundary + 1);
    switch (id % 4) {
    case 0: return 1u << ((id / 4) % depth_bits);
    case 1: return maximum ^ (1u << ((id / 4) % depth_bits));
    case 2: return (id / 4) & 1 ? maximum : 0u;
    default: return (id * (maximum / row_boundary)) & maximum;
    }
  };
  for (UINT width : {row_boundary - 1, row_boundary, row_boundary + 1}) {
    step("D24 depth/stencil copies, width %u, %u mips, %u arrays", width, mips, layers);
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 5, layers, mips,
                           DXGI_FORMAT_R24G8_TYPELESS, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&texture)));
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, subresources * planes> at;
    UINT64 total;
    const UINT64 base = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    device->GetCopyableFootprints(&td, 0, at.size(), base, at.data(), nullptr, nullptr, &total);
    UINT64 end = base;
    for (UINT s = 0; s < at.size(); s++) {
      const UINT plane = s / subresources, mip = s % mips;
      const UINT w = std::max(1u, width >> mip), h = std::max(1u, td.Height >> mip);
      const UINT row_bytes = w * (plane ? sizeof(UINT8) : sizeof(UINT));
      const UINT pitch = (row_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                         D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
      const UINT64 offset = (end + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
      const auto &layout = at[s];
      if (!expect(layout.Offset == offset && layout.Footprint.Width == w && layout.Footprint.Height == h &&
                      layout.Footprint.Depth == 1 && layout.Footprint.RowPitch == pitch &&
                      layout.Footprint.Format == (plane ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS),
                  "unexpected footprint for subresource %u", s))
        return verdict();
      end = offset + UINT64(h - 1) * pitch + row_bytes;
    }
    if (!expect(total == end - base, "invalid depth/stencil footprint total %llu", total))
      return verdict();
    const UINT64 bytes = base + total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    std::vector<UINT8> expected(bytes, UINT8(~0u));
    UINT texels = 0;
    for (UINT s = 0; s < subresources; s++)
      texels += std::max(1u, width >> (s % mips)) * std::max(1u, td.Height >> (s % mips));
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, texels * 2 * sizeof(UINT),
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto values = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, texels * 2 * sizeof(UINT),
                          D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && back && output && values, "depth-copy buffers"))
      return verdict();
    for (UINT plane = 0; plane < planes; plane++)
      for (UINT s = 0; s < subresources; s++) {
        const auto &layout = at[s + plane * subresources];
        const UINT w = std::max(1u, width >> (s % mips)), h = std::max(1u, td.Height >> (s % mips));
        for (UINT y = 0; y < h; y++)
          for (UINT x = 0; x < w; x++) {
            const UINT raw = value(w, s, x, y), word_bytes = plane ? sizeof(UINT8) : sizeof(UINT);
            UINT64 offset = layout.Offset + UINT64(y) * layout.Footprint.RowPitch + x * word_bytes;
            for (UINT b = 0; b < word_bytes; b++)
              expected[offset + b] = UINT8(raw >> (b * 8));
          }
      }
    UINT8 *mapped;
    CHECK(upload->Map(0, nullptr, (void **)&mapped));
    memcpy(mapped, expected.data(), bytes);
    upload->Unmap(0, nullptr);
    CHECK(forget(back.Get()));
    for (UINT plane = 0; plane < planes; plane++) {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = plane ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2DArray.MipLevels = mips;
      srv.Texture2DArray.ArraySize = layers;
      srv.Texture2DArray.PlaneSlice = plane;
      auto handle = views->GetCPUDescriptorHandleForHeapStart();
      handle.ptr += plane * device->GetDescriptorHandleIncrementSize(vd.Type);
      device->CreateShaderResourceView(texture.Get(), &srv, handle);
    }
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pipeline.Get()));
    for (UINT s = 0; s < at.size(); s++) {
      D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                      {.SubresourceIndex = s}};
      D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                      {.PlacedFootprint = at[s]}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *heaps[]{views.Get()};
    list->SetDescriptorHeaps(std::size(heaps), heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    UINT texel = 0;
    for (UINT s = 0; s < subresources; s++) {
      UINT constants[]{std::max(1u, width >> (s % mips)), std::max(1u, td.Height >> (s % mips)), s % mips, s / mips};
      list->SetComputeRoot32BitConstants(2, std::size(constants), constants, 0);
      list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + texel * 2 * sizeof(UINT));
      list->Dispatch((constants[0] + threads - 1) / threads, (constants[1] + threads - 1) / threads, 1);
      texel += constants[0] * constants[1];
    }
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT s = 0; s < at.size(); s++) {
      D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                      {.SubresourceIndex = s}};
      D3D12_TEXTURE_COPY_LOCATION dst{back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                      {.PlacedFootprint = at[s]}};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(values.Get(), output.Get());
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(back->Map(0, nullptr, (void **)&mapped));
    bool same = true;
    for (UINT64 i = 0; i < bytes; i++)
      if (mapped[i] != expected[i]) {
        expect(false, "depth/stencil copied byte %llu: %u, want %u", i, mapped[i], expected[i]);
        same = false;
        break;
      }
    back->Unmap(0, nullptr);
    UINT *observed;
    CHECK(values->Map(0, nullptr, (void **)&observed));
    texel = 0;
    for (UINT s = 0; s < subresources && same; s++)
      for (UINT y = 0; y < std::max(1u, td.Height >> (s % mips)) && same; y++)
        for (UINT x = 0; x < std::max(1u, width >> (s % mips)); x++, texel++) {
          UINT raw = value(std::max(1u, width >> (s % mips)), s, x, y);
          float expected_depth = float(double(raw) / maximum), got;
          memcpy(&got, &observed[2 * texel], sizeof(got));
          if (!expect(got == expected_depth && observed[2 * texel + 1] == UINT8(raw),
                      "subresource %u (%u,%u): depth %.9g, want %.9g; stencil %u, want %u", s, x, y,
                      got, expected_depth, observed[2 * texel + 1], UINT8(raw))) {
            same = false;
            break;
          }
        }
    values->Unmap(0, nullptr);
    if (!same)
      return verdict();
  }
  return verdict();
}
