// contract: positive quality counts promise working multisample targets; every stored sample retains its value.
// "a simple average of the samples" defines fixed-pattern resolves (D3D11.3 19.2.4). formats with _UNORM,
// _UNORM_SRGB, _SNORM or _FLOAT and multisampling "must support Resolve" (19.2.3). CPU palettes contain native
// format encodings (19.1.2 invertibility) and resolve bytes follow their decoded mean and section 3 conversions.
// center patterns use pixel-frequency coverage masks because sample-frequency execution is undefined (19.2.4.1).
// direct, indirect and bundle draws each feed per-sample loads and a separate fixed-pattern resolve readback.
#include "d3d12_test.hpp"
#include <d3d11.h>
#include "../depth_formats.hpp"
#include "../format_msaa.hpp"
#include "../metal_samples.hpp"

int
main(int argc, char **argv) {
  using namespace format_values;
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  auto vertex = compiler.compile(format_msaa::hlsl, "vs", "vs", defines(0, 0));
  std::string pixels[3][2], reads[3];
  for (UINT kind = 0; kind < std::size(pixels); kind++) {
    for (UINT center = 0; center < std::size(pixels[kind]); center++) {
      pixels[kind][center] = compiler.compile(format_msaa::hlsl, center ? "ps_center" : "ps", "ps", defines(kind, 0));
      if (!expect(!pixels[kind][center].empty(), "pixel HLSL did not compile"))
        return verdict();
    }
    reads[kind] = compiler.compile(format_msaa::hlsl, "cs", "cs", defines(kind, 0));
    if (!expect(!reads[kind].empty(), "sample-load HLSL did not compile"))
      return verdict();
  }
  if (!expect(!vertex.empty(), "vertex HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2};
  D3D12_ROOT_PARAMETER parameters[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_UAV},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].Descriptor.ShaderRegister = 0;
  parameters[2].Constants = {0, 0, 4};
  auto root = root_signature(device.Get(), {UINT(std::size(parameters)), parameters});
  if (!expect(bool(root), "root signature was not created"))
    return verdict();
  ComPtr<ID3D12PipelineState> compute[3];
  for (UINT kind = 0; kind < std::size(compute); kind++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(reads[kind])};
    CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&compute[kind])));
  }
  ComPtr<ID3D12DescriptorHeap> heap, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
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
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto args =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(args), "draw arguments were not created"))
    return verdict();
  void *mapped;
  CHECK(args->Map(0, nullptr, &mapped));
  const D3D12_DRAW_ARGUMENTS draw_args{3, 1, 0, 0};
  memcpy(mapped, &draw_args, sizeof(draw_args));
  args->Unmap(0, nullptr);
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  UINT tested = 0;
  for (const auto &f : formats()) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT caps{f.id};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &caps, sizeof(caps))) ||
        !(caps.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
      continue;
    if (f.kind == Shared) {
      expect(false, "RGB9E5 advertises forbidden rendering (19.3.2)");
      continue;
    }
    auto refused = [&](UINT samples, UINT quality) {
      D3D12_RESOURCE_DESC td{
          D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, 1, 1, 1, 1, f.id, {samples, quality}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
      };
      ComPtr<ID3D12Resource> target;
      HRESULT hr = device->CreateCommittedResource(
          &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
      );
      return expect(
          FAILED(hr) && !target, "%s accepted invalid count %u quality %#x: %08lx", f.name.c_str(), samples, quality, hr
      );
    };
    for (UINT samples : {0u, D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT + 1u, UINT_MAX})
      refused(samples, 0);
    // section 19.6 makes MSAA, sample loads and resolve optional for these resurrected formats.
    const bool optional = f.id == DXGI_FORMAT_B5G5R5A1_UNORM || f.id == DXGI_FORMAT_B4G4R4A4_UNORM;
    const bool load = caps.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
    for (UINT samples = 1; samples <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; samples++) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{f.id, samples};
      CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality)));
      constexpr UINT optional_8x_bits = 128; // D3D11.3 19.2.5 makes 8x optional at 128 bits, and 4x required
      if (samples == 1 || (!optional && (samples == 4 || (samples == 8 && metal_has_8x_samples() && f.bytes * CHAR_BIT < optional_8x_bits))))
        expect(quality.NumQualityLevels != 0, "%s has no required %ux quality levels", f.name.c_str(), samples);
      if (!quality.NumQualityLevels) {
        refused(samples, 0);
        continue;
      }
      for (UINT q : {quality.NumQualityLevels, quality.NumQualityLevels + (quality.NumQualityLevels != UINT_MAX)})
        if (q != D3D11_STANDARD_MULTISAMPLE_PATTERN && q != D3D11_CENTER_MULTISAMPLE_PATTERN)
          refused(samples, q);
      if (samples == 1)
        continue;
      step("%s %u samples: advertised capabilities", f.name.c_str(), samples);
      expect(
          caps.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET, "quality levels without target capability"
      );
      if (!optional)
        expect(load, "multisample target without required load capability");
      if (!optional && !f.shader_kind())
        expect(
            caps.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE,
            "normalized/float MSAA format has no required resolve (19.2.3)"
        );
      auto raw = format_msaa::palette(f, samples);
      auto values = format_msaa::values(f, raw);
      std::array<float, 4> clear;
      if (!expect(format_msaa::clear_value(f, raw, clear), "target seed aliases a sample's expected value"))
        return verdict();
      auto source = buffer(
          device.Get(), D3D12_HEAP_TYPE_UPLOAD, values.size() * sizeof(Pixel), D3D12_RESOURCE_STATE_GENERIC_READ
      );
      if (!expect(bool(source), "palette buffer was not created"))
        return verdict();
      CHECK(source->Map(0, nullptr, &mapped));
      memcpy(mapped, values.data(), values.size() * sizeof(Pixel));
      source->Unmap(0, nullptr);
      auto source_handle = heap->GetCPUDescriptorHandleForHeapStart();
      source_handle.ptr += increment;
      D3D12_SHADER_RESOURCE_VIEW_DESC source_view{
          DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
      };
      source_view.Buffer.NumElements = samples;
      source_view.Buffer.StructureByteStride = sizeof(Pixel);
      device->CreateShaderResourceView(source.Get(), &source_view, source_handle);
      std::vector<UINT> qualities{0, quality.NumQualityLevels - 1};
      constexpr UINT max_fixed_samples = 16; // D3D11.3 19.2.4 defines fixed patterns for 1, 2, 4, 8 and 16
      if (samples <= max_fixed_samples && std::has_single_bit(samples)) {
        qualities.push_back(D3D11_STANDARD_MULTISAMPLE_PATTERN);
        qualities.push_back(D3D11_CENTER_MULTISAMPLE_PATTERN);
      }
      std::sort(qualities.begin(), qualities.end());
      qualities.erase(std::unique(qualities.begin(), qualities.end()), qualities.end());
      for (UINT q : qualities) {
        const bool center = q == D3D11_CENTER_MULTISAMPLE_PATTERN;
        const bool fixed = center || q == D3D11_STANDARD_MULTISAMPLE_PATTERN;
        const bool resolve = fixed && (caps.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);
        const UINT width = 3, height = 3, kind = f.shader_kind(),
                   result_bytes = width * height * samples * sizeof(Pixel);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.VS = bytecode(vertex);
        pd.PS = bytecode(pixels[kind][center]);
        pd.SampleMask = ~0u;
        pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        pd.RasterizerState.MultisampleEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = f.id;
        pd.SampleDesc = {samples, q};
        ComPtr<ID3D12PipelineState> graphics;
        CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&graphics)));
        for (UINT path = 0; path < 3; path++) {
          step("%s samples %u quality %#x path %u", f.name.c_str(), samples, q, path);
          D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                 0,
                                 width,
                                 height,
                                 1,
                                 1,
                                 f.id,
                                 {samples, q},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
          ComPtr<ID3D12Resource> target, resolved, resolved_readback;
          CHECK(device->CreateCommittedResource(
              &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
          ));
          auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
          device->CreateRenderTargetView(target.Get(), nullptr, rtv);
          D3D12_SHADER_RESOURCE_VIEW_DESC vd{
              f.id, D3D12_SRV_DIMENSION_TEXTURE2DMS, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
          };
          if (load)
            device->CreateShaderResourceView(target.Get(), &vd, heap->GetCPUDescriptorHandleForHeapStart());
          auto result = buffer(
              device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
          );
          auto poison = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, result_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
          auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
          if (!expect(result && poison && readback, "sample buffers were not created"))
            return verdict();
          CHECK(poison->Map(0, nullptr, &mapped));
          memset(mapped, 0xff, result_bytes);
          poison->Unmap(0, nullptr);
          CHECK(forget(readback.Get()));
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
          if (resolve) {
            if (!expect(!kind, "integer format advertises average resolve"))
              return verdict();
            td.SampleDesc = {1, 0};
            td.Flags = D3D12_RESOURCE_FLAG_NONE;
            CHECK(device->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                IID_PPV_ARGS(&resolved)
            ));
            UINT64 bytes;
            device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            resolved_readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
            if (!expect(bool(resolved_readback), "resolve readback was not created"))
              return verdict();
            CHECK(forget(resolved_readback.Get()));
          }
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), graphics.Get()));
          list->ClearRenderTargetView(rtv, clear.data(), 0, nullptr);
          list->CopyResource(result.Get(), poison.Get());
          transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          const UINT shape[4] = {width, height, samples, 0};
          auto bind = [&](ID3D12GraphicsCommandList *commands) {
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetGraphicsRootSignature(root.Get());
            commands->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
            commands->SetGraphicsRoot32BitConstants(2, UINT(std::size(shape)), shape, 0);
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          };
          bind(list.Get());
          list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
          D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
          D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          if (path == 2) {
            CHECK(bundle_allocator->Reset());
            CHECK(bundle->Reset(bundle_allocator.Get(), graphics.Get()));
            bind(bundle.Get());
          }
          auto draws = path == 2 ? bundle.Get() : list.Get();
          for (UINT s = 0; s < (center ? samples : 1); s++) {
            draws->SetGraphicsRoot32BitConstant(2, s, 3);
            if (path == 1)
              draws->ExecuteIndirect(signature.Get(), 1, args.Get(), 0, nullptr, 0);
            else
              draws->DrawInstanced(3, 1, 0, 0);
          }
          if (path == 2) {
            CHECK(bundle->Close());
            list->ExecuteBundle(bundle.Get());
          }
          if (load) {
            transition(
                list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            list->SetPipelineState(compute[kind].Get());
            list->SetComputeRootSignature(root.Get());
            list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(2, UINT(std::size(shape)), shape, 0);
            list->Dispatch(width, height, 1);
            transition(
                list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            list->CopyResource(readback.Get(), result.Get());
          }
          if (resolve) {
            transition(
                list.Get(), target.Get(),
                load ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE
            );
            list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, f.id);
            transition(list.Get(), resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{resolved.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                to{resolved_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            to.PlacedFootprint = footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          }
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          bool same = true;
          if (load) {
            CHECK(readback->Map(0, nullptr, &mapped));
            for (UINT i = 0; i < width * height * samples; i++) {
              Pixel have;
              memcpy(have.data(), static_cast<uint8_t *>(mapped) + i * sizeof(Pixel), sizeof(Pixel));
              if (!loaded(f, raw.data() + (i % samples) * f.bytes, have)) {
                expect(false, "pixel %u sample %u changed value/index", i / samples, i % samples);
                same = false;
                break;
              }
            }
            readback->Unmap(0, nullptr);
          }
          if (!same)
            return verdict();
          if (resolve) {
            CHECK(resolved_readback->Map(0, nullptr, &mapped));
            for (UINT y = 0; y < height; y++)
              for (UINT x = 0; x < width; x++)
                same &= format_msaa::resolved(
                    f, raw, values,
                    static_cast<const uint8_t *>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch +
                        x * f.bytes
                );
            resolved_readback->Unmap(0, nullptr);
            if (!expect(same, "resolve did not encode the mean of every sample"))
              return verdict();
          }
          tested++;
        }
      }
    }
  }
  expect(tested != 0, "no multisample formats were exercised");
  return verdict();
}
