// contract: objects at the sizes an engine asks for are whole, to their last element.
// - a CBV, SRV and UAV heap for rendering holds 1,000,000 descriptors on every resource binding tier and
//   "1,000,000+" on tier 3 (Microsoft Learn, "Hardware Tiers"): 2,000,000 there, as Unreal Engine 5 asks. a shader
//   reads through the descriptor at the heap's start, around 2^20 and at its end, each written in place or copied
//   from a heap that is not shader visible, and gets that descriptor's own view;
// - a shader visible sampler heap holds 2048 samplers ("Hardware Tiers"), all different here. each one's border is
//   one or zero by the parity of its index's bits, so two indices a power of two apart never agree, and the samplers
//   at the heap's start, middle and end sample with their own border;
// - a texture is 16384 texels wide and high, an array has 2048 layers (Microsoft Learn, "Hardware Feature Levels",
//   maximum texture dimension; D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION), and the last texel of each holds what
//   was copied into it.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint slot; };
Buffer<uint> source : register(t0);
Texture2D<float> white : register(t1);
SamplerState s : register(s0);
RWStructuredBuffer<uint> results : register(u0);
[numthreads(1, 1, 1)] void cs_view() { results[slot] = source[0]; }
// outside the texture: the sampler's border
[numthreads(1, 1, 1)] void cs_border() { results[slot] = 1 + (uint)white.SampleLevel(s, float2(2, 2), 0); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs_view = compiler.compile(hlsl, "cs_view", "cs"), cs_border = compiler.compile(hlsl, "cs_border", "cs");
  if (cs_view.empty() || cs_border.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));

  enum { Slot, Source, White, Sampler, Results };
  D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0}};
  D3D12_ROOT_PARAMETER parameters[5] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                        {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                        {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[Slot].Constants = {0, 0, 1};
  for (UINT i = 0; i < 3; i++)
    parameters[Source + i].DescriptorTable = {1, &ranges[i]};
  auto rs = root_signature(device.Get(), {5, parameters});
  ComPtr<ID3D12PipelineState> view_pso, border_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs_view)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&view_pso)));
  pso_desc.CS = bytecode(cs_border);
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&border_pso)));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the values the views show, one element each, and what the shaders wrote, which starts as none of them
  const UINT slots = 16, none = ~0u;
  auto values = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, slots * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, slots * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto cleared = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, slots * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, slots * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  UINT *mapped;
  CHECK(values->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < slots; i++)
    mapped[i] = 0x51e5000 + i;
  CHECK(cleared->Map(0, nullptr, (void **)&mapped));
  memset(mapped, 0xff, slots * sizeof(UINT));
  // runs the list's dispatches, which wrote `count` results, and gives them back
  auto finish = [&](UINT count, UINT *got) -> HRESULT {
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, count * sizeof(UINT));
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
    UINT *data;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = readback->Map(0, nullptr, (void **)&data))) {
      memcpy(got, data, count * sizeof(UINT));
      readback->Unmap(0, nullptr);
      if (SUCCEEDED(hr = allocator->Reset()))
        hr = list->Reset(allocator.Get(), nullptr);
    }
    return hr;
  };
  // every pass starts from results that are none of its values
  auto start = [&](ID3D12PipelineState *pso, ID3D12DescriptorHeap *const *heaps, UINT heap_count) {
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(results.Get(), 0, cleared.Get(), 0, slots * sizeof(UINT));
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pso);
    list->SetComputeRootSignature(rs.Get());
    list->SetDescriptorHeaps(heap_count, heaps);
    list->SetComputeRootUnorderedAccessView(Results, results->GetGPUVirtualAddress());
  };

  ComPtr<ID3D12DescriptorHeap> staging;
  D3D12_DESCRIPTOR_HEAP_DESC staging_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1};
  CHECK(device->CreateDescriptorHeap(&staging_desc, IID_PPV_ARGS(&staging)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (UINT size : {1000000u, 2000000u}) {
    if (size > 1000000 && options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
      continue;
    step("a shader visible CBV, SRV and UAV heap of %u descriptors", size);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, size, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    if (!expect(hr == S_OK, "CreateDescriptorHeap: %08lx", hr))
      continue;
    std::vector<UINT> indices{0, 1, size / 2, size - 2, size - 1};
    for (UINT index : {(1u << 20) - 1, 1u << 20, (1u << 20) + 1})
      if (index < size)
        indices.push_back(index);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    start(view_pso.Get(), heap.GetAddressOf(), 1);
    for (UINT n = 0; n < indices.size(); n++) {
      D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
      view.Buffer = {n, 1};
      D3D12_CPU_DESCRIPTOR_HANDLE at{cpu.ptr + SIZE_T(indices[n]) * increment};
      // every other one comes from a heap that is not shader visible, as engines stage them
      if (n & 1) {
        device->CreateShaderResourceView(values.Get(), &view, staging->GetCPUDescriptorHandleForHeapStart());
        device->CopyDescriptorsSimple(1, at, staging->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      } else {
        device->CreateShaderResourceView(values.Get(), &view, at);
      }
      list->SetComputeRoot32BitConstant(Slot, n, 0);
      list->SetComputeRootDescriptorTable(Source, {gpu.ptr + UINT64(indices[n]) * increment});
      list->Dispatch(1, 1, 1);
    }
    UINT got[slots];
    CHECK(finish(indices.size(), got));
    CHECK(values->Map(0, nullptr, (void **)&mapped));
    for (UINT n = 0; n < indices.size(); n++)
      expect(got[n] == mapped[n], "the view at descriptor %u of %u, %s: read %x, want %x", indices[n], size,
             n & 1 ? "copied" : "written in place", got[n], mapped[n]);
  }

  step("a shader visible sampler heap of %u samplers, all different", D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
  {
    const UINT size = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;
    ComPtr<ID3D12DescriptorHeap> samplers, views;
    D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, size, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
        view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    HRESULT hr = device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&samplers));
    CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
    // a texture of ones: only a border is anything else
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
                                     D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const float ones[4] = {1, 1, 1, 1};
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture)));
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    device->CreateRenderTargetView(texture.Get(), nullptr, rtvs->GetCPUDescriptorHandleForHeapStart());
    device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
    if (expect(hr == S_OK, "CreateDescriptorHeap: %08lx", hr)) {
      const UINT sampler_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
      // each has a level of detail range of its own
      auto border = [](UINT i) { return (UINT)__builtin_popcount(i) & 1; };
      for (UINT i = 0; i < size; i++) {
        D3D12_SAMPLER_DESC desc{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                D3D12_TEXTURE_ADDRESS_MODE_BORDER};
        desc.MaxLOD = i + 1;
        for (auto &c : desc.BorderColor)
          c = border(i);
        device->CreateSampler(&desc, {samplers->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * sampler_increment});
      }
      list->ClearRenderTargetView(rtvs->GetCPUDescriptorHandleForHeapStart(), ones, 0, nullptr);
      transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      ID3D12DescriptorHeap *heaps[] = {views.Get(), samplers.Get()};
      start(border_pso.Get(), heaps, 2);
      list->SetComputeRootDescriptorTable(White, views->GetGPUDescriptorHandleForHeapStart());
      const UINT used[] = {0, 1, size / 2 - 1, size / 2, size - 2, size - 1};
      for (UINT n = 0; n < std::size(used); n++) {
        list->SetComputeRoot32BitConstant(Slot, n, 0);
        list->SetComputeRootDescriptorTable(Sampler, {samplers->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(used[n]) * sampler_increment});
        list->Dispatch(1, 1, 1);
      }
      UINT got[std::size(used)];
      CHECK(finish(std::size(used), got));
      for (UINT n = 0; n < std::size(used); n++)
        expect(got[n] == 1 + border(used[n]), "the border of sampler %u of %u: %d, want %u", used[n], size,
               (int)got[n] - 1, border(used[n]));
    }
  }

  // a texture as large as Direct3D has them in one direction after another: `value` copied into the last texel of
  // its last layer comes back from there, and the texel before it stays zero
  auto last_texel = [&](const char *what, UINT64 width, UINT height, UINT16 layers) -> HRESULT {
    step("a texture of %llu x %u texels and %u layers: %s", width, height, layers, what);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, layers, 1, DXGI_FORMAT_R8_UNORM, {1, 0}};
    ComPtr<ID3D12Resource> texture;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    if (!expect(hr == S_OK, "CreateCommittedResource: %08lx", hr))
      return S_OK;
    const UINT8 value = 0xa5;
    // two texels a row apart in a buffer (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT), for a copy of 2 x 1 texels
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT8 *texels;
    if (FAILED(hr = upload->Map(0, nullptr, (void **)&texels)))
      return hr;
    texels[0] = 0, texels[1] = value;
    D3D12_TEXTURE_COPY_LOCATION in_buffer{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, in_texture{texture.Get()},
        out_buffer{back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    in_buffer.PlacedFootprint = out_buffer.PlacedFootprint = {0, {DXGI_FORMAT_R8_UNORM, 2, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
    in_texture.SubresourceIndex = layers - 1;
    D3D12_BOX corner{UINT(width - 2), height - 1, 0, UINT(width), height, 1};
    list->CopyTextureRegion(&in_texture, corner.left, corner.top, 0, &in_buffer, nullptr);
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&out_buffer, 0, 0, 0, &in_texture, &corner);
    if (FAILED(hr = submit(device.Get(), queue.Get(), list.Get())) || FAILED(hr = back->Map(0, nullptr, (void **)&texels)))
      return hr;
    expect(texels[0] == 0 && texels[1] == value, "its last two texels: %02x %02x, want 00 %02x", texels[0], texels[1], value);
    back->Unmap(0, nullptr);
    return FAILED(hr = allocator->Reset()) ? hr : list->Reset(allocator.Get(), nullptr);
  };
  CHECK(last_texel("the widest", D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, 4, 1));
  CHECK(last_texel("the highest", 4, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, 1));
  CHECK(last_texel("the widest and highest", D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, 1));
  CHECK(last_texel("the most layers", 4, 4, D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION));
  return verdict();
}
