// contract: CPU texture copies preserve defined bytes, subresource selection and untouched regions.
// "Formats which don't have a power-of-two element size are also not supported and will cause a failure."
// (DirectX-Specs ResourceHeaps, Strip-mine CPU copy operations). these CPU methods require COMMON and a
// CPU-visible custom heap; opaque layouts use Map with null ppData and null/empty ranges. arrays and mips
// have independent contents. volumes permit one mip; row and slice pitches are independent. multi-mip
// volumes and nonnull opaque Map outputs must fail (ResourceHeaps, Map and Unmap).
// D3D11.3 19.5.1: "The regions of BC formats being sourced and/or modified by the Resource Manipulation(5.6)
// operations are required to be a multiple of 4." lower mips still occupy full physical blocks (19.5.1).
// null boxes copy the entire subresource; explicit boxes include the last physical block of each small mip.
// source layouts and defined bytes come from the SDK generator. CPU reads and an independent GPU footprint
// copy check each write, including padded rows, small mips and the bytes outside each updated box.
// https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-writetosubresource
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-readfromsubresource
#include "d3d12_test.hpp"
#include "../copy_formats.hpp"
#include <algorithm>
#include <bit>

int
main(int, char **) {
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_HEAP_PROPERTIES custom;
  device->GetCustomHeapProperties(&custom, 0, D3D12_HEAP_TYPE_READBACK);
  if (!expect(
          custom.Type == D3D12_HEAP_TYPE_CUSTOM && custom.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK &&
              custom.MemoryPoolPreference == D3D12_MEMORY_POOL_L0,
          "no queried CPU-cached system-memory heap"
      ))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  UINT tested = 0;
  for (const auto &f : copy_formats) {
    if (f.depth)
      continue;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT caps{f.id};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &caps, sizeof(caps))))
      continue;
    D3D12_FEATURE_DATA_FORMAT_INFO info{f.id};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)));
    if (info.PlaneCount != 1)
      continue;
    if (caps.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{f.id, 2};
      CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
      if (quality.NumQualityLevels) {
        step("%s: multisampled CPU access is forbidden", f.name);
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 2, 1, 1, f.id, {quality.SampleCount, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ComPtr<ID3D12Resource> texture;
        CHECK(device->CreateCommittedResource(
            &custom, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
        HRESULT mapped = texture->Map(0, nullptr, nullptr);
        expect(FAILED(mapped), "MSAA texture accepts opaque Map");
        if (SUCCEEDED(mapped))
          texture->Unmap(0, nullptr);
        std::vector<uint8_t> data(td.Width * td.Height * f.bytes);
        expect(FAILED(texture->WriteToSubresource(0, nullptr, data.data(), td.Width * f.bytes, 0)),
               "MSAA texture accepts CPU write");
        expect(FAILED(texture->ReadFromSubresource(data.data(), td.Width * f.bytes, 0, 0, nullptr)),
               "MSAA texture accepts CPU read");
      }
    }
    const UINT boundary = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / f.bytes;
    for (auto dimension :
         {D3D12_RESOURCE_DIMENSION_TEXTURE1D, D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
      const auto cap = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D   ? D3D12_FORMAT_SUPPORT1_TEXTURE1D
                       : dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? D3D12_FORMAT_SUPPORT1_TEXTURE2D
                                                                         : D3D12_FORMAT_SUPPORT1_TEXTURE3D;
      if (!(caps.Support1 & cap))
        continue;
      for (UINT blocks : {boundary - 1, boundary, boundary + 1})
        for (UINT padding : {0u, f.bytes}) {
          const UINT width = blocks * f.unit, height = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : f.unit * 3;
          const UINT depth = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 3 : 1, slices = depth > 1 ? depth : 2;
          const UINT mips = depth > 1 ? 1 : std::bit_width(width | height), subs = depth > 1 ? mips : mips * slices;
          D3D12_RESOURCE_DESC td{dimension, 0, width, height, UINT16(slices), UINT16(mips), f.id, {1, 0}};
          ComPtr<ID3D12Resource> texture;
          step(
              "%s CPU texture %uD %ux%ux%u, %u mips, %u arrays/depth, padding %u", f.name,
              dimension - D3D12_RESOURCE_DIMENSION_BUFFER, width, height, depth, mips, slices, padding
          );
          CHECK(device->CreateCommittedResource(
              &custom, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)
          ));
          std::vector<std::vector<uint8_t>> expected(subs);
          std::vector<UINT> pitches(subs), rows(subs), row_bytes(subs), slice_pitches(subs);
          for (UINT sub = 0; sub < subs; sub++) {
            const UINT mip = sub % mips, w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
            row_bytes[sub] = ((w + f.unit - 1) / f.unit) * f.bytes;
            rows[sub] = (h + f.unit - 1) / f.unit;
            pitches[sub] = row_bytes[sub] + padding;
            slice_pitches[sub] = pitches[sub] * rows[sub] + (depth > 1 ? padding : 0);
            expected[sub].resize(slice_pitches[sub] * depth);
            auto &want = expected[sub];
            for (UINT z = 0; z < depth; z++)
              for (UINT y = 0; y < rows[sub]; y++)
                for (UINT x = 0; x < pitches[sub]; x++)
                  want[z * slice_pitches[sub] + y * pitches[sub] + x] = uint8_t(sub + z + y * pitches[sub] + x);
            step("%s subresource %u: CPU upload and read", f.name, sub);
            HRESULT mapped = texture->Map(sub, nullptr, nullptr);
            if (!std::has_single_bit(f.bytes)) {
              expect(FAILED(mapped), "non-power-of-two format accepts opaque Map");
              expect(
                  FAILED(texture->WriteToSubresource(sub, nullptr, want.data(), pitches[sub], slice_pitches[sub])),
                  "non-power-of-two format accepts CPU write"
              );
              std::vector<uint8_t> got(want.size());
              expect(
                  FAILED(texture->ReadFromSubresource(got.data(), pitches[sub], slice_pitches[sub], sub, nullptr)),
                  "non-power-of-two format accepts CPU read"
              );
              if (SUCCEEDED(mapped))
                texture->Unmap(sub, nullptr);
              continue;
            }
            if (!expect(SUCCEEDED(mapped), "opaque Map failed: %08lx", mapped))
              return verdict();
            void *pointer = nullptr;
            HRESULT pointer_map = texture->Map(sub, nullptr, &pointer);
            expect(FAILED(pointer_map), "opaque Map exposes a CPU pointer");
            if (SUCCEEDED(pointer_map))
              texture->Unmap(sub, nullptr);
            const D3D12_RANGE empty{};
            CHECK(texture->Map(sub, &empty, nullptr));
            texture->Unmap(sub, nullptr);
            const D3D12_RANGE read_range{0, sizeof(uint8_t)};
            HRESULT ranged = texture->Map(sub, &read_range, nullptr);
            expect(FAILED(ranged), "opaque Map accepts a nonempty CPU range");
            if (SUCCEEDED(ranged))
              texture->Unmap(sub, nullptr);
            CHECK(texture->WriteToSubresource(sub, nullptr, want.data(), pitches[sub], slice_pitches[sub]));
            if (f.unit > 1 && sub == 0)
              for (UINT offset = 1; offset < f.unit; offset++) {
                const D3D12_BOX boxes[] = {
                    {offset, 0, 0, f.unit, f.unit, 1}, {0, offset, 0, f.unit, f.unit, 1},
                    {0, 0, 0, f.unit - offset, f.unit, 1}, {0, 0, 0, f.unit, f.unit - offset, 1}};
                std::vector<uint8_t> invalid(f.bytes);
                for (UINT edge = 0; edge < std::size(boxes); edge++) {
                  step("%s: unaligned BC edge %u, offset %u", f.name, edge, offset);
                  expect(texture->WriteToSubresource(sub, &boxes[edge], want.data(), f.bytes, f.bytes) == E_INVALIDARG,
                         "unaligned BC write accepted");
                  expect(
                      texture->ReadFromSubresource(invalid.data(), f.bytes, f.bytes, sub, &boxes[edge]) == E_INVALIDARG,
                      "unaligned BC read accepted");
                }
              }
            const UINT physical_width = ((w + f.unit - 1) / f.unit) * f.unit,
                       physical_height = ((h + f.unit - 1) / f.unit) * f.unit;
            std::vector<UINT> xs{0, (physical_width / f.unit / 2) * f.unit, (physical_width / f.unit - 1) * f.unit};
            std::sort(xs.begin(), xs.end());
            xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            for (UINT x : xs)
              for (UINT y : {0u, (physical_height / f.unit - 1) * f.unit})
                for (UINT z = 0; z < depth; z += std::max(1u, depth - 1)) {
                  const D3D12_BOX box{x, y, z, x + f.unit, y + f.unit, z + 1};
                  std::vector<uint8_t> changed(f.bytes + padding), got(changed.size(), uint8_t(~0u));
                  for (UINT b = 0; b < f.bytes; b++)
                    changed[b] = uint8_t(~(sub + z + y * w + x + b));
                  step("%s subresource %u: CPU box [%u,%u..%u,%u]", f.name, sub, x, y, box.right, box.bottom);
                  CHECK(texture->WriteToSubresource(
                      sub, &box, changed.data(), UINT(changed.size()), UINT(changed.size())
                  ));
                  memcpy(
                      want.data() + z * slice_pitches[sub] + (y / f.unit) * pitches[sub] + (x / f.unit) * f.bytes,
                      changed.data(), f.bytes
                  );
                  CHECK(texture->ReadFromSubresource(got.data(), UINT(got.size()), UINT(got.size()), sub, &box));
                  for (UINT b = 0; b < got.size(); b++)
                    if ((b >= f.bytes || (f.defined & (1u << b))) &&
                        !expect(got[b] == (b < f.bytes ? changed[b] : uint8_t(~0u)), "box byte %u differs", b))
                      return verdict();
                }
            std::vector<uint8_t> got(want.size() + f.bytes, uint8_t(~0u));
            CHECK(texture->ReadFromSubresource(got.data(), pitches[sub], slice_pitches[sub], sub, nullptr));
            for (UINT b = 0; b < got.size(); b++) {
              const UINT in_slice = b % slice_pitches[sub], in_row = in_slice % pitches[sub];
              bool active = b < want.size() && in_slice < rows[sub] * pitches[sub] && in_row < row_bytes[sub];
              if ((!active || (f.defined & (1u << (in_row % f.bytes)))) &&
                  !expect(got[b] == (active ? want[b] : uint8_t(~0u)), "subresource %u byte %u differs", sub, b))
                return verdict();
            }
            texture->Unmap(sub, nullptr);
          }
          if (!std::has_single_bit(f.bytes))
            continue;
          std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subs);
          UINT64 size;
          device->GetCopyableFootprints(&td, 0, subs, 0, footprints.data(), nullptr, nullptr, &size);
          auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
          if (!expect(bool(readback), "GPU copy readback was not created"))
            return verdict();
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), nullptr));
          transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
          for (UINT sub = 0; sub < subs; sub++) {
            D3D12_TEXTURE_COPY_LOCATION from{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            from.SubresourceIndex = sub;
            to.PlacedFootprint = footprints[sub];
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          }
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          void *mapped;
          CHECK(readback->Map(0, nullptr, &mapped));
          bool same = true;
          for (UINT sub = 0; sub < subs; sub++)
            for (UINT z = 0; z < depth; z++)
              for (UINT y = 0; y < rows[sub]; y++)
                for (UINT b = 0; b < row_bytes[sub]; b++) {
                  const auto *at = static_cast<const uint8_t *>(mapped) + footprints[sub].Offset +
                                   (z * rows[sub] + y) * footprints[sub].Footprint.RowPitch;
                  if (same && (f.defined & (1u << (b % f.bytes))) &&
                      at[b] != expected[sub][z * slice_pitches[sub] + y * pitches[sub] + b]) {
                    expect(false, "GPU copy subresource %u row %u byte %u differs", sub, y, b);
                    same = false;
                  }
                }
          readback->Unmap(0, nullptr);
          if (!same)
            return verdict();
          if (depth > 1) {
            auto multi_desc = td;
            multi_desc.MipLevels = 2;
            ComPtr<ID3D12Resource> multi;
            CHECK(device->CreateCommittedResource(
                &custom, D3D12_HEAP_FLAG_NONE, &multi_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&multi)
            ));
            for (UINT sub = 0; sub < multi_desc.MipLevels; sub++) {
              HRESULT mapped_multi = multi->Map(sub, nullptr, nullptr);
              expect(FAILED(mapped_multi), "multi-mip volume accepts opaque Map");
              if (SUCCEEDED(mapped_multi))
                multi->Unmap(sub, nullptr);
              std::vector<uint8_t> got(expected[0].size());
              expect(
                  FAILED(multi->WriteToSubresource(sub, nullptr, expected[0].data(), pitches[0], slice_pitches[0])),
                  "multi-mip volume accepts CPU write"
              );
              expect(
                  FAILED(multi->ReadFromSubresource(got.data(), pitches[0], slice_pitches[0], sub, nullptr)),
                  "multi-mip volume accepts CPU read"
              );
            }
          }
          tested++;
        }
    }
  }
  expect(tested != 0, "no CPU texture copies were checked against GPU readback");
  return verdict();
}
