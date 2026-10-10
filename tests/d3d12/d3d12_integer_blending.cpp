// contract: blending on an integer target is refused only when the pixel shader declares that output, even with
// a zero write mask. "If a given o# register has no components declared for output then the RenderTarget at that
// output slot is not modified regardless of any other settings (such as write masks or blend modes)."
// (D3D11.3 16.9.1). the runtime validates "Whether blend state is compatible with render target formats."
// (Microsoft Learn, D3D12_GRAPHICS_PIPELINE_STATE_DESC, Remarks); vkd3d-proton's
// test_integer_blending_pipeline_state records S_OK for no output and E_INVALIDARG for an integer output.
// the scalar formats are controls for float, unsigned and signed outputs; slots come from the API's target array.
// accepted float blending must remain enabled: ONE/ONE with ADD ("Add source 1 and source 2.", Microsoft Learn,
// D3D12_BLEND_OP) gives 3 when the shader outputs 1 and the target holds 2.
#include "d3d12_test.hpp"
#include <array>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
void ps_none() {}
TYPE ps() : TARGET { return TYPE(1); }
struct Pair { TYPE value : TARGET; float other : OTHER_TARGET; };
Pair ps_pair() { Pair p; p.value = TYPE(1); p.other = 1; return p; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  constexpr UINT targets = std::size(desc.RTVFormats);
  const struct {
    DXGI_FORMAT format;
    const char *type;
  } scalars[] = {{DXGI_FORMAT_R32_FLOAT, "float"}, {DXGI_FORMAT_R32_UINT, "uint"}, {DXGI_FORMAT_R32_SINT, "int"}};
  std::array<std::array<std::string, targets>, std::size(scalars)> pixels, pairs;
  const std::vector<std::string> defaults = {"TYPE=float", "TARGET=SV_Target0", "OTHER_TARGET=SV_Target1"};
  auto vs = compiler.compile(hlsl, "vs", "vs", defaults);
  auto none = compiler.compile(hlsl, "ps_none", "ps", defaults);
  if (!expect(!vs.empty() && !none.empty(), "HLSL did not compile"))
    return verdict();
  for (UINT kind = 0; kind < std::size(scalars); kind++)
    for (UINT slot = 0; slot < targets; slot++) {
      step("compile %s output at slot %u", scalars[kind].type, slot);
      const std::vector<std::string> defines = {
          std::string("TYPE=") + scalars[kind].type, "TARGET=SV_Target" + std::to_string(slot),
          "OTHER_TARGET=SV_Target" + std::to_string((slot + 1) % targets)
      };
      pixels[kind][slot] = compiler.compile(hlsl, "ps", "ps", defines);
      pairs[kind][slot] = compiler.compile(hlsl, "ps_pair", "ps", defines);
      if (!expect(!pixels[kind][slot].empty() && !pairs[kind][slot].empty(), "HLSL did not compile"))
        return verdict();
    }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(), {});
  if (!expect(bool(rs), "root signature was not created"))
    return verdict();
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs);
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.SampleDesc = {1, 0};
  for (auto &rt : desc.BlendState.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  }

  desc.BlendState.RenderTarget[0].BlendEnable = true;
  for (auto pixel : {bytecode(none), D3D12_SHADER_BYTECODE{}}) {
    step("no targets, pixel shader %u", pixel.BytecodeLength != 0);
    desc.PS = pixel;
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(hr == S_OK && pso, "no-target pipeline returned %08lx", hr);
  }

  enum Output { None, Null, Target, Other, Both };
  for (UINT kind = 0; kind < std::size(scalars); kind++)
    for (UINT slot = 0; slot < targets; slot++)
      for (auto output : {None, Null, Target, Other, Both})
        for (bool independent : {false, true})
          for (bool blend : {false, true})
            for (UINT mask : {0u, UINT(D3D12_COLOR_WRITE_ENABLE_ALL)}) {
              desc.PS = output == None ? bytecode(none)
                        : output == Null ? D3D12_SHADER_BYTECODE{}
                        : output == Target ? bytecode(pixels[kind][slot])
                        : output == Both ? bytecode(pairs[kind][slot])
                                         : bytecode(pixels[0][(slot + 1) % targets]);
              desc.NumRenderTargets = output == Other || output == Both ? targets : slot + 1;
              for (auto &format : desc.RTVFormats)
                format = DXGI_FORMAT_R32_FLOAT;
              desc.RTVFormats[slot] = scalars[kind].format;
              desc.BlendState.IndependentBlendEnable = independent;
              for (auto &rt : desc.BlendState.RenderTarget)
                rt.BlendEnable = false, rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
              auto &rt = desc.BlendState.RenderTarget[independent ? slot : 0];
              rt.BlendEnable = blend;
              rt.RenderTargetWriteMask = mask;
              step(
                  "format %u, slot %u, output %u, independent %u, blend %u, mask %u", scalars[kind].format, slot,
                  output, independent, blend, mask
              );
              ComPtr<ID3D12PipelineState> pso;
              HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
              HRESULT want =
                  blend && (output == Target || output == Both) && scalars[kind].format != DXGI_FORMAT_R32_FLOAT
                      ? E_INVALIDARG
                      : S_OK;
              expect(hr == want, "CreateGraphicsPipelineState returned %08lx, expected %08lx", hr, want);
              expect(want == S_OK ? bool(pso) : !pso, "pipeline pointer disagrees with the expected result");
            }

  step("ONE/ONE ADD on a float target");
  desc.PS = bytecode(pixels[0][0]);
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  desc.BlendState.IndependentBlendEnable = FALSE;
  auto &rt = desc.BlendState.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ONE;
  rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));

  const UINT size = 2;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                  {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto rtv = views->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(readback), "readback buffer was not created"))
    return verdict();
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  const float clear[] = {2, 2, 2, 2};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  into.PlacedFootprint = footprint;
  list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  const char *got;
  CHECK(readback->Map(0, nullptr, (void **)&got));
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++) {
      float value = *(const float *)(got + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(float));
      expect(value == 1 + clear[0], "pixel %u,%u is %g, expected %g", x, y, value, 1 + clear[0]);
    }
  readback->Unmap(0, nullptr);
  return verdict();
}
