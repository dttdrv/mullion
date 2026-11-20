// contract: all 64 DWORDs of root constants reach the shader; partial updates preserve other values, including
// across bundles. "Root constants cost 1 DWORD each" and "The maximum size of a root signature is 64 DWORDs."
// SetGraphicsRoot32BitConstants: "It is possible to partially set the root signature constants."
// a pixel per DWORD exposes the whole budget without consuming a root entry for the output.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setgraphicsroot32bitconstants
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-constants-directly-in-the-root-signature
#include "d3d12_test.hpp"
#include <array>
#include <limits>

static const char hlsl[] = R"hlsl(
cbuffer constants : register(b0) { FIELDS };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps(float4 position : SV_Position) : SV_Target {
  uint i = uint(position.x);
  READS
  return 0;
}
)hlsl";

int main(int argc, char **argv) {
  constexpr UINT count = D3D12_MAX_ROOT_COST, rows = 4;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // root constants cannot contain arrays (Microsoft Learn, Using Constants Directly in the Root Signature).
  std::string fields, reads;
  constexpr UINT components = 4; // HLSL uint4
  for (UINT group = 0; group < count / components; group++) {
    auto name = "words" + std::to_string(group);
    fields += "uint4 " + name + ";";
    reads += "if (i / 4 == " + std::to_string(group) + ") return " + name + "[i % 4];";
  }
  auto vs = compiler.compile(hlsl, "vs", "vs", {"FIELDS=" + fields, "READS=" + reads});
  auto ps = compiler.compile(hlsl, "ps", "ps", {"FIELDS=" + fields, "READS=" + reads});
  if (!expect(!vs.empty() && !ps.empty(), "root constant shaders did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  param.Constants.Num32BitValues = count;
  auto root = root_signature(device.Get(), {1, &param});
  if (!expect(root != nullptr, "64-DWORD root signature"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{root.Get(), bytecode(vs), bytecode(ps)};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = ~0u;
  pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  pd.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list, bundle;
  D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, q.Type, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(),
                                  IID_PPV_ARGS(&bundle)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, count, rows, 1, 1, pd.RTVFormats[0], {1, 0},
                         D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT at;
  UINT64 bytes;
  device->GetCopyableFootprints(&td, 0, 1, 0, &at, nullptr, nullptr, &bytes);
  auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(back != nullptr, "readback buffer"))
    return verdict();
  std::array<UINT, count> values;
  for (UINT i = 0; i < count; i++)
    values[i] = (i & 1) ? ~i : i;
  values[0] = 0;
  values[1] = 1;
  values[count - 2] = UINT(std::numeric_limits<INT>::min());
  values[count - 1] = std::numeric_limits<UINT>::max();
  std::array<std::array<UINT, count>, rows> expected;
  D3D12_VIEWPORT viewport{0, 0, float(count), float(rows), 0, 1};
  list->RSSetViewports(1, &viewport);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->SetGraphicsRootSignature(root.Get());
  const float sentinel[4] = {float(count)};
  list->ClearRenderTargetView(rtv, sentinel, 0, nullptr);
  for (UINT row = 0; row < rows; row++) {
    step("root DWORD row %u: %s", row, row < 2 ? "direct" : row == 2 ? "bundle" : "parent after bundle");
    D3D12_RECT rect{0, LONG(row), LONG(count), LONG(row + 1)};
    list->RSSetScissorRects(1, &rect);
    if (!row)
      list->SetGraphicsRoot32BitConstants(0, count, values.data(), 0);
    if (row == 1) {
      for (UINT i = count / 2 - 1; i <= count / 2 + 1; i++)
        values[i] ^= std::numeric_limits<UINT>::max();
      list->SetGraphicsRoot32BitConstants(0, 3, values.data() + count / 2 - 1, count / 2 - 1);
      values.back() = count;
      list->SetGraphicsRoot32BitConstant(0, values.back(), count - 1);
    }
    if (row == 2) {
      bundle->SetGraphicsRootSignature(root.Get());
      bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      values.front() = std::numeric_limits<UINT>::max();
      values.back() = 0;
      bundle->SetGraphicsRoot32BitConstant(0, values.front(), 0);
      bundle->SetGraphicsRoot32BitConstants(0, 1, &values.back(), count - 1);
      bundle->DrawInstanced(3, 1, 0, 0);
      CHECK(bundle->Close());
      list->ExecuteBundle(bundle.Get());
    } else
      list->DrawInstanced(3, 1, 0, 0);
    expected[row] = values;
  }
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION dst{back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = at}};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  UINT8 *got;
  CHECK(back->Map(0, nullptr, (void **)&got));
  for (UINT row = 0; row < rows; row++)
    for (UINT i = 0; i < count; i++) {
      UINT value;
      memcpy(&value, got + at.Offset + row * at.Footprint.RowPitch + i * sizeof(value), sizeof(value));
      expect(value == expected[row][i], "row %u DWORD %u: %#x, want %#x", row, i, value, expected[row][i]);
    }
  back->Unmap(0, nullptr);
  return verdict();
}
