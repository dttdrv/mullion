// contract: overlapping pixel shader invocations see preceding primitives' ROV writes in submission order.
// "ROVs guarantee the order of UAV accesses for any pair of overlapping pixel shader invocations" and reads
// "must reflect writes by a previous invocation" (Microsoft Learn, Rasterizer-ordered views, Overview:
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/rasterizer-order-views).
// root slots with different stage visibility may overlap registers (Microsoft Learn, Creating a Root Signature,
// Shader Visibility: "different bindings per shader stage using an overlapping namespace").
// each instance records its preceding ROV values and the vertex stage's separate binding. raw and structured
// buffers use roots, separate and mixed tables, or SM 6.6's heap; a texture uses a table. three constants precede
// them, so parameter indices differ from argument field indices. spaces, visibility, order and instance counts vary.
#include "d3d12_test.hpp"
#include <array>

static const char hlsl[] = R"hlsl(
struct V {
  float4 pos : SV_Position;
  nointerpolation uint instance : INSTANCE;
  nointerpolation uint vertex_value : VALUE;
};
#ifdef VERTEX_SHADER
RWByteAddressBuffer vertex_values : register(u2, SPACE);
V vs(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  V v;
  v.pos = float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
  v.instance = instance;
  v.vertex_value = MARKER;
  return v;
}
V vs_split(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  V v = vs(id, instance);
  v.vertex_value = vertex_values.Load(0);
  return v;
}
#else
RasterizerOrderedByteAddressBuffer raw_values : register(u2, SPACE);
RasterizerOrderedStructuredBuffer<uint> structured_values : register(u5, SPACE);
RasterizerOrderedTexture2D<uint> texels : register(u9, SPACE);
RWStructuredBuffer<uint3> observed : register(u7, SPACE);
cbuffer constants : register(b0, SPACE) { uint first, second, third; };
void ps(V v) {
  uint at = uint(v.pos.y) * WIDTH + uint(v.pos.x);
  uint a = raw_values.Load(at * WORD_BYTES);
  uint b = structured_values[at];
  observed[v.instance * WIDTH * HEIGHT + at] = uint3(a, b, v.vertex_value);
  raw_values.Store(at * WORD_BYTES, v.instance + first + second + third);
  structured_values[at] = v.instance + first + second + third;
}
void ps_texture(V v) {
  uint2 coord = uint2(v.pos.xy);
  uint at = coord.y * WIDTH + coord.x;
  uint value = texels[coord];
  observed[v.instance * WIDTH * HEIGHT + at] = uint3(value, value, v.vertex_value);
  texels[coord] = v.instance + first + second + third;
}
#ifdef DIRECT_HEAP
void ps_heap(V v) {
  RasterizerOrderedByteAddressBuffer raw = ResourceDescriptorHeap[RAW_DESCRIPTOR];
  RasterizerOrderedStructuredBuffer<uint> structured = ResourceDescriptorHeap[STRUCTURED_DESCRIPTOR];
  uint at = uint(v.pos.y) * WIDTH + uint(v.pos.x);
  observed[v.instance * WIDTH * HEIGHT + at] = uint3(raw.Load(at * WORD_BYTES), structured[at], v.vertex_value);
  raw.Store(at * WORD_BYTES, v.instance + first + second + third);
  structured[at] = v.instance + first + second + third;
}
#endif
#endif
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT width = 17, height = 19;
  const UINT counts[] = {1, 2, 257};
  const UINT pixels = width * height, value_bytes = pixels * sizeof(UINT);
  using Observation = std::array<UINT, 3>;
  const UINT output_bytes = pixels * counts[std::size(counts) - 1] * sizeof(Observation);
  const UINT marker = ~UINT(0);
  std::array<std::string, 2> vs, vs_split, ps, ps_texture, ps_heap;
  for (UINT space = 0; space < vs.size(); space++) {
    std::vector<std::string> defines = {
        "SPACE=space" + std::to_string(space), "WIDTH=" + std::to_string(width),
        "HEIGHT=" + std::to_string(height), "WORD_BYTES=" + std::to_string(sizeof(UINT)),
        "MARKER=" + std::to_string(marker) + "u", "VERTEX_SHADER=1"
    };
    vs[space] = compiler.compile(hlsl, "vs", "vs", defines);
    vs_split[space] = compiler.compile(hlsl, "vs_split", "vs", defines);
    defines.pop_back();
    ps[space] = compiler.compile(hlsl, "ps", "ps", defines);
    ps_texture[space] = compiler.compile(hlsl, "ps_texture", "ps", defines);
    if (!expect(!vs[space].empty() && !vs_split[space].empty() && !ps[space].empty() && !ps_texture[space].empty(),
                "space %u: HLSL did not compile", space))
      return verdict();
    if (compiler.dxc) {
      defines.insert(defines.end(), {"DIRECT_HEAP=1", "RAW_DESCRIPTOR=0", "STRUCTURED_DESCRIPTOR=1"});
      ps_heap[space] = compiler.compile(hlsl, "ps_heap", "ps_6_6", defines);
      if (!expect(!ps_heap[space].empty(), "space %u: heap HLSL did not compile", space))
        return verdict();
    }
  }

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (!options.ROVsSupported) {
    printf("skipped: no rasterizer ordered views\n");
    return 77;
  }
  ComPtr<ID3D12Resource> values[2];
  for (auto &value : values) {
    value = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, value_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!expect(!!value, "a ROV buffer could not be made"))
      return verdict();
  }
  auto vertex_values = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, value_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto markers = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, value_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!vertex_values && !!markers, "the vertex stage buffers could not be made"))
    return verdict();
  UINT *words;
  CHECK(markers->Map(0, nullptr, (void **)&words));
  for (UINT at = 0; at < pixels; at++)
    words[at] = marker;
  markers->Unmap(0, nullptr);
  ComPtr<ID3D12Resource> texture;
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  const D3D12_RESOURCE_DESC texture_desc{
      D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0},
      D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  };
  CHECK(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&texture)));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 texture_bytes;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &texture_bytes);
  auto texture_zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, texture_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(!!texture_zeros, "the texture upload buffer could not be made"))
    return verdict();
  void *texture_data;
  CHECK(texture_zeros->Map(0, nullptr, &texture_data));
  memset(texture_data, 0, texture_bytes);
  texture_zeros->Unmap(0, nullptr);
  ComPtr<ID3D12DescriptorHeap> heap;
  const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(std::size(values) + 2), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
  };
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT descriptor_bytes = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_UNORDERED_ACCESS_VIEW_DESC raw{DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER};
  raw.Buffer.NumElements = pixels;
  raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(values[0].Get(), nullptr, &raw, handle);
  handle.ptr += descriptor_bytes;
  D3D12_UNORDERED_ACCESS_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER};
  structured.Buffer.NumElements = pixels;
  structured.Buffer.StructureByteStride = sizeof(UINT);
  device->CreateUnorderedAccessView(values[1].Get(), nullptr, &structured, handle);
  handle.ptr += descriptor_bytes;
  device->CreateUnorderedAccessView(texture.Get(), nullptr, nullptr, handle);
  auto observed = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, output_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(!!observed && !!zeros && !!readback, "the output, upload and readback buffers could not be made"))
    return verdict();
  handle.ptr += descriptor_bytes;
  structured.Buffer.NumElements = output_bytes / sizeof(Observation);
  structured.Buffer.StructureByteStride = sizeof(Observation);
  device->CreateUnorderedAccessView(observed.Get(), nullptr, &structured, handle);
  void *data;
  CHECK(zeros->Map(0, nullptr, &data));
  memset(data, 0, output_bytes);
  zeros->Unmap(0, nullptr);
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());

  for (UINT space = 0; space < vs.size(); space++)
    for (auto visibility : {D3D12_SHADER_VISIBILITY_ALL, D3D12_SHADER_VISIBILITY_PIXEL})
      for (bool reverse : {false, true})
        for (UINT mode = 0; mode < (compiler.dxc ? 5u : 4u); mode++) {
          const UINT registers[] = {2, 5, 7};
          const bool split = visibility == D3D12_SHADER_VISIBILITY_PIXEL;
          const bool mixed = mode == 3;
          const bool direct = mode == 4;
          D3D12_ROOT_PARAMETER params[1 + std::size(registers) + 1]{};
          const UINT vertex_param = mixed || direct ? 2 : std::size(params) - 1;
          const UINT constants[] = {0, 0, 1};
          params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
          params[0].Constants = {0, space, UINT(std::size(constants))};
          D3D12_DESCRIPTOR_RANGE ranges[std::size(registers)]{};
          for (UINT i = 0; !direct && i < std::size(registers); i++) {
            const UINT which = reverse ? std::size(registers) - 1 - i : i;
            auto &param = params[i + 1];
            param.ShaderVisibility = visibility;
            if (mixed) {
              ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, registers[which], space,
                           UINT(which < std::size(values) ? which : std::size(values) + 1)};
              continue;
            }
            if (mode && which < std::size(values)) {
              ranges[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1,
                           mode == 2 && which == 1 ? 9 : registers[which], space};
              param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
              param.DescriptorTable = {1, &ranges[i]};
            } else {
              param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
              param.Descriptor = {registers[which], space};
            }
          }
          if (mixed) {
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable = {UINT(std::size(ranges)), ranges};
          }
          if (direct) {
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[1].Descriptor = {registers[2], space};
            params[1].ShaderVisibility = visibility;
          }
          params[vertex_param].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
          params[vertex_param].Descriptor = {registers[0], space};
          params[vertex_param].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
          auto rs = root_signature(
              device.Get(), {vertex_param + UINT(split), params, 0, nullptr,
                             direct ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
                                    : D3D12_ROOT_SIGNATURE_FLAG_NONE}
          );
          if (!expect(!!rs, "the root signature could not be made"))
            return verdict();
          D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
          desc.pRootSignature = rs.Get();
          desc.VS = bytecode(split ? vs_split[space] : vs[space]);
          desc.PS = bytecode(direct ? ps_heap[space] : mode == 2 ? ps_texture[space] : ps[space]);
          desc.SampleMask = ~0u;
          desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
          desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
          desc.SampleDesc = {1, 0};
          ComPtr<ID3D12PipelineState> pipeline;
          CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
          for (UINT count : counts) {
            step("mode %u, space %u, visibility %u, reverse %u, %u overlapping instances", mode, space,
                 UINT(visibility), UINT(reverse), count);
            CHECK(forget(readback.Get()));
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), pipeline.Get()));
            list->CopyBufferRegion(vertex_values.Get(), 0, markers.Get(), 0, value_bytes);
            transition(list.Get(), vertex_values.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_TEXTURE_COPY_LOCATION from{texture_zeros.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            from.PlacedFootprint = footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (auto &value : values) {
              list->CopyBufferRegion(value.Get(), 0, zeros.Get(), 0, value_bytes);
              transition(list.Get(), value.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            list->CopyBufferRegion(observed.Get(), 0, zeros.Get(), 0, output_bytes);
            transition(list.Get(), observed.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetGraphicsRootSignature(rs.Get());
            list->SetGraphicsRoot32BitConstants(0, std::size(constants), constants, 0);
            list->SetDescriptorHeaps(1, heap.GetAddressOf());
            if (mixed)
              list->SetGraphicsRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
            if (direct)
              list->SetGraphicsRootUnorderedAccessView(1, observed->GetGPUVirtualAddress());
            ID3D12Resource *bound[] = {values[0].Get(), values[1].Get(), observed.Get()};
            for (UINT i = 0; !mixed && !direct && i < std::size(registers); i++) {
              const UINT which = reverse ? std::size(registers) - 1 - i : i;
              if (mode && which < std::size(values)) {
                auto table = heap->GetGPUDescriptorHandleForHeapStart();
                table.ptr += (mode == 2 && which == 1 ? std::size(values) : which) * descriptor_bytes;
                list->SetGraphicsRootDescriptorTable(i + 1, table);
              } else
                list->SetGraphicsRootUnorderedAccessView(i + 1, bound[which]->GetGPUVirtualAddress());
            }
            if (split)
              list->SetGraphicsRootUnorderedAccessView(vertex_param, vertex_values->GetGPUVirtualAddress());
            const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
            const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, count, 0, 0);
            transition(
                list.Get(), observed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
            );
            list->CopyResource(readback.Get(), observed.Get());
            transition(list.Get(), observed.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            for (auto &value : values)
              transition(list.Get(), value.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COPY_DEST);
            transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            transition(list.Get(), vertex_values.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            CHECK(submit(device.Get(), queue.Get(), list.Get()));
            const Observation *got;
            CHECK(readback->Map(0, nullptr, (void **)&got));
            unsigned wrong = 0;
            for (UINT instance = 0; instance < count; instance++)
              for (UINT at = 0; at < pixels; at++) {
                const auto &pair = got[instance * pixels + at];
                if ((pair[0] != instance || pair[1] != instance || pair[2] != marker) && wrong++ < 4)
                  expect(false, "instance %u, pixel %u,%u read %u,%u, vertex %#x; want %u,%u, vertex %#x", instance,
                         at % width, at / width, pair[0], pair[1], pair[2], instance, instance, marker);
              }
            expect(wrong <= 4, "and %u more wrong pairs", wrong - 4);
            readback->Unmap(0, nullptr);
          }
        }
  return verdict();
}
