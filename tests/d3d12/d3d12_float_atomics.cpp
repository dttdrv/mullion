// contract: shader model 6.6's float atomics on typed resources, an R32_FLOAT RWTexture2D<float> and RWBuffer<float>:
// "InterlockedExchange", "InterlockedCompareExchangeFloatBitwise" and "InterlockedCompareStoreFloatBitwise" have
// overloads for "typed resources ... declared with HLSL type float and ... format R32_FLOAT", which every shader
// model 6.6 device supports (HLSL Shader Model 6.6, Int64 and Float Atomics). an exchange returns what memory held
// and leaves the new value; a bitwise compare exchanges only on equal bits, so -0.0 does not match 0.0.
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Int64_and_Float_Atomics.html
// a group's threads then all exchange their own value into one element: each value is returned once, as is the
// element's first, and the element keeps the one value no thread got back.
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>

static constexpr UINT threads = 96, elements = 8;

static const char hlsl[] = R"hlsl(
RWTexture2D<float> texels : register(u0);
RWBuffer<float> words : register(u1);
RWStructuredBuffer<float> got : register(u2);
// element i of either, which hold 1.0 at 0 to 3, 0.0 at 4 and 5, and 7.0 at 6 and 7
#define RUN(at, out)                                                                                                  \
  {                                                                                                                   \
    float was;                                                                                                        \
    InterlockedExchange(at(0), 2.0f, was);                                                                            \
    got[out + 0] = was;                                                                                               \
    InterlockedCompareStoreFloatBitwise(at(1), 1.0f, 3.0f);                                                           \
    InterlockedCompareStoreFloatBitwise(at(2), 5.0f, 4.0f);                                                           \
    InterlockedCompareExchangeFloatBitwise(at(3), 1.0f, 6.0f, was);                                                   \
    got[out + 1] = was;                                                                                               \
    InterlockedCompareExchangeFloatBitwise(at(4), -0.0f, 8.0f, was);                                                  \
    got[out + 2] = was;                                                                                               \
    InterlockedCompareStoreFloatBitwise(at(5), 0.0f, 9.0f);                                                           \
  }
#define TEXEL(i) texels[uint2(i, 0)]
#define WORD(i) words[i]
[numthreads(1, 1, 1)]
void singles() {
  RUN(TEXEL, 0)
  RUN(WORD, 3)
}
// thread t exchanges t + 10 into element 7 of each
[numthreads(THREADS, 1, 1)]
void contended(uint t : SV_GroupIndex) {
  float was;
  InterlockedExchange(texels[uint2(7, 0)], t + 10.0f, was);
  got[6 + t] = was;
  InterlockedExchange(words[7], t + 10.0f, was);
  got[6 + THREADS + t] = was;
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: float atomics are shader model 6.6, DXIL only\n");
    return 77;
  }
  const std::vector<std::string> defines = {"THREADS=" + std::to_string(threads)};
  auto singles = compiler.compile(hlsl, "singles", "cs_6_6", defines),
       contended = compiler.compile(hlsl, "contended", "cs_6_6", defines);
  if (!expect(!singles.empty() && !contended.empty(), "HLSL did not compile"))
    return verdict();

  step("create the device and the pipelines");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  parameter.DescriptorTable = {1, &range};
  auto rs = root_signature(device.Get(), {1, &parameter});
  CHECK(rs ? S_OK : E_FAIL);
  ComPtr<ID3D12PipelineState> singles_pso, contended_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(singles)};
  HRESULT created = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&singles_pso));
  expect(SUCCEEDED(created), "the single atomics' pipeline: %#lx", created);
  desc.CS = bytecode(contended);
  created = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&contended_pso));
  expect(SUCCEEDED(created), "the contended exchanges' pipeline: %#lx", created);
  if (!singles_pso || !contended_pso)
    return verdict();

  step("create the resources");
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, elements, 1, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)
  ));
  const UINT got_count = 6 + 2 * threads, row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  auto words = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, elements * 4, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto got = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, got_count * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  const float initial[elements] = {1, 1, 1, 1, 0, 0, 7, 7};
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, row, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped;
  CHECK(upload->Map(0, nullptr, &mapped));
  memcpy(mapped, initial, sizeof(initial));
  upload->Unmap(0, nullptr);
  // the texture's row, the buffer's elements, then what the shaders returned
  const UINT64 words_at = row, got_at = 2 * row;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, got_at + got_count * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  CHECK(forget(readback.Get()));

  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  auto handle = views->GetCPUDescriptorHandleForHeapStart();
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  device->CreateUnorderedAccessView(texture.Get(), nullptr, nullptr, handle);
  handle.ptr += increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC words_view{DXGI_FORMAT_R32_FLOAT, D3D12_UAV_DIMENSION_BUFFER};
  words_view.Buffer = {0, elements};
  device->CreateUnorderedAccessView(words.Get(), nullptr, &words_view, handle);
  handle.ptr += increment;
  D3D12_UNORDERED_ACCESS_VIEW_DESC got_view{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  got_view.Buffer = {0, got_count, 4};
  device->CreateUnorderedAccessView(got.Get(), nullptr, &got_view, handle);

  step("run the atomics");
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), singles_pso.Get(), IID_PPV_ARGS(&list)));
  D3D12_TEXTURE_COPY_LOCATION placed{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      texel_location{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  placed.PlacedFootprint = {0, {DXGI_FORMAT_R32_FLOAT, elements, 1, 1, row}};
  list->CopyTextureRegion(&texel_location, 0, 0, 0, &placed, nullptr);
  list->CopyBufferRegion(words.Get(), 0, upload.Get(), 0, elements * 4);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list.Get(), words.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *heaps[] = {views.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch(1, 1, 1);
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  list->ResourceBarrier(1, &uav);
  list->SetPipelineState(contended_pso.Get());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), words.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), got.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  placed.pResource = readback.Get();
  list->CopyTextureRegion(&placed, 0, 0, 0, &texel_location, nullptr);
  list->CopyBufferRegion(readback.Get(), words_at, words.Get(), 0, elements * 4);
  list->CopyBufferRegion(readback.Get(), got_at, got.Get(), 0, got_count * 4);
  HRESULT hr = submit(device.Get(), queue.Get(), list.Get());
  if (!expect(SUCCEEDED(hr), "the dispatches failed: %#lx", hr))
    return verdict();

  step("read back");
  uint8_t *bytes;
  CHECK(readback->Map(0, nullptr, (void **)&bytes));
  auto value = [&](UINT64 at) { return *reinterpret_cast<const float *>(bytes + at); };
  auto bits = [&](UINT64 at) { return *reinterpret_cast<const uint32_t *>(bytes + at); };
  // what elements 0 to 5 hold after the singles, and what the singles returned
  const float held[6] = {2, 3, 1, 6, 0, 9}, returned[3] = {1, 1, 0};
  const char *names[2] = {"texture", "buffer"};
  const UINT64 element_at[2] = {0, words_at};
  for (UINT r = 0; r < 2; r++) {
    for (UINT i = 0; i < 6; i++)
      expect(value(element_at[r] + 4 * i) == held[i] && bits(element_at[r] + 4 * i) != 0x80000000,
             "%s element %u holds %g (%#x), want %g", names[r], i, value(element_at[r] + 4 * i),
             bits(element_at[r] + 4 * i), held[i]);
    for (UINT i = 0; i < 3; i++)
      expect(value(got_at + 4 * (3 * r + i)) == returned[i], "%s single %u returned %g, want %g", names[r], i,
             value(got_at + 4 * (3 * r + i)), returned[i]);
    // the element's first value and every thread's are each returned once, but for the one left in the element
    std::vector<float> seen = {value(element_at[r] + 4 * 7)}, want = {7};
    for (UINT t = 0; t < threads; t++) {
      seen.push_back(value(got_at + 4 * (6 + r * threads + t)));
      want.push_back(t + 10.0f);
    }
    std::sort(seen.begin(), seen.end());
    std::sort(want.begin(), want.end());
    unsigned wrong = 0;
    for (UINT i = 0; i < seen.size(); i++)
      if (seen[i] != want[i] && wrong++ < 4)
        expect(false, "%s contended exchanges: value %u of those seen is %g, want %g", names[r], i, seen[i], want[i]);
    expect(value(element_at[r] + 4 * 6) == 7, "%s element 6 holds %g, want 7", names[r], value(element_at[r] + 4 * 6));
  }
  readback->Unmap(0, nullptr);
  return verdict();
}
