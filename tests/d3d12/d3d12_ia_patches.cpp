// contract: D3D11.3 11.8.1.2: "Sending un-tessellated patches to NULL GS + Stream Output is valid."
// "As with other primitive types, only complete patches get written out; if there is not enough room to store a
// complete patch, it is discarded." 8.16/8.18 make vertex/instance IDs independent of the starting locations.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// the PSO uses PATCH topology without HS/DS; the D3D12 PSO contract requires PATCH when HS/DS are present, not the converse.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc
// compare every output/counter/guard word through direct, indirect, bundle and bundled-indirect draws.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>

static const char hlsl[] = R"hlsl(
uint4 vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) : VALUE {
  return uint4(vid, iid, vid * 7 + 1, iid * 11 + vid);
}
)hlsl";

int main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile the vertex shader before creating a device");
  auto code = compiler.compile(hlsl, "vs", "vs");
  if (!expect(!code.empty(), "HLSL did not compile")) return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  auto root = root_signature(device.Get(), {0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT});
  if (!expect(bool(root), "no root signature")) return verdict();
  using Vertex = std::array<UINT, 4>;
  const UINT word = sizeof(UINT), vertex_bytes = sizeof(Vertex), sentinel = 0xa5a5a5a5;
  const UINT maximum = D3D12_IA_PATCH_MAX_CONTROL_POINT_COUNT, first = 2, index_offset = 1, instances = 2;
  const D3D12_SO_DECLARATION_ENTRY declaration{0, "VALUE", 0, 0, 4, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{root.Get(), bytecode(code)};
  desc.StreamOutput = {&declaration, 1, &vertex_bytes, 1, D3D12_SO_NO_RASTERIZED_STREAM};
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
  std::vector<UINT> indices(index_offset + first + 2 * maximum + 1);
  for (UINT i = 0; i < indices.size(); i++) indices[i] = (i * 13 + 5) % (2 * maximum + 1);
  ComPtr<ID3D12Resource> index_buffers[2];
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT};
  const UINT index_sizes[] = {sizeof(UINT16), sizeof(UINT)};
  for (UINT f = 0; f < std::size(formats); f++) {
    index_buffers[f] = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, indices.size() * index_sizes[f], D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(bool(index_buffers[f]), "no index buffer")) return verdict();
    void *mapped;
    CHECK(index_buffers[f]->Map(0, nullptr, &mapped));
    for (UINT i = 0; i < indices.size(); i++)
      if (f) static_cast<UINT *>(mapped)[i] = indices[i];
      else static_cast<UINT16 *>(mapped)[i] = indices[i];
    index_buffers[f]->Unmap(0, nullptr);
  }
  const UINT argument_stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  ComPtr<ID3D12CommandSignature> signatures[2];
  for (UINT indexed = 0; indexed < 2; indexed++) {
    const D3D12_INDIRECT_ARGUMENT_DESC action{indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
    const D3D12_COMMAND_SIGNATURE_DESC signature{argument_stride, 1, &action};
    CHECK(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&signatures[indexed])));
  }
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  std::vector<UINT> patch_sizes;
  for (UINT points = maximum; points; points--)
    patch_sizes.push_back(points);
  patch_sizes.push_back(patch_sizes.front());
  for (UINT points : patch_sizes) {
    std::vector<UINT> counts{0, points - 1, points, points + 1, 2 * points - 1, 2 * points, 2 * points + 1};
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    for (UINT capacity : {0u, points - 1, points, 2 * points - 1, 2 * points + 1}) {
      struct Case { UINT path, indexed, format, count; };
      std::vector<Case> cases;
      for (UINT path = 0; path < 4; path++)
        for (UINT indexed = 0; indexed < 2; indexed++)
          for (UINT f = 0; f < (indexed ? std::size(formats) : 1); f++)
            for (UINT count : counts) cases.push_back({path, indexed, f, count});
      // each segment has a prefix, room for whole vertices plus one word, then an eight-byte counter allocation.
      const UINT counter_at = word + capacity * vertex_bytes + word;
      const UINT segment_bytes = counter_at + sizeof(UINT64), total_bytes = cases.size() * segment_bytes;
      std::vector<UINT> initial(total_bytes / word, sentinel), wanted(initial);
      std::vector<unsigned char> commands(cases.size() * argument_stride);
      for (UINT c = 0; c < cases.size(); c++) {
        const auto test = cases[c];
        const UINT base = c * segment_bytes / word, counter = base + counter_at / word;
        initial[counter] = initial[counter + 1] = wanted[counter] = wanted[counter + 1] = 0;
        UINT at = base + 1, remaining = capacity, written = 0;
        for (UINT instance = 0; instance < instances; instance++)
          for (UINT patch = 0; patch < test.count / points && remaining >= points; patch++) {
            for (UINT cp = 0; cp < points; cp++) {
              const UINT vertex = patch * points + cp;
              const UINT id = test.indexed ? indices[index_offset + first + vertex] : vertex;
              const Vertex value{id, instance, id * 7 + 1, instance * 11 + id};
              std::copy(value.begin(), value.end(), wanted.begin() + at);
              at += value.size();
            }
            remaining -= points;
            written += points;
          }
        wanted[counter] = written * vertex_bytes;
        const D3D12_DRAW_ARGUMENTS plain{test.count, instances, first, 5};
        const D3D12_DRAW_INDEXED_ARGUMENTS indexed{test.count, instances, first, 3, 5};
        memcpy(commands.data() + c * argument_stride, test.indexed ? (const void *)&indexed : &plain,
               test.indexed ? sizeof(indexed) : sizeof(plain));
      }
      auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, commands.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
      auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, total_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(upload && arguments && output && readback, "buffer creation failed")) return verdict();
      void *mapped;
      CHECK(upload->Map(0, nullptr, &mapped));
      memcpy(mapped, initial.data(), total_bytes);
      upload->Unmap(0, nullptr);
      CHECK(arguments->Map(0, nullptr, &mapped));
      memcpy(mapped, commands.data(), commands.size());
      arguments->Unmap(0, nullptr);
      CHECK(allocator->Reset());
      CHECK(bundle_allocator->Reset());
      CHECK(list->Reset(allocator.Get(), pso.Get()));
      list->CopyBufferRegion(output.Get(), 0, upload.Get(), 0, total_bytes);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
      list->SetGraphicsRootSignature(root.Get());
      std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
      for (UINT c = 0; c < cases.size(); c++) {
        const auto test = cases[c];
        const bool bundled = test.path >= 2, indirect = test.path & 1;
        const auto topology = D3D_PRIMITIVE_TOPOLOGY(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + points - 1);
        list->IASetPrimitiveTopology(topology);
        const D3D12_INDEX_BUFFER_VIEW ib{index_buffers[test.format]->GetGPUVirtualAddress() + index_offset * index_sizes[test.format],
            UINT(indices.size() - index_offset) * index_sizes[test.format], formats[test.format]};
        list->IASetIndexBuffer(&ib);
        const auto address = output->GetGPUVirtualAddress() + c * segment_bytes;
        const D3D12_STREAM_OUTPUT_BUFFER_VIEW so{address + word, capacity * vertex_bytes + word, address + counter_at};
        list->SOSetTargets(0, 1, &so);
        auto draw = list.Get();
        if (bundled) {
          ComPtr<ID3D12GraphicsCommandList> bundle;
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(), IID_PPV_ARGS(&bundle)));
          draw = bundle.Get();
          draw->IASetPrimitiveTopology(topology);
          bundles.push_back(bundle);
        }
        if (indirect) draw->ExecuteIndirect(signatures[test.indexed].Get(), 1, arguments.Get(), c * argument_stride, nullptr, 0);
        else if (test.indexed) draw->DrawIndexedInstanced(test.count, instances, first, 3, 5);
        else draw->DrawInstanced(test.count, instances, first, 5);
        if (bundled) { CHECK(draw->Close()); list->ExecuteBundle(draw); }
      }
      list->SOSetTargets(0, 0, nullptr);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, total_bytes);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      step("patch %u capacity %u: %zu collected direct/indirect/bundle cases", points, capacity, cases.size());
      const UINT *got;
      CHECK(readback->Map(0, nullptr, (void **)&got));
      for (UINT i = 0; i < wanted.size(); i++)
        if (got[i] != wanted[i]) {
          const auto test = cases[i * word / segment_bytes];
          expect(false, "path %u indexed %u format %u count %u word %u is %#x, want %#x", test.path, test.indexed,
                 formats[test.format], test.count, (i * word % segment_bytes) / word, got[i], wanted[i]);
          break;
        }
      readback->Unmap(0, nullptr);
    }
  }
  return verdict();
}
