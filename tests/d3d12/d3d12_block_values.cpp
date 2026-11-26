// contract: BC palettes decode before filtering, including virtual mips smaller than one physical block.
// "Decompression always occurs before filtering." (D3D11.3 19.5.1). "Values that the reference decodes to 0.0,
// 1.0 or -1.0 must always be exact." (19.5.2). BC1-5 bounds follow 19.5.2/.6-.12; BC6H/BC7 decoding must be
// "bit accurate" (19.5.13/.14). all 16-bit endpoint words/pairs and all block positions are enumerated; BC6H
// covers zero endpoints in every legal/reserved mode, then nonzero endpoints, signed extremes, delta wrap,
// every index in all four ONE modes, and the signed 16.4 -INF edge (19.5.13.3/.5-.10;
// Learn's BC6H Format completes signed unquantize).
// BC7 covers every zero-endpoint mode plus varying mode 6.
// footprint uploads, same-format copies and typed/typeless SRVs feed direct, indirect and bundle draws.
// a typeless resource "must be fully qualified as one of the subformats" (19.1.1); signed and unsigned
// views interpret the same stored blocks.
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc6h-format
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/bc7-format
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include "../block_values.hpp"

using namespace block_values;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto vertex = compiler.compile(hlsl, "vs", "vs", {"POINT_SAMPLE=0"});
  std::string pixels[2];
  for (UINT mode = 0; mode < std::size(pixels); mode++)
    pixels[mode] = compiler.compile(hlsl, "ps", "ps", {"POINT_SAMPLE=" + std::to_string(mode)});
  if (!expect(!vertex.empty() && !pixels[0].empty() && !pixels[1].empty(), "block HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER parameters[2] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].Constants = {0, 0, 3};
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  auto root = root_signature(device.Get(), {UINT(std::size(parameters)), parameters, 1, &sampler});
  if (!expect(bool(root), "root signature was not created"))
    return verdict();
  ComPtr<ID3D12PipelineState> pipelines[std::size(pixels)];
  for (UINT mode = 0; mode < std::size(pixels); mode++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = bytecode(vertex);
    pd.PS = bytecode(pixels[mode]);
    pd.SampleMask = ~0u;
    pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipelines[mode])));
  }
  ComPtr<ID3D12DescriptorHeap> heap, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
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
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  const D3D12_DRAW_ARGUMENTS draw_args{3, 1, 0, 0};
  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(draw_args), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(args), "argument buffer was not created"))
    return verdict();
  void *mapped;
  CHECK(args->Map(0, nullptr, &mapped));
  memcpy(mapped, &draw_args, sizeof(draw_args));
  args->Unmap(0, nullptr);
  UINT tested = 0;
  for (auto f : formats())
    for (bool parent : {false, true})
      for (UINT width : {side * (1u << CHAR_BIT), side * 3}) {
        UINT height = width == side * 3 ? side * 2 : width, mips = std::bit_width(width | height);
        step("%s parent %u: %ux%u, %u mips", f.name, parent, width, height, mips);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT caps{f.id};
        HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &caps, sizeof(caps));
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,     width, height, 1, UINT16(mips),
                               parent ? f.parent : f.id,           {1, 0}};
        ComPtr<ID3D12Resource> texture, copied;
        HRESULT created = device->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
        );
        if (FAILED(hr) || !(caps.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D)) {
          expect(FAILED(created) && !texture, "unsupported BC storage was created: %08lx", created);
          continue;
        }
        if (!expect(created == S_OK && texture, "advertised BC storage failed: %08lx", created))
          return verdict();
        CHECK(device->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copied)
        ));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips);
        std::vector<std::vector<uint8_t>> bytes(mips);
        UINT64 total;
        device->GetCopyableFootprints(&td, 0, mips, 0, footprints.data(), nullptr, nullptr, &total);
        auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!expect(bool(upload), "upload buffer was not created"))
          return verdict();
        CHECK(upload->Map(0, nullptr, &mapped));
        memset(mapped, 0, total);
        for (UINT mip = 0; mip < mips; mip++) {
          UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
          bytes[mip] = image(f, wide, high, mip);
          UINT pitch = (wide + side - 1) / side * f.bytes;
          for (UINT row = 0; row < (high + side - 1) / side; row++)
            memcpy(
                static_cast<uint8_t *>(mapped) + footprints[mip].Offset + row * footprints[mip].Footprint.RowPitch,
                bytes[mip].data() + row * pitch, pitch
            );
        }
        upload->Unmap(0, nullptr);
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        for (UINT mip = 0; mip < mips; mip++) {
          D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
              to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          from.PlacedFootprint = footprints[mip];
          to.SubresourceIndex = mip;
          list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(copied.Get(), texture.Get());
        transition(
            list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
        transition(
            list.Get(), copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        );
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        for (UINT mip = 0; mip < mips; mip++)
          for (UINT mode = 0; mode < std::size(pixels); mode++)
            for (UINT path = 0; path < 3; path++) {
              if (!(caps.Support1 & (mode ? D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE : D3D12_FORMAT_SUPPORT1_SHADER_LOAD)))
                continue;
              UINT wide = std::max(1u, width >> mip), high = std::max(1u, height >> mip);
              step(
                  "%s parent %u mip %u (%ux%u) %s path %u", f.name, parent, mip, wide, high,
                  mode ? "point sample" : "load after copy", path
              );
              D3D12_SHADER_RESOURCE_VIEW_DESC vd{
                  f.id, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
              };
              vd.Texture2D.MipLevels = mips;
              device->CreateShaderResourceView(
                  mode ? texture.Get() : copied.Get(), &vd, heap->GetCPUDescriptorHandleForHeapStart()
              );
              D3D12_RESOURCE_DESC output_desc{
                  D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                  0,
                  wide,
                  high,
                  1,
                  1,
                  DXGI_FORMAT_R32G32B32A32_FLOAT,
                  {1, 0},
                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
              };
              ComPtr<ID3D12Resource> target;
              CHECK(device->CreateCommittedResource(
                  &properties, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                  IID_PPV_ARGS(&target)
              ));
              auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
              device->CreateRenderTargetView(target.Get(), nullptr, rtv);
              D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
              device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
              auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
              if (!expect(bool(readback), "readback was not created"))
                return verdict();
              CHECK(forget(readback.Get()));
              CHECK(allocator->Reset());
              CHECK(list->Reset(allocator.Get(), pipelines[mode].Get()));
              auto bind = [&](ID3D12GraphicsCommandList *commands) {
                ID3D12DescriptorHeap *heaps[] = {heap.Get()};
                commands->SetDescriptorHeaps(1, heaps);
                commands->SetGraphicsRootSignature(root.Get());
                commands->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
                const UINT shape[] = {wide, high, mip};
                commands->SetGraphicsRoot32BitConstants(1, UINT(std::size(shape)), shape, 0);
                commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
              };
              bind(list.Get());
              D3D12_VIEWPORT viewport{0, 0, float(wide), float(high), 0, 1};
              D3D12_RECT scissor{0, 0, LONG(wide), LONG(high)};
              list->RSSetViewports(1, &viewport);
              list->RSSetScissorRects(1, &scissor);
              list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
              const float poison[] = {-1, -1, -1, -1};
              list->ClearRenderTargetView(rtv, poison, 0, nullptr);
              if (path == 0)
                list->DrawInstanced(3, 1, 0, 0);
              else if (path == 1)
                list->ExecuteIndirect(signature.Get(), 1, args.Get(), 0, nullptr, 0);
              else {
                CHECK(bundle_allocator->Reset());
                CHECK(bundle->Reset(bundle_allocator.Get(), pipelines[mode].Get()));
                bind(bundle.Get());
                bundle->DrawInstanced(3, 1, 0, 0);
                CHECK(bundle->Close());
                list->ExecuteBundle(bundle.Get());
              }
              transition(
                  list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE
              );
              D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                  to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
              to.PlacedFootprint = footprint;
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
              CHECK(submit(device.Get(), queue.Get(), list.Get()));
              CHECK(readback->Map(0, nullptr, &mapped));
              bool same = true;
              for (UINT y = 0; same && y < high; y++)
                for (UINT x = 0; x < wide; x++) {
                  auto pixel =
                      reinterpret_cast<const float *>(
                          static_cast<const uint8_t *>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch
                      ) +
                      x * 4;
                  if (!matches(f, bytes[mip].data(), wide, x, y, pixel)) {
                    expect(false, "texel %u,%u is %.9g %.9g %.9g %.9g", x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
                    same = false;
                    break;
                  }
                }
              readback->Unmap(0, nullptr);
              tested++;
            }
      }
  expect(tested != 0, "no BC values were sampled");
  return verdict();
}
