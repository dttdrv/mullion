// contract: a descriptor table may hold descriptors of resources that have been released, as long as no shader that
// runs reads through them. "the application is only responsible for initializing the descriptor heap with valid
// descriptors in areas that will actually be referenced during shader execution" (DirectX-Specs, Resource Binding,
// "Setting Descriptor Tables in the Root Arguments"; for shader resource views from resource binding tier 2), and
// it has "to make sure that it does not use an SRV descriptor when the underlying D3D resource it depends on has
// been destroyed" (the same, "Descriptors"): use is what is forbidden, not the descriptor's being there. engines
// leave such descriptors in the tables they bind: Unreal copies a material's views from the heap they were made in
// into the bound heap for each draw, the views of textures it has streamed out among them.
// a table of four shader resource views: a texture and a buffer that stay, and a texture and a buffer that are
// released, once placed on heaps that stay and once committed. the two that go are SIDE texels a side and SIDE
// squared elements, and what is read is their last texel and element. the resource of slot s holds s + 1 there. a
// pixel shader
// and a compute shader give what they read through each slot: the two that stay always, the two that go only when
// a root constant says so, which no compiler can know. before each draw and dispatch the table is copied afresh
// from the heap the views were made in. held to:
// - all alive: every slot's word when all are read, and 0 for the two that are not when they are not (the controls);
// - two released, not read: the words of the two that stay, from the draw and from the dispatch, and the device lives;
// - null descriptors written over the released ones, all read: 0 for them, since an unbound view reads as 0
//   ("SRVs which return default values", the same, "NULL Descriptors").
// GONE=1 (argv[2]) reads through the released descriptors instead, which Direct3D leaves undefined (on Windows the
// memory of a heap that lives is still there and old bytes are read): the test prints what was read and holds only
// that the device lives and the next draw and dispatch are right. COMMITTED=n takes the placed (0) or the committed
// (1) resources alone, and SIDE=n makes the two that go as large as memory that is given back when they are
// released. GONE is not part of the suite: a GPU that faults takes the rest of the process's work with it.
// the arguments after the front end are NAME=VALUE.
#include "d3d12_test.hpp"
#include <array>
#include <map>

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint all; uint side; };
Texture2D<uint> texture_stays : register(t0);
Texture2D<uint> texture_goes : register(t1);
StructuredBuffer<uint> buffer_stays : register(t2);
StructuredBuffer<uint> buffer_goes : register(t3);
uint4 read() {
  uint4 got = uint4(texture_stays.Load(int3(0, 0, 0)), 0, buffer_stays[0], 0);
  if (all) {
    got.y = texture_goes.Load(int3(side - 1, side - 1, 0));
    got.w = buffer_goes[side * side - 1];
  }
  return got;
}
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2(id & 1, id >> 1) * 4 - 1, 0, 1); }
uint4 ps() : SV_Target { return read(); }
RWStructuredBuffer<uint4> result : register(u0);
[numthreads(1, 1, 1)] void cs() { result[0] = read(); }
)hlsl";

// the slots in the order of the shader's registers and of what it gives: textures first, and every second one goes
static const UINT slots = 4;
using Words = std::array<UINT, slots>;
static bool
is_texture(UINT slot) {
  return slot < 2;
}
static bool
goes(UINT slot) {
  return slot % 2 == 1;
}

// the going resources, of `side` texels a side and that many squared elements, placed on heaps that stay or
// committed; `gone` reads through their descriptors. false when the device wants every descriptor of a table valid,
// and nothing was run
static bool
released(const Compiler &compiler, bool placed, bool gone, UINT side) {
  const char *how = placed ? "placed on heaps that stay" : "committed";
  step("a texture and a buffer %s", how);
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"), cs = compiler.compile(hlsl, "cs", "cs");
  Words every, staying;
  for (UINT slot = 0; slot < slots; slot++)
    every[slot] = slot + 1, staying[slot] = goes(slot) ? 0 : slot + 1;

  ComPtr<ID3D12Device> device;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  if (!expect(!vs.empty() && !ps.empty() && !cs.empty() && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
                  SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))),
              "no shaders or no device"))
    return true;
  if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
    return false;
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slots};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  params[1].Constants = {0, 0, 2};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
  const DXGI_FORMAT format = DXGI_FORMAT_R32_UINT, target_format = DXGI_FORMAT_R32G32B32A32_UINT;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = target_format;
  desc.SampleDesc = {1, 0};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  // the views are made in a heap no shader sees and copied into the one that is bound
  D3D12_DESCRIPTOR_HEAP_DESC made_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots}, rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1},
      table_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12PipelineState> draws, dispatches;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12DescriptorHeap> made, table, rtv_heap;
  if (!expect(SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draws))) &&
                  SUCCEEDED(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&dispatches))) &&
                  SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) &&
                  SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                  SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
                  SUCCEEDED(device->CreateDescriptorHeap(&made_desc, IID_PPV_ARGS(&made))) &&
                  SUCCEEDED(device->CreateDescriptorHeap(&table_desc, IID_PPV_ARGS(&table))) &&
                  SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap))),
              "no pipelines, queue or heaps"))
    return true;
  const UINT step_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto view_at = [&](UINT slot) {
    auto handle = made->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += slot * step_size;
    return handle;
  };
  auto view_of = [&](UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{is_texture(slot) ? format : DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    if (is_texture(slot))
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, view.Texture2D = {0, 1};
    else
      view.Buffer = {0, goes(slot) ? side * side : 1, sizeof(UINT)};
    return view;
  };

  // one word a slot, each where a copy to a texture may begin
  const UINT64 apart = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto staged = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, slots * apart, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *words;
  if (!expect(SUCCEEDED(staged->Map(0, nullptr, (void **)&words)), "no staging buffer"))
    return true;
  const D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, format, {1, 0}},
      buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, sizeof(UINT), 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Heap> heaps[slots];
  ComPtr<ID3D12Resource> resources[slots];
  for (UINT slot = 0; slot < slots; slot++) {
    // where the slot's word lies: at the last texel or element
    const UINT last = goes(slot) ? side - 1 : 0, elements = goes(slot) ? side * side : 1;
    auto resource_desc = is_texture(slot) ? texture_desc : buffer_desc;
    if (is_texture(slot))
      resource_desc.Width = resource_desc.Height = last + 1;
    else
      resource_desc.Width = UINT64(elements) * sizeof(UINT);
    HRESULT created;
    if (goes(slot) && placed) {
      auto needs = device->GetResourceAllocationInfo(0, 1, &resource_desc);
      D3D12_HEAP_DESC heap_desc{needs.SizeInBytes, gpu, needs.Alignment,
                                is_texture(slot) ? D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES : D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
      created = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heaps[slot]));
      if (SUCCEEDED(created))
        created = device->CreatePlacedResource(heaps[slot].Get(), 0, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resources[slot]));
    } else
      created = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resources[slot]));
    if (!expect(created == S_OK, "the resource of slot %u: %08lx", slot, created))
      return true;
    memcpy(words + slot * apart, &every[slot], sizeof(UINT));
    if (is_texture(slot)) {
      D3D12_TEXTURE_COPY_LOCATION to{resources[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{staged.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      device->GetCopyableFootprints(&texture_desc, 0, 1, slot * apart, &from.PlacedFootprint, nullptr, nullptr, nullptr);
      list->CopyTextureRegion(&to, last, last, 0, &from, nullptr);
    } else
      list->CopyBufferRegion(resources[slot].Get(), UINT64(elements - 1) * sizeof(UINT), staged.Get(), slot * apart, sizeof(UINT));
    transition(list.Get(), resources[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    auto view = view_of(slot);
    device->CreateShaderResourceView(resources[slot].Get(), &view, view_at(slot));
  }
  D3D12_RESOURCE_DESC target_desc = texture_desc;
  target_desc.Format = target_format, target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> target;
  if (!expect(submit(device.Get(), queue.Get(), list.Get()) == S_OK &&
                  SUCCEEDED(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                            IID_PPV_ARGS(&target))),
              "the resources were not filled or there is no target"))
    return true;
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 target_bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &target_bytes);
  auto result = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(Words), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto drawn_back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto dispatched_back = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(Words), D3D12_RESOURCE_STATE_COPY_DEST);

  // a draw and a dispatch that read through the table, the slots that go only when `all`; false when the device did
  // not live through them
  Words drawn, dispatched;
  auto reads = [&](UINT all) {
    drawn.fill(~0u), dispatched.fill(~0u);
    if (FAILED(forget(drawn_back.Get())) || FAILED(forget(dispatched_back.Get())) || FAILED(allocator->Reset()) ||
        FAILED(list->Reset(allocator.Get(), nullptr)))
      return expect(false, "the list could not be begun");
    device->CopyDescriptorsSimple(slots, table->GetCPUDescriptorHandleForHeapStart(), made->GetCPUDescriptorHandleForHeapStart(),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *bound[] = {table.Get()};
    const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    const D3D12_RECT scissor{0, 0, 1, 1};
    list->SetDescriptorHeaps(1, bound);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootDescriptorTable(0, table->GetGPUDescriptorHandleForHeapStart());
    const UINT constants[] = {all, side};
    list->SetGraphicsRoot32BitConstants(1, std::size(constants), constants, 0);
    list->SetPipelineState(draws.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, table->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, std::size(constants), constants, 0);
    list->SetComputeRootUnorderedAccessView(2, result->GetGPUVirtualAddress());
    list->SetPipelineState(dispatches.Get());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{drawn_back.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(dispatched_back.Get(), result.Get());
    transition(list.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HRESULT ran = submit(device.Get(), queue.Get(), list.Get());
    const void *got;
    for (auto [back, value] : {std::pair{drawn_back.Get(), &drawn}, std::pair{dispatched_back.Get(), &dispatched}})
      if (SUCCEEDED(back->Map(0, nullptr, (void **)&got))) {
        memcpy(value, got, sizeof(Words));
        back->Unmap(0, nullptr);
      }
    return expect(ran == S_OK, "the device did not live through it: %08lx", ran);
  };
  auto holds = [&](UINT all, const Words &want) {
    if (reads(all))
      expect(drawn == want && dispatched == want, "the draw read %u %u %u %u and the dispatch %u %u %u %u, want %u %u %u %u", drawn[0], drawn[1],
             drawn[2], drawn[3], dispatched[0], dispatched[1], dispatched[2], dispatched[3], want[0], want[1], want[2], want[3]);
  };

  step("%s: all four alive and all read", how);
  holds(1, every);
  step("%s: all four alive, the two that will go not read", how);
  holds(0, staying);
  step("%s: the two released", how);
  for (UINT slot = 0; slot < slots; slot++)
    if (goes(slot)) {
      ULONG left = resources[slot].Reset();
      expect(left == 0, "the resource of slot %u still has %lu references", slot, left);
    }
  // more than once: a library may put off what a release frees
  for (UINT round = 0; round < 3; round++) {
    step("%s: two released and not read, round %u", how, round);
    holds(0, staying);
  }
  if (gone) {
    step("%s: two released and read, undefined in Direct3D", how);
    bool lives = reads(1);
    printf("%s, through the released descriptors: the draw read %#x %#x %#x %#x and the dispatch %#x %#x %#x %#x (all alive: %u %u %u %u); the "
           "device %s\n",
           how, drawn[0], drawn[1], drawn[2], drawn[3], dispatched[0], dispatched[1], dispatched[2], dispatched[3], every[0], every[1], every[2],
           every[3], lives ? "lives" : "is removed");
    step("%s: after that, two released and not read", how);
    holds(0, staying);
    return true;
  }
  for (UINT slot = 0; slot < slots; slot++)
    if (goes(slot)) {
      auto view = view_of(slot);
      device->CreateShaderResourceView(nullptr, &view, view_at(slot));
    }
  step("%s: null descriptors in the place of the released ones, all read", how);
  holds(1, staying);
  return true;
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // both kinds, not read, of 64 texels a side
  std::map<std::string, UINT> numbers{{"GONE", 0}, {"SIDE", 64}};
  for (int i = 2; i < argc; i++)
    if (auto eq = strchr(argv[i], '='))
      numbers[std::string(argv[i], eq)] = strtoul(eq + 1, nullptr, 0);
  bool ran = false;
  for (bool placed : {true, false})
    if (!numbers.count("COMMITTED") || placed == !numbers["COMMITTED"])
      ran |= released(compiler, placed, numbers["GONE"], numbers["SIDE"]);
  if (!ran) {
    printf("skipped: resource binding tier 1 wants every descriptor of a table valid\n");
    return 77;
  }
  return verdict();
}
