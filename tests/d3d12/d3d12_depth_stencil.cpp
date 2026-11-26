// contract: D3D11.3 17.9-17.13 defines depth enable, comparison, stencil updates and read-only DSV write gates.
// "The conversion of z to Depth Buffer precision uses round to nearest (+0.5 and truncate)." (17.11).
// "If no Depth Buffer is bound, the depth test always passes." (17.11). section 16.9 defines conservative depth.
// source: D3D11_3_FunctionalSpec.htm; Microsoft Learn D3D12_DEPTH_STENCIL_DESC and D3D12_STENCIL_OP.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_depth_stencil_desc
// initial depth/stencil bytes are uploaded per plane; om_depth.hpp derives the final color and both planes.
// shader stencil reference replaces the API reference in comparison and REPLACE.
// all byte references, comparisons and read/write masks are checked when reported.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/shader-specified-stencil-reference-value
#include "d3d12_test.hpp"
#include "../om_depth.hpp"
#include <map>
#include <tuple>

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(om::depth_hlsl, "vs", "vs");
  const char *entries[] = {"ps", "ps_depth", "ps_le", "ps_ge", "ps_stencil"};
  std::string ps[std::size(entries)];
  for (UINT i = 0; i < std::size(entries); i++) {
    ps[i] = compiler.compile(
        om::depth_hlsl, entries[i], "ps",
        {"CONSERVATIVE=1", "STENCIL_REFERENCE=1", "STENCIL_MASK=" + std::to_string(D3D11_DEFAULT_STENCIL_READ_MASK)}
    );
    if (!expect(!ps[i].empty(), "depth HLSL %s did not compile", entries[i]))
      return verdict();
  }
  if (!expect(!vs.empty(), "vertex HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  bool stencil_reference = options.PSSpecifiedStencilRefSupported;
  if (!stencil_reference)
    printf("unsupported: pixel-shader stencil reference\n");
  const UINT width = om::depth_width, faces = 2;
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = 4;
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
  if (!expect(bool(indirect), "indirect arguments missing"))
    return verdict();
  D3D12_DRAW_ARGUMENTS *arguments;
  CHECK(indirect->Map(0, nullptr, (void **)&arguments));
  *arguments = {3, 1, 0, 0};
  indirect->Unmap(0, nullptr);
  for (auto format : om::depth_formats) {
    const auto cases = om::depth_cases(stencil_reference && format.stencil_bits);
    const UINT height = UINT(cases.size()) * faces;
    if (!expect(height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "case surface exceeds the API's texture limit"))
      return verdict();
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format.value};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (!expect(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL, "depth format %u not supported", format.value))
      return verdict();
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                             0,
                             width,
                             height,
                             1,
                             1,
                             format.value,
                             {1, 0},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    auto color_desc = desc;
    color_desc.Format = DXGI_FORMAT_R32_UINT;
    color_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const UINT plane_count = format.stencil_bits ? 2 : 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_at{}, planes[2]{};
    UINT64 total;
    device->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_at, nullptr, nullptr, &total);
    UINT64 plane_base = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    UINT64 row_bytes[2]{};
    device->GetCopyableFootprints(&desc, 0, plane_count, plane_base, planes, nullptr, row_bytes, nullptr);
    total = planes[plane_count - 1].Offset + UINT64(planes[plane_count - 1].Footprint.RowPitch) * height;
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && readback, "upload/readback missing"))
      return verdict();
    unsigned char *mapped;
    CHECK(upload->Map(0, nullptr, (void **)&mapped));
    memset(mapped, 0, total);
    for (UINT p = 0; p < plane_count; p++) {
      if (!expect(row_bytes[p] % width == 0, "plane row is not a whole number of texels"))
        return verdict();
      UINT stride = UINT(row_bytes[p] / width);
      for (UINT y = 0; y < height; y++)
        for (UINT x = 0; x < width; x++) {
          auto to = mapped + planes[p].Offset + y * planes[p].Footprint.RowPitch + x * stride;
          if (!p) {
            UINT value = om::depth_word(format, om::initial_depth(x));
            memcpy(to, &value, format.depth_bits / CHAR_BIT);
          } else
            *to = UINT8(x - 1);
        }
    }
    upload->Unmap(0, nullptr);
    auto key = [](const om::DepthCase &k) {
      const auto &s = k.state;
      return std::tuple{
          k.shader,
          s.DepthEnable,
          s.DepthWriteMask,
          s.DepthFunc,
          s.StencilEnable,
          s.StencilReadMask,
          s.StencilWriteMask,
          s.FrontFace.StencilFailOp,
          s.FrontFace.StencilDepthFailOp,
          s.FrontFace.StencilPassOp,
          s.FrontFace.StencilFunc,
          s.BackFace.StencilFailOp,
          s.BackFace.StencilDepthFailOp,
          s.BackFace.StencilPassOp,
          s.BackFace.StencilFunc
      };
    };
    // references and read-only views are dynamic; retain one PSO per description across execution paths.
    std::map<decltype(key(cases.front())), ComPtr<ID3D12PipelineState>> pipelines;
    for (UINT path = 0; path < 3; path++) {
      step("depth format %u, path %u: upload bytes", format.value, path);
      CHECK(allocator->Reset());
      CHECK(bundle_allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      CHECK(forget(readback.Get()));
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> depth, color;
      CHECK(device->CreateCommittedResource(
          &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depth)
      ));
      CHECK(device->CreateCommittedResource(
          &props, D3D12_HEAP_FLAG_NONE, &color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)
      ));
      for (UINT p = 0; p < plane_count; p++) {
        D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
            to{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.PlacedFootprint = planes[p];
        to.SubresourceIndex = p;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      D3D12_RESOURCE_STATES states[2] = {D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE};
      auto set_plane = [&](UINT p, D3D12_RESOURCE_STATES state) {
        if (states[p] == state)
          return;
        D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition = {depth.Get(), p, states[p], state};
        list->ResourceBarrier(1, &b);
        states[p] = state;
      };
      ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
      D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1}, dh{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4};
      CHECK(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvs)));
      CHECK(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsvs)));
      auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(color.Get(), nullptr, rtv);
      D3D12_CPU_DESCRIPTOR_HANDLE dsv[4];
      for (UINT f = 0; f < std::size(dsv); f++) {
        dsv[f] = dsvs->GetCPUDescriptorHandleForHeapStart();
        dsv[f].ptr += f * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!format.stencil_bits && (f & D3D12_DSV_FLAG_READ_ONLY_STENCIL))
          continue;
        D3D12_DEPTH_STENCIL_VIEW_DESC view{format.value, D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAGS(f)};
        device->CreateDepthStencilView(depth.Get(), &view, dsv[f]);
      }
      const float zero[4] = {};
      list->ClearRenderTargetView(rtv, zero, 0, nullptr);
      std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
      for (UINT i = 0; i < cases.size(); i++) {
        const auto &k = cases[i];
        if ((!format.stencil_bits && (k.flags & D3D11_DSV_READ_ONLY_STENCIL)))
          continue;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = rs.Get();
        p.VS = bytecode(vs);
        p.PS = bytecode(ps[k.shader]);
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = color_desc.Format;
        p.DSVFormat = format.value;
        p.SampleDesc = {1, 0};
        p.SampleMask = ~0u;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        p.RasterizerState.DepthClipEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        const auto &s = k.state;
        p.DepthStencilState = {
            s.DepthEnable,
            D3D12_DEPTH_WRITE_MASK(s.DepthWriteMask),
            D3D12_COMPARISON_FUNC(s.DepthFunc),
            s.StencilEnable,
            s.StencilReadMask,
            s.StencilWriteMask
        };
        auto convert = [](const D3D11_DEPTH_STENCILOP_DESC &d) {
          return D3D12_DEPTH_STENCILOP_DESC{
              D3D12_STENCIL_OP(d.StencilFailOp), D3D12_STENCIL_OP(d.StencilDepthFailOp),
              D3D12_STENCIL_OP(d.StencilPassOp), D3D12_COMPARISON_FUNC(d.StencilFunc)
          };
        };
        p.DepthStencilState.FrontFace = convert(s.FrontFace);
        p.DepthStencilState.BackFace = convert(s.BackFace);
        auto [entry, inserted] = pipelines.try_emplace(key(k));
        auto &pipeline = entry->second;
        if (inserted) {
          HRESULT hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline));
          if (!expect(hr == S_OK && pipeline, "depth case %u PSO returned %08lx", i, hr))
            return verdict();
        }
        for (UINT plane = 0; plane < plane_count; plane++)
          set_plane(
              plane, k.flags & (plane ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_READ_ONLY_DEPTH)
                         ? D3D12_RESOURCE_STATE_DEPTH_READ
                         : D3D12_RESOURCE_STATE_DEPTH_WRITE
          );
        list->OMSetRenderTargets(1, &rtv, FALSE, k.absent ? nullptr : &dsv[k.flags]);
        for (UINT face = 0; face < faces; face++) {
          UINT y = i * faces + face;
          step("depth format %u, path %u, case %u, face %u", format.value, path, i, face);
          D3D12_VIEWPORT viewport{0, float(y), float(width), 1, k.min, k.max};
          D3D12_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          auto record = [&](ID3D12GraphicsCommandList *commands) {
            commands->SetPipelineState(pipeline.Get());
            commands->SetGraphicsRootSignature(rs.Get());
            UINT words[] = {std::bit_cast<UINT>(k.depth), face, k.reference, 0};
            commands->SetGraphicsRoot32BitConstants(0, std::size(words), words, 0);
            commands->OMSetStencilRef(k.shader == 4 ? k.reference ^ D3D11_DEFAULT_STENCIL_READ_MASK : k.reference);
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
        }
      }
      transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT plane = 0; plane < plane_count; plane++)
        set_plane(plane, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT copy = 0; copy <= plane_count; copy++) {
        D3D12_TEXTURE_COPY_LOCATION from{copy ? depth.Get() : color.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.SubresourceIndex = copy ? copy - 1 : 0;
        to.PlacedFootprint = copy ? planes[copy - 1] : color_at;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const unsigned char *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < cases.size(); i++)
        for (UINT face = 0; face < faces; face++) {
          const auto &k = cases[i];
          UINT y = i * faces + face;
          step("depth format %u, path %u, case %u, face %u: readback", format.value, path, i, face);
          bool active = !(!format.stencil_bits && (k.flags & D3D11_DSV_READ_ONLY_STENCIL));
          for (UINT x = 0; x < width; x++) {
            om::DepthResult want{om::depth_word(format, om::initial_depth(x)), 0, UINT8(x - 1)};
            if (active && x && x + 1 < width)
              want = om::depth_result(format, k, x, face);
            UINT depth_word = 0, color_word;
            memcpy(
                &depth_word, got + planes[0].Offset + y * planes[0].Footprint.RowPitch + x * (row_bytes[0] / width),
                format.depth_bits / CHAR_BIT
            );
            memcpy(
                &color_word, got + color_at.Offset + y * color_at.Footprint.RowPitch + x * sizeof(UINT), sizeof(UINT)
            );
            UINT8 stencil = format.stencil_bits
                                ? got[planes[1].Offset + y * planes[1].Footprint.RowPitch + x * (row_bytes[1] / width)]
                                : 0;
            if (!expect(
                    depth_word == want.depth && color_word == want.color &&
                        (!format.stencil_bits || stencil == want.stencil),
                    "column %u: depth %#x/%#x, color %u/%u, stencil %u/%u", x, depth_word, want.depth, color_word,
                    want.color, stencil, want.stencil
                ))
              break;
          }
        }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
