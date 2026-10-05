// contract: SetPredication skips draws, indexed draws, ExecuteIndirect, dispatches, copies, tile copies, resolves and clears when
// its buffer's 64-bit value meets the op (EQUAL_ZERO: zero; NOT_EQUAL_ZERO: not zero), and runs them otherwise
// (Direct3D 12 "Predication": the operations that honor it). each case leaves its own value in its own pixel column,
// buffer element or small resource; the list runs twice with other predicate values, so each run must decide anew.
// the expectations apply the op to the values. what the list does before its first SetPredication, the resets of
// everything the cases write, runs whatever the predicates are, and ClearState ends predication: a dispatch after it
// runs too.
#include "d3d12_test.hpp"
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint value; uint column; };
RWStructuredBuffer<uint> o : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps() : SV_Target { return value; }
[numthreads(1, 1, 1)] void cs() { o[column] = value; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto vs = compiler.compile(hlsl, "vs", "vs"), ps = compiler.compile(hlsl, "ps", "ps"),
       cs = compiler.compile(hlsl, "cs", "cs");
  if (vs.empty() || ps.empty() || cs.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  enum Kind {
    Draw,
    DrawIndexed,
    Indirect,
    Dispatch,
    CopyBuffer,
    CopyTexture,
    CopyAll,
    ClearTarget,
    ClearDepth,
    ClearUnorderedUint,
    ClearUnorderedFloat,
    Resolve,
    ResolveRegion,
    Tiles,
    Kinds
  };
  const char *const names[Kinds] = {"Draw", "DrawIndexed", "ExecuteIndirect", "Dispatch", "CopyBufferRegion",
                                    "CopyTextureRegion", "CopyResource", "ClearRenderTargetView",
                                    "ClearDepthStencilView", "ClearUnorderedAccessViewUint",
                                    "ClearUnorderedAccessViewFloat", "ResolveSubresource", "ResolveSubresourceRegion",
                                    "CopyTiles"};
  const D3D12_PREDICATION_OP ops[] = {D3D12_PREDICATION_OP_EQUAL_ZERO, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO};
  const UINT per_kind = 2 * 2;         // op, which of the two predicate values
  const UINT cases = Kinds * per_kind; // a case is a kind's `c % per_kind`-th
  const UINT cleared = cases;          // the element the dispatch after ClearState writes
  const UINT64 runs[2][2] = {{0, 5}, {9, 0}};
  // what a whole-resource copy brings, a cleared depth, the depth before, and a resolved white pixel
  const UINT marker = 0xc0ffee, white = 0xffffffff;
  const float depth_cleared = 0.5f, depth_before = 1;

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(vs), bytecode(ps)};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> draw_pso, compute_pso;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draw_pso)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&compute_pso)));
  D3D12_INDIRECT_ARGUMENT_DESC draw_arg{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
  D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw_arg};
  ComPtr<ID3D12CommandSignature> sig;
  CHECK(device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&sig)));

  // the upload buffer: the two predicate values, a triangle's indices, one draw's arguments, a zero predicate, the
  // marker, every case's value, which a copy takes, and zeros for what is reset by a copy
  const UINT indices_at = 64, args_at = 128, zero_at = 192, marker_at = 224, values_at = 256,
             zeros_at = values_at + cases * 4, elements = cases + 1, own_size = 16;
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, zeros_at + elements * 4, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  memset(mapped, 0, zeros_at + elements * 4);
  const uint16_t indices[3] = {0, 1, 2};
  const D3D12_DRAW_ARGUMENTS args{3, 1, 0, 0};
  memcpy(mapped + indices_at, indices, sizeof(indices));
  memcpy(mapped + args_at, &args, sizeof(args));
  for (UINT i = 0; i < own_size / 4; i++)
    memcpy(mapped + marker_at + i * 4, &marker, 4);
  for (UINT c = 0; c < cases; c++) {
    UINT value = c + 1;
    memcpy(mapped + values_at + c * 4, &value, 4);
  }
  auto va = upload->GetGPUVirtualAddress();

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](UINT width, DXGI_FORMAT format, UINT samples, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, 1, 1, 1, format, {samples, 0},
                          D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ComPtr<ID3D12Resource> out;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out));
    return out;
  };
  const auto UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RT = D3D12_RESOURCE_STATE_RENDER_TARGET,
             DEST = D3D12_RESOURCE_STATE_COPY_DEST, SOURCE = D3D12_RESOURCE_STATE_COPY_SOURCE,
             RESOLVE_DEST = D3D12_RESOURCE_STATE_RESOLVE_DEST;
  const auto color = DXGI_FORMAT_R8G8B8A8_UNORM;
  // what the cases write: the dispatches' and buffer copies' elements, the draws' and texture copies' and target
  // clears' columns, the depth clears' columns, the unordered clears' elements, and for each whole-resource copy and
  // resolve a resource of its own
  auto out = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, elements * 4, UA, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto target = texture(cases, DXGI_FORMAT_R32_UINT, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, RT);
  auto depth = texture(cases, DXGI_FORMAT_D32_FLOAT, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  auto unordered = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, cases * 4, UA, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto unordered_float = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, cases * 4, UA, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto region = texture(cases, color, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, RESOLVE_DEST);
  ComPtr<ID3D12Resource> own[per_kind], resolved[per_kind];
  for (UINT i = 0; i < per_kind; i++) {
    own[i] = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, own_size, DEST);
    resolved[i] = texture(1, color, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, RESOLVE_DEST);
  }
  // a reserved buffer with a mapped tile for each tile copy, and the linear tile they bring, which starts with the marker
  const UINT tile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  D3D12_RESOURCE_DESC tiled_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, per_kind * tile, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  ComPtr<ID3D12Resource> tiled;
  CHECK(device->CreateReservedResource(&tiled_desc, DEST, nullptr, IID_PPV_ARGS(&tiled)));
  ComPtr<ID3D12Heap> tiles;
  D3D12_HEAP_DESC tiles_desc{per_kind * tile, {D3D12_HEAP_TYPE_DEFAULT}};
  CHECK(device->CreateHeap(&tiles_desc, IID_PPV_ARGS(&tiles)));
  auto linear = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, tile, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *linear_mapped;
  CHECK(linear->Map(0, nullptr, &linear_mapped));
  memset(linear_mapped, 0, tile);
  memcpy(linear_mapped, &marker, 4);
  // what copies and resolves take: every case's value in its column, the marker, and a white multisampled pixel
  auto values = texture(cases, DXGI_FORMAT_R32_UINT, 1, D3D12_RESOURCE_FLAG_NONE, DEST);
  auto own_source = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, own_size, DEST);
  auto multisampled = texture(1, color, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, RT);
  if (!target || !depth || !region || !values || !multisampled) {
    printf("failed: textures\n");
    return 1;
  }

  // render target views: the target, the multisampled pixel, the region and each resolve's own; a depth view; and
  // the unordered buffers' views
  enum { TARGET, MULTISAMPLED, REGION, RESOLVED };
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs, uavs;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RESOLVED + per_kind};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsvs)));
  heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&uavs)));
  auto at = [&](ID3D12DescriptorHeap *h, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT i) {
    auto handle = h->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += i * device->GetDescriptorHandleIncrementSize(type);
    return handle;
  };
  auto rtv_of = [&](UINT i) { return at(rtvs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, i); };
  device->CreateRenderTargetView(target.Get(), nullptr, rtv_of(TARGET));
  device->CreateRenderTargetView(multisampled.Get(), nullptr, rtv_of(MULTISAMPLED));
  device->CreateRenderTargetView(region.Get(), nullptr, rtv_of(REGION));
  for (UINT i = 0; i < per_kind; i++)
    device->CreateRenderTargetView(resolved[i].Get(), nullptr, rtv_of(RESOLVED + i));
  auto rtv = rtv_of(TARGET), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
  ID3D12Resource *const unordered_buffers[2] = {unordered.Get(), unordered_float.Get()};
  D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu[2];
  D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu[2];
  for (UINT i = 0; i < 2; i++) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER};
    view.Buffer.NumElements = cases;
    uav_cpu[i] = at(uavs.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, i);
    uav_gpu[i] = uavs->GetGPUDescriptorHandleForHeapStart();
    uav_gpu[i].ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateUnorderedAccessView(unordered_buffers[i], nullptr, &view, uav_cpu[i]);
  }

  // the readback: rows of the target, the depth and the region, each resolve's own pixel, then the buffers
  const UINT pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, placement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  const UINT target_at = 0, depth_at = placement, region_at = 2 * placement, resolved_at = 3 * placement,
             out_at = resolved_at + per_kind * placement, unordered_at = out_at + elements * 4,
             unordered_float_at = unordered_at + cases * 4, own_at = unordered_float_at + cases * 4,
             tiled_at = own_at + per_kind * own_size, readback_size = tiled_at + per_kind * 4;
  static_assert(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT >= Kinds * 4 * 4, "a row of cases fits a placement");
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, readback_size, DEST);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  const D3D12_TILED_RESOURCE_COORDINATE first_tile{};
  const D3D12_TILE_REGION_SIZE all_tiles{per_kind};
  const D3D12_TILE_RANGE_FLAGS mapped_range = D3D12_TILE_RANGE_FLAG_NONE;
  const UINT heap_start = 0;
  queue->UpdateTileMappings(
      tiled.Get(), 1, &first_tile, &all_tiles, tiles.Get(), 1, &mapped_range, &heap_start, &all_tiles.NumTiles,
      D3D12_TILE_MAPPING_FLAG_NONE
  );
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), draw_pso.Get(), IID_PPV_ARGS(&list)));
  auto row = [&](ID3D12Resource *buffer_of, UINT offset, DXGI_FORMAT format, UINT width) {
    D3D12_TEXTURE_COPY_LOCATION location{buffer_of, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    location.PlacedFootprint = {offset, {format, width, 1, 1, pitch}};
    return location;
  };
  auto whole = [](ID3D12Resource *of) { return D3D12_TEXTURE_COPY_LOCATION{of, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; };

  // once: the sources
  {
    auto to = whole(values.Get()), from = row(upload.Get(), values_at, DXGI_FORMAT_R32_UINT, cases);
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), values.Get(), DEST, SOURCE);
    list->CopyBufferRegion(own_source.Get(), 0, upload.Get(), marker_at, own_size);
    transition(list.Get(), own_source.Get(), DEST, SOURCE);
    const float ones[4] = {1, 1, 1, 1};
    list->ClearRenderTargetView(rtv_of(MULTISAMPLED), ones, 0, nullptr);
    transition(list.Get(), multisampled.Get(), RT, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), draw_pso.Get()));
  }

  // every run starts from nothing written, before any predicate is set
  const float zero[4] = {};
  const UINT zeros[4] = {};
  list->ClearRenderTargetView(rtv, zero, 0, nullptr);
  list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth_before, 0, 0, nullptr);
  ID3D12DescriptorHeap *heaps[] = {uavs.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->ClearUnorderedAccessViewUint(uav_gpu[0], uav_cpu[0], unordered.Get(), zeros, 0, nullptr);
  list->ClearUnorderedAccessViewFloat(uav_gpu[1], uav_cpu[1], unordered_float.Get(), zero, 0, nullptr);
  transition(list.Get(), out.Get(), UA, DEST);
  list->CopyBufferRegion(out.Get(), 0, upload.Get(), zeros_at, elements * 4);
  transition(list.Get(), out.Get(), DEST, UA);
  transition(list.Get(), region.Get(), RESOLVE_DEST, RT);
  list->ClearRenderTargetView(rtv_of(REGION), zero, 0, nullptr);
  transition(list.Get(), region.Get(), RT, RESOLVE_DEST);
  for (UINT i = 0; i < per_kind; i++) {
    list->CopyBufferRegion(own[i].Get(), 0, upload.Get(), zeros_at, own_size);
    list->CopyBufferRegion(tiled.Get(), i * tile, upload.Get(), zeros_at, 4);
    transition(list.Get(), resolved[i].Get(), RESOLVE_DEST, RT);
    list->ClearRenderTargetView(rtv_of(RESOLVED + i), zero, 0, nullptr);
    transition(list.Get(), resolved[i].Get(), RT, RESOLVE_DEST);
  }

  list->SetComputeRootSignature(rs.Get());
  list->SetGraphicsRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0, 0, (float)cases, 1, 0, 1};
  list->RSSetViewports(1, &viewport);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_INDEX_BUFFER_VIEW ib{va + indices_at, sizeof(indices), DXGI_FORMAT_R16_UINT};
  list->IASetIndexBuffer(&ib);
  for (UINT c = 0; c < cases; c++) {
    auto kind = Kind(c / per_kind);
    const UINT constants[2] = {c + 1, c}, mine = c % per_kind;
    const float value[4] = {float(c + 1)};
    const UINT uint_value[4] = {c + 1};
    D3D12_RECT column{(LONG)c, 0, (LONG)c + 1, 1};
    D3D12_BOX box{c, 0, 0, c + 1, 1, 1};
    // resource states change whatever the predicate says, so they are set around the predicated operation
    if (kind == CopyBuffer)
      transition(list.Get(), out.Get(), UA, DEST);
    if (kind == CopyTexture)
      transition(list.Get(), target.Get(), RT, DEST);
    list->SetPredication(upload.Get(), (c & 1) * 8, ops[(c >> 1) & 1]);
    switch (kind) {
    case Dispatch:
      list->SetPipelineState(compute_pso.Get());
      list->SetComputeRoot32BitConstants(0, 2, constants, 0);
      list->Dispatch(1, 1, 1);
      break;
    case Draw:
    case DrawIndexed:
    case Indirect:
      list->SetPipelineState(draw_pso.Get());
      list->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
      list->RSSetScissorRects(1, &column);
      if (kind == Draw)
        list->DrawInstanced(3, 1, 0, 0);
      else if (kind == DrawIndexed)
        list->DrawIndexedInstanced(3, 1, 0, 0, 0);
      else
        list->ExecuteIndirect(sig.Get(), 1, upload.Get(), args_at, nullptr, 0);
      break;
    case CopyBuffer:
      list->CopyBufferRegion(out.Get(), c * 4, upload.Get(), values_at + c * 4, 4);
      break;
    case CopyTexture: {
      auto to = whole(target.Get()), from = whole(values.Get());
      list->CopyTextureRegion(&to, c, 0, 0, &from, &box);
      break;
    }
    case CopyAll:
      list->CopyResource(own[mine].Get(), own_source.Get());
      break;
    case ClearTarget:
      list->ClearRenderTargetView(rtv, value, 1, &column);
      break;
    case ClearDepth:
      list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth_cleared, 0, 1, &column);
      break;
    case ClearUnorderedUint:
      list->ClearUnorderedAccessViewUint(uav_gpu[0], uav_cpu[0], unordered.Get(), uint_value, 1, &column);
      break;
    case ClearUnorderedFloat:
      list->ClearUnorderedAccessViewFloat(uav_gpu[1], uav_cpu[1], unordered_float.Get(), value, 1, &column);
      break;
    case Resolve:
      list->ResolveSubresource(resolved[mine].Get(), 0, multisampled.Get(), 0, color);
      break;
    case ResolveRegion: {
      ComPtr<ID3D12GraphicsCommandList1> list1;
      CHECK(list.As(&list1));
      D3D12_RECT pixel{0, 0, 1, 1};
      list1->ResolveSubresourceRegion(region.Get(), 0, c, 0, multisampled.Get(), 0, &pixel, color, D3D12_RESOLVE_MODE_AVERAGE);
      break;
    }
    case Tiles: {
      const D3D12_TILED_RESOURCE_COORDINATE at{mine};
      const D3D12_TILE_REGION_SIZE one{1};
      list->CopyTiles(tiled.Get(), &at, &one, linear.Get(), 0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
      break;
    }
    default:
      break;
    }
    list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    if (kind == CopyBuffer)
      transition(list.Get(), out.Get(), DEST, UA);
    if (kind == CopyTexture)
      transition(list.Get(), target.Get(), DEST, RT);
  }
  // a predicate that skips in every run (the zeros at `zero_at`), then ClearState
  list->SetPredication(upload.Get(), zero_at, D3D12_PREDICATION_OP_EQUAL_ZERO);
  list->ClearState(compute_pso.Get());
  const UINT after_clear[2] = {cleared + 1, cleared};
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
  list->SetComputeRoot32BitConstants(0, 2, after_clear, 0);
  list->Dispatch(1, 1, 1);

  // read everything back, and leave the states as the list found them
  auto read_row = [&](ID3D12Resource *of, D3D12_RESOURCE_STATES state, UINT offset, DXGI_FORMAT format, UINT width) {
    transition(list.Get(), of, state, SOURCE);
    auto to = row(readback.Get(), offset, format, width), from = whole(of);
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), of, SOURCE, state);
  };
  auto read_buffer = [&](ID3D12Resource *of, D3D12_RESOURCE_STATES state, UINT offset, UINT size) {
    transition(list.Get(), of, state, SOURCE);
    list->CopyBufferRegion(readback.Get(), offset, of, 0, size);
    transition(list.Get(), of, SOURCE, state);
  };
  read_row(target.Get(), RT, target_at, DXGI_FORMAT_R32_UINT, cases);
  read_row(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, depth_at, DXGI_FORMAT_R32_FLOAT, cases);
  read_row(region.Get(), RESOLVE_DEST, region_at, color, cases);
  for (UINT i = 0; i < per_kind; i++) {
    read_row(resolved[i].Get(), RESOLVE_DEST, resolved_at + i * placement, color, 1);
    read_buffer(own[i].Get(), DEST, own_at + i * own_size, own_size);
  }
  transition(list.Get(), tiled.Get(), DEST, SOURCE);
  for (UINT i = 0; i < per_kind; i++)
    list->CopyBufferRegion(readback.Get(), tiled_at + i * 4, tiled.Get(), i * tile, 4);
  transition(list.Get(), tiled.Get(), SOURCE, DEST);
  read_buffer(out.Get(), UA, out_at, elements * 4);
  read_buffer(unordered.Get(), UA, unordered_at, cases * 4);
  read_buffer(unordered_float.Get(), UA, unordered_float_at, cases * 4);
  CHECK(list->Close());

  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  unsigned failures = 0;
  UINT64 done = 0;
  for (auto &values_of_run : runs) {
    memcpy(mapped, values_of_run, sizeof(values_of_run));
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    CHECK(queue->Signal(fence.Get(), ++done));
    CHECK(fence->SetEventOnCompletion(done, nullptr));
    const uint8_t *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    auto word = [&](UINT offset) {
      UINT value;
      memcpy(&value, got + offset, 4);
      return value;
    };
    auto real = [&](UINT offset) {
      float value;
      memcpy(&value, got + offset, 4);
      return value;
    };
    for (UINT c = 0; c < cases; c++) {
      bool skip = (values_of_run[c & 1] == 0) == (ops[(c >> 1) & 1] == D3D12_PREDICATION_OP_EQUAL_ZERO);
      auto kind = Kind(c / per_kind);
      const UINT mine = c % per_kind;
      // what the case left, and what a run and a skip leave
      double value, ran = c + 1, skipped = 0;
      switch (kind) {
      case Dispatch:
      case CopyBuffer:
        value = word(out_at + c * 4);
        break;
      case CopyAll:
        value = word(own_at + mine * own_size);
        ran = marker;
        break;
      case Tiles:
        value = word(tiled_at + mine * 4);
        ran = marker;
        break;
      case ClearDepth:
        value = real(depth_at + c * 4);
        ran = depth_cleared;
        skipped = depth_before;
        break;
      case ClearUnorderedUint:
        value = word(unordered_at + c * 4);
        break;
      case ClearUnorderedFloat:
        value = real(unordered_float_at + c * 4);
        break;
      case Resolve:
        value = word(resolved_at + mine * placement);
        ran = white;
        break;
      case ResolveRegion:
        value = word(region_at + c * 4);
        ran = white;
        break;
      default:
        value = word(target_at + c * 4);
        break;
      }
      double want = skip ? skipped : ran;
      if (value != want && failures++ < 16)
        printf("predicate %llu, %s (case %u): %g, want %g\n", values_of_run[c & 1], names[kind], c, value, want);
    }
    UINT after = word(out_at + cleared * 4);
    if (after != cleared + 1 && failures++ < 16)
      printf("predicate %llu: the dispatch after ClearState wrote %u, want %u\n", values_of_run[0], after, cleared + 1);
    readback->Unmap(0, nullptr);
  }
  printf("%s: %u wrong cases over %zu runs\n", failures ? "failed" : "passed", failures, std::size(runs));
  return failures != 0;
}
