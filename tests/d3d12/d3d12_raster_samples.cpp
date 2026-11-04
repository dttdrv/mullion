// contract: D3D11.3 15.4.1: "When multisampling, implementations MUST clip against clip distances at subsample
// resolution." 22.4.22: "If sampleIndex is out of bounds a zero vector is returned." ../raster_samples.hpp
// provides standard sample positions and independent per-sample edge coverage; no resolve can hide sample swaps.
// DXIL.rst operations 75/76/77/90/91 and D3D12_RASTERIZER_DESC use these same sample and coverage contracts.
#include "d3d12_test.hpp"
#include "../raster_samples.hpp"
#include <cstddef>
#include <map>

int main(int argc, char **argv) {
  using namespace raster_samples;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  struct Shaders {
    std::string vs, clip, sample, index, modifier, varying, centroid, perspective, perspective_centroid, depth, query,
        output, forced, uav, cs, single_cs;
  };
  std::map<unsigned, Shaders> shaders;
  if (!raster::reference_contract())
    return 1;
  step("compile all sample counts before device creation");
  for (unsigned count : {1u, 2u, 4u, 8u, 16u}) {
    auto &s = shaders[count];
    std::vector<std::string> defs = {"WIDTH=" + std::to_string(width), "COUNT=" + std::to_string(count),
                                     "STORAGE_COUNT=" + std::to_string(count)};
    s.vs = compiler.compile(raster::hlsl, "vs", "vs", {"DISTANCES=0"});
    s.clip = compiler.compile(raster::hlsl, "vs", "vs", {"DISTANCES=1"});
    s.sample = compiler.compile(raster_samples::hlsl, "ps_sample", "ps", defs);
    s.index = compiler.compile(raster_samples::hlsl, "ps_index", "ps", defs);
    s.modifier = compiler.compile(raster_samples::hlsl, "ps_modifier", "ps", defs);
    s.varying = compiler.compile(raster_samples::hlsl, "ps_varying", "ps", defs);
    s.centroid = compiler.compile(raster_samples::hlsl, "ps_centroid", "ps", defs);
    s.depth = compiler.compile(raster_samples::hlsl, "ps_depth", "ps", defs);
    defs.push_back("PERSPECTIVE=1");
    s.perspective = compiler.compile(raster_samples::hlsl, "ps_varying", "ps", defs);
    s.perspective_centroid = compiler.compile(raster_samples::hlsl, "ps_centroid", "ps", defs);
    defs.pop_back();
    s.query = compiler.compile(raster_samples::hlsl, "ps_query", "ps", defs);
    s.output = compiler.compile(raster_samples::hlsl, "ps_export", "ps", defs);
    s.cs = compiler.compile(raster_samples::hlsl, "cs", "cs", defs);
    s.uav = compiler.compile(raster_samples::hlsl, "ps_uav", "ps", defs);
    s.forced = compiler.compile(raster_samples::hlsl, "ps_forced", "ps", defs);
    defs.back() = "STORAGE_COUNT=1";
    s.single_cs = compiler.compile(raster_samples::hlsl, "cs", "cs", defs);
    for (auto code : {&s.vs, &s.clip, &s.sample, &s.index, &s.modifier, &s.varying, &s.centroid, &s.perspective,
                      &s.perspective_centroid, &s.depth, &s.query, &s.output, &s.forced, &s.uav, &s.cs, &s.single_cs})
      if (code->empty())
        return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  // DXGI_SAMPLE_DESC: "The valid range is between zero and one less than the level returned" by the query.
  // https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ns-dxgicommon-dxgi_sample_desc
  step("sample-count boundaries and quality-level resource validation");
  for (UINT count : {0u, 1u, 2u, 3u, 4u, 8u, 16u, UINT(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT),
                     UINT(D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT + 1), UINT_MAX}) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{DXGI_FORMAT_R32G32B32A32_FLOAT, count,
                                                          D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE};
    HRESULT query = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support));
    bool supported = SUCCEEDED(query) && support.NumQualityLevels != 0;
    if (!count || count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT)
      expect(!supported, "invalid sample count%u advertised %u qualities", count, support.NumQualityLevels);
    std::vector<UINT> qualities = {0, support.NumQualityLevels};
    if (support.NumQualityLevels > 1)
      qualities.push_back(support.NumQualityLevels - 1);
    for (UINT quality : qualities) {
      D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                             0,
                             width,
                             height,
                             1,
                             1,
                             DXGI_FORMAT_R32G32B32A32_FLOAT,
                             {count, quality},
                             D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> texture;
      HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   nullptr, IID_PPV_ARGS(&texture));
      bool valid =
          count && count <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT && supported && quality < support.NumQualityLevels;
      expect(valid ? hr == S_OK && texture : FAILED(hr) && !texture,
             "sample count%u quality%u of%u, query%#lx create%#lx", count, quality, support.NumQualityLevels, query,
             hr);
    }
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params, 0, nullptr,
                                          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!expect(!!rs, "root signature was not created"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
  const auto geometry = raster_samples::cases();
  const UINT vertex_bytes = sizeof(Vertex) * Triangle;
  auto vb =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, vertex_bytes * geometry.size(), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!vb, "vertex upload was not created"))
    return verdict();
  char *mapped_vertices;
  CHECK(vb->Map(0, nullptr, reinterpret_cast<void **>(&mapped_vertices)));
  for (size_t i = 0; i < geometry.size(); i++) {
    memset(mapped_vertices + i * vertex_bytes, 0, vertex_bytes);
    memcpy(mapped_vertices + i * vertex_bytes, geometry[i].vertices.data(),
           geometry[i].vertices.size() * sizeof(Vertex));
  }
  vb->Unmap(0, nullptr);
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, geometry.size() * sizeof(D3D12_DRAW_ARGUMENTS),
                          D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!arguments, "indirect arguments were not created"))
    return verdict();
  D3D12_DRAW_ARGUMENTS *draws;
  CHECK(arguments->Map(0, nullptr, reinterpret_cast<void **>(&draws)));
  for (size_t i = 0; i < geometry.size(); i++)
    draws[i] = {UINT(geometry[i].vertices.size()), 1, 0, 0};
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC draw{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC sig{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&signature)));
  D3D12_INPUT_ELEMENT_DESC elements[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, position),
                                          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                         {"DISTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, distance),
                                          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                         {"DISTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                                          offsetof(Vertex, distance) + sizeof(Color),
                                          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  for (auto &[count, s] : shaders)
    for (bool centered : {false, true})
      for (auto mode : {RenderMode::Regular, RenderMode::RtvForced, RenderMode::RtvForcedMsaa, RenderMode::UavForced,
                        RenderMode::UavDefault}) {
        const bool forced = mode != RenderMode::Regular, forced_msaa = mode == RenderMode::RtvForcedMsaa,
                   uav_only = mode == RenderMode::UavForced || mode == RenderMode::UavDefault;
        if (forced_msaa && count == 1)
          continue;
        if (mode == RenderMode::UavDefault && count != 1)
          continue;
        if (centered && (forced || count == 1))
          continue;
        if (forced && !forced_msaa && count == 2)
          continue;
        const UINT storage_count = forced && !forced_msaa ? 1 : count;
        const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{format, storage_count,
                                                              D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE};
        CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support)));
        if (storage_count == 1 || storage_count == 4)
          expect(support.NumQualityLevels > 0, "required %ux RGBA32_FLOAT support was not reported", count);
        if (!support.NumQualityLevels) {
          printf("unsupported: %ux RGBA32_FLOAT\n", count);
          continue;
        }
        auto all = forced ? forced_jobs(forced_msaa ? count : 1) : jobs(count, centered);
        if (uav_only)
          for (auto &j : all)
            j.pass = Uav;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               0,
                               width,
                               height,
                               1,
                               1,
                               format,
                               {storage_count, storage_count == 1
                                                   ? 0u
                                                   : UINT(centered ? DXGI_CENTER_MULTISAMPLE_QUALITY_PATTERN
                                                                   : DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN)},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ComPtr<ID3D12Resource> target, query;
        // a sample count the device reports is one it creates: a refusal is a wrong result, not the test's end
        step("%ux center %u mode %u targets", count, centered, unsigned(mode));
        if (!expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                              IID_PPV_ARGS(&target))),
                    "%ux render target was refused", storage_count))
          continue;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (!expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                              IID_PPV_ARGS(&query))),
                    "%ux texture was refused", storage_count))
          continue;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        ComPtr<ID3D12DescriptorHeap> rtvs, srvs, cpu_uav;
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvs)));
        auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);
        UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{format, D3D12_SRV_DIMENSION_TEXTURE2DMS};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        auto cpu = srvs->GetCPUDescriptorHandleForHeapStart();
        auto gpu = srvs->GetGPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(target.Get(), &sd, cpu);
        device->CreateShaderResourceView(query.Get(), &sd, {cpu.ptr + stride});
        device->CreateShaderResourceView(nullptr, &sd, {cpu.ptr + stride * 2});
        UINT64 slot = UINT64(width) * height * storage_count * sizeof(Color);
        auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, slot, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto readback =
            buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slot * all.size(), D3D12_RESOURCE_STATE_COPY_DEST);
        if (!expect(output && readback, "sample readback resources were not created"))
          return verdict();
        if (uav_only) {
          hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
          CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpu_uav)));
          D3D12_UNORDERED_ACCESS_VIEW_DESC output_desc{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
          output_desc.Buffer.NumElements = width * height * storage_count;
          output_desc.Buffer.StructureByteStride = sizeof(Color);
          device->CreateUnorderedAccessView(output.Get(), nullptr, &output_desc,
                                            cpu_uav->GetCPUDescriptorHandleForHeapStart());
          device->CopyDescriptorsSimple(1, {cpu.ptr + stride * 3}, cpu_uav->GetCPUDescriptorHandleForHeapStart(),
                                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        ComPtr<ID3D12PipelineState> compute;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{rs.Get(), bytecode(forced && !forced_msaa ? s.single_cs : s.cs)};
        CHECK(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&compute)));
        std::map<std::array<unsigned, 5>, ComPtr<ID3D12PipelineState>> pipelines;
        bool refused = false;
        for (unsigned path = 0; path < 3; path++) {
          CHECK(forget(readback.Get()));
          ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
          ComPtr<ID3D12GraphicsCommandList> list;
          CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
          CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
          CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)));
          std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
          for (size_t n = 0; n < all.size(); n++) {
            const auto &j = all[n];
            const auto &c = geometry[j.geometry];
            step("path %u center %u %ux sample job %zu geometry %zu pass %u mask %#x", path, centered, count, n,
                 j.geometry, j.pass, j.mask);
            std::array<unsigned, 5> key{c.mode, c.shape, unsigned(j.pass == Unbound ? Queries : j.pass), j.mask,
                                        unsigned(c.multisample) | (unsigned(c.aa) << 1)};
            auto &pso = pipelines[key];
            if (!pso) {
              D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
              pd.pRootSignature = rs.Get();
              pd.VS = bytecode(c.mode == Clip ? s.clip : s.vs);
              pd.PS = bytecode(j.pass == Uav                                ? s.uav
                               : j.pass == PerspectiveVarying               ? s.perspective
                               : j.pass == PerspectiveCentroid              ? s.perspective_centroid
                               : j.pass == PositionDepth                    ? s.depth
                               : j.pass == Varying                          ? s.varying
                               : j.pass == Centroid                         ? s.centroid
                               : (j.pass == Forced || j.pass == ForcedMsaa) ? s.forced
                               : j.pass == IndexOnly                        ? s.index
                               : j.pass == ModifierOnly                     ? s.modifier
                               : j.pass == Shading                          ? s.sample
                               : j.pass == Export                           ? s.output
                                                                            : s.query);
              pd.InputLayout = {elements, UINT(std::size(elements))};
              pd.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
              pd.RasterizerState.DepthClipEnable = TRUE;
              pd.RasterizerState.ForcedSampleCount = forced_msaa                                ? 1
                                                     : forced && mode != RenderMode::UavDefault ? count
                                                                                                : 0;
              pd.RasterizerState.MultisampleEnable = c.multisample;
              pd.RasterizerState.AntialiasedLineEnable = c.aa;
              pd.SampleMask = j.mask;
              pd.SampleDesc = td.SampleDesc;
              pd.NumRenderTargets = uav_only ? 0 : 1;
              pd.RTVFormats[0] = uav_only ? DXGI_FORMAT_UNKNOWN : format;
              pd.PrimitiveTopologyType = c.shape == Point  ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                         : c.shape == Line ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                           : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
              pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
              HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
              // D3D12_RASTERIZER_DESC: ForcedSampleCount is 0, 1, 4, 8 or, optionally, 16, independently of texture
              // sample counts, but only those the GPU has are implemented: the others are refused, and their mode's
              // other cases are not drawn
              D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{format, count};
              CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)));
              const bool lacking = forced && !forced_msaa && !levels.NumQualityLevels;
              if (lacking)
                expect(hr == E_INVALIDARG, "a forced count this GPU lacks gave HRESULT %#lx", hr);
              else if (FAILED(hr))
                expect(false, "pipeline was refused (HRESULT %#lx)", hr);
              if (FAILED(hr) || lacking) {
                refused = true;
                break;
              }
            }
            ID3D12DescriptorHeap *heaps[] = {srvs.Get()};
            list->SetDescriptorHeaps(1, heaps);
            D3D12_VIEWPORT vp{0, 0, width, height, 0, 1};
            D3D12_RECT rect{0, 0, width, height};
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &rect);
            if (uav_only) {
              list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
              const UINT clear[lanes] = {};
              list->ClearUnorderedAccessViewUint({gpu.ptr + stride * 3}, cpu_uav->GetCPUDescriptorHandleForHeapStart(),
                                                 output.Get(), clear, 0, nullptr);
              D3D12_RESOURCE_BARRIER cleared{D3D12_RESOURCE_BARRIER_TYPE_UAV};
              cleared.UAV.pResource = output.Get();
              list->ResourceBarrier(1, &cleared);
            } else {
              list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
              const float clear[lanes] = {};
              list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            }
            ComPtr<ID3D12GraphicsCommandList> bundle;
            if (path == 1)
              CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), pso.Get(),
                                              IID_PPV_ARGS(&bundle)));
            auto commands = path == 1 ? bundle.Get() : list.Get();
            commands->SetGraphicsRootSignature(rs.Get());
            if (uav_only)
              commands->SetGraphicsRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
            commands->SetGraphicsRootDescriptorTable(0, {gpu.ptr + stride * (j.pass == Unbound ? 2 : 1)});
            commands->SetPipelineState(pso.Get());
            D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress() + j.geometry * vertex_bytes, vertex_bytes,
                                          sizeof(Vertex)};
            commands->IASetVertexBuffers(0, 1, &view);
            commands->IASetPrimitiveTopology(c.shape == Point  ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                                             : c.shape == Line ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                               : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (path == 2)
              commands->ExecuteIndirect(signature.Get(), 1, arguments.Get(), j.geometry * sizeof(D3D12_DRAW_ARGUMENTS),
                                        nullptr, 0);
            else
              commands->DrawInstanced(c.vertices.size(), 1, 0, 0);
            if (bundle) {
              CHECK(bundle->Close());
              list->ExecuteBundle(bundle.Get());
              bundles.push_back(bundle);
            }
            if (!uav_only) {
              transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
              list->SetComputeRootSignature(rs.Get());
              list->SetComputeRootDescriptorTable(0, gpu);
              list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
              list->SetPipelineState(compute.Get());
              list->Dispatch(width, height, 1);
            }
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(readback.Get(), n * slot, output.Get(), 0, slot);
            transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!uav_only)
              transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_RENDER_TARGET);
          }
          if (refused) {
            CHECK(list->Close());
            break;
          }
          CHECK(submit(device.Get(), queue.Get(), list.Get()));
          Color *result;
          CHECK(readback->Map(0, nullptr, reinterpret_cast<void **>(&result)));
          for (size_t n = 0; n < all.size(); n++) {
            auto &j = all[n];
            step("read path %u %ux job %zu pass %u mask %#x", path, count, n, j.pass, j.mask);
            raster_samples::compare(result + n * width * height * storage_count, geometry[j.geometry], count, j.mask,
                                    j.pass, centered);
          }
          readback->Unmap(0, nullptr);
        }
      }
  return verdict();
}
