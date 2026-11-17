// contract: a texture UAV's store, atomic and load at an address past its size do nothing to the texture, wherever
// the address is. "Out of bounds addressing means nothing gets written to memory" (D3D11.3 22.4.9, store_uav_typed);
// immediate atomics write nothing there and return an undefined value (22.17.24); a typed UAV load there returns 0
// (22.4.8, as ld: 22.4.6). the addresses are the texture's own size, past 16 bits (an engine builds them as
// (group << 5) | lane, which passes 65535 at group 2048, dttdrv/mullion#238), past Metal's largest texture, with the
// sign bit and all bits set, and past an array's last slice. each one wraps to its own texel at any width of 6 bits or
// more, so a store that lands inside instead shows where. a 2D texture and a 2D array of R32_UINT, every texel seeded
// with a value of its own; one thread a point stores a marker, adds 1 to it atomically and loads it again; held to:
// in bounds, the atomic returns the marker and the load the marker plus 1, and every other texel keeps its seed
#include "d3d12_test.hpp"

static constexpr UINT size = 64, slices = 2;
static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint kind; };
RWTexture2D<uint> plain : register(u0);
RWTexture2DArray<uint> array : register(u1);
RWStructuredBuffer<uint2> got : register(u2);
StructuredBuffer<uint3> points : register(t0);
[numthreads(8, 8, 1)] void fill(uint3 p : SV_DispatchThreadID) {
  plain[p.xy] = 1 + p.x + 64 * p.y;
  array[p] = 0x10000 + p.x + 64 * p.y + 4096 * p.z;
}
[numthreads(1, 1, 1)] void poke(uint3 group : SV_GroupID) {
  uint i = group.x, marker = 0x40000000u | (kind << 8) | i, original, after;
  uint3 p = points[i];
  if (kind == 0) {
    plain[p.xy] = marker;
    InterlockedAdd(plain[p.xy], 1, original);
    after = plain[p.xy];
  } else {
    array[p] = marker;
    InterlockedAdd(array[p], 1, original);
    after = array[p];
  }
  got[kind * POINTS + i] = uint2(original, after);
}
)hlsl";

struct Point {
  UINT x, y, z;
};
// the first and the last three are inside the 2D texture, the first inside the array too
static constexpr Point points[] = {
    {5, 7, 1},           {64, 0, 0},          {0, 64, 0},          {65536 + 9, 3, 0},   {11, 65536 + 13, 0},
    {65536 + 17, 65536 + 19, 0}, {65535, 21, 0}, {4096 + 25, 27, 0},  {16384 + 29, 31, 0}, {0x80000000u | 33, 35, 0},
    {0xffffffffu, 37, 0}, {39, 0xffffffffu, 0}, {41, 43, slices},    {45, 47, 65536 + 1}, {49, 51, 0xffffffffu},
};
static constexpr UINT count = sizeof(points) / sizeof(points[0]);

static bool
inside(UINT kind, const Point &p) {
  return p.x < size && p.y < size && (kind == 0 || p.z < slices);
}

static UINT
marker(UINT kind, UINT i) {
  return 0x40000000u | (kind << 8) | i;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::vector<std::string> defines{"POINTS=" + std::to_string(count)};
  auto fill = compiler.compile(hlsl, "fill", "cs", defines), poke = compiler.compile(hlsl, "poke", "cs", defines);
  if (!expect(!fill.empty() && !poke.empty(), "shaders compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2};
  D3D12_ROOT_PARAMETER params[4] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV},
                                    {D3D12_ROOT_PARAMETER_TYPE_SRV}};
  params[0].Constants = {0, 0, 1};
  params[1].DescriptorTable = {1, &range};
  params[2].Descriptor = {2, 0};
  params[3].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {4, params});
  ComPtr<ID3D12PipelineState> fill_pso, poke_pso;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(fill)};
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&fill_pso)));
  pso_desc.CS = bytecode(poke);
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&poke_pso)));

  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                           D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> plain, array;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&plain)));
  auto array_desc = desc;
  array_desc.DepthOrArraySize = slices;
  CHECK(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &array_desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&array)));
  // each slice of each texture at its own footprint in one readback buffer
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[1 + slices];
  UINT64 array_bytes, plain_bytes;
  device->GetCopyableFootprints(&desc, 0, 1, 0, footprints, nullptr, nullptr, &plain_bytes);
  plain_bytes = (plain_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT *
                D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  device->GetCopyableFootprints(&array_desc, 0, slices, plain_bytes, footprints + 1, nullptr, nullptr, &array_bytes);
  auto texels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, plain_bytes + array_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto got = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 2 * count * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto got_back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 2 * count * 8, D3D12_RESOURCE_STATE_COPY_DEST);
  auto point_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(points), D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(texels && got && got_back && point_buffer, "buffers are made"))
    return verdict();
  void *mapped;
  CHECK(point_buffer->Map(0, nullptr, &mapped));
  memcpy(mapped, points, sizeof(points));
  point_buffer->Unmap(0, nullptr);
  CHECK(forget(texels.Get()));
  CHECK(forget(got_back.Get()));

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateUnorderedAccessView(plain.Get(), nullptr, nullptr, cpu);
  cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_UNORDERED_ACCESS_VIEW_DESC array_view{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2DARRAY};
  array_view.Texture2DArray = {0, 0, slices, 0};
  device->CreateUnorderedAccessView(array.Get(), nullptr, &array_view, cpu);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), fill_pso.Get(),
                                  IID_PPV_ARGS(&list)));
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(2, got->GetGPUVirtualAddress());
  list->SetComputeRootShaderResourceView(3, point_buffer->GetGPUVirtualAddress());
  list->Dispatch(size / 8, size / 8, slices);
  D3D12_RESOURCE_BARRIER uav{D3D12_RESOURCE_BARRIER_TYPE_UAV};
  list->ResourceBarrier(1, &uav);
  list->SetPipelineState(poke_pso.Get());
  for (UINT kind : {0u, 1u}) {
    list->SetComputeRoot32BitConstants(0, 1, &kind, 0);
    list->Dispatch(count, 1, 1);
    list->ResourceBarrier(1, &uav);
  }
  transition(list.Get(), plain.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), array.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  transition(list.Get(), got.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT s = 0; s < 1 + slices; s++) {
    D3D12_TEXTURE_COPY_LOCATION dst{texels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = footprints[s];
    D3D12_TEXTURE_COPY_LOCATION src{s ? array.Get() : plain.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = s ? s - 1 : 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  list->CopyResource(got_back.Get(), got.Get());
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  const UINT(*results)[2];
  const BYTE *bytes;
  CHECK(got_back->Map(0, nullptr, (void **)&results));
  CHECK(texels->Map(0, nullptr, (void **)&bytes));
  for (UINT kind : {0u, 1u}) {
    step("the %s", kind ? "2D array" : "2D texture");
    UINT wrong = 0;
    // what the point's own thread saw: inside, its marker and the sum; outside, a load of 0
    for (UINT i = 0; i < count; i++) {
      auto &r = results[kind * count + i];
      bool in = inside(kind, points[i]);
      UINT want = in ? marker(kind, i) + 1 : 0;
      if (r[1] != want || (in && r[0] != marker(kind, i))) {
        if (wrong++ < 4)
          printf("  point %u (%#x, %#x, %#x): the atomic returned %#x and the load read %#x, expected %s%#x\n", i,
                 points[i].x, points[i].y, points[i].z, r[0], r[1], in ? "the marker and " : "a load of ", want);
      }
    }
    expect(!wrong, "%u of %u points' own reads are wrong", wrong, count);
    // every texel: its seed, but the points inside, which hold their marker plus 1
    UINT changed = 0;
    for (UINT s = 0; s < (kind ? slices : 1); s++) {
      auto &f = footprints[kind ? 1 + s : 0];
      for (UINT y = 0; y < size; y++)
        for (UINT x = 0; x < size; x++) {
          UINT want = kind ? 0x10000 + x + 64 * y + 4096 * s : 1 + x + 64 * y;
          for (UINT i = 0; i < count; i++)
            if (inside(kind, points[i]) && points[i].x == x && points[i].y == y && (!kind || points[i].z == s))
              want = marker(kind, i) + 1;
          UINT have = *(const UINT *)(bytes + f.Offset + y * f.Footprint.RowPitch + x * 4);
          if (have != want && changed++ < 4)
            printf("  texel (%u, %u, %u) holds %#x, expected %#x\n", x, y, s, have, want);
        }
    }
    expect(!changed, "%u texels hold what no store inside the texture put there", changed);
  }
  texels->Unmap(0, nullptr);
  got_back->Unmap(0, nullptr);
  return verdict();
}
