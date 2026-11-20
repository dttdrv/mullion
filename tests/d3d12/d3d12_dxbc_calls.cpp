// contract: "Subroutines can nest 32 deep." Conditional call and return test any set bit (D3D11.3 22.7.10-11,
// 22.7.17; quoted and evaluated on the CPU in dxbc_calls.hpp). the SM5.1 instructions run through direct draws,
// bundles and ExecuteIndirect. DXIL has no corresponding conditional-call opcode.
#include "d3d12_test.hpp"
#include "../dxbc_calls.hpp"

int
main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "dxbc")) {
    printf("skipped: conditional-call opcodes require DXBC\n");
    return 77;
  }
  Compiler compiler;
  auto vs = compiler.compile(dxbc_calls::hlsl, "vs", "vs"), seed = compiler.compile(dxbc_calls::hlsl, "ps", "ps");
  if (!expect(!vs.empty() && !seed.empty(), "call witness HLSL compiled"))
    return verdict();
  auto cases = dxbc_calls::cases(seed);
  if (!expect(!cases.empty(), "SM5.1 conditional-call programs generated"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_CBV};
  auto root = root_signature(device.Get(), {1, &param});
  if (!expect(!!root, "call root signature"))
    return verdict();
  const UINT alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  auto constants =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, cases.size() * alignment, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!constants, "call constants"))
    return verdict();
  char *mapped;
  CHECK(constants->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
  memset(mapped, 0, cases.size() * alignment);
  for (size_t i = 0; i < cases.size(); i++)
    memcpy(mapped + i * alignment, cases[i].input.data(), sizeof(dxbc_calls::Value));
  constants->Unmap(0, nullptr);
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                           0,
                           1,
                           1,
                           1,
                           1,
                           DXGI_FORMAT_R32G32B32A32_UINT,
                           {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&target)));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto arguments =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(readback && arguments, "call readback and draw arguments"))
    return verdict();
  CHECK(arguments->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
  const D3D12_DRAW_ARGUMENTS draw{3, 1, 0, 0};
  memcpy(mapped, &draw, sizeof(draw));
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(draw), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list, bundle;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&bundle)));
  CHECK(bundle->Close());
  std::string previous;
  ComPtr<ID3D12PipelineState> pso;
  for (size_t i = 0; i < cases.size(); i++) {
    auto &c = cases[i];
    step("create SM5.1 %s", c.name.c_str());
    if (c.code != previous) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{root.Get(), bytecode(vs), bytecode(c.code)};
      pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      pipeline.SampleMask = UINT32_MAX;
      pipeline.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pipeline.NumRenderTargets = 1;
      pipeline.RTVFormats[0] = desc.Format;
      pipeline.SampleDesc = {1, 0};
      pso.Reset();
      CHECK(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso)));
      previous = c.code;
    }
    const auto address = constants->GetGPUVirtualAddress() + i * alignment;
    const char *paths[] = {"direct", "bundle", "ExecuteIndirect"};
    for (unsigned path = 0; path < std::size(paths); path++) {
      step("SM5.1 %s condition %#x, %s", c.name.c_str(), c.input[0], paths[path]);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      const float clear[4] = {};
      const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
      const D3D12_RECT scissor{0, 0, 1, 1};
      list->ClearRenderTargetView(rtv, clear, 0, nullptr);
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      list->SetGraphicsRootSignature(root.Get());
      list->SetGraphicsRootConstantBufferView(0, address);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      if (path == 1) {
        CHECK(bundle_allocator->Reset());
        CHECK(bundle->Reset(bundle_allocator.Get(), pso.Get()));
        bundle->SetGraphicsRootSignature(root.Get());
        bundle->SetGraphicsRootConstantBufferView(0, address);
        bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bundle->DrawInstanced(draw.VertexCountPerInstance, draw.InstanceCount, draw.StartVertexLocation,
                              draw.StartInstanceLocation);
        CHECK(bundle->Close());
        list->ExecuteBundle(bundle.Get());
      } else if (path == 2) {
        list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
      } else {
        list->DrawInstanced(draw.VertexCountPerInstance, draw.InstanceCount, draw.StartVertexLocation,
                            draw.StartInstanceLocation);
      }
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
      dxbc_calls::check(reinterpret_cast<const uint32_t *>(mapped), c);
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
