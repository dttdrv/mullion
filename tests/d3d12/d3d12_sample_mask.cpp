// contract: D3D11.3 17.17: "SampleMask is always applied, regardless of whether
// Multisample rendertargets are bound or not." 17.18 fixes A2C endpoints and
// says "If the Pixel Shader outputs oMask (output coverage), Alpha-to-Coverage
// is disabled." count the stored constant-one samples, since mask-bit mapping
// is implementation-defined. every reported count and numeric quality runs
// through direct, bundle and ExecuteIndirect draws, with per-sample readback.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#include "d3d12_test.hpp"
#include "../om_samples.hpp"
#include "../metal_samples.hpp"

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto defines = om::sample_defines();
  auto vs = compiler.compile(om::sample_hlsl, "vs", "vs", defines);
  std::string ps[] = {
      compiler.compile(om::sample_hlsl, "ps", "ps", defines),
      compiler.compile(om::sample_hlsl, "ps_coverage", "ps", defines)
  };
  std::string cs[2];
  for (UINT single = 0; single < std::size(cs); single++) {
    auto d = defines;
    d.push_back("SINGLE=" + std::to_string(single));
    d.push_back("COMPUTE_READ=1");
    cs[single] = compiler.compile(om::sample_hlsl, "cs", "cs", d);
  }
  if (!expect(
          !vs.empty() && !ps[0].empty() && !ps[1].empty() && !cs[0].empty() && !cs[1].empty(),
          "sample-mask HLSL did not compile"
      ))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {UINT(std::size(params)), params});
  if (!expect(bool(rs), "sample root signature missing"))
    return verdict();
  ComPtr<ID3D12PipelineState> compute[std::size(cs)];
  for (UINT i = 0; i < std::size(cs); i++) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rs.Get();
    p.CS = bytecode(cs[i]);
    CHECK(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&compute[i])));
  }
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator, bundle_allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto indirect =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(indirect), "indirect arguments missing"))
    return verdict();
  void *mapped;
  CHECK(indirect->Map(0, nullptr, &mapped));
  *static_cast<D3D12_DRAW_ARGUMENTS *>(mapped) = {3, 1, 0, 0};
  indirect->Unmap(0, nullptr);
  for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples++) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{DXGI_FORMAT_R32_FLOAT, samples};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support));
    if (!expect(
            (samples != 1 && samples != 4 && (samples != 8 || !metal_has_8x_samples())) || (hr == S_OK && support.NumQualityLevels),
            "required R32_FLOAT sample count %u returned %08lx, %u qualities", samples, hr, support.NumQualityLevels
        ))
      return verdict();
    if (FAILED(hr) || !support.NumQualityLevels) {
      printf("unsupported R32_FLOAT sample count %u: %08lx, %u qualities\n", samples, hr, support.NumQualityLevels);
      continue;
    }
    const auto cases = om::sample_cases(samples);
    const UINT width = om::sample_width, height = UINT(cases.size()), metadata_words = 3;
    const UINT word_count = metadata_words + width * height * samples;
    const UINT64 bytes = word_count * sizeof(UINT);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(upload && readback, "sample transfer buffers missing"))
      return verdict();
    CHECK(upload->Map(0, nullptr, &mapped));
    std::fill_n(static_cast<UINT *>(mapped), word_count, UINT_MAX);
    upload->Unmap(0, nullptr);
    for (UINT quality = 0; quality < support.NumQualityLevels; quality++) {
      std::vector<ComPtr<ID3D12PipelineState>> pipelines(cases.size());
      for (UINT i = 0; i < cases.size(); i++) {
        const auto &k = cases[i];
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
        p.pRootSignature = rs.Get();
        p.VS = bytecode(vs);
        p.PS = bytecode(ps[k.coverage]);
        p.NumRenderTargets = 1;
        p.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        p.SampleDesc = {samples, quality};
        p.SampleMask = k.mask;
        p.BlendState.AlphaToCoverageEnable = k.alpha;
        auto &r = p.BlendState.RenderTarget[0];
        r = {
            FALSE,
            FALSE,
            D3D12_BLEND_ONE,
            D3D12_BLEND_ZERO,
            D3D12_BLEND_OP_ADD,
            D3D12_BLEND_ONE,
            D3D12_BLEND_ZERO,
            D3D12_BLEND_OP_ADD,
            D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL
        };
        p.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
        p.RasterizerState.DepthClipEnable = p.RasterizerState.MultisampleEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        HRESULT created = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipelines[i]));
        if (!expect(
                created == S_OK && pipelines[i], "sample count %u, quality %u, case %u PSO returned %08lx", samples,
                quality, i, created
            ))
          return verdict();
      }
      for (UINT path = 0; path < 3; path++) {
        step("sample count %u, quality %u, path %u: initialize", samples, quality, path);
        CHECK(allocator->Reset());
        CHECK(bundle_allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        CHECK(forget(readback.Get()));
        D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{
            D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            0,
            width,
            height,
            1,
            1,
            DXGI_FORMAT_R32_FLOAT,
            {samples, quality},
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
        };
        ComPtr<ID3D12Resource> target;
        CHECK(device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
        ));
        auto result = buffer(
            device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        );
        if (!expect(bool(result), "sample result buffer missing"))
          return verdict();
        list->CopyBufferRegion(result.Get(), 0, upload.Get(), 0, bytes);
        transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
        D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
            sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        CHECK(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvs)));
        CHECK(device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvs)));
        auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format;
        sd.ViewDimension = samples == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (samples == 1)
          sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(target.Get(), &sd, srvs->GetCPUDescriptorHandleForHeapStart());
        const float black[4]{};
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        std::vector<ComPtr<ID3D12GraphicsCommandList>> bundles;
        for (UINT y = 0; y < cases.size(); y++) {
          step("sample count %u, quality %u, path %u, case %u", samples, quality, path, y);
          D3D12_VIEWPORT viewport{0, float(y), float(width), 1, 0, 1};
          D3D12_RECT scissor{1, LONG(y), LONG(width - 1), LONG(y + 1)};
          list->RSSetViewports(1, &viewport);
          list->RSSetScissorRects(1, &scissor);
          auto record = [&](ID3D12GraphicsCommandList *commands) {
            commands->SetPipelineState(pipelines[y].Get());
            commands->SetGraphicsRootSignature(rs.Get());
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (path == 2)
              commands->ExecuteIndirect(signature.Get(), 1, indirect.Get(), 0, nullptr, 0);
            else
              commands->DrawInstanced(3, 1, 0, 0);
          };
          if (path == 1) {
            auto &bundle = bundles.emplace_back();
            CHECK(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator.Get(), nullptr, IID_PPV_ARGS(&bundle)
            ));
            record(bundle.Get());
            CHECK(bundle->Close());
            list->ExecuteBundle(bundle.Get());
          } else
            record(list.Get());
        }
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        transition(
            list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        );
        list->SetPipelineState(compute[samples == 1].Get());
        list->SetComputeRootSignature(rs.Get());
        ID3D12DescriptorHeap *heaps[] = {srvs.Get()};
        list->SetDescriptorHeaps(std::size(heaps), heaps);
        list->SetComputeRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(1, result->GetGPUVirtualAddress());
        list->Dispatch(width, height, 1);
        transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, bytes);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const UINT *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        expect(
            got[0] == width && got[1] == height && got[2] == samples, "sample dimensions %u,%u,%u, want %u,%u,%u",
            got[0], got[1], got[2], width, height, samples
        );
        for (UINT y = 0; y < cases.size(); y++) {
          step("sample count %u, quality %u, path %u, case %u: readback", samples, quality, path, y);
          for (UINT x = 0; x < width; x++) {
            UINT count = 0;
            for (UINT sid = 0; sid < samples; sid++) {
              UINT value = got[metadata_words + (y * width + x) * samples + sid];
              expect(value == 0 || value == std::bit_cast<UINT>(1.0f), "pixel %u sample %u has %#x", x, sid, value);
              count += value == std::bit_cast<UINT>(1.0f);
            }
            UINT want = om::sample_result(cases[y], samples, x);
            expect(
                count == want, "pixel %u has %u covered samples, want %u, mask %#x, A2C %u, coverage %u", x, count,
                want, cases[y].mask, cases[y].alpha, cases[y].coverage
            );
          }
        }
        readback->Unmap(0, nullptr);
      }
    }
  }
  return verdict();
}
