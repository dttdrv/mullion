// contract: query results in custom heaps must be ready for later GPU copies, including cached CPU-visible heaps.
// READBACK resources "cannot be changed away from" COPY_DEST (D3D12_HEAP_TYPE, Microsoft Learn); custom heaps do not
// have that restriction. timestamps sample "at the instant that the GPU is finished with all the preceding workload"
// (D3D11.3 20.4.3), and CSInvocations is the "Number of individual Compute Shader threads invoked" (20.4.7).
// timestamp values must lie between the queue's clock calibrations and equal a separate resolve of the same queries.
// a triangle list's submitted vertices and primitives, and a dispatch's group and thread counts, give the statistics.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d12_test.hpp"
#include <algorithm>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  const float2 corners[] = {
    float2(-1, 1), float2(1, 1), float2(-1, -1),
    float2(-1, -1), float2(1, 1), float2(1, -1)
  };
  return float4(corners[id], 0.5, 1);
}
uint ps() : SV_Target { return 1; }
RWStructuredBuffer<uint> output : register(u0);
[numthreads(THREADS, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) { output[id.x] = id.x + 1; }
)hlsl";

int
main(int argc, char **argv) {
  const UINT threads = 3, statistics = 4, vertices_per_triangle = 3, triangles_per_quad = 2,
             vertices_per_quad = vertices_per_triangle * triangles_per_quad;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  step("compile VS, PS and CS");
  const std::vector<std::string> defines = {"THREADS=" + std::to_string(threads)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines);
  if (vs.empty() || ps.empty() || cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  // d3d12_queries exercises Metal's probed 4096-sample buffers; this range crosses two of them
  const UINT stamps = 8193;
  const UINT64 gap = sizeof(UINT64), bytes = gap + stamps * sizeof(UINT64) + sizeof(UINT64);
  D3D12_HEAP_PROPERTIES custom, combined, private_heap;
  device->GetCustomHeapProperties(&custom, 0, D3D12_HEAP_TYPE_READBACK);
  device->GetCustomHeapProperties(&combined, 0, D3D12_HEAP_TYPE_UPLOAD);
  device->GetCustomHeapProperties(&private_heap, 0, D3D12_HEAP_TYPE_DEFAULT);
  if (!expect(
          custom.Type == D3D12_HEAP_TYPE_CUSTOM && custom.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
          "GetCustomHeapProperties(READBACK): type %d, CPU pages %d", custom.Type, custom.CPUPageProperty
      ))
    return verdict();
  D3D12_RESOURCE_DESC desc{
      D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
      D3D12_RESOURCE_FLAG_NONE
  };
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST),
       copied = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST),
       poison = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(readback && copied && poison, "timestamp buffers could not be made"))
    return verdict();
  CHECK(forget(poison.Get()));

  ComPtr<ID3D12QueryHeap> timestamps, pipeline;
  D3D12_QUERY_HEAP_DESC timestamp_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, stamps + 1},
      pipeline_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, statistics + 1};
  CHECK(device->CreateQueryHeap(&timestamp_desc, IID_PPV_ARGS(&timestamps)));
  CHECK(device->CreateQueryHeap(&pipeline_desc, IID_PPV_ARGS(&pipeline)));

  auto draw_rs = root_signature(device.Get(), {});
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter};
  auto dispatch_rs = root_signature(device.Get(), root_desc);
  if (!expect(draw_rs && dispatch_rs, "root signatures could not be made"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{draw_rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.RasterizerState.DepthClipEnable = TRUE;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  graphics.SampleDesc = {1, 0};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{dispatch_rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> draw_pso, dispatch_pso;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&draw_pso)));
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&dispatch_pso)));
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, threads * statistics * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto words = buffer(
      device.Get(), D3D12_HEAP_TYPE_READBACK, threads * statistics * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST
  );
  if (!expect(output && words, "dispatch buffers could not be made"))
    return verdict();
  const UINT size = 4;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                              0,
                              size,
                              size,
                              1,
                              1,
                              DXGI_FORMAT_R32_UINT,
                              {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(size), LONG(size)};

  for (auto properties :
       {D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_READBACK}, custom, combined, private_heap,
        D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT}}) {
    ComPtr<ID3D12Resource> dst;
    CHECK(device->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dst)
    ));
    const bool gpu_readable = properties.Type != D3D12_HEAP_TYPE_READBACK;
    auto observed = gpu_readable ? copied.Get() : dst.Get();
    for (UINT run = 0; run < 2; run++) {
      step(
          "heap %u, CPU pages %u, run %u: resolve timestamps, then copy on the GPU", properties.Type,
          properties.CPUPageProperty, run
      );
      CHECK(forget(readback.Get()));
      CHECK(forget(copied.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->CopyBufferRegion(dst.Get(), 0, poison.Get(), 0, bytes);
      for (UINT i = 1; i <= stamps; i++)
        list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
      for (auto destination : {dst.Get(), readback.Get()}) {
        list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1, 1, destination, gap);
        list->ResolveQueryData(
            timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2, stamps - 1, destination, gap + sizeof(UINT64)
        );
      }
      if (gpu_readable) {
        transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(copied.Get(), 0, dst.Get(), 0, bytes);
        transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      }
      UINT64 before, after, cpu;
      CHECK(queue->GetClockCalibration(&before, &cpu));
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(queue->GetClockCalibration(&after, &cpu));
      const UINT64 *values, *reference;
      CHECK(observed->Map(0, nullptr, (void **)&values));
      CHECK(readback->Map(0, nullptr, (void **)&reference));
      for (UINT i = 0; i < stamps; i++) {
        const UINT64 value = values[1 + i];
        if (!expect(
                value >= before && value <= after && value != 0 && (!i || value >= values[i]),
                "timestamp %u is %llu, clock interval [%llu, %llu]", i, value, before, after
            ))
          break;
        if (!expect(
                value == reference[1 + i], "GPU copy of timestamp %u is %llu, separate resolve is %llu", i, value,
                reference[1 + i]
            ))
          break;
      }
      expect(values[0] == ~UINT64(0) && values[stamps + 1] == ~UINT64(0), "timestamp resolve overwrote a guard");
      readback->Unmap(0, nullptr);
      observed->Unmap(0, nullptr);

      step(
          "heap %u, CPU pages %u, run %u: statistics over draws and dispatches", properties.Type,
          properties.CPUPageProperty, run
      );
      CHECK(forget(copied.Get()));
      CHECK(forget(words.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      list->CopyBufferRegion(dst.Get(), 0, poison.Get(), 0, bytes);
      list->CopyBufferRegion(output.Get(), 0, poison.Get(), 0, output->GetDesc().Width);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      for (UINT i = 0; i < statistics; i++) {
        list->BeginQuery(pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i + 1);
        list->SetPipelineState(draw_pso.Get());
        list->SetGraphicsRootSignature(draw_rs.Get());
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(vertices_per_quad, i + 1, 0, 0);
        list->SetPipelineState(dispatch_pso.Get());
        list->SetComputeRootSignature(dispatch_rs.Get());
        list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
        list->Dispatch(i + 1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_UAV};
        barrier.UAV.pResource = output.Get();
        list->ResourceBarrier(1, &barrier);
        list->EndQuery(pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i + 1);
      }
      list->ResolveQueryData(pipeline.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1, statistics, dst.Get(), gap);
      if (gpu_readable) {
        transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(copied.Get(), 0, dst.Get(), 0, bytes);
        transition(list.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      }
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(words.Get(), 0, output.Get(), 0, output->GetDesc().Width);
      transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const BYTE *data;
      CHECK(observed->Map(0, nullptr, (void **)&data));
      auto counts = reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(data + gap);
      for (UINT i = 0; i < statistics; i++) {
        const UINT64 instances = i + 1;
        const auto &count = counts[i];
        expect(
            count.IAVertices == vertices_per_quad * instances && count.IAPrimitives == triangles_per_quad * instances &&
                count.VSInvocations == vertices_per_quad * instances &&
                count.CInvocations == triangles_per_quad * instances &&
                count.CPrimitives == triangles_per_quad * instances && count.PSInvocations >= size * size * instances &&
                count.CSInvocations == threads * instances,
            "query %u: IA %llu/%llu, VS %llu, clip %llu/%llu, PS %llu, CS %llu", i, count.IAVertices,
            count.IAPrimitives, count.VSInvocations, count.CInvocations, count.CPrimitives, count.PSInvocations,
            count.CSInvocations
        );
        expect(
            !count.GSInvocations && !count.GSPrimitives && !count.HSInvocations && !count.DSInvocations,
            "query %u counted an unused stage", i
        );
      }
      expect(
          std::all_of(data, data + gap, [](BYTE value) { return value == 0xff; }) &&
              std::all_of(
                  data + gap + statistics * sizeof(*counts), data + bytes, [](BYTE value) { return value == 0xff; }
              ),
          "statistics resolve overwrote a guard"
      );
      observed->Unmap(0, nullptr);
      const UINT *written;
      CHECK(words->Map(0, nullptr, (void **)&written));
      for (UINT i = 0; i < threads * statistics; i++)
        expect(written[i] == i + 1, "dispatch word %u is %u, want %u", i, written[i], i + 1);
      words->Unmap(0, nullptr);
    }
  }
  return verdict();
}
