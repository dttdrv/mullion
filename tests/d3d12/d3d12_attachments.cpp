// contract: an undeclared color output leaves its target unchanged, and an unbound output is discarded, without
// changing another slot. "If a given o# register has no components declared for output then the RenderTarget at
// that output slot is not modified regardless of any other settings (such as write masks or blend modes)."
// (D3D11.3 16.9.1). "If a Stage-to-Memory interface outputs more Elements or components within Elements than there
// are destination memory bindings to accommodate, the extra data is discarded." (4.4.3.3).
// each pixel is one binding mask against one pipeline mask and variant, from D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT.
// every target starts with its own value; additive blending adds only the outputs with valid, bound RTV slots.
// each mask alternates pipelines with different values. full lists rotate the shader output mask by one slot on
// the first draw, so formats and outputs vary independently. each binding row is submitted separately; queries
// count the draws with no attachments too: dropping those draws cannot satisfy the pixel checks alone. a stencil
// target increments once only where the pipeline names it and it is bound: "Increment the stencil value by 1,
// and clamp the result." (Microsoft Learn, D3D12_STENCIL_OP_INCR_SAT).
#include "d3d12_test.hpp"

static const char vertex[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT slots = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT, masks = 1u << slots, variants = 2;
  const UINT width = masks * variants;
  auto vs = compiler.compile(vertex, "vs", "vs");
  if (!expect(!vs.empty(), "vertex HLSL did not compile"))
    return verdict();
  std::vector<std::string> shaders(masks * variants);
  for (UINT shader = 0; shader < shaders.size(); shader++) {
    UINT mask = shader % masks, value = shader / masks + 1;
    step("compile output mask %#x, value multiplier %u", mask, value);
    std::string fields, writes;
    for (UINT slot = 0; slot < slots; slot++)
      if (mask & (1u << slot)) {
        auto name = std::to_string(slot);
        fields += "float c" + name + " : SV_Target" + name + "; ";
        writes += "o.c" + name + " = " + std::to_string((slot + 1) * value) + "; ";
      }
    auto text = mask ? "struct O {" + fields + "}; O ps() { O o;" + writes + "return o; }" : "void ps() {}";
    shaders[shader] = compiler.compile("PROGRAM", "ps", "ps", {"PROGRAM=" + text});
    if (!expect(!shaders[shader].empty(), "pixel HLSL did not compile, mask %#x, multiplier %u", mask, value))
      return verdict();
  }

  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(rs != nullptr, "root signature"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  const DXGI_FORMAT format = DXGI_FORMAT_R32_FLOAT;
  ComPtr<ID3D12Resource> targets[slots];
  D3D12_RESOURCE_DESC texture{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, masks, 1, 1, format, {1, 0},
                            D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, slots + 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  D3D12_CPU_DESCRIPTOR_HANDLE handles[slots + 1];
  for (UINT slot = 0; slot <= slots; slot++) {
    handles[slot] = rtvs->GetCPUDescriptorHandleForHeapStart();
    handles[slot].ptr += slot * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (slot < slots) {
      CHECK(device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
          IID_PPV_ARGS(&targets[slot])
      ));
      device->CreateRenderTargetView(targets[slot].Get(), nullptr, handles[slot]);
    } else {
      D3D12_RENDER_TARGET_VIEW_DESC null_desc{format, D3D12_RTV_DIMENSION_TEXTURE2D};
      device->CreateRenderTargetView(nullptr, &null_desc, handles[slot]);
    }
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
  UINT64 bytes;
  device->GetCopyableFootprints(&texture, 0, 1, 0, &at, nullptr, nullptr, &bytes);
  auto depth_desc = texture;
  depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  ComPtr<ID3D12Resource> depth;
  ComPtr<ID3D12DescriptorHeap> dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        nullptr, IID_PPV_ARGS(&depth)));
  auto dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT stencil_at;
  UINT64 stencil_bytes;
  device->GetCopyableFootprints(&depth_desc, 1, 1, 0, &stencil_at, nullptr, nullptr, &stencil_bytes);
  auto stencil = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, stencil_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto counts = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(readback && counts && stencil, "readback buffers"))
    return verdict();
  ComPtr<ID3D12QueryHeap> query;
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1};
  CHECK(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query)));

  for (UINT mode = 0; mode < 3; mode++) {
    bool full = mode != 0;
    const char *name = !full ? "compact" : mode == 2 ? "full with alternating stencil" : "full";
    step("%s slot lists: create pipelines", name);
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(masks * variants);
    for (UINT pipeline = 0; pipeline < pipelines.size(); pipeline++) {
      UINT mask = pipeline / variants, variant = pipeline % variants;
      UINT outputs = full && !variant ? ((mask << 1) | (mask >> (slots - 1))) & (masks - 1) : mask;
      step("%s lists, create pipeline %#x, variant %u, outputs %#x", name, mask, variant, outputs);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(shaders[variant * masks + outputs])};
      desc.BlendState.IndependentBlendEnable = TRUE;
      for (UINT slot = 0; slot < slots; slot++) {
        if (mask & (1u << slot)) {
          desc.NumRenderTargets = slot + 1;
          desc.RTVFormats[slot] = format;
        }
        auto &blend = desc.BlendState.RenderTarget[slot];
        blend.BlendEnable = TRUE;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.SrcBlend = blend.SrcBlendAlpha = blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ONE;
      }
      if (full)
        desc.NumRenderTargets = slots;
      desc.SampleMask = ~0u;
      desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.SampleDesc = {1, 0};
      if (!variant) {
        desc.DSVFormat = depth_desc.Format;
        desc.DepthStencilState.StencilEnable = TRUE;
        desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR_SAT,
                                            D3D12_COMPARISON_FUNC_ALWAYS};
        desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
      }
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[pipeline])));
    }
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(masks), 0, 1};
    for (UINT bound = 0; bound < masks; bound++) {
      bool depth_bound = mode == 2 && (bound & 1);
      step("%s lists, bound %#x, %s pipeline order", name, bound, full ? "descending" : "ascending");
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      if (!bound) {
        CHECK(forget(counts.Get()));
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0, 0, 0, nullptr);
        for (UINT slot = 0; slot < slots; slot++) {
          const float clear[4] = {float((slot + 1) * (masks + 1))};
          list->ClearRenderTargetView(handles[slot], clear, 0, nullptr);
        }
      }
      list->SetGraphicsRootSignature(rs.Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      list->RSSetViewports(1, &viewport);
      UINT count = 0;
      D3D12_CPU_DESCRIPTOR_HANDLE bound_handles[slots];
      for (UINT slot = 0; slot < slots; slot++) {
        bound_handles[slot] = handles[bound & (1u << slot) ? slot : slots];
        if (bound & (1u << slot))
          count = slot + 1;
      }
      list->OMSetRenderTargets(full ? slots : count, bound_handles, FALSE, depth_bound ? &dsv : nullptr);
      if (!bound)
        list->BeginQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      for (UINT draw = 0; draw < masks; draw++) {
        UINT mask = full ? masks - draw - 1 : draw;
        for (UINT variant = 0; variant < variants; variant++) {
          UINT x = mask * variants + variant;
          const D3D12_RECT rect{LONG(x), LONG(bound), LONG(x + 1), LONG(bound + 1)};
          list->RSSetScissorRects(1, &rect);
          list->SetPipelineState(pipelines[mask * variants + variant].Get());
          list->DrawInstanced(3, 1, 0, 0);
        }
      }
      if (!bound) {
        list->EndQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, counts.Get(), 0);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      if (!bound) {
        const UINT64 *samples;
        CHECK(counts->Map(0, nullptr, (void **)&samples));
        expect(*samples == masks * variants, "%s lists, attachment-free draws: %llu samples, want %u",
               name, *samples, masks * variants);
        counts->Unmap(0, nullptr);
      }
    }

    for (UINT slot = 0; slot <= slots; slot++) {
      bool stencil_slot = slot == slots;
      auto target = stencil_slot ? depth.Get() : targets[slot].Get();
      auto result = stencil_slot ? stencil.Get() : readback.Get();
      auto footprint = stencil_slot ? stencil_at : at;
      auto state = stencil_slot ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      CHECK(forget(result));
      transition(list.Get(), target, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                       {.SubresourceIndex = UINT(stencil_slot)}},
          to{result, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const char *got;
      CHECK(result->Map(0, nullptr, (void **)&got));
      for (UINT bound = 0; bound < masks; bound++) {
        step("%s lists, read bound %#x, slot %u", name, bound, slot);
        for (UINT x = 0; x < width; x++) {
          UINT mask = x / variants, variant = x % variants;
          float value;
          auto pixel = got + footprint.Offset + bound * footprint.Footprint.RowPitch;
          if (stencil_slot)
            value = uint8_t(pixel[x]);
          else
            memcpy(&value, pixel + x * sizeof(value), sizeof(value));
          UINT outputs = full ? ((mask << 1) | (mask >> (slots - 1))) & (masks - 1) : mask;
          float want = (slot + 1) * (masks + 1);
          if (stencil_slot)
            want = mode == 2 && (bound & 1) && !variant;
          else if (bound & mask & (variant ? mask : outputs) & (1u << slot))
            want += (slot + 1) * (variant + 1);
          if (!expect(value == want, "%s lists, bound %#x, pipeline %#x, variant %u, slot %u: %g, want %g",
                      name, bound, mask, variant, slot, value, want)) {
            result->Unmap(0, nullptr);
            return verdict();
          }
        }
      }
      result->Unmap(0, nullptr);
    }
  }
  return verdict();
}
