// contract: a compute shader reads a mipmapped texture, the same texture through a view whose min-LOD clamp drops
// mip 0 (a load of mip 0 there is 0), and a typed buffer (a read past the view is 0 even with data behind it); it
// queries sizes, rewrites a typed UAV texture and buffer in place (a write past the view changes nothing), runs
// buffer and texture atomics and a UAV counter. a structured view reads its elements, and an element index whose
// byte offset wraps 32 bits (0x40000000 at stride 16) reads 0 like any other index past the view. every result equals what the same inputs give on the CPU, and the
// order-dependent results (atomics, counter) have the only values any order can give.
#include "d3d12_test.hpp"
#include <algorithm>
#include <vector>

static const char hlsl[] = R"hlsl(
Texture2D<uint4> tex : register(t0);
Buffer<uint4> typed : register(t1);
Texture2D<uint4> clamped : register(t2);
StructuredBuffer<uint4> structured : register(t3);
RWTexture2D<uint> rwtex : register(u0);
RWBuffer<uint> rwtyped : register(u1);
RWStructuredBuffer<uint> atom : register(u2);
RWStructuredBuffer<uint4> result : register(u3);
RWStructuredBuffer<uint> counted : register(u4);
RWTexture2D<uint> bits : register(u5);
[numthreads(W, H, 1)] void cs(uint3 t : SV_DispatchThreadID) {
  uint i = t.y * W + t.x;
  uint w, h, mips;
  tex.GetDimensions(1, w, h, mips);
  uint n; typed.GetDimensions(n);
  uint count = counted.IncrementCounter();
  rwtex[t.xy] = rwtex[t.xy] * 2 + 1;
  rwtyped[i] = rwtyped[i] + i;
  rwtyped[i + N] = 0xdead;
  uint old;
  InterlockedAdd(atom[0], 1);
  InterlockedMax(atom[1], i);
  InterlockedCompareExchange(atom[2], 0, i + 1, old);
  InterlockedOr(bits[uint2(0, 0)], 1u << (i % 32));
  result[i * 2] = uint4(tex.Load(int3(t.xy, 0)).x + tex.Load(int3(t.xy / 2, 1)).x, typed.Load(i).x + typed.Load(i + N).x,
                        (w * 1000 + h) * 1000 + mips * 10 + (n == N), count);
  uint e = i % ELEMENTS;
  result[i * 2 + 1] = uint4(clamped.Load(int3(t.xy, 0)).x, clamped.Load(int3(t.xy / 2, 1)).x, structured[e].x,
                            structured[e + 0x40000000u].x);
}
)hlsl";

static ComPtr<ID3D12Resource>
texture(ID3D12Device *device, UINT w, UINT h, UINT16 mips, D3D12_RESOURCE_FLAGS flags) {
  D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0,    w, h, 1, mips, DXGI_FORMAT_R32_UINT, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN,       flags};
  ComPtr<ID3D12Resource> res;
  device->CreateCommittedResource(
      &props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res)
  );
  return res;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // the typed SRV's buffer holds 2n dwords; the structured view sees them as 16-byte elements
  const UINT w = 8, h = 4, n = w * h, mips = 2, elements = 2 * n * 4 / 16;
  auto cs = compiler.compile(
      hlsl, "cs", "cs", {"W=" + std::to_string(w), "H=" + std::to_string(h), "N=" + std::to_string(n),
                         "ELEMENTS=" + std::to_string(elements)}
  );
  if (cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE ranges[3] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0, 0, 3},
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 9}
  };
  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  param.DescriptorTable = {3, ranges};
  auto rs = root_signature(device.Get(), {1, &param});
  if (!rs) {
    printf("failed: root signature\n");
    return 1;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  // inputs, distinct everywhere so a wrong texel or element shows; the elements after each buffer view are nonzero
  // guards, so a read or write past the view is visible
  std::vector<UINT> mip0(n), mip1(n / 4), typed_in(2 * n), rwtex_in(n), rwtyped_in(2 * n);
  for (UINT i = 0; i < n; i++) {
    mip0[i] = 1000 + i;
    rwtex_in[i] = 5 * i + 1;
  }
  for (UINT i = 0; i < 2 * n; i++) {
    typed_in[i] = 3 * i + 7;
    rwtyped_in[i] = 11 * i + 2;
  }
  for (UINT i = 0; i < n / 4; i++)
    mip1[i] = 50000 + i;

  auto tex = texture(device.Get(), w, h, mips, D3D12_RESOURCE_FLAG_NONE);
  auto rwtex = texture(device.Get(), w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto bits = texture(device.Get(), 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  // the typed SRV reads its own buffer; one UAV buffer holds the typed UAV, the atomics, the results and the counted
  // UAV with its counter
  const UINT64 rwtyped_at = 0, atom_at = 512, result_at = 1024, counted_at = 2048,
               counter_at = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT, buf_size = 2 * counter_at, srv_size = 2 * n * 4;
  auto srv_buf = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, srv_size, D3D12_RESOURCE_STATE_COPY_DEST);
  auto buf = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, buf_size, D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 3 * buf_size, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 3 * buf_size, D3D12_RESOURCE_STATE_COPY_DEST);

  // upload layout: the UAV buffer image, the SRV buffer image, then each texture subresource at its footprint
  ID3D12Resource *textures[4] = {tex.Get(), tex.Get(), rwtex.Get(), bits.Get()};
  const UINT subresources[4] = {0, 1, 0, 0};
  const UINT64 srv_at = buf_size;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[4];
  UINT64 offset = srv_at + srv_size, bytes;
  for (UINT s = 0; s < 4; s++) {
    auto desc = textures[s]->GetDesc();
    auto align = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    device->GetCopyableFootprints(
        &desc, subresources[s], 1, (offset + align - 1) / align * align, &fp[s], nullptr, nullptr, &bytes
    );
    offset = fp[s].Offset + bytes;
  }
  char *up;
  CHECK(upload->Map(0, nullptr, (void **)&up));
  memset(up, 0, 3 * buf_size);
  memcpy(up + rwtyped_at, rwtyped_in.data(), 2 * n * 4);
  memcpy(up + srv_at, typed_in.data(), srv_size);
  const std::vector<UINT> *images[3] = {&mip0, &mip1, &rwtex_in};
  for (int s = 0; s < 3; s++)
    for (UINT y = 0; y < fp[s].Footprint.Height; y++)
      memcpy(
          up + fp[s].Offset + y * fp[s].Footprint.RowPitch, images[s]->data() + y * fp[s].Footprint.Width,
          fp[s].Footprint.Width * 4
      );

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 10, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto at = [&](UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + i * step};
  };
  device->CreateShaderResourceView(tex.Get(), nullptr, at(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{
      DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
  };
  srv.Buffer = {0, n};
  device->CreateShaderResourceView(srv_buf.Get(), &srv, at(1));
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D = {0, mips, 0, 1.0f};
  device->CreateShaderResourceView(tex.Get(), &srv, at(2));
  device->CreateUnorderedAccessView(rwtex.Get(), nullptr, nullptr, at(3));
  D3D12_UNORDERED_ACCESS_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER};
  view.Buffer = {rwtyped_at / 4, n};
  device->CreateUnorderedAccessView(buf.Get(), nullptr, &view, at(4));
  view.Format = DXGI_FORMAT_UNKNOWN;
  view.Buffer = {atom_at / 4, 4, 4};
  device->CreateUnorderedAccessView(buf.Get(), nullptr, &view, at(5));
  view.Buffer = {result_at / 16, 2 * n, 16};
  device->CreateUnorderedAccessView(buf.Get(), nullptr, &view, at(6));
  view.Buffer = {counted_at / 4, n, 4, counter_at};
  device->CreateUnorderedAccessView(buf.Get(), buf.Get(), &view, at(7));
  device->CreateUnorderedAccessView(bits.Get(), nullptr, nullptr, at(8));
  // the typed SRV's buffer again, as 16-byte structures
  srv.Format = DXGI_FORMAT_UNKNOWN;
  srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  srv.Buffer = {0, elements, 16};
  device->CreateShaderResourceView(srv_buf.Get(), &srv, at(9));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list)));
  list->CopyBufferRegion(buf.Get(), 0, upload.Get(), 0, buf_size);
  list->CopyBufferRegion(srv_buf.Get(), 0, upload.Get(), srv_at, srv_size);
  for (UINT s = 0; s < 4; s++) {
    D3D12_TEXTURE_COPY_LOCATION
    dst{textures[s], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = subresources[s]}},
        src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp[s]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  for (auto res : {tex.Get(), srv_buf.Get()})
    transition(list.Get(), res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  for (auto res : {buf.Get(), rwtex.Get(), bits.Get()})
    transition(list.Get(), res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch(1, 1, 1);
  for (auto res : {buf.Get(), rwtex.Get(), bits.Get()})
    transition(list.Get(), res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, buf.Get(), 0, buf_size);
  for (UINT s = 2; s < 4; s++) {
    D3D12_TEXTURE_COPY_LOCATION src{textures[s], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = fp[s]}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  auto u32 = [&](UINT64 offset) { return *(UINT *)(out + offset); };
  unsigned mismatches = 0;
  auto expect = [&](const char *what, UINT i, UINT got, UINT want) {
    if (got != want && mismatches++ < 8)
      printf("%s %u: got %u, want %u\n", what, i, got, want);
  };
  std::vector<UINT> counters;
  for (UINT i = 0; i < n; i++) {
    UINT x = i % w, y = i / w, mip1_texel = mip1[(y / 2) * (w / 2) + x / 2];
    auto r = (UINT *)(out + result_at + i * 32);
    expect("texture", i, r[0], mip0[i] + mip1_texel);
    expect("typed buffer", i, r[1], typed_in[i]);
    expect("sizes", i, r[2], ((w / 2) * 1000 + h / 2) * 1000 + mips * 10 + 1);
    counters.push_back(r[3]);
    expect("clamped-off mip", i, r[4], 0);
    expect("mip under the clamp", i, r[5], mip1_texel);
    expect("structured element", i, r[6], typed_in[(i % elements) * 4]);
    expect("structured index past 32-bit offsets", i, r[7], 0);
    expect("rw texture", i, u32(fp[2].Offset + y * fp[2].Footprint.RowPitch + x * 4), rwtex_in[i] * 2 + 1);
    expect("rw typed buffer", i, u32(rwtyped_at + i * 4), rwtyped_in[i] + i);
    expect("guard after the typed UAV", i, u32(rwtyped_at + (n + i) * 4), rwtyped_in[n + i]);
  }
  // order-dependent results: the counter hands out 0..n-1 once each, cmpxchg lets exactly one thread's i + 1 in
  std::sort(counters.begin(), counters.end());
  for (UINT i = 0; i < n; i++)
    expect("counter value", i, counters[i], i);
  expect("counter", 0, u32(counter_at), n);
  expect("atomic add", 0, u32(atom_at), n);
  expect("atomic max", 0, u32(atom_at + 4), n - 1);
  expect("atomic cmpxchg in range", 0, u32(atom_at + 8) >= 1 && u32(atom_at + 8) <= n, 1);
  expect("texture atomic or", 0, u32(fp[3].Offset), n >= 32 ? ~0u : (1u << n) - 1);
  printf("%s: %u mismatches\n", mismatches ? "failed" : "passed", mismatches);
  return mismatches != 0;
}
