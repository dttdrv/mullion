// contract: "Divide by zero returns 0xffffffff for both quotient and remainder." (D3D11.3 22.12.9).
// dxbc_arithmetic.hpp quotes and evaluates bitfields, products, carry/borrow, floating specials and conversions.
// the SM5.1 programs run through direct draws, bundles and ExecuteIndirect; DXIL cannot stand in for DXBC here.
#include "d3d12_test.hpp"
#include "../dxbc_arithmetic.hpp"

int
main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "dxbc")) {
    printf("skipped: these instruction encodings require DXBC\n");
    return 77;
  }
  using namespace dxbc_arithmetic;
  constexpr UINT rows = D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
  const UINT side = UINT(std::sqrt(rows));
  if (!expect(side * side == rows, "constant-buffer rows form the test's square"))
    return verdict();
  Compiler compiler;
  const std::vector<std::string> defines{"ROWS=" + std::to_string(rows), "SIDE=" + std::to_string(side)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), seed = compiler.compile(hlsl, "ps", "ps", defines);
  if (!expect(!vs.empty() && !seed.empty(), "arithmetic HLSL compiled"))
    return verdict();
  std::vector<std::string> shaders;
  for (unsigned subject = 0; subject < std::size(names); subject++) {
    shaders.push_back(program(seed, Subject(subject), side));
    if (!expect(!shaders.back().empty(), "%s instructions generated", names[subject]))
      return verdict();
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_CBV};
  auto root = root_signature(device.Get(), {1, &param});
  if (!expect(!!root, "arithmetic root signature"))
    return verdict();
  auto constants =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, rows * sizeof(Value), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto arguments =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(constants && arguments, "arithmetic constants and draw arguments"))
    return verdict();
  void *mapped;
  CHECK(arguments->Map(0, nullptr, &mapped));
  const D3D12_DRAW_ARGUMENTS draw{3, 1, 0, 0};
  memcpy(mapped, &draw, sizeof(draw));
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(draw), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                           0,
                           side,
                           side,
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
  if (!expect(!!readback, "arithmetic readback"))
    return verdict();
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
  const D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
  const D3D12_RECT scissor{0, 0, LONG(side), LONG(side)};
  const Value padding{0, 0, 0, UINT32_MAX};
  uint64_t checked = 0;
  for (unsigned subject = 0; subject < shaders.size(); subject++) {
    step("create SM5.1 %s", names[subject]);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{root.Get(), bytecode(vs), bytecode(shaders[subject])};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT32_MAX;
    pipeline.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = desc.Format;
    pipeline.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pso)));
    auto all = inputs(Subject(subject));
    unsigned wrong = 0;
    for (size_t start = 0; start < all.size(); start += rows) {
      std::vector<Value> batch(rows, padding);
      std::copy_n(all.begin() + start, std::min<size_t>(rows, all.size() - start), batch.begin());
      CHECK(constants->Map(0, nullptr, &mapped));
      memcpy(mapped, batch.data(), rows * sizeof(Value));
      constants->Unmap(0, nullptr);
      const char *paths[] = {"direct", "bundle", "ExecuteIndirect"};
      for (unsigned path = 0; path < std::size(paths); path++) {
        step("SM5.1 %s inputs %zu..%zu, %s", names[subject], start, std::min(start + rows, all.size()), paths[path]);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pso.Get()));
        const float clear[4] = {};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (path == 1) {
          CHECK(bundle_allocator->Reset());
          CHECK(bundle->Reset(bundle_allocator.Get(), pso.Get()));
          bundle->SetGraphicsRootSignature(root.Get());
          bundle->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
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
        CHECK(readback->Map(0, nullptr, &mapped));
        for (UINT i = 0; i < rows; i++) {
          auto got = reinterpret_cast<const uint32_t *>(static_cast<const char *>(mapped) +
                                                        (i / side) * footprint.Footprint.RowPitch) +
                     (i % side) * std::tuple_size_v<Value>;
          auto want = reference(Subject(subject), batch[i]);
          for (UINT c = 0; c < want.size(); c++, checked++)
            if (!allowed(Subject(subject), c, got[c], want[c], batch[i]) && wrong++ < want.size())
              expect(false, "input %zu (%#x,%#x,%#x,%#x), component %u: %#x, want %#x", start + i, batch[i][0],
                     batch[i][1], batch[i][2], batch[i][3], c, got[c], want[c]);
        }
        readback->Unmap(0, nullptr);
      }
    }
    expect(wrong == 0, "%u incorrect %s results", wrong, names[subject]);
  }
  printf("%llu arithmetic components checked\n", static_cast<unsigned long long>(checked));
  return verdict();
}
