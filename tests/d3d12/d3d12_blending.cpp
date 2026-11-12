// contract: D3D11.3 17.1-17.6 blend equations also govern D3D12's identically named blend fields.
// "The min and max blend ops ignore D3D11_BLEND modes" (17.1); "The masks simply limit writes." (17.15).
// section 3 defines storage conversion; sRGB blending is linear (5.6.6). exact destination bytes are uploaded.
// source: D3D11_3_FunctionalSpec.htm; Microsoft Learn, D3D12_RENDER_TARGET_BLEND_DESC and D3D12_BLEND.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_render_target_blend_desc
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_blend
// om_blend.hpp supplies the CPU equations. direct, bundle and ExecuteIndirect draws meet the same pixel oracle.
#include "d3d12_test.hpp"
#include "../om_blend.hpp"

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT targets = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
  const std::vector<std::string> defines{"TARGETS=" + std::to_string(targets)};
  auto vs = compiler.compile(om::blend_hlsl, "vs", "vs", defines);
  const std::string pixels[] = {
      compiler.compile(om::blend_hlsl, "ps", "ps", defines), compiler.compile(om::blend_hlsl, "ps_dual", "ps", defines),
      compiler.compile(om::blend_hlsl, "ps_special", "ps", defines)
  };
  if (!expect(
          !vs.empty() && !pixels[0].empty() && !pixels[1].empty() && !pixels[2].empty(), "blend HLSL did not compile"
      ))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto cases = om::blend_cases();
  D3D12_FEATURE_DATA_D3D12_OPTIONS13 options{};
  HRESULT optional = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options, sizeof(options));
  if (SUCCEEDED(optional) && options.AlphaBlendFactorSupported)
    for (auto factor : {D3D12_BLEND_ALPHA_FACTOR, D3D12_BLEND_INV_ALPHA_FACTOR})
      for (UINT role = 0; role < 4; role++) {
        auto k = om::baseline();
        auto &r = k.state.RenderTarget[0];
        if (role == 0)
          r.SrcBlend = D3D11_BLEND(factor);
        if (role == 1)
          r.DestBlend = D3D11_BLEND(factor);
        if (role == 2)
          r.SrcBlendAlpha = D3D11_BLEND(factor);
        if (role == 3)
          r.DestBlendAlpha = D3D11_BLEND(factor);
        cases.push_back(k);
      }
  else
    printf("unsupported: alpha blend factors (query %08lx)\n", optional);
  const UINT width = UINT(cases.size()) + 2;
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = sizeof(om::Color) * 2 / sizeof(UINT);
  auto rs = root_signature(device.Get(), {1, &param});
  if (!expect(bool(rs), "root signature missing"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect buffer missing"))
    return verdict();
  D3D12_DRAW_ARGUMENTS *draw;
  CHECK(indirect->Map(0, nullptr, (void **)&draw));
  *draw = {3, 1, 0, 0};
  indirect->Unmap(0, nullptr);
  for (auto format : om::formats) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format.value};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (!expect(
            format.value != DXGI_FORMAT_R8G8B8A8_UNORM || (support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE),
            "mandatory UNORM blending not supported"
        ))
      return verdict();
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE)) {
      printf("unsupported blend format %u\n", format.value);
      continue;
    }
    const UINT stride = sizeof(om::Color) * format.bits / (sizeof(float) * CHAR_BIT);
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D,     0, width, 1, 1, 1, format.value, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    const UINT64 placement = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, placement * targets, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && readback, "upload/readback missing"))
      return verdict();
    std::vector<unsigned char> initial(width * stride);
    for (UINT x = 0; x < width; x++)
      om::seed(format, cases[x && x <= cases.size() ? x - 1 : 0].destination, initial.data() + x * stride);
    unsigned char *mapped;
    CHECK(upload->Map(0, nullptr, (void **)&mapped));
    memcpy(mapped + footprint.Offset, initial.data(), initial.size());
    upload->Unmap(0, nullptr);
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(cases.size());
    for (UINT path = 0; path < 3; path++) {
      step("format %u, path %u: upload and draw", format.value, path);
      CHECK(allocator->Reset());
      CHECK(bundle_allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      CHECK(forget(readback.Get()));
      D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, targets};
      ComPtr<ID3D12DescriptorHeap> heap;
      CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
      D3D12_CPU_DESCRIPTOR_HANDLE rtvs[targets];
      ComPtr<ID3D12Resource> textures[targets];
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      for (UINT slot = 0; slot < targets; slot++) {
        CHECK(device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[slot])
        ));
        D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            to{textures[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(
            list.Get(), textures[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET
        );
        rtvs[slot] = heap->GetCPUDescriptorHandleForHeapStart();
        rtvs[slot].ptr += slot * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(textures[slot].Get(), nullptr, rtvs[slot]);
      }
      D3D12_VIEWPORT viewport{0, 0, float(width), 1, 0, 1};
      list->RSSetViewports(1, &viewport);
      std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
      // a list keeps its blend factor from draw to draw, so the direct path sets it only when it changes: a new
      // pipeline alone must keep it right
      const decltype(cases)::value_type *factor_set = nullptr;
      for (UINT i = 0; i < cases.size(); i++) {
        const auto &k = cases[i];
        if (k.special && (format.normalized || format.bits != 32))
          continue;
        step("format %u, path %u, equation %u, dual %u", format.value, path, i, k.dual);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = rs.Get();
        p.VS = bytecode(vs);
        p.PS = bytecode(pixels[k.dual ? 1 : k.special ? 2 : 0]);
        p.BlendState.IndependentBlendEnable = k.state.IndependentBlendEnable;
        for (UINT slot = 0; slot < targets; slot++) {
          const auto &r = k.state.RenderTarget[slot];
          p.BlendState.RenderTarget[slot] = {
              r.BlendEnable,
              FALSE,
              D3D12_BLEND(r.SrcBlend),
              D3D12_BLEND(r.DestBlend),
              D3D12_BLEND_OP(r.BlendOp),
              D3D12_BLEND(r.SrcBlendAlpha),
              D3D12_BLEND(r.DestBlendAlpha),
              D3D12_BLEND_OP(r.BlendOpAlpha),
              D3D12_LOGIC_OP_NOOP,
              r.RenderTargetWriteMask
          };
          // dual source blending has one render target
          if (!k.dual || !slot)
            p.RTVFormats[slot] = format.value;
        }
        p.SampleMask = ~0u;
        p.SampleDesc = {1, 0};
        p.NumRenderTargets = k.dual ? 1 : targets;
        p.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        p.RasterizerState.DepthClipEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        auto &pipeline = pipelines[i];
        if (!pipeline) {
          HRESULT hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline));
          if (!expect(hr == S_OK && pipeline, "blend pipeline returned %08lx", hr))
            return verdict();
        }
        list->OMSetRenderTargets(p.NumRenderTargets, rtvs, FALSE, nullptr);
        D3D12_RECT rect{LONG(i + 1), 0, LONG(i + 2), 1};
        list->RSSetScissorRects(1, &rect);
        auto record = [&](ID3D12GraphicsCommandList *commands) {
          commands->SetPipelineState(pipeline.Get());
          commands->SetGraphicsRootSignature(rs.Get());
          const om::Color params[] = {k.source, k.secondary};
          commands->SetGraphicsRoot32BitConstants(0, param.Constants.Num32BitValues, params, 0);
          if (path || !factor_set || factor_set->null_factor != k.null_factor || factor_set->factor != k.factor)
            commands->OMSetBlendFactor(k.null_factor ? nullptr : k.factor.data());
          commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          if (path == 2)
            commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
          else
            commands->DrawInstanced(3, 1, 0, 0);
        };
        if (path == 1) {
          auto &bundle = bundles.emplace_back();
          CHECK(device->CreateCommandList(
              0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)
          ));
          record(bundle.Get());
          CHECK(bundle->Close());
          list->ExecuteBundle(bundle.Get());
        } else
          record(list.Get());
        if (!path)
          factor_set = &k;
      }
      for (UINT slot = 0; slot < targets; slot++) {
        transition(
            list.Get(), textures[slot].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE
        );
        D3D12_TEXTURE_COPY_LOCATION from{textures[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = footprint;
        // each placed footprint begins at the API's required alignment, not at the row's last byte.
        to.PlacedFootprint.Offset += slot * placement;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const unsigned char *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT slot = 0; slot < targets; slot++) {
        UINT64 offset = footprint.Offset + slot * placement;
        for (UINT x = 0; x < width; x++) {
          bool written = x && x <= cases.size();
          if (written) {
            const auto &k = cases[x - 1];
            written = !(k.special && (format.normalized || format.bits != 32)) && (!k.dual || !slot);
          }
          if (written) {
            step("format %u, path %u, slot %u, equation %u", format.value, path, slot, x - 1);
            om::compare(format, cases[x - 1], slot, initial.data() + x * stride, got + offset + x * stride);
          } else
            expect(
                !memcmp(got + offset + x * stride, initial.data() + x * stride, stride), "untouched column %u changed",
                x
            );
        }
      }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
