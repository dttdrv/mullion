// contract: ClearUnorderedAccessViewUint "copies the lower ni bits from each array element i to the corresponding
// channel, where ni is the number of bits in the ith channel of the resource format", with no conversion (Microsoft
// Learn, ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint). the values carry a half and a float NaN with a
// payload, denormals, a negative zero and snorm's -128, and for R11G11B10 a second set with a NaN with a payload and
// both infinities (11 and 10 bits), which a clear that converted values would change.
// https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-clearunorderedaccessviewuint
#include "d3d12_test.hpp"

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<ID3D12DescriptorHeap> gpu, cpu;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&gpu)));
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu)));

  const UINT sets[][4] = {{0x7f807c01, 0x00000001, 0x80008000, 0xfffffe80}, {0x000007c1, 0x000007c0, 0x000003e0, 0}};
  // each format's channel widths from its lowest bits, in the order DXGI names them
  const struct {
    DXGI_FORMAT format;
    std::vector<UINT> widths;
    UINT set = 0;
  } formats[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, {8, 8, 8, 8}},          {DXGI_FORMAT_R8_UNORM, {8}},
      {DXGI_FORMAT_R16G16B16A16_UNORM, {16, 16, 16, 16}},  {DXGI_FORMAT_R10G10B10A2_UNORM, {10, 10, 10, 2}},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, {16, 16, 16, 16}},  {DXGI_FORMAT_R16_FLOAT, {16}},
      {DXGI_FORMAT_R32_FLOAT, {32}},                       {DXGI_FORMAT_R32G32B32A32_FLOAT, {32, 32, 32, 32}},
      {DXGI_FORMAT_R11G11B10_FLOAT, {11, 11, 10}},         {DXGI_FORMAT_R8G8B8A8_SNORM, {8, 8, 8, 8}},
      {DXGI_FORMAT_R11G11B10_FLOAT, {11, 11, 10}, 1},
  };
  constexpr UINT width = 3, height = 2;
  UINT exercised = 0, failed = 0;
  for (auto &[format, widths, set] : formats) {
    auto &values = sets[set];
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
      continue;
    exercised++;
    std::vector<uint8_t> want;
    UINT bits = 0;
    for (UINT w : widths)
      bits += w;
    for (UINT byte = 0; byte < bits / 8; byte++) {
      uint8_t b = 0;
      for (UINT bit = 0; bit < 8; bit++) {
        UINT at = byte * 8 + bit, channel = 0;
        while (at >= widths[channel])
          at -= widths[channel++];
        b |= ((values[channel] >> at) & 1) << bit;
      }
      want.push_back(b);
    }

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    };
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture)
    ));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 total;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(forget(readback.Get()));
    device->CreateUnorderedAccessView(texture.Get(), nullptr, nullptr, gpu->GetCPUDescriptorHandleForHeapStart());
    device->CreateUnorderedAccessView(texture.Get(), nullptr, nullptr, cpu->GetCPUDescriptorHandleForHeapStart());

    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), nullptr));
    ID3D12DescriptorHeap *heaps[] = {gpu.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->ClearUnorderedAccessViewUint(
        gpu->GetGPUDescriptorHandleForHeapStart(), cpu->GetCPUDescriptorHandleForHeapStart(), texture.Get(), values, 0,
        nullptr
    );
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{
        texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
    };
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    uint8_t *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        auto texel = got + y * footprint.Footprint.RowPitch + x * want.size();
        if (memcmp(texel, want.data(), want.size()) && !wrong++) {
          printf("format %u set %u texel %u,%u:", format, set, x, y);
          for (size_t i = 0; i < want.size(); i++)
            printf(" %02x/%02x", texel[i], want[i]);
          printf(" (got/want)\n");
        }
      }
    readback->Unmap(0, nullptr);
    failed += wrong != 0;
  }
  printf("%s: %u of %u clears store other bits than the clear's\n", failed || !exercised ? "failed" : "passed", failed,
         exercised);
  return failed || !exercised;
}
