// contract: dual-source blending with a pixel shader that never writes o1 is a valid pipeline. o1 feeds only the
// SRC1 factors ("This feature enables Output Merger to use both the Pixel Shader outputs o0 and o1 simultaneously",
// D3D11.3 17.6); an unwritten second source is undefined, not an error ("If the second color input to the blender
// is not written in the shader ... the value of the second input is undefined", Vulkan, Dual-Source Blending).
// the SRC1 factors here drive alpha alone, which the write mask drops, so the color is o0's whatever o1 holds; a
// shader that writes o1 drives the color through SRC1_COLOR as the control.
// a sample count the device reports no quality levels for (D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS) is refused
// with E_INVALIDARG; one it reports is accepted. every count up to D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT is tried.
// what Windows does of a pipeline's description, as vkd3d-proton's test_create_graphics_pipeline_state and
// test_create_compute_pipeline_state read it there: a root signature has as many references after a pipeline is made
// of it as before, and none are left once both are released; a format given for a render target past
// NumRenderTargets is refused with E_INVALIDARG, so is a shader given for a stage of another kind
// (test_mismatching_pso_stages), and so is stream output with a root signature that lacks
// D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT ("The app is opting in to using Stream Output", Microsoft Learn,
// D3D12_ROOT_SIGNATURE_FLAGS); with the flag the same description makes a pipeline.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// https://docs.vulkan.org/spec/latest/chapters/framebuffer.html
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
float4 ps() : SV_Target0 { return float4(64, 128, 192, 255) / 255; }
struct Dual { float4 o0 : SV_Target0; float4 o1 : SV_Target1; };
Dual ps_dual() { Dual d; d.o0 = float4(64, 128, 192, 255) / 255; d.o1 = float4(1, 0.5, 0, 1); return d; }
[numthreads(1, 1, 1)] void cs() {}
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       ps_dual = compiler.compile(hlsl, "ps_dual", "ps"), cs = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!vs.empty() && !ps.empty() && !ps_dual.empty() && !cs.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
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
  const auto format = DXGI_FORMAT_R8G8B8A8_UNORM;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root.Get();
  desc.VS = bytecode(vs);
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.SampleDesc = {1, 0};
  desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.DestBlend = D3D12_BLEND_ZERO;
  blend.SrcBlendAlpha = D3D12_BLEND_SRC1_ALPHA;
  blend.DestBlendAlpha = D3D12_BLEND_INV_SRC1_ALPHA;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                D3D12_COLOR_WRITE_ENABLE_BLUE;

  D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, format, {1, 0},
                         D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
  UINT64 bytes;
  device->GetCopyableFootprints(&td, 0, 1, 0, &at, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(readback != nullptr, "readback buffer"))
    return verdict();

  struct Case {
    const char *name;
    const std::string &ps;
    D3D12_BLEND src;
    UINT8 expected[4];
  };
  // o0 * ONE, or o0 * o1 for the control; alpha keeps the cleared zero
  const Case cases[] = {{"o1 unwritten", ps, D3D12_BLEND_ONE, {64, 128, 192, 0}},
                        {"o1 written", ps_dual, D3D12_BLEND_SRC1_COLOR, {64, 64, 0, 0}}};
  for (auto &c : cases) {
    step("dual source, %s", c.name);
    desc.PS = bytecode(c.ps);
    blend.SrcBlend = c.src;
    ComPtr<ID3D12PipelineState> state;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    if (!expect(hr == S_OK && state, "CreateGraphicsPipelineState returned %08lx", hr))
      continue;
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
    D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = at}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT8 *mapped;
    CHECK(readback->Map(0, nullptr, (void **)&mapped));
    UINT8 got[4];
    memcpy(got, mapped + at.Offset, sizeof(got));
    readback->Unmap(0, nullptr);
    expect(!memcmp(got, c.expected, sizeof(got)), "pixel %u %u %u %u, expected %u %u %u %u", got[0], got[1], got[2],
           got[3], c.expected[0], c.expected[1], c.expected[2], c.expected[3]);
  }

  desc.PS = bytecode(ps);
  blend = {};
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  for (UINT count = 1; count <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; count *= 2) {
    step("sample count %u", count);
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{format, count};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)));
    desc.SampleDesc = {count, 0};
    ComPtr<ID3D12PipelineState> state;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    if (levels.NumQualityLevels)
      expect(hr == S_OK && state, "reported count returned %08lx", hr);
    else
      expect(hr == E_INVALIDARG && !state, "unreported count returned %08lx", hr);
  }
  desc.SampleDesc = {1, 0};

  step("a root signature's references");
  {
    auto count = [](IUnknown *object) {
      object->AddRef();
      return object->Release();
    };
    auto signature = root_signature(device.Get(), {});
    ULONG before = count(signature.Get());
    ComPtr<ID3D12PipelineState> graphics, compute;
    desc.pRootSignature = signature.Get();
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&graphics));
    expect(hr == S_OK && count(signature.Get()) == before,
           "a graphics pipeline (%08lx) leaves its root signature %lu references, it had %lu", hr, count(signature.Get()),
           before);
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{signature.Get(), bytecode(cs)};
    hr = device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute));
    expect(hr == S_OK && count(signature.Get()) == before,
           "a compute pipeline (%08lx) leaves its root signature %lu references, it had %lu", hr, count(signature.Get()),
           before);
    graphics.Reset();
    compute.Reset();
    ULONG left = signature.Detach()->Release();
    expect(!left, "the root signature has %lu references after its pipelines and the last of its own", left);
    desc.pRootSignature = root.Get();
  }

  step("a format for a render target past NumRenderTargets");
  for (UINT i = 1; i < std::size(desc.RTVFormats); i++) {
    desc.RTVFormats[i] = format;
    ComPtr<ID3D12PipelineState> state;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    expect(hr == E_INVALIDARG && !state, "a format for render target %u of 1 returned %08lx", i, hr);
    desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
  }

  step("a shader given for another stage");
  {
    ComPtr<ID3D12PipelineState> state;
    desc.PS = bytecode(vs);
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    expect(hr == E_INVALIDARG && !state, "a vertex shader as the pixel shader returned %08lx", hr);
    desc.PS = bytecode(ps);
    desc.VS = bytecode(ps);
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state));
    expect(hr == E_INVALIDARG && !state, "a pixel shader as the vertex shader returned %08lx", hr);
    desc.VS = bytecode(vs);
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{root.Get(), bytecode(vs)};
    hr = device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&state));
    expect(hr == E_INVALIDARG && !state, "a vertex shader as the compute shader returned %08lx", hr);
  }

  step("stream output and the root signature's flag");
  {
    const D3D12_SO_DECLARATION_ENTRY entries[] = {{0, "SV_Position", 0, 0, 4, 0}};
    const UINT strides[] = {16};
    desc.StreamOutput = {entries, 1, strides, 1, 0};
    ComPtr<ID3D12PipelineState> refused, made;
    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&refused));
    expect(hr == E_INVALIDARG && !refused, "stream output with a root signature that does not allow it returned %08lx", hr);
    auto allowing = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
    desc.pRootSignature = allowing.Get();
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&made));
    expect(hr == S_OK && made, "stream output with a root signature that allows it returned %08lx", hr);
  }
  return verdict();
}
