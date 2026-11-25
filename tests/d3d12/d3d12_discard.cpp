// contract: DiscardResource leaves a resource's contents undefined and nothing else: what the GPU wrote to other
// resources, and what the list writes to the discarded one after it, are as they would be without it
// (ID3D12GraphicsCommandList::DiscardResource: "the resource will be left in an undefined state"; a discard of
// rectangles leaves the rest as it was), and so do a render pass's discarding accesses
// (D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD, D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD, and a resolve whose
// source need not be preserved, PreserveResolveSource). the cases: two 4x targets drawn in one pass, one resolved and
// discarded, the other resolved after and again in a later list; a target discarded, cleared and half drawn; one
// discarded with a rectangle and half drawn; one discarded and drawn whole; one discarded while its pass draws, then
// drawn again; a render pass resolving its 4x target without preserving it; one beginning with a discard and drawing
// the whole target. every texel read is the one the case wrote; a discarded texel nothing wrote after is not read.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-discardresource
#include "d3d12_test.hpp"
#include <d3d12.h>

static constexpr UINT size = 64;

static const char hlsl[] = R"hlsl(
cbuffer Color : register(b0) { float4 color; float2 range; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 at = float2(id == 1 ? 3 : -1, id == 2 ? -3 : 1);
  return float4(at, 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target0 {
  if (position.x < range.x || position.x >= range.y)
    discard;
  return color;
}
struct Two { float4 a : SV_Target0; float4 b : SV_Target1; };
Two ps_two(float4 position : SV_Position) {
  Two o;
  o.a = color;
  o.b = color.bgra;
  return o;
}
)hlsl";

static uint32_t
packed(const float c[4]) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= uint32_t(c[i] * 255 + 0.5f) << (8 * i);
  return v;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_two = compiler.compile(hlsl, "ps_two", "ps");
  if (!expect(!vs.empty() && !ps.empty() && !ps_two.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList4> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  parameter.Constants = {0, 0, 6};
  D3D12_ROOT_SIGNATURE_DESC rs_desc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  auto signature = root_signature(device.Get(), rs_desc);
  auto pipeline = [&](const std::string &pixel, UINT targets, UINT samples) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = signature.Get();
    desc.VS = bytecode(vs);
    desc.PS = bytecode(pixel);
    for (UINT i = 0; i < targets; i++) {
      desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    desc.BlendState.IndependentBlendEnable = TRUE;
    desc.SampleMask = ~0u;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = targets;
    desc.SampleDesc.Count = samples;
    ComPtr<ID3D12PipelineState> pso;
    device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    return pso;
  };
  auto one = pipeline(ps, 1, 1), one4 = pipeline(ps, 1, 4), two4 = pipeline(ps_two, 2, 4);
  if (!expect(one && one4 && two4, "pipelines were not made"))
    return verdict();

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  UINT next_rtv = 0;
  auto texture = [&](UINT samples, D3D12_CPU_DESCRIPTOR_HANDLE *rtv, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                             {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
    if (rtv && res) {
      *rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
      rtv->ptr += next_rtv++ * rtv_size;
      device->CreateRenderTargetView(res.Get(), nullptr, *rtv);
    }
    return res;
  };
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size * size * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
  auto begin = [&]() -> HRESULT {
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    list->SetGraphicsRootSignature(signature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    return S_OK;
  };
  // a draw of `color` over the columns [from, to)
  auto draw = [&](ID3D12PipelineState *pso, const float color[4], float from = 0, float to = size) {
    float constants[6] = {color[0], color[1], color[2], color[3], from, to};
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, 6, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);
  };
  // the texels of a single-sampled target in COPY_SOURCE, after the list it is read in ran
  auto read = [&](ID3D12Resource *target, std::vector<uint32_t> &out) -> HRESULT {
    D3D12_TEXTURE_COPY_LOCATION src{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, size * 4};
    forget(readback.Get());
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    void *bytes;
    CHECK(readback->Map(0, nullptr, &bytes));
    out.assign((uint32_t *)bytes, (uint32_t *)bytes + size * size);
    readback->Unmap(0, nullptr);
    return S_OK;
  };
  // that the columns [from, to) of `texels` are `want`
  auto check = [&](const std::vector<uint32_t> &texels, uint32_t want, UINT from = 0, UINT to = size) {
    for (UINT y = 0; y < size; y++)
      for (UINT x = from; x < to; x++)
        if (texels[y * size + x] != want) {
          expect(false, "texel %u,%u is %08x, want %08x", x, y, texels[y * size + x], want);
          return;
        }
  };
  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
  const float bgra_red[4] = {0, 0, 1, 1};
  std::vector<uint32_t> texels;
  const auto rt = D3D12_RESOURCE_STATE_RENDER_TARGET;

  {
    step("two 4x targets of one pass, the first resolved and discarded, the second resolved after and later");
    D3D12_CPU_DESCRIPTOR_HANDLE h[2];
    auto a = texture(4, &h[0], rt), b = texture(4, &h[1], rt);
    auto ra = texture(1, nullptr, D3D12_RESOURCE_STATE_RESOLVE_DEST), rb = texture(1, nullptr, D3D12_RESOURCE_STATE_RESOLVE_DEST),
         rb2 = texture(1, nullptr, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    CHECK(begin());
    list->OMSetRenderTargets(2, h, FALSE, nullptr);
    draw(two4.Get(), red);
    transition(list.Get(), a.Get(), rt, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    transition(list.Get(), b.Get(), rt, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresource(ra.Get(), 0, a.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    transition(list.Get(), a.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, rt);
    list->DiscardResource(a.Get(), nullptr);
    list->ResolveSubresource(rb.Get(), 0, b.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(begin());
    list->ResolveSubresource(rb2.Get(), 0, b.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (auto r : {ra.Get(), rb.Get(), rb2.Get()})
      transition(list.Get(), r, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    for (auto [r, want, name] : {std::tuple{ra.Get(), packed(red), "first"}, std::tuple{rb.Get(), packed(bgra_red), "second"},
                                 std::tuple{rb2.Get(), packed(bgra_red), "second, resolved later"}}) {
      step("two 4x targets: the %s resolve", name);
      CHECK(begin());
      CHECK(read(r, texels));
      check(texels, want);
    }
  }

  // a single-sampled target filled red in its own list, then the case's list
  auto filled = [&](D3D12_CPU_DESCRIPTOR_HANDLE &h) {
    auto t = texture(1, &h, rt);
    expect(SUCCEEDED(begin()), "Reset failed");
    list->OMSetRenderTargets(1, &h, FALSE, nullptr);
    draw(one.Get(), red);
    expect(SUCCEEDED(submit(device.Get(), queue.Get(), list.Get())), "the fill did not run");
    return t;
  };
  auto result = [&](ID3D12Resource *t) -> HRESULT {
    transition(list.Get(), t, rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return read(t, texels);
  };
  {
    step("discarded, cleared, half drawn");
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    auto t = filled(h);
    CHECK(begin());
    list->DiscardResource(t.Get(), nullptr);
    list->ClearRenderTargetView(h, blue, 0, nullptr);
    list->OMSetRenderTargets(1, &h, FALSE, nullptr);
    draw(one.Get(), green, 0, size / 2);
    CHECK(result(t.Get()));
    check(texels, packed(green), 0, size / 2);
    check(texels, packed(blue), size / 2, size);
  }
  {
    step("a rectangle discarded, half drawn");
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    auto t = filled(h);
    CHECK(begin());
    D3D12_RECT left{0, 0, LONG(size / 2), LONG(size)};
    D3D12_DISCARD_REGION region{1, &left, 0, 1};
    list->DiscardResource(t.Get(), &region);
    list->OMSetRenderTargets(1, &h, FALSE, nullptr);
    draw(one.Get(), green, 0, size / 2);
    CHECK(result(t.Get()));
    check(texels, packed(green), 0, size / 2);
    check(texels, packed(red), size / 2, size);
  }
  {
    step("discarded, drawn whole");
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    auto t = filled(h);
    CHECK(begin());
    list->DiscardResource(t.Get(), nullptr);
    list->OMSetRenderTargets(1, &h, FALSE, nullptr);
    draw(one.Get(), green);
    CHECK(result(t.Get()));
    check(texels, packed(green));
  }
  {
    step("discarded as its pass draws, then drawn again");
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    auto t = filled(h);
    CHECK(begin());
    list->OMSetRenderTargets(1, &h, FALSE, nullptr);
    draw(one.Get(), white, 0, size / 2);
    list->DiscardResource(t.Get(), nullptr);
    draw(one.Get(), green, size / 2, size);
    CHECK(result(t.Get()));
    check(texels, packed(green), size / 2, size);
  }
  {
    step("a render pass resolving its 4x target without preserving it");
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    auto m = texture(4, &h, rt);
    auto r = texture(1, nullptr, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    CHECK(begin());
    D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS sub{0, 0, 0, 0, {0, 0, LONG(size), LONG(size)}};
    D3D12_RENDER_PASS_RENDER_TARGET_DESC target{h};
    target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
    target.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    memcpy(target.BeginningAccess.Clear.ClearValue.Color, blue, sizeof(blue));
    target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
    target.EndingAccess.Resolve = {m.Get(), r.Get(), 1, &sub, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE, FALSE};
    list->BeginRenderPass(1, &target, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    draw(one4.Get(), green, 0, size / 2);
    list->EndRenderPass();
    transition(list.Get(), r.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(read(r.Get(), texels));
    check(texels, packed(green), 0, size / 2);
    check(texels, packed(blue), size / 2, size);
  }
  {
    step("a render pass beginning with a discard, drawing the whole target");
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    auto t = filled(h);
    CHECK(begin());
    D3D12_RENDER_PASS_RENDER_TARGET_DESC target{h};
    target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
    target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
    list->BeginRenderPass(1, &target, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
    draw(one.Get(), green);
    list->EndRenderPass();
    CHECK(result(t.Get()));
    check(texels, packed(green));
  }
  return verdict();
}
