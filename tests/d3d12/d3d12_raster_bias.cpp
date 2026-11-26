// contract: D3D11.3 15.10: "Biasing is constant for a given primitive, with the same value added to the z
// for each vertex before interpolator setup." D3D12_RASTERIZER_DESC links to the same depth-bias formulas.
// ../raster_bias.hpp computes float32 bias, signed clamping and encoded depth for every header depth format.
// depth bytes are compared after direct, bundle and ExecuteIndirect draws, without a pixel shader replacing z.
// stream pipelines omitting rasterizer state must also retain zero bias on flat and sloped primitives.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_rasterizer_desc
#include "d3d12_test.hpp"
#include "../raster_bias.hpp"
#include <cstddef>

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!raster::reference_contract())
    return 1;
  step("compile depth-only vertex shader before device creation");
  auto code = compiler.compile(raster::hlsl, "vs", "vs", {"DISTANCES=0"});
  if (code.empty())
    return 1;
  ComPtr<ID3D12Device2> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto rs = root_signature(device.Get(),
                           {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!expect(!!rs, "root signature was not created"))
    return verdict();
  const auto all = raster_bias::cases();
  const UINT vertex_bytes = sizeof(raster::Vertex) * raster::Triangle;
  auto vb = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertex_bytes * all.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS) * all.size(),
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(vb && args, "uploads were not created"))
    return verdict();
  char *vertices;
  D3D12_DRAW_ARGUMENTS *arguments;
  CHECK(vb->Map(0, nullptr, reinterpret_cast<void **>(&vertices)));
  CHECK(args->Map(0, nullptr, reinterpret_cast<void **>(&arguments)));
  for (size_t i = 0; i < all.size(); i++) {
    auto geometry = raster_bias::geometry(all[i]);
    memset(vertices + i * vertex_bytes, 0, vertex_bytes);
    memcpy(vertices + i * vertex_bytes, geometry.vertices.data(), geometry.vertices.size() * sizeof(raster::Vertex));
    arguments[i] = {UINT(geometry.vertices.size()), 1, 0, 0};
  }
  vb->Unmap(0, nullptr);
  args->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC draw{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC sig{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&signature)));
  D3D12_INPUT_ELEMENT_DESC input{"POSITION",
                                 0,
                                 DXGI_FORMAT_R32G32B32A32_FLOAT,
                                 0,
                                 offsetof(raster::Vertex, position),
                                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                                 0};
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  for (const auto &format : raster_bias::formats()) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format.value};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
      printf("unsupported: %s\n", format.name.c_str());
      continue;
    }
    D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                           0,
                           raster::width,
                           raster::height,
                           1,
                           1,
                           format.value,
                           {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN,
                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> depth;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                          IID_PPV_ARGS(&depth)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    ComPtr<ID3D12DescriptorHeap> descriptors;
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)));
    auto dsv = descriptors->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes, row_bytes;
    device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, nullptr, &row_bytes, &bytes);
    const unsigned pixel_bytes = row_bytes / footprint.Footprint.Width;
    const UINT64 slot = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * all.size(), D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(!!readback, "depth readback was not created"))
      return verdict();
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(all.size());
    for (size_t i = 0; i < all.size(); i++) {
      const auto &c = all[i];
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
      pd.pRootSignature = rs.Get();
      pd.VS = bytecode(code);
      pd.InputLayout = {&input, 1};
      pd.RasterizerState = {c.wire ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
      pd.RasterizerState.DepthClipEnable = c.depth_clip;
      pd.RasterizerState.DepthBias = c.bias;
      pd.RasterizerState.DepthBiasClamp = c.clamp;
      pd.RasterizerState.SlopeScaledDepthBias = c.scale;
      pd.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
      pd.SampleDesc = {1, 0};
      pd.DSVFormat = format.value;
      pd.DepthStencilState.DepthEnable = TRUE;
      pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      pd.PrimitiveTopologyType = c.shape == raster::Point  ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                 : c.shape == raster::Line ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                           : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      if (c.default_state) {
        struct Stream {
          struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            ID3D12RootSignature *value;
          } root;
          struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            D3D12_SHADER_BYTECODE value;
          } vertex;
          struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            D3D12_INPUT_LAYOUT_DESC value;
          } input;
          struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            D3D12_DEPTH_STENCIL_DESC value;
          } depth;
          struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            DXGI_FORMAT value;
          } format;
          struct alignas(void *) {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
            D3D12_PRIMITIVE_TOPOLOGY_TYPE value;
          } topology;
        } stream{{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, pd.pRootSignature},
                 {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, pd.VS},
                 {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, pd.InputLayout},
                 {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, pd.DepthStencilState},
                 {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, pd.DSVFormat},
                 {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, pd.PrimitiveTopologyType}};
        D3D12_PIPELINE_STATE_STREAM_DESC description{sizeof(stream), &stream};
        CHECK(device->CreatePipelineState(&description, IID_PPV_ARGS(&pipelines[i])));
      } else {
        CHECK(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipelines[i])));
      }
    }
    for (unsigned path = 0; path < 3; path++) {
      CHECK(forget(readback.Get()));
      ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
      CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
      CHECK(
          device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
      std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
      for (size_t i = 0; i < all.size(); i++) {
        const auto &c = all[i];
        step("%s path %u bias case %zu", format.name.c_str(), path, i);
        D3D12_VIEWPORT vp{0, 0, raster::width, raster::height, c.low, c.high};
        D3D12_RECT rect{0, 0, raster::width, raster::height};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &rect);
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        ComPtr<ID3D12GraphicsCommandList> bundle;
        if (path == 1)
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pipelines[i].Get(),
                                          IID_PPV_ARGS(&bundle)));
        auto commands = path == 1 ? bundle.Get() : list.Get();
        commands->SetGraphicsRootSignature(rs.Get());
        commands->SetPipelineState(pipelines[i].Get());
        D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress() + i * vertex_bytes, vertex_bytes,
                                      sizeof(raster::Vertex)};
        commands->IASetVertexBuffers(0, 1, &view);
        commands->IASetPrimitiveTopology(c.shape == raster::Point  ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                                         : c.shape == raster::Line ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                                   : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (path == 2)
          commands->ExecuteIndirect(signature.Get(), 1, args.Get(), i * sizeof(D3D12_DRAW_ARGUMENTS), nullptr, 0);
        else
          commands->DrawInstanced(c.shape, 1, 0, 0);
        if (bundle) {
          CHECK(bundle->Close());
          list->ExecuteBundle(bundle.Get());
          bundles.push_back(bundle);
        }
        transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        into.PlacedFootprint.Offset = i * slot;
        list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
        transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      char *mapped;
      CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)));
      for (size_t i = 0; i < all.size(); i++) {
        step("read %s path %u bias case %zu", format.name.c_str(), path, i);
        raster_bias::compare(format, all[i], mapped + i * slot, footprint.Footprint.RowPitch, pixel_bytes);
      }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
