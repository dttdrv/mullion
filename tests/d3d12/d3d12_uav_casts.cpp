// contract: "D3D allows Texture1D/2D/3D resources created with any of the following small set of 32-bit per element
// formats to have UAVs created from them with R32_UINT/_SINT/_FLOAT as the type: DXGI_FORMAT_R10G10B10A2_TYPELESS,
// DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8X8_TYPELESS,
// DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R32_TYPELESS" (D3D11.3 functional specification 5.3.9.5, Limitations on
// Typed UAVs). a store through such a view writes its 32 bits as they are, which a copy then reads
// back byte for byte. D3D12 keeps it: vkd3d-proton's test_typed_srv_uav_cast (tests/d3d12_descriptors.c) expects
// R32_UINT, R32_SINT and R32_FLOAT unordered access views of R8G8B8A8_TYPELESS and R16G16_TYPELESS textures to be
// made ("5.3.9.5 from D3D11 functional spec. 32-bit typeless formats can be viewed as R32{U,I,F}"). Forza Horizon 5
// logs refused unordered access views (dttdrv/mullion#34).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#5.3.9.5%20Limitations%20on%20Typed%20UAVs
// every texel's value has four different bytes, and the float view's values are normal floats, which a store keeps.
#include "d3d12_test.hpp"

static const char shader[] = R"(
RWTexture2D<TYPE> target : register(u0);
uint value(uint2 at) {
  return ((at.x * 0x01030507u + at.y * 0x0b0d1113u + 0x10204080u) & 0x807fffffu) | 0x3f000000u;
}
[numthreads(4, 4, 1)] void cs(uint2 at : SV_DispatchThreadID) {
  target[at] = CAST(value(at));
}
)";

// what the shader stores at a texel
static UINT
value(UINT x, UINT y) {
  return ((x * 0x01030507u + y * 0x0b0d1113u + 0x10204080u) & 0x807fffffu) | 0x3f000000u;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0};
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  parameter.DescriptorTable = {1, &range};
  auto root = root_signature(device.Get(), {1, &parameter});
  CHECK(root ? S_OK : E_FAIL);

  // a pipeline per view type
  struct Pipeline {
    const char *type;
    ComPtr<ID3D12PipelineState> state;
  } pipelines[] = {{"uint"}, {"int"}, {"float"}};
  for (auto &pipeline : pipelines) {
    auto code = compiler.compile(shader, "cs", "cs",
                                 {std::string("TYPE=") + pipeline.type, std::string("CAST=as") + pipeline.type});
    if (!expect(!code.empty(), "the %s shader does not compile", pipeline.type))
      return verdict();
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{root.Get(), bytecode(code)};
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline.state)));
  }

  constexpr UINT size = 4;
  // stores through the descriptor with the pipeline, and reads the texture's bytes back
  auto store = [&](ID3D12Resource *texture, const Pipeline &pipeline, std::vector<UINT> &texels) -> HRESULT {
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 total;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    if (HRESULT hr = forget(readback.Get()); FAILED(hr))
      return hr;
    if (HRESULT hr = allocator->Reset(); FAILED(hr))
      return hr;
    if (HRESULT hr = list->Reset(allocator.Get(), pipeline.state.Get()); FAILED(hr))
      return hr;
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
        src{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (HRESULT hr = submit(device.Get(), queue.Get(), list.Get()); FAILED(hr))
      return hr;
    BYTE *got;
    if (HRESULT hr = readback->Map(0, nullptr, (void **)&got); FAILED(hr))
      return hr;
    texels.resize(size * size);
    for (UINT y = 0; y < size; y++)
      memcpy(&texels[y * size], got + y * footprint.Footprint.RowPitch, size * sizeof(UINT));
    readback->Unmap(0, nullptr);
    return S_OK;
  };

  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                 DXGI_FORMAT_B8G8R8A8_TYPELESS,    DXGI_FORMAT_B8G8R8X8_TYPELESS,
                                 DXGI_FORMAT_R16G16_TYPELESS,      DXGI_FORMAT_R32_TYPELESS};
  const DXGI_FORMAT views[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32_FLOAT};
  for (auto format : formats)
    for (UINT v = 0; v < std::size(views); v++) {
      step("%s view of a texture of format %u", pipelines[v].type, format);
      D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
      ComPtr<ID3D12Resource> texture;
      if (!expect(SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&texture))),
                  "the texture is refused"))
        continue;
      D3D12_UNORDERED_ACCESS_VIEW_DESC view{views[v], D3D12_UAV_DIMENSION_TEXTURE2D};
      device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, heap->GetCPUDescriptorHandleForHeapStart());
      std::vector<UINT> texels;
      CHECK(store(texture.Get(), pipelines[v], texels));
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++)
          expect(texels[y * size + x] == value(x, y), "texel %u,%u is %08x, want %08x", x, y, texels[y * size + x],
                 value(x, y));
    }
  return verdict();
}
