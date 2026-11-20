// contract: typed texture/buffer loads, samples, stores and render targets follow the format's conversion rules.
// "The starting n-bit value is converted to float (0.0f, 1.0f, 2.0f, etc.) and then divided by (2n-1)."
// (D3D11.3 3.2.3.5). "Round-to-zero must be used during conversion to another float format." (3.2.2).
// normalized writes permit "0.6f ULP" (3.2.3.4/.6/.8); sRGB reads permit "0.5f ULP" on the encoded side (3.2.3.7).
// the shared CPU reference enumerates 8/16-bit domains, small-float fields, packed 16-bit words and RGB9E5;
// it checks NaN classes, zero signs, channel defaults and integer clamping. DXIL.rst TextureLoad/TextureStore
// and BufferLoad/BufferStore define the typed operations. direct and indirect dispatches, and direct, bundle
// and indirect draws, must produce the same bytes for both shader front ends. supported caps select legal uses.
// "the input and the output data must be identical in memory" (19.1.2), except the extra SNORM minimum
// and NaN payloads (19.1.2.1): Load and point/linear samples at texel centers feed the same-format RTV.
// "The views just reinterpret the data in memory as if it is the type in the view format"
// (DirectX-Specs RelaxedCasting): SRV/UAV/RTV operations cover typeless parents and every legal ordinary
// fully typed same-family parent when OPTIONS3 advertises support and the parent admits the resource flags;
// FLOAT/non-FLOAT and SNORM/UNORM are excluded.
// https://microsoft.github.io/DirectX-Specs/d3d/RelaxedCasting.html
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include "../format_values.hpp"

using namespace format_values;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  std::string codes[3][7], pixels[3][4];
  auto vertex = compiler.compile(hlsl, "vs", "vs", defines(0, 0));
  if (!expect(!vertex.empty(), "vertex HLSL did not compile"))
    return verdict();
  for (UINT kind = 0; kind < std::size(codes); kind++) {
    for (UINT mode = 0; mode < std::size(pixels[kind]); mode++) {
      if (kind && mode >= 2)
        continue;
      pixels[kind][mode] = compiler.compile(hlsl, "ps", "ps", defines(kind, mode + 7));
      if (!expect(!pixels[kind][mode].empty(), "pixel HLSL did not compile"))
        return verdict();
    }
    for (UINT mode = 0; mode < std::size(codes[kind]); mode++) {
      if (kind && mode == 1)
        continue;
      codes[kind][mode] = compiler.compile(hlsl, "cs", "cs", defines(kind, mode));
      if (!expect(!codes[kind][mode].empty(), "compute HLSL did not compile"))
        return verdict();
    }
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
  const bool fully_typed =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) &&
      options.CastingFullyTypedFormatSupported;
  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2}};
  D3D12_ROOT_PARAMETER parameters[3] = {
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
      {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}
  };
  parameters[0].DescriptorTable = {1, &ranges[0]};
  parameters[1].DescriptorTable = {1, &ranges[1]};
  parameters[2].Constants = {0, 0, 2};
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_STATIC_SAMPLER_DESC samplers[] = {sampler, sampler};
  samplers[1].ShaderRegister = 1;
  samplers[1].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  auto root =
      root_signature(device.Get(), {UINT(std::size(parameters)), parameters, UINT(std::size(samplers)), samplers});
  if (!expect(bool(root), "root signature was not created"))
    return verdict();
  ComPtr<ID3D12PipelineState> compute[3][7];
  for (UINT kind = 0; kind < std::size(compute); kind++)
    for (UINT mode = 0; mode < std::size(compute[kind]); mode++)
      if (!codes[kind][mode].empty()) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{root.Get(), bytecode(codes[kind][mode])};
        CHECK(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&compute[kind][mode])));
      }
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
  ComPtr<ID3D12DescriptorHeap> heap, rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = [&](UINT slot) {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += slot * increment;
    return h;
  };
  auto gpu = [&](UINT slot) {
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += slot * increment;
    return h;
  };
  ComPtr<ID3D12CommandSignature> dispatch_signature, draw_signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch_signature)));
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
  CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw_signature)));
  D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT};
  UINT tested = 0;
  const auto layouts = formats();
  for (const auto &f : layouts) {
    step("%s: capabilities", f.name.c_str());
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{f.id};
    HRESULT caps = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(caps)) {
      expect(caps == E_FAIL && !support.Support1 && !support.Support2, "unsupported format hr %08lx", caps);
      continue;
    }
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
      continue;
    std::vector<DXGI_FORMAT> resource_formats{f.id};
    const auto family = f.name.substr(0, f.name.find('_', strlen("DXGI_FORMAT_")));
    const struct {
      DXGI_FORMAT id;
      const char *name;
    } sdk[] = {DXGI_FORMATS};
    for (const auto &candidate : sdk) {
      bool legal = family + "_TYPELESS" == candidate.name;
      if (fully_typed)
        for (const auto &other : layouts)
          if (other.id == candidate.id && other.id != f.id &&
              other.name.substr(0, other.name.find('_', strlen("DXGI_FORMAT_"))) == family) {
            const bool float_cast = (f.kind == Floating) != (other.kind == Floating);
            const bool normalized_cast = (f.kind == Snorm && (other.kind == Unorm || other.kind == Srgb)) ||
                                         (other.kind == Snorm && (f.kind == Unorm || f.kind == Srgb));
            legal = !float_cast && !normalized_cast;
          }
      if (!legal)
        continue;
      D3D12_FEATURE_DATA_FORMAT_SUPPORT storage{candidate.id};
      if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &storage, sizeof(storage))) &&
          (storage.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
        resource_formats.push_back(candidate.id);
    }
    if (f.kind == Shared)
      expect(
          !(support.Support1 &
            (D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)),
          "RGB9E5 advertises forbidden writes (D3D11.3 19.3.2)"
      );
    auto bytes = inputs(f);
    auto values = sources(f, bytes);
    const UINT width = 1u << CHAR_BIT, count = UINT(values.size()), height = std::bit_ceil((count + width - 1) / width);
    const UINT pixels_count = width * height, kind = f.shader_kind(), group = std::tuple_size_v<Pixel>;
    const UINT groups_x = (width + group - 1) / group, groups_y = (height + group - 1) / group;
    bytes.resize(pixels_count * f.bytes);
    values.resize(pixels_count);
    for (UINT i = count; i < pixels_count; i++)
      values[i] = decode(f, bytes.data() + i * f.bytes);
    auto upload = [&](const void *data, UINT64 size) {
      auto res = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (!res)
        return res;
      void *mapped;
      if (FAILED(res->Map(0, nullptr, &mapped)))
        return ComPtr<ID3D12Resource>{};
      memcpy(mapped, data, size);
      res->Unmap(0, nullptr);
      return res;
    };
    auto source = upload(values.data(), values.size() * sizeof(Pixel));
    auto raw = upload(bytes.data(), bytes.size());
    const D3D12_DISPATCH_ARGUMENTS dispatch_args{groups_x, groups_y, 1};
    const D3D12_DRAW_ARGUMENTS draw_args{3, 1, 0, 0};
    auto dispatch_buffer = upload(&dispatch_args, sizeof(dispatch_args));
    auto draw_buffer = upload(&draw_args, sizeof(draw_args));
    if (!expect(source && raw && dispatch_buffer && draw_buffer, "upload buffers were not created"))
      return verdict();
    D3D12_SHADER_RESOURCE_VIEW_DESC source_view{
        DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    source_view.Buffer.NumElements = pixels_count;
    source_view.Buffer.StructureByteStride = sizeof(Pixel);
    device->CreateShaderResourceView(source.Get(), &source_view, cpu(1));
    const UINT result_bytes = pixels_count * sizeof(Pixel);
    auto result = buffer(
        device.Get(), D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    std::vector<Pixel> poison(pixels_count, Pixel{~0u, ~0u, ~0u, ~0u});
    auto poison_buffer = upload(poison.data(), result_bytes);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(result && poison_buffer && readback, "result buffers were not created"))
      return verdict();
    D3D12_UNORDERED_ACCESS_VIEW_DESC result_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
    result_view.Buffer.NumElements = pixels_count;
    result_view.Buffer.StructureByteStride = sizeof(Pixel);
    device->CreateUnorderedAccessView(result.Get(), nullptr, &result_view, cpu(2));
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, f.id, {1, 0}};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 footprint_bytes;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, nullptr, &footprint_bytes);
    std::vector<uint8_t> padded(footprint_bytes);
    for (UINT y = 0; y < height; y++)
      memcpy(
          padded.data() + footprint.Offset + y * footprint.Footprint.RowPitch, bytes.data() + y * width * f.bytes,
          width * f.bytes
      );
    auto texture_upload = upload(padded.data(), padded.size());
    auto filtered_bytes = linear_inputs(f, bytes);
    for (UINT y = 0; y < height; y++)
      memcpy(
          padded.data() + footprint.Offset + y * footprint.Footprint.RowPitch,
          filtered_bytes.data() + y * width * f.bytes, width * f.bytes
      );
    auto filtered_upload = upload(padded.data(), padded.size());
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width * f.bytes; x++)
        padded[footprint.Offset + y * footprint.Footprint.RowPitch + x] = ~bytes[y * width * f.bytes + x];
    auto inverse_upload = upload(padded.data(), padded.size());
    auto texture_readback =
        buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, footprint_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(
            texture_upload && filtered_upload && inverse_upload && texture_readback,
            "texture transfer buffers were not created"
        ))
      return verdict();
    for (DXGI_FORMAT storage : resource_formats) {
      td.Format = storage;
      D3D12_FEATURE_DATA_FORMAT_SUPPORT storage_support{storage};
      CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &storage_support, sizeof(storage_support)));
      for (UINT mode = 0; mode <= 10; mode++) {
        bool typed_buffer = mode == 2 || mode == 5 || mode == 6;
        bool unordered = mode >= 3 && mode <= 6, store = mode == 4 || mode == 6 || mode >= 7;
        if (storage != f.id &&
            (typed_buffer ||
             (unordered && !(storage_support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) ||
             (mode >= 7 && !(storage_support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))))
          continue;
        if (f.kind == Shared && (unordered || store))
          continue;
        if ((mode == 0 || mode == 8) && !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
          continue;
        if ((mode == 1 || mode >= 9) && (kind || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)))
          continue;
        if (typed_buffer && !(support.Support1 & D3D12_FORMAT_SUPPORT1_BUFFER))
          continue;
        if (unordered && !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
          continue;
        if ((mode == 3 || mode == 5) && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
          continue;
        if ((mode == 4 || mode == 6) && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
          continue;
        if (mode >= 7 && !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
          continue;
        ComPtr<ID3D12PipelineState> graphics;
        if (mode >= 7) {
          D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
          pd.pRootSignature = root.Get();
          pd.VS = bytecode(vertex);
          pd.PS = bytecode(pixels[kind][mode - 7]);
          pd.SampleMask = ~0u;
          pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
          pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
          pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
          pd.NumRenderTargets = 1;
          pd.RTVFormats[0] = f.id;
          pd.SampleDesc.Count = 1;
          CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&graphics)));
        }
        for (UINT path = 0; path < 3; path++) {
          step(
              "%s storage %u: mode %u, path %u, %u values (%ux%u)", f.name.c_str(), storage, mode, path, pixels_count,
              width, height
          );
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), mode >= 7 ? graphics.Get() : compute[kind][mode].Get()));
          CHECK(forget(readback.Get()));
          CHECK(forget(texture_readback.Get()));
          ComPtr<ID3D12Resource> target, input;
          auto state = unordered   ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                       : mode >= 7 ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                   : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
          if (typed_buffer) {
            target = buffer(
                device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes.size(), D3D12_RESOURCE_STATE_COPY_DEST,
                unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE
            );
            if (!expect(bool(target), "typed buffer was not created"))
              return verdict();
            list->CopyBufferRegion(target.Get(), 0, raw.Get(), 0, bytes.size());
          } else {
            td.Flags = unordered   ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                       : mode >= 7 ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                   : D3D12_RESOURCE_FLAG_NONE;
            CHECK(device->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&target)
            ));
            D3D12_TEXTURE_COPY_LOCATION from{
                mode >= 8 ? inverse_upload.Get() : texture_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
            },
                to{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            from.PlacedFootprint = footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          }
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
          if (unordered) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC vd{
                f.id, typed_buffer ? D3D12_UAV_DIMENSION_BUFFER : D3D12_UAV_DIMENSION_TEXTURE2D
            };
            if (typed_buffer)
              vd.Buffer.NumElements = pixels_count;
            device->CreateUnorderedAccessView(target.Get(), nullptr, &vd, cpu(3));
          } else if (mode >= 7) {
            D3D12_RENDER_TARGET_VIEW_DESC target_view{f.id, D3D12_RTV_DIMENSION_TEXTURE2D};
            device->CreateRenderTargetView(target.Get(), &target_view, rtvs->GetCPUDescriptorHandleForHeapStart());
            if (mode >= 8) {
              auto input_desc = td;
              input_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
              CHECK(device->CreateCommittedResource(
                  &properties, D3D12_HEAP_FLAG_NONE, &input_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                  IID_PPV_ARGS(&input)
              ));
              D3D12_TEXTURE_COPY_LOCATION from{
                  mode == 10 ? filtered_upload.Get() : texture_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
              },
                  to{input.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
              from.PlacedFootprint = footprint;
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
              transition(
                  list.Get(), input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
              );
              D3D12_SHADER_RESOURCE_VIEW_DESC vd{
                  f.id, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
              };
              vd.Texture2D.MipLevels = 1;
              device->CreateShaderResourceView(input.Get(), &vd, cpu(0));
            }
          } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC vd{
                f.id, typed_buffer ? D3D12_SRV_DIMENSION_BUFFER : D3D12_SRV_DIMENSION_TEXTURE2D,
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
            };
            if (typed_buffer)
              vd.Buffer.NumElements = pixels_count;
            else
              vd.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(target.Get(), &vd, cpu(0));
          }
          transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
          list->CopyBufferRegion(result.Get(), 0, poison_buffer.Get(), 0, result_bytes);
          transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          auto bind = [&](ID3D12GraphicsCommandList *commands) {
            ID3D12DescriptorHeap *heaps[] = {heap.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            const UINT dimensions[] = {width, pixels_count};
            if (mode >= 7) {
              commands->SetGraphicsRootSignature(root.Get());
              commands->SetGraphicsRootDescriptorTable(0, gpu(0));
              commands->SetGraphicsRootDescriptorTable(1, gpu(2));
              commands->SetGraphicsRoot32BitConstants(2, UINT(std::size(dimensions)), dimensions, 0);
              commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            } else {
              commands->SetComputeRootSignature(root.Get());
              commands->SetComputeRootDescriptorTable(0, gpu(0));
              commands->SetComputeRootDescriptorTable(1, gpu(2));
              commands->SetComputeRoot32BitConstants(2, UINT(std::size(dimensions)), dimensions, 0);
            }
          };
          bind(list.Get());
          if (mode >= 7) {
            auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
            D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            if (path == 0)
              list->DrawInstanced(3, 1, 0, 0);
            else if (path == 1)
              list->ExecuteIndirect(draw_signature.Get(), 1, draw_buffer.Get(), 0, nullptr, 0);
            else {
              CHECK(bundle_allocator->Reset());
              CHECK(bundle->Reset(bundle_allocator.Get(), graphics.Get()));
              bind(bundle.Get());
              bundle->DrawInstanced(3, 1, 0, 0);
              CHECK(bundle->Close());
              list->ExecuteBundle(bundle.Get());
            }
          } else if (path == 0)
            list->Dispatch(groups_x, groups_y, 1);
          else if (path == 1)
            list->ExecuteIndirect(dispatch_signature.Get(), 1, dispatch_buffer.Get(), 0, nullptr, 0);
          else {
            CHECK(bundle_allocator->Reset());
            CHECK(bundle->Reset(bundle_allocator.Get(), compute[kind][mode].Get()));
            bind(bundle.Get());
            bundle->Dispatch(groups_x, groups_y, 1);
            CHECK(bundle->Close());
            list->ExecuteBundle(bundle.Get());
          }
          if (store) {
            transition(list.Get(), target.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (typed_buffer)
              list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, bytes.size());
            else {
              D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                  to{texture_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
              to.PlacedFootprint = footprint;
              list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
          } else {
            transition(
                list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            list->CopyResource(readback.Get(), result.Get());
            transition(
                list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
          }
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          auto read_resource = store && !typed_buffer ? texture_readback.Get() : readback.Get();
          void *data;
          CHECK(read_resource->Map(0, nullptr, &data));
          const auto *mapped = static_cast<const uint8_t *>(data);
          bool same = true;
          for (UINT i = 0; i < pixels_count; i++) {
            if (store) {
              const uint8_t *at =
                  mapped + (typed_buffer ? i * f.bytes
                                         : footprint.Offset + (i / width) * footprint.Footprint.RowPitch +
                                               (i % width) * f.bytes);
              same = mode >= 8 ? inverted(f, (mode == 10 ? filtered_bytes : bytes).data() + i * f.bytes, at, mode >= 9)
                               : stored(f, values[i], at);
            } else {
              Pixel have;
              memcpy(have.data(), mapped + i * sizeof(Pixel), sizeof(Pixel));
              same = loaded(f, bytes.data() + i * f.bytes, have, mode == 1);
            }
            if (!same) {
              expect(false, "%s value %u does not match the CPU conversion", store ? "encoded" : "decoded", i);
              break;
            }
          }
          if (same && store && mode < 8)
            same = expect(
                ordered(
                    f, values, mapped + (typed_buffer ? 0 : footprint.Offset), typed_buffer ? pixels_count : width,
                    typed_buffer ? 0 : footprint.Footprint.RowPitch
                ),
                "normalized conversion decreases or maps equal values differently"
            );
          read_resource->Unmap(0, nullptr);
          tested++;
          if (!same)
            return verdict();
        }
      }
    }
  }
  expect(tested != 0, "no format operations ran");
  return verdict();
}
