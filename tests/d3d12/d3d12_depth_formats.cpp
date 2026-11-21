// contract: advertised 1D/2D depth formats create usable targets, whose copied planes retain depth and stencil.
// "If c > 1.0f, including INF, it is clamped to 1.0f. If c < 0.0f, including -INF, it is clamped to 0.0f."
// "This conversion is permitted tolerance of 0.6f ULP" (D3D11.3 3.2.3.6, integer side). "The full extent of the
// resource view is always cleared." (5.2.3.1). format component widths/order follow 19.1.3.1; depth and stencil
// occupy distinct copy subresources in D3D12 (Microsoft Learn, Subresources, Plane slice). the feature-level 11
// format table requires 1D D16/D24 targets as well as 2D. SDK formats and queried planes determine copy counts;
// expected bytes come from depth quantization, not the returned footprint's interpretation.
// DXGI_SAMPLE_DESC: "The valid range is between zero and one less than the level returned" by the quality query;
// standard/center patterns are the two named exceptions (D3D11.3 19.2.4). nontrivial SampleDesc values
// require Texture2D (Microsoft Learn, D3D12_RESOURCE_DESC, SampleDesc).
// https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ns-dxgicommon-dxgi_sample_desc
// ResourceHeaps, Map and Unmap: "No depth-stencil resources" applies even on a CPU-visible custom heap.
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/subresources
// "Drivers must initialize X8 bits to zero when copying to planar depth placed texture data."
// https://microsoft.github.io/DirectX-Specs/d3d/PlanarDepthStencilDDISpec.html, R24G8 depth plane.
// plane SRVs obey "defaults for missing components are 0 for any component except the A component,
// whose default is 1" (D3D11.3 19.1.3.3), with depth in R and stencil in G. UNORM codes are divided
// by 2^n-1 (3.2.3.5); UINT widens with zero padding (3.2.3.12). shader loads are checked against the
// separately checked copy bytes for every mip/slice/plane, through direct and indirect dispatch.
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include "../depth_readback.hpp"
#include "../depth_values.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>

static const char hlsl[] = R"hlsl(
cbuffer Shape : register(b0) { uint wide; uint high; uint mip; uint layer; uint offset; };
#if DIMENSION == 1
Texture1DArray<TYPE> tex : register(t0);
#else
Texture2DArray<TYPE> tex : register(t0);
#endif
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1,1,1)] void cs(uint2 at : SV_DispatchThreadID) {
  if (at.x >= wide || at.y >= high) return;
#if DIMENSION == 1
  result[offset + at.x] = asuint(tex.Load(int3(at.x,layer,mip)));
#else
  result[offset + at.y * wide + at.x] = asuint(tex.Load(int4(at,layer,mip)));
#endif
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  std::string codes[2][2];
  for (UINT dim = 0; dim < std::size(codes); dim++)
    for (UINT plane = 0; plane < std::size(codes[dim]); plane++) {
      codes[dim][plane] = compiler.compile(
          hlsl, "cs", "cs",
          {"DIMENSION=" + std::to_string(dim + 1), std::string("TYPE=") + (plane ? "uint4" : "float4")}
      );
      if (!expect(!codes[dim][plane].empty(), "depth plane HLSL did not compile"))
        return verdict();
    }
  UINT group[3]{};
  if (!expect(depth_readback_group(group), "could not read the depth readback threadgroup"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER parameters[] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  parameters[0].DescriptorTable = {1, &range};
  parameters[2].Constants = {0, 0, 5};
  auto root = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  if (!expect(bool(root), "depth root signature was not created"))
    return verdict();
  ComPtr<ID3D12PipelineState> pipelines[2][2];
  for (UINT dim = 0; dim < std::size(pipelines); dim++)
    for (UINT plane = 0; plane < std::size(pipelines[dim]); plane++) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(codes[dim][plane])};
      CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipelines[dim][plane])));
    }
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list, bundle;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)
  ));
  CHECK(list->Close());
  CHECK(bundle->Close());
  for (const auto &f : depth_values::formats()) {
    const auto format = f.id;
    const auto *name = f.name.c_str();
    const auto depth_bits = f.depth_bits, stencil_bits = f.stencil_bits, unused_bits = f.unused_bits;
    const bool floating = f.floating, stencil = stencil_bits != 0;
    const auto parent = f.parent;
    const auto &plane_formats = f.planes;
    if (!expect(
            parent != DXGI_FORMAT_UNKNOWN && plane_formats[0] != DXGI_FORMAT_UNKNOWN &&
                (!stencil || plane_formats[1] != DXGI_FORMAT_UNKNOWN),
            "depth family names were not found in the SDK"
        ))
      return verdict();
    for (UINT samples = 0; samples <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT + 1; samples++) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, samples};
      if (samples && samples <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT)
        CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
      std::vector<UINT> qualities{
          0, quality.NumQualityLevels, quality.NumQualityLevels + (quality.NumQualityLevels != UINT_MAX)};
      if (quality.NumQualityLevels > 1)
        qualities.push_back(quality.NumQualityLevels - 1);
      if (quality.NumQualityLevels) {
        qualities.push_back(DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN);
        qualities.push_back(DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN);
      }
      for (auto dimension : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D})
        for (UINT q : qualities)
          for (bool validate_only : {false, true}) {
            step("%s %uD samples %u quality %#x, %s", name, dimension - D3D12_RESOURCE_DIMENSION_BUFFER,
                 samples, q, validate_only ? "validation" : "allocation");
            D3D12_RESOURCE_DESC td{dimension, 0, 1, 1, 1, 1, format, {samples, q},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
            D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
            ComPtr<ID3D12Resource> target;
            const bool valid = (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D || (samples == 1 && !q)) &&
                               quality.NumQualityLevels &&
                               (q < quality.NumQualityLevels || q == DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN ||
                                q == DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN);
            HRESULT hr = device->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                __uuidof(ID3D12Resource), validate_only ? nullptr : reinterpret_cast<void **>(target.GetAddressOf()));
            const HRESULT wanted = !valid ? E_INVALIDARG : validate_only ? S_FALSE : S_OK;
            expect(hr == wanted && bool(target) == (valid && !validate_only),
                   "multisample creation %08lx, want %08lx", hr, wanted);
          }
    }
    D3D12_HEAP_PROPERTIES custom;
    device->GetCustomHeapProperties(&custom, 0, D3D12_HEAP_TYPE_READBACK);
    for (auto dimension : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D}) {
      step("%s %uD: depth CPU access is forbidden", name, dimension - D3D12_RESOURCE_DIMENSION_BUFFER);
      D3D12_RESOURCE_DESC td{dimension, 0, 1, 1, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
      ComPtr<ID3D12Resource> texture;
      CHECK(device->CreateCommittedResource(
          &custom, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
      HRESULT mapped = texture->Map(0, nullptr, nullptr);
      expect(FAILED(mapped), "depth texture accepts opaque Map");
      if (SUCCEEDED(mapped))
        texture->Unmap(0, nullptr);
    }
    const UINT packed_bytes = (depth_bits + stencil_bits + unused_bits) / CHAR_BIT;
    const UINT page_texels = MULLION_PAGE_SIZE / packed_bytes;
    const UINT widths[] = {
        1, std::max(1u, group[0] - 1), group[0], group[0] + 1, page_texels - 1, page_texels, page_texels + 1
    };
    const UINT heights[] = {1, std::max(1u, group[1] - 1), group[1], group[1] + 1};
    for (auto dimension : {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D})
      for (UINT variant = 0; variant < std::size(widths); variant++) {
        UINT width = widths[variant];
        UINT height = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : heights[variant % std::size(heights)];
        UINT mips = std::bit_width(width | height), slices = 2, per_plane = mips * slices;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT caps{format};
        CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &caps, sizeof(caps)));
        const UINT required = D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL |
                              (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                                                                               : D3D12_FORMAT_SUPPORT1_TEXTURE2D);
        step(
            "%s %uD %ux%u %u mips %u slices", name, dimension - D3D12_RESOURCE_DIMENSION_BUFFER, width, height, mips,
            slices
        );
        if (!expect((caps.Support1 & required) == required, "required depth storage is not advertised"))
          continue;
        D3D12_FEATURE_DATA_FORMAT_INFO info{format};
        CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)));
        if (!expect(info.PlaneCount == (stencil ? 2 : 1), "incorrect plane count %u", info.PlaneCount))
          continue;
        D3D12_RESOURCE_DESC td{
            dimension,
            0,
            width,
            height,
            UINT16(slices),
            UINT16(mips),
            format,
            {1, 0},
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
        };
        D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
        ComPtr<ID3D12Resource> texture;
        HRESULT hr = device->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&texture)
        );
        if (!expect(hr == S_OK && texture, "advertised depth target creation returned %08lx", hr))
          continue;
        auto readable_desc = td;
        readable_desc.Format = parent;
        ComPtr<ID3D12Resource> readable;
        CHECK(device->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &readable_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readable)
        ));
        ComPtr<ID3D12DescriptorHeap> shader_heap;
        D3D12_DESCRIPTOR_HEAP_DESC shd{
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, info.PlaneCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        };
        CHECK(device->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&shader_heap)));
        const UINT shader_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT plane = 0; plane < info.PlaneCount; plane++) {
          D3D12_FEATURE_DATA_FORMAT_SUPPORT plane_caps{plane_formats[plane]};
          CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &plane_caps, sizeof(plane_caps)));
          expect(
              plane_caps.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD,
              "required depth/stencil plane load not advertised"
          );
          D3D12_SHADER_RESOURCE_VIEW_DESC vd{
              plane_formats[plane],
              dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY
                                                              : D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
          };
          if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
            vd.Texture1DArray = {0, mips, 0, slices};
          else
            vd.Texture2DArray = {0, mips, 0, slices, plane};
          auto handle = shader_heap->GetCPUDescriptorHandleForHeapStart();
          handle.ptr += plane * shader_increment;
          device->CreateShaderResourceView(readable.Get(), &vd, handle);
        }
        std::vector<UINT> offsets(per_plane);
        UINT texels = 0;
        std::vector<D3D12_DISPATCH_ARGUMENTS> arguments(per_plane);
        for (UINT sub = 0; sub < per_plane; sub++) {
          offsets[sub] = texels;
          arguments[sub] = {std::max(1u, width >> (sub % mips)), std::max(1u, height >> (sub % mips)), 1};
          texels += arguments[sub].ThreadGroupCountX * arguments[sub].ThreadGroupCountY;
        }
        const UINT64 shader_bytes = UINT64(texels) * info.PlaneCount * 4 * sizeof(UINT);
        auto shader_output = buffer(
            device.Get(), D3D12_HEAP_TYPE_DEFAULT, shader_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        );
        auto shader_readback =
            buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, shader_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        auto poison = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, shader_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto indirect = buffer(
            device.Get(), D3D12_HEAP_TYPE_UPLOAD, arguments.size() * sizeof(arguments[0]),
            D3D12_RESOURCE_STATE_GENERIC_READ
        );
        if (!expect(shader_output && shader_readback && poison && indirect, "plane shader buffers were not created"))
          return verdict();
        void *mapped;
        CHECK(poison->Map(0, nullptr, &mapped));
        memset(mapped, ~0, shader_bytes);
        poison->Unmap(0, nullptr);
        CHECK(indirect->Map(0, nullptr, &mapped));
        memcpy(mapped, arguments.data(), arguments.size() * sizeof(arguments[0]));
        indirect->Unmap(0, nullptr);
        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, per_plane};
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const UINT total_subresources = per_plane * info.PlaneCount;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(total_subresources);
        UINT64 total;
        device->GetCopyableFootprints(&td, 0, total_subresources, 0, footprints.data(), nullptr, nullptr, &total);
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(bool(readback), "depth readback buffer was not created"))
          return verdict();
        auto state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        auto readable_state = D3D12_RESOURCE_STATE_COPY_DEST;
        auto output_state = D3D12_RESOURCE_STATE_COPY_DEST;
        const float above_quarter = std::nextafter(0.25f, 1.0f);
        const float depths[] = {0, -0.0f, 0.25f, above_quarter, std::nextafter(above_quarter, 1.0f), 0.5f, 0.75f, 1};
        for (UINT phase = 0; phase < std::size(depths); phase++)
          for (UINT path = 0; path < 3; path++) {
            step(
                "%s %uD width %u phase %u: clear all mips/slices and copy all planes", name,
                dimension - D3D12_RESOURCE_DIMENSION_BUFFER, width, phase
            );
            CHECK(forget(readback.Get()));
            CHECK(forget(shader_readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), nullptr));
            if (state != D3D12_RESOURCE_STATE_DEPTH_WRITE)
              transition(list.Get(), texture.Get(), state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            for (UINT sub = 0; sub < per_plane; sub++) {
              D3D12_DEPTH_STENCIL_VIEW_DESC vd{
                  format, dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? D3D12_DSV_DIMENSION_TEXTURE1DARRAY
                                                                          : D3D12_DSV_DIMENSION_TEXTURE2DARRAY
              };
              if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
                vd.Texture1DArray = {sub % mips, sub / mips, 1};
              else
                vd.Texture2DArray = {sub % mips, sub / mips, 1};
              auto handle = heap->GetCPUDescriptorHandleForHeapStart();
              handle.ptr += sub * increment;
              device->CreateDepthStencilView(texture.Get(), &vd, handle);
              D3D12_RECT rect{0, 0, LONG(std::max(1u, width >> (sub % mips))),
                              LONG(std::max(1u, height >> (sub % mips)))};
              list->ClearDepthStencilView(
                  handle, D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0)),
                  depths[(phase + sub) % std::size(depths)],
                  UINT8((phase * per_plane + sub) * UINT8_MAX / (std::size(depths) * per_plane - 1)),
                  path == 1 ? 1 : 0, path == 1 ? &rect : nullptr
              );
            }
            transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            for (UINT sub = 0; sub < total_subresources; sub++) {
              D3D12_TEXTURE_COPY_LOCATION from{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                  to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
              from.SubresourceIndex = sub;
              to.PlacedFootprint = footprints[sub];
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            if (readable_state != D3D12_RESOURCE_STATE_COPY_DEST)
              transition(list.Get(), readable.Get(), readable_state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(readable.Get(), texture.Get());
            transition(
                list.Get(), readable.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            readable_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (output_state != D3D12_RESOURCE_STATE_COPY_DEST)
              transition(list.Get(), shader_output.Get(), output_state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(shader_output.Get(), 0, poison.Get(), 0, shader_bytes);
            transition(
                list.Get(), shader_output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
            ID3D12DescriptorHeap *heaps[] = {shader_heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            if (path == 2) {
              CHECK(bundle_allocator->Reset());
              CHECK(bundle->Reset(bundle_allocator.Get(), nullptr));
              bundle->SetDescriptorHeaps(1, heaps);
            }
            auto commands = path == 2 ? bundle.Get() : list.Get();
            commands->SetComputeRootSignature(root.Get());
            commands->SetComputeRootUnorderedAccessView(1, shader_output->GetGPUVirtualAddress());
            for (UINT plane = 0; plane < info.PlaneCount; plane++) {
              commands->SetPipelineState(pipelines[dimension - D3D12_RESOURCE_DIMENSION_TEXTURE1D][plane].Get());
              auto handle = shader_heap->GetGPUDescriptorHandleForHeapStart();
              handle.ptr += plane * shader_increment;
              commands->SetComputeRootDescriptorTable(0, handle);
              for (UINT sub = 0; sub < per_plane; sub++) {
                const UINT constants[] = {
                    arguments[sub].ThreadGroupCountX, arguments[sub].ThreadGroupCountY, sub % mips, sub / mips,
                    plane * texels + offsets[sub]
                };
                commands->SetComputeRoot32BitConstants(2, UINT(std::size(constants)), constants, 0);
                if (path == 1)
                  commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), sub * sizeof(arguments[0]), nullptr, 0);
                else
                  commands->Dispatch(arguments[sub].ThreadGroupCountX, arguments[sub].ThreadGroupCountY, 1);
              }
            }
            if (path == 2) {
              CHECK(bundle->Close());
              list->ExecuteBundle(bundle.Get());
            }
            transition(
                list.Get(), shader_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            output_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->CopyResource(shader_readback.Get(), shader_output.Get());
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            void *data;
            CHECK(readback->Map(0, nullptr, &data));
            void *shader_data;
            CHECK(shader_readback->Map(0, nullptr, &shader_data));
            bool same = true;
            for (UINT sub = 0; same && sub < total_subresources; sub++) {
              const UINT local = sub % per_plane, mip = local % mips, plane = sub / per_plane;
              const UINT texel_bytes = plane ? sizeof(UINT8) : depth_bits / CHAR_BIT;
              double want = depths[(phase + local) % std::size(depths)];
              for (UINT y = 0; same && y < std::max(1u, height >> mip); y++)
                for (UINT x = 0; x < std::max(1u, width >> mip); x++) {
                  const auto *at = static_cast<const uint8_t *>(data) + footprints[sub].Offset +
                                   y * footprints[sub].Footprint.RowPitch +
                                   x * (depth_bits == 24 && !plane ? sizeof(UINT) : texel_bytes);
                  bool correct;
                  if (plane)
                    correct =
                        *at == UINT8((phase * per_plane + local) * UINT8_MAX / (std::size(depths) * per_plane - 1));
                  else if (floating) {
                    float have;
                    memcpy(&have, at, sizeof(have));
                    correct = have == float(want);
                  } else {
                    UINT have = 0;
                    memcpy(&have, at, texel_bytes);
                    correct = std::fabs(double(have) - want * ((uint64_t(1) << depth_bits) - 1)) <= 0.6;
                    if (depth_bits == 24)
                      correct &= at[depth_bits / CHAR_BIT] == 0;
                  }
                  if (!correct) {
                    expect(false, "subresource %u plane %u texel %u,%u has incorrect depth/stencil", sub, plane, x, y);
                    same = false;
                    break;
                  }
                  UINT expected[] = {0, 0, 0, plane ? 1u : std::bit_cast<UINT>(1.0f)};
                  if (plane)
                    expected[1] = *at;
                  else if (floating)
                    memcpy(expected, at, sizeof(float));
                  else {
                    UINT code = 0;
                    memcpy(&code, at, texel_bytes);
                    expected[0] = std::bit_cast<UINT>(float(double(code) / ((uint64_t(1) << depth_bits) - 1)));
                  }
                  const auto *pixel =
                      static_cast<const UINT *>(shader_data) +
                      (plane * texels + offsets[local] + y * std::max(1u, width >> mip) + x) * std::size(expected);
                  if (!expect(
                          !memcmp(pixel, expected, sizeof(expected)),
                          "path %u plane %u subresource %u texel %u,%u: shader %08x %08x %08x %08x, expected %08x %08x %08x %08x",
                          path, plane, local, x, y, pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1],
                          expected[2], expected[3]
                      )) {
                    same = false;
                    break;
                  }
                }
            }
            shader_readback->Unmap(0, nullptr);
            readback->Unmap(0, nullptr);
            if (!same)
              return verdict();
          }
      }
  }
  return verdict();
}
