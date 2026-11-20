// contract: non-renderable texture formats do not prevent a graphics pipeline from being created.
// vkd3d-proton's Windows witness test_render_target_support_validation: "No errors are created. Only logical
// conclusion is that bogus formats are nulled out." (tests/d3d12_pso.c). for stage-to-memory outputs without a
// destination binding, "the extra data is discarded." (D3D11.3 4.4.3.3).
// https://github.com/HansKristian-Work/vkd3d-proton/blob/master/tests/d3d12_pso.c
// ordinary and geometry pipelines exercise both Metal pipeline descriptions, at every color slot, with valid
// color formats, depth formats, compressed formats and unknown formats. an invalid format enum still fails.
// typed slot-zero controls draw one into a zero-cleared target, so dropping every format cannot pass.
// null slots still validate blend state: "It's not valid for LogicOpEnable and BlendEnable to both be TRUE."
// (Microsoft Learn, D3D12_RENDER_TARGET_BLEND_DESC); logic operations also require IndependentBlendEnable FALSE
// (D3D12_BLEND_DESC, Remarks).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_render_target_blend_desc
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_blend_desc
// a later declared integer target still refuses blending, even with a zero write mask (vkd3d-proton's
// test_integer_blending_pipeline_state): a null slot must not stop validation of following slots.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>

static const char hlsl[] = R"hlsl(
struct V { float4 position : SV_Position; };
V vs(uint id : SV_VertexID) {
  V result;
  result.position = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
  return result;
}
[maxvertexcount(3)]
void gs(triangle V verticesIn[3], inout TriangleStream<V> verticesOut) {
  for (uint i = 0; i < 3; i++) verticesOut.Append(verticesIn[i]);
}
float4 ps() : SV_Target { return 1; }
uint ps_uint() : SV_Target { return 1; }
uint ps_other() : SV_Target1 { return 1; }
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), gs = compiler.compile(hlsl, "gs", "gs"),
       ps = compiler.compile(hlsl, "ps", "ps"), ps_uint = compiler.compile(hlsl, "ps_uint", "ps"),
       ps_other = compiler.compile(hlsl, "ps_other", "ps");
  if (!expect(!vs.empty() && !gs.empty() && !ps.empty() && !ps_uint.empty() && !ps_other.empty(),
              "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto root = root_signature(device.Get(), {});
  if (!expect(root != nullptr, "root signature"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get();
  desc.VS = bytecode(vs);
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.SampleDesc = {1, 0};
  desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_TYPELESS,
                                DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_BC6H_UF16,
                                DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC1_UNORM};
  for (bool geometry : {false, true}) {
    desc.GS = geometry ? bytecode(gs) : D3D12_SHADER_BYTECODE{};
    desc.PS = bytecode(ps);
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    desc.BlendState.IndependentBlendEnable = TRUE;
    auto &blend = desc.BlendState.RenderTarget[1];
    blend.LogicOpEnable = TRUE;
    blend.LogicOp = D3D12_LOGIC_OP_COPY;
    for (auto format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_BC6H_UF16})
      for (bool enabled : {false, true}) {
        step("geometry %u, invalid logic state, format %u, blend %u", geometry, format, enabled);
        desc.RTVFormats[1] = format;
        blend.BlendEnable = enabled;
        ComPtr<ID3D12PipelineState> state;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
        expect(hr == E_INVALIDARG && !state, "invalid logic state returned %08lx", hr);
      }
    step("geometry %u, integer blending after a null slot", geometry);
    desc.RTVFormats[0] = DXGI_FORMAT_BC6H_UF16;
    desc.RTVFormats[1] = DXGI_FORMAT_R32_UINT;
    desc.PS = bytecode(ps_other);
    blend.LogicOpEnable = FALSE;
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ComPtr<ID3D12PipelineState> rejected;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&rejected));
    expect(hr == E_INVALIDARG && !rejected, "integer blending after a null slot returned %08lx", hr);
    blend = {};
    desc.BlendState.IndependentBlendEnable = FALSE;
    for (UINT slot = 0; slot < std::size(desc.RTVFormats); slot++)
      for (auto format : formats) {
        step("geometry %u, slot %u, format %u", geometry, slot, format);
        std::fill(std::begin(desc.RTVFormats), std::end(desc.RTVFormats), DXGI_FORMAT_UNKNOWN);
        desc.NumRenderTargets = slot + 1;
        desc.RTVFormats[slot] = format;
        desc.PS = bytecode(format == DXGI_FORMAT_R32_UINT ? ps_uint : ps);
        ComPtr<ID3D12PipelineState> state;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
        if (!expect(hr == S_OK && state, "CreateGraphicsPipelineState returned %08lx", hr))
          return verdict();
        if (slot == 0 && (format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R32_UINT)) {
          D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, format, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
          D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
          ComPtr<ID3D12Resource> target;
          CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
          device->CreateRenderTargetView(target.Get(), nullptr, rtv);
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
          UINT64 bytes;
          device->GetCopyableFootprints(&td, 0, 1, 0, &at, nullptr, nullptr, &bytes);
          auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
          if (!expect(readback != nullptr, "readback buffer"))
            return verdict();
          CHECK(forget(readback.Get()));
          CHECK(allocator->Reset());
          CHECK(list->Reset(allocator.Get(), nullptr));
          const float zero[4]{};
          list->ClearRenderTargetView(rtv, zero, 0, nullptr);
          list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
          list->SetPipelineState(state.Get());
          list->SetGraphicsRootSignature(root.Get());
          D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
          D3D12_RECT scissor{0, 0, 1, 1};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          list->DrawInstanced(3, 1, 0, 0);
          transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
          D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                        {.PlacedFootprint = at}};
          list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          UINT *mapped;
          CHECK(readback->Map(0, nullptr, (void **)&mapped));
          UINT got = mapped[at.Offset / sizeof(*mapped)];
          readback->Unmap(0, nullptr);
          expect(got == (format == DXGI_FORMAT_R32_UINT ? 1u : std::bit_cast<UINT>(1.0f)),
                 "valid color attachment lost its output: %#x", got);
        }
      }
    step("geometry %u, invalid format enum", geometry);
    desc.RTVFormats[0] = DXGI_FORMAT_FORCE_UINT;
    desc.NumRenderTargets = 1;
    std::fill(std::begin(desc.RTVFormats) + 1, std::end(desc.RTVFormats), DXGI_FORMAT_UNKNOWN);
    ComPtr<ID3D12PipelineState> state;
    expect(FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state))) && !state,
           "invalid format enum accepted");
  }
  return verdict();
}
