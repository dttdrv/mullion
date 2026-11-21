// contract: RGB32 texture copies retain all 96 bits per texel; CPU subresource methods refuse 12-byte elements.
// "but the raw value of memory happens to be equal" (D3D11.3 5.6.2); CopyTextureRegion performs no conversion.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion
// expected bytes are the supplied data, and footprint pitches come from GetCopyableFootprints. a typed buffer
// view's out-of-bounds ld "returns 0 in all components present in the format of the resource, and the defaults
// (0,0,0,1)" (D3D11.3 22.4.7, ld2dms), and Buffer is required of R32G32B32 (Format support for Direct3D Feature
// Level 11.0 hardware).
// DirectX-Specs ResourceHeaps, Map and Unmap / Strip-mine CPU copy operations, excludes element sizes that are
// not powers of two from opaque Map and CPU subresource copies.
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <bit>

static const char hlsl[] = R"hlsl(
#if DIM == 0
Buffer<TYPE> tex : register(t0);
#elif DIM == 1
Texture1DArray<TYPE> tex : register(t0);
#elif DIM == 2
Texture2DArray<TYPE> tex : register(t0);
#else
Texture3D<TYPE> tex : register(t0);
#endif
RWStructuredBuffer<uint4> result : register(u0);
cbuffer Layout : register(b0) { uint width, height, mip, slice, base; };
[numthreads(1,1,1)] void cs(uint3 id : SV_DispatchThreadID) {
#if DIM == 0
  // each of these loads takes some components alone: green, then red and alpha, then blue
  TYPE value = tex.Load(id.x);
  value.xw = tex.Load(id.x + mip).xw;
  value.z = tex.Load(id.x + 2 * mip).z;
#elif DIM == 1
  TYPE value = tex.Load(int3(id.x,slice,mip));
#elif DIM == 2
  TYPE value = tex.Load(int4(id.xy,slice,mip));
#else
  TYPE value = tex.Load(int4(id,mip));
#endif
  result[base + (id.z * height + id.y) * width + id.x] = asuint(value);
}
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // dimension 0 is a typed buffer
  std::string shaders[4][3];
  const char *types[] = {"float4", "uint4", "int4"};
  for (UINT dimension = 0; dimension < std::size(shaders); dimension++)
    for (UINT kind = 0; kind < std::size(types); kind++) {
      shaders[dimension][kind] = compiler.compile(
          hlsl, "cs", "cs", {"DIM=" + std::to_string(dimension), std::string("TYPE=") + types[kind]});
      if (shaders[dimension][kind].empty()) {
        printf("failed: RGB32 dimension %u type %s HLSL compilation\n", dimension, types[kind]);
        return 1;
      }
    }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[0].DescriptorTable = {1, &range};
  params[2].Constants = {0, 0, 5};
  auto root = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(!!root, "RGB32 root signature"))
    return verdict();
  for (auto format : {DXGI_FORMAT_R32G32B32_TYPELESS, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32_UINT,
                      DXGI_FORMAT_R32G32B32_SINT}) {
    const auto typed = format == DXGI_FORMAT_R32G32B32_TYPELESS ? DXGI_FORMAT_R32G32B32_UINT : format;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{typed};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    for (auto flag : {D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}) {
      if (flag == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
        continue;
      D3D12_RESOURCE_DESC invalid{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,   5, 3, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,       flag};
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> rejected;
      const auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &invalid,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rejected));
      step("RGB32 format %u unsupported binding %u", format, flag);
      expect(hr == E_INVALIDARG && !rejected, "unsupported RGB32 binding returned %#x", unsigned(hr));
    }
    for (UINT count : {1u, 31u, 32u, 33u}) {
      if (format == DXGI_FORMAT_R32G32B32_TYPELESS)
        break;
      step("RGB32 format %u typed buffer of %u elements: ld, its first element and past its last", format, count);
      const UINT texel = sizeof(std::array<UINT, 3>), kind = format == DXGI_FORMAT_R32G32B32_FLOAT ? 0
                                                             : format == DXGI_FORMAT_R32G32B32_SINT ? 2
                                                                                                    : 1;
      // one element before the view's first and one after its last, which it must not read
      std::vector<UINT> words(3 * (count + 2));
      for (UINT i = 0; i < words.size(); i++)
        words[i] = kind == 0 ? std::bit_cast<UINT>(float(i + 17)) : kind == 2 ? UINT(-int(i + 17)) : i + 17;
      std::vector<std::array<UINT, 4>> want(count + 1);
      const UINT one = kind == 0 ? std::bit_cast<UINT>(1.0f) : 1;
      for (UINT i = 0; i < count; i++)
        want[i] = {words[3 * (i + 1)], words[3 * (i + 1) + 1], words[3 * (i + 1) + 2], one};
      want[count] = {0, 0, 0, one};
      auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, words.size() * sizeof(UINT),
                           D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, want.size() * sizeof(want[0]),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, want.size() * sizeof(want[0]),
                             D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(source && output && readback, "RGB32 typed buffer resources"))
        return verdict();
      void *mapped;
      CHECK(source->Map(0, nullptr, &mapped));
      memcpy(mapped, words.data(), words.size() * sizeof(UINT));
      source->Unmap(0, nullptr);
      D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
      ComPtr<ID3D12DescriptorHeap> descriptors;
      CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors)));
      D3D12_SHADER_RESOURCE_VIEW_DESC view{format, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
      view.Buffer = {1, count};
      device->CreateShaderResourceView(source.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());
      D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{root.Get(), bytecode(shaders[0][kind])};
      ComPtr<ID3D12PipelineState> pso;
      CHECK(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      list->SetComputeRootSignature(root.Get());
      auto heap = descriptors.Get();
      list->SetDescriptorHeaps(1, &heap);
      list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
      list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
      UINT layout[] = {count + 1, 1, 0, 0, 0};
      list->SetComputeRoot32BitConstants(2, std::size(layout), layout, 0);
      list->Dispatch(count + 1, 1, 1);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyResource(readback.Get(), output.Get());
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, &mapped));
      std::vector<std::array<UINT, 4>> got(want.size());
      memcpy(got.data(), mapped, got.size() * sizeof(got[0]));
      readback->Unmap(0, nullptr);
      for (UINT i = 0; i < want.size(); i++)
        if (!expect(got[i] == want[i], "element %u is %#x %#x %#x %#x, want %#x %#x %#x %#x", i, got[i][0], got[i][1],
                    got[i][2], got[i][3], want[i][0], want[i][1], want[i][2], want[i][3]))
          break;
    }
    for (auto dimension :
         {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_DIMENSION_TEXTURE3D})
      for (UINT width : {1u, 5u, 31u, 32u, 33u}) {
        const UINT height = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 3;
        const UINT depth = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 3 : 1;
        const UINT slices = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : 2;
        const UINT mips = std::bit_width(width | height | depth), count = mips * slices;
        const UINT texel = sizeof(std::array<UINT, 3>);
        D3D12_RESOURCE_DESC desc{dimension, 0, width, height, UINT16(depth * slices), UINT16(mips), format, {1, 0}};
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> texture, other;
        CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&texture)));
        auto other_desc = desc;
        other_desc.Format =
            format == DXGI_FORMAT_R32G32B32_FLOAT ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT;
        CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &other_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&other)));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        UINT64 total;
        device->GetCopyableFootprints(&desc, 0, count, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, footprints.data(),
                                      nullptr, nullptr, &total);
        total += footprints.front().Offset;
        auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(upload && readback, "RGB32 transfer buffers"))
          return verdict();
        uint8_t *mapped;
        CHECK(upload->Map(0, nullptr, (void **)&mapped));
        std::vector<uint8_t> expected(total);
        for (UINT64 b = 0; b < total; b++)
          expected[b] = uint8_t(b * 29 + b / texel + 17);
        for (auto &at : footprints)
          for (UINT z = 0; z < at.Footprint.Depth; z++)
            for (UINT y = 0; y < at.Footprint.Height; y++)
              for (UINT x = 0; x < at.Footprint.Width; x++)
                for (UINT c = 0; c < 3; c++) {
                  UINT value = 17 + (z * at.Footprint.Height + y) * at.Footprint.Width + x + c;
                  if (format == DXGI_FORMAT_R32G32B32_FLOAT)
                    value = std::bit_cast<UINT>(float(value));
                  if (format == DXGI_FORMAT_R32G32B32_SINT)
                    value = UINT(-int(value));
                  const UINT64 offset = at.Offset + (z * at.Footprint.Height + y) * at.Footprint.RowPitch + x * texel;
                  memcpy(expected.data() + offset + c * sizeof(value), &value, sizeof(value));
                }
        memcpy(mapped, expected.data(), expected.size());
        upload->Unmap(0, nullptr);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, total);
        for (UINT sub = 0; sub < count; sub++) {
          D3D12_TEXTURE_COPY_LOCATION dst{
              texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
          D3D12_TEXTURE_COPY_LOCATION src{
              upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[sub]}};
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(other.Get(), texture.Get());
        transition(list.Get(), other.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT sub = 0; sub < count; sub++) {
          D3D12_TEXTURE_COPY_LOCATION src{
              other.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
          D3D12_TEXTURE_COPY_LOCATION dst{
              readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprints[sub]}};
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        step("RGB32 format %u dimension %u width %u: footprint, mips, slices, CopyResource", format, dimension, width);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(readback->Map(0, nullptr, (void **)&mapped));
        bool same = !memcmp(mapped, expected.data(), expected.size());
        readback->Unmap(0, nullptr);
        if (!expect(same, "RGB32 footprint lost bytes"))
          return verdict();

        step("RGB32 format %u dimension %u width %u: shader ld and default alpha", format, dimension, width);
        UINT texels = 0;
        std::vector<std::array<UINT, 4>> loaded;
        for (auto &at : footprints)
          for (UINT z = 0; z < at.Footprint.Depth; z++)
            for (UINT y = 0; y < at.Footprint.Height; y++)
              for (UINT x = 0; x < at.Footprint.Width; x++) {
                std::array<UINT, 4> value{};
                memcpy(value.data(),
                       expected.data() + at.Offset + (z * at.Footprint.Height + y) * at.Footprint.RowPitch + x * texel,
                       texel);
                value.back() = format == DXGI_FORMAT_R32G32B32_FLOAT ? std::bit_cast<UINT>(1.0f) : 1;
                loaded.push_back(value);
              }
        auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, loaded.size() * sizeof(loaded[0]),
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto shader_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, loaded.size() * sizeof(loaded[0]),
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(output && shader_readback, "RGB32 shader buffers"))
          return verdict();
        const UINT kind = format == DXGI_FORMAT_R32G32B32_FLOAT ? 0 : format == DXGI_FORMAT_R32G32B32_SINT ? 2 : 1;
        const UINT dim = dimension - D3D12_RESOURCE_DIMENSION_BUFFER;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{root.Get(), bytecode(shaders[dim][kind])};
        ComPtr<ID3D12PipelineState> pso;
        CHECK(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        ComPtr<ID3D12DescriptorHeap> descriptors;
        CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptors)));
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format == DXGI_FORMAT_R32G32B32_TYPELESS ? DXGI_FORMAT_R32G32B32_UINT : format;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
          view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
          view.Texture1DArray.MipLevels = mips;
          view.Texture1DArray.ArraySize = slices;
        } else if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
          view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          view.Texture2DArray.MipLevels = mips;
          view.Texture2DArray.ArraySize = slices;
        } else {
          view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          view.Texture3D.MipLevels = mips;
        }
        device->CreateShaderResourceView(texture.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootSignature(root.Get());
        auto descriptor_heap = descriptors.Get();
        list->SetDescriptorHeaps(1, &descriptor_heap);
        list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
        for (UINT sub = 0; sub < count; sub++) {
          auto &at = footprints[sub];
          UINT layout[] = {at.Footprint.Width, at.Footprint.Height, sub % mips, sub / mips, texels};
          list->SetComputeRoot32BitConstants(2, std::size(layout), layout, 0);
          list->Dispatch(at.Footprint.Width, at.Footprint.Height, at.Footprint.Depth);
          texels += at.Footprint.Width * at.Footprint.Height * at.Footprint.Depth;
        }
        transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(shader_readback.Get(), output.Get());
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(shader_readback->Map(0, nullptr, (void **)&mapped));
        same = !memcmp(mapped, loaded.data(), loaded.size() * sizeof(loaded[0]));
        shader_readback->Unmap(0, nullptr);
        if (!expect(same, "RGB32 shader ld changed RGB or the default alpha"))
          return verdict();

        step("RGB32 format %u dimension %u width %u: offset regions and untouched buffer bytes", format, dimension,
             width);
        CHECK(forget(readback.Get()));
        std::vector<uint8_t> region_expected(total, 0xff);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        transition(list.Get(), other.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT sub = 0; sub < count; sub++) {
          auto &at = footprints[sub];
          D3D12_BOX box{at.Footprint.Width - 1, at.Footprint.Height - 1, at.Footprint.Depth - 1,
                        at.Footprint.Width,     at.Footprint.Height,     at.Footprint.Depth};
          D3D12_TEXTURE_COPY_LOCATION src{
              upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = at}};
          D3D12_TEXTURE_COPY_LOCATION dst{
              texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT sub = 0; sub < count; sub++) {
          auto &at = footprints[sub];
          D3D12_BOX first{0, 0, 0, 1, 1, 1};
          D3D12_TEXTURE_COPY_LOCATION src{
              texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
          D3D12_TEXTURE_COPY_LOCATION dst{
              other.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
          list->CopyTextureRegion(&dst, at.Footprint.Width / 2, at.Footprint.Height / 2, at.Footprint.Depth / 2, &src,
                                  &first);
        }
        transition(list.Get(), other.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT sub = 0; sub < count; sub++) {
          auto &at = footprints[sub];
          D3D12_BOX middle{at.Footprint.Width / 2,     at.Footprint.Height / 2,     at.Footprint.Depth / 2,
                           at.Footprint.Width / 2 + 1, at.Footprint.Height / 2 + 1, at.Footprint.Depth / 2 + 1};
          D3D12_TEXTURE_COPY_LOCATION src{
              other.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = sub}};
          D3D12_TEXTURE_COPY_LOCATION dst{
              readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = at}};
          list->CopyTextureRegion(&dst, at.Footprint.Width - 1, at.Footprint.Height - 1, at.Footprint.Depth - 1, &src,
                                  &middle);
          const UINT64 last =
              at.Offset +
              ((at.Footprint.Depth - 1) * at.Footprint.Height + at.Footprint.Height - 1) * at.Footprint.RowPitch +
              (at.Footprint.Width - 1) * texel;
          memcpy(region_expected.data() + last, expected.data() + last, texel);
        }
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(readback->Map(0, nullptr, (void **)&mapped));
        same = !memcmp(mapped, region_expected.data(), region_expected.size());
        readback->Unmap(0, nullptr);
        if (!expect(same, "RGB32 region lost bytes or overwrote its border"))
          return verdict();

        D3D12_HEAP_PROPERTIES cpu{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
        ComPtr<ID3D12Resource> cpu_texture;
        auto cpu_desc = desc;
        cpu_desc.MipLevels = 1;
        CHECK(device->CreateCommittedResource(&cpu, D3D12_HEAP_FLAG_NONE, &cpu_desc, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&cpu_texture)));
        for (UINT sub = 0; sub < slices; sub++) {
          auto &at = footprints[sub * mips];
          for (bool boxed : {false, true}) {
            D3D12_BOX box{at.Footprint.Width - 1, at.Footprint.Height - 1, at.Footprint.Depth - 1,
                          at.Footprint.Width,     at.Footprint.Height,     at.Footprint.Depth};
            const UINT wide = boxed ? 1 : at.Footprint.Width, high = boxed ? 1 : at.Footprint.Height,
                       deep = boxed ? 1 : at.Footprint.Depth, row = wide * texel + sizeof(UINT),
                       image = row * high + sizeof(UINT);
            std::vector<uint8_t> source(image * deep), destination(source.size(), 0);
            for (UINT b = 0; b < source.size(); b++)
              source[b] = uint8_t(b * 13 + sub + 201);
            step("RGB32 format %u dimension %u width %u sub %u box %u: CPU read/write", format, dimension, width, sub,
                 boxed);
            HRESULT mapped_cpu = cpu_texture->Map(sub, nullptr, nullptr);
            expect(FAILED(mapped_cpu), "RGB32 opaque Map accepts a non-power-of-two element size");
            expect(FAILED(cpu_texture->WriteToSubresource(sub, boxed ? &box : nullptr, source.data(), row, image)),
                   "RGB32 CPU write accepts a non-power-of-two element size");
            expect(FAILED(cpu_texture->ReadFromSubresource(destination.data(), row, image, sub, boxed ? &box : nullptr)),
                   "RGB32 CPU read accepts a non-power-of-two element size");
            if (SUCCEEDED(mapped_cpu))
              cpu_texture->Unmap(sub, nullptr);
          }
        }
      }
  }
  return verdict();
}
