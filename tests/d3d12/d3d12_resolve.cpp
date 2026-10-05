// contract: a resolve gives each destination pixel of its region one value from the source pixel's samples
// (D3D12_RESOLVE_MODE: MIN "the source samples to their minimum value" and MAX, for "any render target or depth
// stencil format"; AVERAGE "their average value", for non-integer formats "including the depth plane", which is also
// ResolveSubresource), and DECOMPRESS, between resources of one sample count, keeps each sample. the region is the
// source rect placed at DstX, DstY ("the width of the destination region is the same as the width of the source
// rect"), so pixels around it keep what they held, also when the whole source goes into a larger destination. a
// destination needs no render target flag, and a depth source may be denied to shaders. the source is the subresource
// the resolve names ("SrcSubresource"): a single-sampled texture's last mip, whose mips each hold their own value,
// decompressed whole into a larger destination, is that mip's size and value there, and the rest keeps what it held.
// each sample of each source pixel gets its own value: a pixel's samples hold the multiples of `pixels` in an order
// that turns with the pixel and slice, plus the pixel's index, drawn at sample frequency (stencil one sample and
// pixel at a time, through SV_Coverage and the stencil reference). expectations fold the same values on the CPU.
#include "d3d12_test.hpp"
#include <algorithm>
#include <bit>
#include <iterator>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint slice; uint only; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint value(float4 pos, uint s) {
  uint2 p = uint2(pos.xy);
  return ((TURN * s + p.x + 2 * p.y + slice) % SAMPLES) * PIXELS + p.x + p.y * SIZE;
}
float4 ps_float(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target { return value(pos, s) * SCALE; }
uint4 ps_uint(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target { return value(pos, s); }
int4 ps_sint(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target { return int(value(pos, s)) - BIAS; }
float ps_depth(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Depth { return value(pos, s) * SCALE; }
uint ps_stencil() : SV_Coverage { return 1u << only; }

Texture2DMS<float> kept : register(t0);
RWStructuredBuffer<float> o : register(u0);
[numthreads(SIZE, SIZE, 1)] void cs(uint2 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLES; s++)
    o[(p.y * SIZE + p.x) * SAMPLES + s] = kept.Load(p, s);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // a turn coprime to the sample count gives a pixel every multiple once
  const UINT size = 8, samples = 4, turn = 3, pixels = size * size, slices = 2, bias = 100;
  const UINT sentinel = samples * pixels - 5; // what a destination holds before, in the units of the values
  // where a resolve goes: the whole first slice into a destination of its size; a rect of the last slice that starts
  // off the origin, placed elsewhere in the last mip of a larger destination's last slice; and the whole first slice
  // into the corner of that destination's first mip
  enum Place { Whole, Region, Corner, Places };
  const char *const place_names[Places] = {"", " region", " corner"};
  const D3D12_RECT rect{2, 1, 7, 6};
  const UINT to[2] = {1, 2}, mips = 2, large = size << (mips - 1);
  auto value = [&](UINT x, UINT y, UINT s, UINT slice) {
    return ((turn * s + x + 2 * y + slice) % samples) * pixels + x + y * size;
  };
  enum Class { Float, Unorm, Uint, Sint, Depth, Stencil };
  struct Kind {
    const char *name;
    Class type;
    DXGI_FORMAT format;
    UINT plane, source;
  };
  const Kind kinds[] = {
      {"R32_FLOAT", Float, DXGI_FORMAT_R32_FLOAT, 0, 0},
      {"R8G8B8A8_UNORM", Unorm, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1},
      {"R32_UINT", Uint, DXGI_FORMAT_R32_UINT, 0, 2},
      {"R32_SINT", Sint, DXGI_FORMAT_R32_SINT, 0, 3},
      {"D32_FLOAT", Depth, DXGI_FORMAT_D32_FLOAT, 0, 4},
      {"D32_FLOAT_S8X24_UINT depth", Depth, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0, 5},
      {"D32_FLOAT_S8X24_UINT stencil", Stencil, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 1, 5},
  };
  const UINT kind_count = std::size(kinds), source_count = 6;
  auto planar = [](const Kind &k) { return k.type >= Depth; };
  // a value in a kind's format: the unorm's and depth's steps are exact in a float
  const double unorm = 255, depth_steps = samples * pixels;
  auto in_format = [&](const Kind &k, double v) {
    return k.type == Unorm ? v / unorm : k.type == Depth ? v / depth_steps : k.type == Sint ? v - bias : v;
  };
  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size), "SAMPLES=" + std::to_string(samples), "TURN=" + std::to_string(turn),
      "PIXELS=" + std::to_string(pixels), "BIAS=" + std::to_string(bias)
  };
  auto scaled = [&](double steps) {
    auto d = defines;
    d.push_back("SCALE=(1.0/" + std::to_string(steps) + ")");
    return d;
  };
  auto vs = compiler.compile(hlsl, "vs", "vs", scaled(1));
  // the pixel shader that draws each source
  const std::string ps[source_count] = {
      compiler.compile(hlsl, "ps_float", "ps", scaled(1)),     compiler.compile(hlsl, "ps_float", "ps", scaled(unorm)),
      compiler.compile(hlsl, "ps_uint", "ps", scaled(1)),      compiler.compile(hlsl, "ps_sint", "ps", scaled(1)),
      compiler.compile(hlsl, "ps_depth", "ps", scaled(depth_steps)), compiler.compile(hlsl, "ps_depth", "ps", scaled(depth_steps)),
  };
  auto ps_stencil = compiler.compile(hlsl, "ps_stencil", "ps", scaled(1)), cs = compiler.compile(hlsl, "cs", "cs", scaled(1));
  bool compiled = !vs.empty() && !ps_stencil.empty() && !cs.empty();
  for (auto &code : ps)
    compiled &= !code.empty();
  if (!compiled) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER constants{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
  constants.Constants = {0, 0, 2};
  auto draw_rs = root_signature(device.Get(), {1, &constants});
  D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER read_params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  read_params[0].DescriptorTable = {1, &srv};
  auto read_rs = root_signature(device.Get(), {2, read_params});

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  auto texture = [&](DXGI_FORMAT format, UINT width, UINT array, UINT mip_count, UINT sample_count,
                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out) {
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, width, (UINT16)array, (UINT16)mip_count,
                             format, {sample_count, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out));
  };
  auto target_flag = [&](const Kind &k) {
    return planar(k) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  };
  auto target_state = [&](const Kind &k) {
    return planar(k) ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  };
  // sources by their first kind; whole destinations, without the render target flag where D3D12 lets a texture be
  // without one, and region destinations, which the test clears first
  ComPtr<ID3D12Resource> sources[source_count], whole[kind_count], part[kind_count], decompressed;
  for (UINT k = 0; k < kind_count; k++) {
    auto &kind = kinds[k];
    // a depth source no shader of the application's may read resolves as well
    if (!sources[kind.source])
      CHECK(texture(
          kind.format, size, slices, 1, samples,
          target_flag(kind) | (planar(kind) ? D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE : D3D12_RESOURCE_FLAG_NONE),
          target_state(kind), sources[kind.source]
      ));
    CHECK(texture(
        kind.format, size, 1, 1, 1, planar(kind) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, whole[k]
    ));
    CHECK(texture(kind.format, large, slices, mips, 1, target_flag(kind), target_state(kind), part[k]));
  }
  CHECK(texture(
      kinds[0].format, size, 1, 1, samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
      decompressed
  ));

  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, source_count * slices + Places * kind_count + 2 + mips},
      dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, source_count * slices + Places * kind_count},
      srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)));
  CHECK(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap)));
  // a view of one slice of a source, and of a place's subresource of the larger destination, in the kind's heap
  auto handle = [&](const Kind &k, UINT index) {
    auto type = planar(k) ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    return D3D12_CPU_DESCRIPTOR_HANDLE{
        (planar(k) ? dsv_heap : rtv_heap)->GetCPUDescriptorHandleForHeapStart().ptr +
        index * device->GetDescriptorHandleIncrementSize(type)
    };
  };
  auto view = [&](const Kind &k, ID3D12Resource *resource, bool multisampled, UINT mip, UINT slice, UINT index) {
    auto at = handle(k, index);
    if (planar(k)) {
      D3D12_DEPTH_STENCIL_VIEW_DESC desc{k.format};
      desc.ViewDimension = multisampled ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
      if (multisampled)
        desc.Texture2DMSArray = {slice, 1};
      else
        desc.Texture2DArray = {mip, slice, 1};
      device->CreateDepthStencilView(resource, &desc, at);
    } else {
      D3D12_RENDER_TARGET_VIEW_DESC desc{k.format};
      desc.ViewDimension = multisampled ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      if (multisampled)
        desc.Texture2DMSArray = {slice, 1};
      else
        desc.Texture2DArray = {mip, slice, 1};
      device->CreateRenderTargetView(resource, &desc, at);
    }
    return at;
  };
  auto part_mip = [&](Place place) { return place == Region ? mips - 1 : 0; };
  auto part_slice = [&](Place place) { return place == Region ? slices - 1 : 0; };
  D3D12_CPU_DESCRIPTOR_HANDLE source_view[source_count][slices], part_view[kind_count][Places];
  for (UINT k = 0; k < kind_count; k++) {
    for (UINT slice = 0; slice < slices; slice++)
      source_view[kinds[k].source][slice] = view(kinds[k], sources[kinds[k].source].Get(), true, 0, slice, kinds[k].source * slices + slice);
    for (auto place : {Region, Corner})
      part_view[k][place] = view(
          kinds[k], part[k].Get(), false, part_mip(place), part_slice(place), source_count * slices + Places * k + place
      );
  }
  auto decompressed_view = handle(kinds[0], source_count * slices + Places * kind_count);
  device->CreateRenderTargetView(decompressed.Get(), nullptr, decompressed_view);
  D3D12_SHADER_RESOURCE_VIEW_DESC kept{kinds[0].format, D3D12_SRV_DIMENSION_TEXTURE2DMS};
  kept.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  device->CreateShaderResourceView(decompressed.Get(), &kept, srv_heap->GetCPUDescriptorHandleForHeapStart());

  // a source's pipeline writes every sample of its plane; stencil's writes the reference where the shader covers
  auto pipeline = [&](const Kind &k, const std::string &code, bool stencil, ComPtr<ID3D12PipelineState> &out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = draw_rs.Get();
    desc.VS = bytecode(vs);
    desc.PS = bytecode(code);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.MultisampleEnable = TRUE;
    desc.DepthStencilState.DepthEnable = planar(k) && !stencil;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = stencil;
    desc.DepthStencilState.StencilWriteMask = 0xff;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS
    };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (planar(k))
      desc.DSVFormat = k.format;
    else
      desc.NumRenderTargets = 1, desc.RTVFormats[0] = k.format;
    desc.SampleDesc = {samples, 0};
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out));
  };
  ComPtr<ID3D12PipelineState> draw[source_count], draw_stencil, read_pso;
  for (auto &kind : kinds) {
    bool stencil = kind.type == Stencil;
    CHECK(pipeline(kind, stencil ? ps_stencil : ps[kind.source], stencil, stencil ? draw_stencil : draw[kind.source]));
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC read_desc{read_rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&read_desc, IID_PPV_ARGS(&read_pso)));

  // what each resolve left, read back: a slot holds the rows of one subresource
  struct Check {
    UINT kind;
    D3D12_RESOLVE_MODE mode;
    Place place;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rows;
  };
  std::vector<Check> checks;
  const UINT kept_bytes = pixels * samples * sizeof(float);
  // the resource and subresource a kind's resolve writes in a place, and a slot that holds any's rows
  auto destination = [&](UINT k, Place place) { return (place == Whole ? whole : part)[k].Get(); };
  auto subresource = [&](const Kind &k, Place place) {
    return place == Whole ? k.plane : part_mip(place) + part_slice(place) * mips + k.plane * mips * slices;
  };
  UINT64 slot = 0;
  for (UINT k = 0; k < kind_count; k++)
    for (auto place : {Whole, Region, Corner}) {
      UINT64 bytes;
      auto desc = destination(k, place)->GetDesc();
      device->GetCopyableFootprints(&desc, subresource(kinds[k], place), 1, 0, nullptr, nullptr, nullptr, &bytes);
      const UINT64 placement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
      slot = std::max(slot, (bytes + placement - 1) / placement * placement);
    }
  const D3D12_RESOLVE_MODE modes[] = {D3D12_RESOLVE_MODE_MIN, D3D12_RESOLVE_MODE_MAX, D3D12_RESOLVE_MODE_AVERAGE};
  auto averages = [](const Kind &k) { return k.type == Float || k.type == Unorm || k.type == Depth; };
  UINT check_count = 0;
  for (auto &k : kinds)
    check_count += Places * (averages(k) ? 3 : 2);
  // the checks' slots, the kept samples, then a slot for what came of a mip
  const UINT64 mip_at = (check_count * slot + kept_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, mip_at + slot, D3D12_RESOURCE_STATE_COPY_DEST);
  auto kept_samples = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, kept_bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12GraphicsCommandList1> list1;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list.As(&list1));
  list->SetGraphicsRootSignature(draw_rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT everything{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT k = 0; k < kind_count; k++) {
    auto &kind = kinds[k];
    for (UINT slice = 0; slice < slices; slice++) {
      auto at = source_view[kind.source][slice];
      list->OMSetRenderTargets(!planar(kind), &at, FALSE, planar(kind) ? &at : nullptr);
      list->SetGraphicsRoot32BitConstant(0, slice, 0);
      if (kind.type != Stencil) {
        list->RSSetScissorRects(1, &everything);
        list->SetPipelineState(draw[kind.source].Get());
        list->DrawInstanced(3, 1, 0, 0);
        continue;
      }
      list->SetPipelineState(draw_stencil.Get());
      for (UINT p = 0; p < pixels * samples; p++) {
        LONG x = p / samples % size, y = p / samples / size;
        D3D12_RECT pixel{x, y, x + 1, y + 1};
        list->RSSetScissorRects(1, &pixel);
        list->SetGraphicsRoot32BitConstant(0, p % samples, 1);
        list->OMSetStencilRef(value(x, y, p % samples, slice));
        list->DrawInstanced(3, 1, 0, 0);
      }
    }
  }
  for (UINT k = 0; k < kind_count; k++)
    if (!kinds[k].plane)
      transition(list.Get(), sources[kinds[k].source].Get(), target_state(kinds[k]), D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  // a depth plane decompressed in place is as it was, for the resolves below
  for (auto &kind : kinds)
    if (kind.type == Depth)
      for (UINT slice = 0; slice < slices; slice++)
        list1->ResolveSubresourceRegion(
            sources[kind.source].Get(), slice, 0, 0, sources[kind.source].Get(), slice, nullptr, kind.format,
            D3D12_RESOLVE_MODE_DECOMPRESS
        );

  auto read = [&](ID3D12Resource *resource, UINT subresource) {
    auto desc = resource->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION from{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.SubresourceIndex = subresource;
    device->GetCopyableFootprints(&desc, subresource, 1, (checks.size() - 1) * slot, &into.PlacedFootprint, nullptr, nullptr, nullptr);
    checks.back().rows = into.PlacedFootprint;
    transition(list.Get(), resource, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    transition(list.Get(), resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  };
  const float before = sentinel;
  for (UINT k = 0; k < kind_count; k++) {
    auto &kind = kinds[k];
    auto source = sources[kind.source].Get();
    for (auto mode : modes) {
      if (mode == D3D12_RESOLVE_MODE_AVERAGE && !averages(kind))
        continue;
      // the first slice, whole; a color's average through ResolveSubresource
      if (mode == D3D12_RESOLVE_MODE_AVERAGE && !planar(kind))
        list->ResolveSubresource(whole[k].Get(), 0, source, 0, kind.format);
      else
        list1->ResolveSubresourceRegion(
            whole[k].Get(), subresource(kind, Whole), 0, 0, source, kind.plane * slices, nullptr, kind.format, mode
        );
      checks.push_back({k, mode, Whole});
      read(whole[k].Get(), subresource(kind, Whole));
      for (auto place : {Region, Corner}) {
        float color[4];
        std::fill_n(color, 4, (float)in_format(kind, before));
        if (planar(kind))
          list->ClearDepthStencilView(
              part_view[k][place], kind.type == Stencil ? D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH,
              in_format(kind, before), sentinel, 0, nullptr
          );
        else
          list->ClearRenderTargetView(part_view[k][place], color, 0, nullptr);
        transition(list.Get(), part[k].Get(), target_state(kind), D3D12_RESOURCE_STATE_RESOLVE_DEST);
        bool region = place == Region;
        list1->ResolveSubresourceRegion(
            part[k].Get(), subresource(kind, place), region ? to[0] : 0, region ? to[1] : 0, source,
            part_slice(place) + kind.plane * slices, region ? const_cast<D3D12_RECT *>(&rect) : nullptr, kind.format, mode
        );
        checks.push_back({k, mode, place});
        read(part[k].Get(), subresource(kind, place));
        transition(list.Get(), part[k].Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, target_state(kind));
      }
    }
  }
  // the rect's samples, each kept, in a multisampled destination that held the sentinel
  const float before_color[4] = {before, before, before, before};
  list->ClearRenderTargetView(decompressed_view, before_color, 0, nullptr);
  transition(list.Get(), decompressed.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  list1->ResolveSubresourceRegion(
      decompressed.Get(), 0, to[0], to[1], sources[0].Get(), 0, const_cast<D3D12_RECT *>(&rect), kinds[0].format,
      D3D12_RESOLVE_MODE_DECOMPRESS
  );
  transition(list.Get(), decompressed.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[] = {srv_heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(read_rs.Get());
  list->SetPipelineState(read_pso.Get());
  list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1, kept_samples->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), kept_samples.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), check_count * slot, kept_samples.Get(), 0, kept_bytes);
  // a texture whose mip m holds the sentinel and 1 + m more, and its last mip decompressed into the corner of a
  // texture of the first mip's size, which held the sentinel
  const UINT last_mip = mips - 1, last_size = size >> last_mip;
  ComPtr<ID3D12Resource> mipped, from_mip;
  CHECK(texture(kinds[0].format, size, 1, mips, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, mipped));
  CHECK(texture(kinds[0].format, size, 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, from_mip));
  auto mip_value = [&](UINT mip) { return before + 1 + mip; };
  for (UINT mip = 0; mip < mips; mip++) {
    auto at = view(kinds[0], mipped.Get(), false, mip, 0, source_count * slices + Places * kind_count + 1 + mip);
    const float holds[4] = {mip_value(mip), mip_value(mip), mip_value(mip), mip_value(mip)};
    list->ClearRenderTargetView(at, holds, 0, nullptr);
  }
  auto from_mip_view = handle(kinds[0], source_count * slices + Places * kind_count + 1 + mips);
  device->CreateRenderTargetView(from_mip.Get(), nullptr, from_mip_view);
  list->ClearRenderTargetView(from_mip_view, before_color, 0, nullptr);
  transition(list.Get(), from_mip.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST);
  transition(list.Get(), mipped.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  list1->ResolveSubresourceRegion(from_mip.Get(), 0, 0, 0, mipped.Get(), last_mip, nullptr, kinds[0].format, D3D12_RESOLVE_MODE_DECOMPRESS);
  D3D12_TEXTURE_COPY_LOCATION mip_from{from_mip.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      mip_into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  auto from_mip_desc = from_mip->GetDesc();
  device->GetCopyableFootprints(&from_mip_desc, 0, 1, mip_at, &mip_into.PlacedFootprint, nullptr, nullptr, nullptr);
  transition(list.Get(), from_mip.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyTextureRegion(&mip_into, 0, 0, 0, &mip_from, nullptr);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  if (checks.size() != check_count) {
    printf("failed: %zu resolves recorded, want %u\n", checks.size(), check_count);
    return 1;
  }

  const char *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));
  const char *const mode_names[] = {"DECOMPRESS", "MIN", "MAX", "AVERAGE"};
  unsigned failures = 0, texels = 0;
  // the source pixel a destination pixel resolves in a place, or none
  auto resolved = [&](Place place, UINT x, UINT y, UINT &from_x, UINT &from_y) {
    const D3D12_RECT all{0, 0, (LONG)size, (LONG)size}, &from = place == Region ? rect : all;
    const UINT at[2] = {place == Region ? to[0] : 0, place == Region ? to[1] : 0};
    from_x = x - at[0] + from.left, from_y = y - at[1] + from.top;
    return x >= at[0] && y >= at[1] && from_x < (UINT)from.right && from_y < (UINT)from.bottom;
  };
  for (auto &c : checks) {
    auto &kind = kinds[c.kind];
    for (UINT y = 0; y < c.rows.Footprint.Height; y++)
      for (UINT x = 0; x < c.rows.Footprint.Width; x++, texels++) {
        UINT from_x, from_y, slice = part_slice(c.place);
        double want = sentinel;
        if (resolved(c.place, x, y, from_x, from_y)) {
          double least = value(from_x, from_y, 0, slice), most = least, sum = 0;
          for (UINT s = 0; s < samples; s++) {
            double v = value(from_x, from_y, s, slice);
            least = std::min(least, v), most = std::max(most, v), sum += v;
          }
          want = c.mode == D3D12_RESOLVE_MODE_MIN ? least : c.mode == D3D12_RESOLVE_MODE_MAX ? most : sum / samples;
        }
        want = in_format(kind, want);
        auto texel = out + c.rows.Offset + y * c.rows.Footprint.RowPitch + x * (kind.type == Stencil ? 1 : 4);
        double got;
        bool same = true;
        switch (kind.type) {
        case Float:
        case Depth:
          got = *(const float *)texel;
          break;
        case Unorm:
          // every channel holds the value
          got = (UINT8)texel[0] / unorm;
          for (int channel = 1; channel < 4; channel++)
            same &= texel[channel] == texel[0];
          break;
        case Uint:
          got = *(const UINT *)texel;
          break;
        case Sint:
          got = *(const INT *)texel;
          break;
        case Stencil:
          got = (UINT8)*texel;
          break;
        }
        if ((!same || got != want) && failures++ < 16)
          printf("%s %s%s at %u,%u: %g, want %g\n", kind.name, mode_names[c.mode], place_names[c.place], x, y, got, want);
      }
  }
  auto kept_out = (const float *)(out + check_count * slot);
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++)
      for (UINT s = 0; s < samples; s++, texels++) {
        UINT from_x, from_y;
        double want = resolved(Region, x, y, from_x, from_y) ? value(from_x, from_y, s, 0) : sentinel;
        double got = kept_out[(y * size + x) * samples + s];
        if (got != want && failures++ < 16)
          printf("DECOMPRESS region at %u,%u sample %u: %g, want %g\n", x, y, s, got, want);
      }
  for (UINT y = 0; y < size; y++)
    for (UINT x = 0; x < size; x++, texels++) {
      float got = *(const float *)(out + mip_into.PlacedFootprint.Offset + y * mip_into.PlacedFootprint.Footprint.RowPitch + x * sizeof(float));
      float want = x < last_size && y < last_size ? mip_value(last_mip) : before;
      if (got != want && failures++ < 16)
        printf("DECOMPRESS of mip %u at %u,%u: %g, want %g\n", last_mip, x, y, got, want);
    }
  readback->Unmap(0, nullptr);
  printf("%s: %u wrong of %u texels in %zu resolves\n", failures ? "failed" : "passed", failures, texels, checks.size() + 2);
  return failures != 0;
}
