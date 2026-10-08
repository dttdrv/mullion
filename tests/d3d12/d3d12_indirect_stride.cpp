// contract: ExecuteIndirect reads each command at the argument pointer advanced by ByteStride, including at and
// past 4 GB: "pCommandSignature->Interpret(Arguments);" then "Arguments += pCommandSignature->GetByteStride();"
// (Microsoft Learn, ID3D12GraphicsCommandList::ExecuteIndirect, Remarks, both count-buffer forms:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect).
// command i colors pixel i with i + 1, or dispatches i + 1 groups that increment word i. wrapped addresses contain
// earlier commands or distinct commands for other pixels and words, so a wrong read produces a different result.
// "For Draw() and DrawInstanced(), VertexID starts at 0, and it increments for every vertex." (D3D11.3 8.16), so
// the root constant selects the pixel. command bytes are read back before execution to check the sparse upload.
#include "d3d12_test.hpp"
#include <algorithm>
#include <limits>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint command; };
struct V { float4 p : SV_Position; nointerpolation uint value : VALUE; };
V vs() {
  V v;
  v.p = float4((command + 0.5) * 2 / SLOTS - 1, 0, 0, 1);
  v.value = command + 1;
  return v;
}
uint ps(V v) : SV_Target { return v.value; }
RWStructuredBuffer<uint> dst : register(u0);
[numthreads(1, 1, 1)] void cs() { InterlockedAdd(dst[command], 1); }
)hlsl";

int
main(int argc, char **argv) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) {
    printf("skipped: tiled resources are not supported\n");
    return 77;
  }
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT64 wrap = UINT64(std::numeric_limits<UINT>::max()) + 1;
  const UINT power = wrap / 4, commands = wrap / power + 3, slots = 2 * commands;
  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, output_bytes = slots * sizeof(UINT);
  std::vector<std::string> defines = {"SLOTS=" + std::to_string(slots)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines);
  if (vs.empty() || ps.empty() || cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants = {0, 0, 1};
  auto rs = root_signature(device.Get(), {2, params});
  if (!expect(!!rs, "root signature"))
    return verdict();
  ComPtr<ID3D12PipelineState> draw_pso, dispatch_pso;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC draw_desc{rs.Get(), bytecode(vs), bytecode(ps)};
  draw_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  draw_desc.SampleMask = ~0u;
  draw_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  draw_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  draw_desc.NumRenderTargets = 1;
  draw_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  draw_desc.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&draw_desc, IID_PPV_ARGS(&draw_pso)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC dispatch_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&dispatch_desc, IID_PPV_ARGS(&dispatch_pso)));

  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  slots,
                                  1,
                                  1,
                                  1,
                                  DXGI_FORMAT_R32_UINT,
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 readback_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_bytes);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!expect(!!output, "dispatch output buffer"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (UINT stride : {power, power + tile})
    for (UINT64 offset : {UINT64(0), UINT64(sizeof(UINT))})
      for (bool dispatch : {false, true}) {
        step("reserve arguments: stride %u, offset %llu, %s", stride, offset, dispatch ? "dispatch" : "draw");
        D3D12_RESOURCE_DESC arg_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                     0,
                                     offset + UINT64(commands) * stride,
                                     1,
                                     1,
                                     1,
                                     DXGI_FORMAT_UNKNOWN,
                                     {1, 0},
                                     D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
        ComPtr<ID3D12Resource> args;
        HRESULT hr =
            device->CreateReservedResource(&arg_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&args));
        if (FAILED(hr) || !args || !args->GetGPUVirtualAddress()) {
          printf("skipped: cannot reserve %llu argument bytes: HRESULT %08lx, GPU address %llu\n", arg_desc.Width, hr,
                 args ? args->GetGPUVirtualAddress() : 0);
          return trace::wrong ? verdict() : 77;
        }
        struct Record {
          UINT64 at;
          UINT id;
        };
        std::vector<Record> records;
        for (UINT i = 0; i < commands; i++)
          records.push_back({offset + UINT64(i) * stride, i});
        for (UINT i = 0; i < commands; i++) {
          UINT64 at = offset + (UINT64(i) * stride) % wrap;
          if (std::none_of(records.begin(), records.end(), [&](auto &r) { return r.at == at; }))
            records.push_back({at, commands + i});
        }
        const UINT record_bytes =
            sizeof(UINT) + (dispatch ? sizeof(D3D12_DISPATCH_ARGUMENTS) : sizeof(D3D12_DRAW_ARGUMENTS));
        auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK,
                               std::max<UINT64>(readback_bytes, records.size() * record_bytes),
                               D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(!!readback, "readback buffer"))
          return verdict();
        ComPtr<ID3D12Heap> heap;
        D3D12_HEAP_DESC heap_desc{records.size() * tile, {D3D12_HEAP_TYPE_DEFAULT}};
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
        const UINT64 zeros_at = heap_desc.SizeInBytes, count_at = zeros_at + output_bytes + sizeof(UINT);
        auto upload =
            buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, count_at + sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!expect(!!upload, "tile upload and count buffer"))
          return verdict();
        char *bytes;
        CHECK(upload->Map(0, nullptr, (void **)&bytes));
        memset(bytes, 0, count_at + sizeof(UINT));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        for (UINT r = 0; r < records.size(); r++) {
          auto [at, id] = records[r];
          D3D12_TILED_RESOURCE_COORDINATE coordinate{UINT(at / tile)};
          D3D12_TILE_REGION_SIZE one{1};
          queue->UpdateTileMappings(args.Get(), 1, &coordinate, &one, heap.Get(), 1, nullptr, &r, nullptr,
                                    D3D12_TILE_MAPPING_FLAG_NONE);
          auto data = bytes + UINT64(r) * tile + at % tile;
          memcpy(data, &id, sizeof(id));
          if (dispatch) {
            D3D12_DISPATCH_ARGUMENTS value{id + 1, 1, 1};
            memcpy(data + sizeof(id), &value, sizeof(value));
          } else {
            D3D12_DRAW_ARGUMENTS value{1, 1, id, 0};
            memcpy(data + sizeof(id), &value, sizeof(value));
          }
          list->CopyBufferRegion(args.Get(), at / tile * tile, upload.Get(), UINT64(r) * tile, tile);
        }
        transition(list.Get(), args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        for (UINT r = 0; r < records.size(); r++)
          list->CopyBufferRegion(readback.Get(), UINT64(r) * record_bytes, args.Get(), records[r].at, record_bytes);
        transition(list.Get(), args.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        char *copied;
        CHECK(readback->Map(0, nullptr, (void **)&copied));
        for (UINT r = 0; r < records.size(); r++)
          expect(!memcmp(copied + UINT64(r) * record_bytes, bytes + UINT64(r) * tile + records[r].at % tile,
                         record_bytes), "argument bytes at %llu", records[r].at);
        readback->Unmap(0, nullptr);

        D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {
            {D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT},
            {dispatch ? D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW}};
        arguments[0].Constant = {0, 0, 1};
        D3D12_COMMAND_SIGNATURE_DESC signature_desc{stride, 2, arguments};
        ComPtr<ID3D12CommandSignature> signature;
        CHECK(device->CreateCommandSignature(&signature_desc, rs.Get(), IID_PPV_ARGS(&signature)));
        for (UINT requested : {commands, 0u, commands - 1, commands + 1})
          for (bool indirect : {false, true}) {
            const UINT count = std::min(requested, commands);
            const bool counted = requested != commands;
            step("stride %u, offset %llu, %s, %s, %s count %u", stride, offset, dispatch ? "dispatch" : "draw",
                 indirect ? "indirect" : "direct", counted ? "buffer" : "CPU", requested);
            memcpy(bytes + count_at, &requested, sizeof(requested));
            CHECK(forget(readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), dispatch ? dispatch_pso.Get() : draw_pso.Get()));
            if (dispatch) {
              list->CopyBufferRegion(output.Get(), 0, upload.Get(), zeros_at, output_bytes);
              transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
              list->SetComputeRootSignature(rs.Get());
              list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
            } else {
              const float clear[4] = {};
              list->ClearRenderTargetView(rtv, clear, 0, nullptr);
              list->SetGraphicsRootSignature(rs.Get());
              list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
              D3D12_VIEWPORT viewport{0, 0, float(slots), 1, 0, 1};
              D3D12_RECT scissor{0, 0, LONG(slots), 1};
              list->RSSetViewports(1, &viewport);
              list->RSSetScissorRects(1, &scissor);
              list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            }
            if (indirect)
              list->ExecuteIndirect(signature.Get(), commands, args.Get(), offset, counted ? upload.Get() : nullptr,
                                    count_at);
            else
              for (UINT i = 0; i < count; i++) {
                if (dispatch) {
                  list->SetComputeRoot32BitConstant(0, i, 0);
                  list->Dispatch(i + 1, 1, 1);
                } else {
                  list->SetGraphicsRoot32BitConstant(0, i, 0);
                  list->DrawInstanced(1, 1, i, 0);
                }
              }
            if (dispatch) {
              transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
              list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, output_bytes);
              transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            } else {
              transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_COPY_SOURCE);
              D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
                  dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
              dst.PlacedFootprint = footprint;
              list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
              transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            UINT *got;
            CHECK(readback->Map(0, nullptr, (void **)&got));
            for (UINT i = 0; i < slots; i++) {
              const UINT want = i < count ? i + 1 : 0;
              expect(got[i] == want, "%s %u: %u, want %u", dispatch ? "word" : "pixel", i, got[i], want);
            }
            readback->Unmap(0, nullptr);
          }
        upload->Unmap(0, nullptr);
      }
  return verdict();
}
