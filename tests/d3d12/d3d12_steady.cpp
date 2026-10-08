// contract: ordered commands reading equivalent scene data produce the same frame, and frame 0 must match the
// scene. "However observable rendering results must match results produced by serial processing of tasks."
// (D3D11.3 4.2). its exceptions are excluded here: "These types of Views explicitly allow unordered results,
// leaving the burden to applications to make careful choices of atomic instructions to access Unordered
// Transaction Views if deterministic and implementation invariant output is desired." (4.2); "If a copy involves
// writing to the same memory location multiple times because multiple locations in the destination resource are
// mapped to the same tile memory, the resulting writes to multi-mapped tiles are nondeterministic/nonrepeatable -
// accesses happen in whatever order the hardware happens to execute the copy." (5.9.3.3); "If multiple tile
// coordinates in one or more views is bound to the same memory location, reads and writes from different paths to
// the same memory will occur in a nondeterministic/nonrepeatable order of memory accesses." (5.9.4); "Observe that
// when the viewport location is fractional, which results in rounding to determine the implicit scissor, there is
// effectively a non-deterministic zone of up to 1/2 pixel wide along the left and top edges within the scissor area,
// not covered by the viewport." (15.6.1). resources here are committed, viewport edges integral, UAV words have one
// writer each, and barriers order the dispatch before its indirect draws and the two passes.
// three meshes, each at every mip and in three instances, cover texel-aligned cells. nearer meshes hide later,
// differently textured meshes. point sampling (7.18.7, 7.18.10) selects the uploaded texel at each pixel's centre;
// the second pass samples that first picture. every pixel of frame 0 is derived from those texels and the clear.
// each of 200 frames has fresh descriptor copies, a fresh constant slice and its own prefilled readback. pooled
// allocators retain recordings past an arena run, then reset only after their last frame's fence has completed.
// frame n's tint and scale are both n + 1, so tint / scale is one; another frame's tint changes the picture. two
// draws of the second pass change only the CBV, from twice that tint to that tint, overwriting the first draw.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cstddef>

static const char hlsl[] = R"hlsl(
cbuffer Frame : register(b0) { float4 tint; };
cbuffer Object : register(b1) { uint mesh; uint column; uint mip; float depth; float scale; };
Texture2D<float4> texture0 : register(t0);
SamplerState point_clamp : register(s0);
RWStructuredBuffer<uint> arguments : register(u0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(float2 corner : POSITION, float2 uv : TEXCOORD0, uint instance : SV_InstanceID) {
  V v;
  float2 pixel = (float2(column, instance) + corner) * CELL;
  v.pos = float4(pixel / float2(WIDTH, HEIGHT) * float2(2, -2) + float2(-1, 1), depth, 1);
  v.uv = uv;
  return v;
}
float4 ps(V v) : SV_Target { return texture0.SampleLevel(point_clamp, v.uv, mip) * (tint / scale); }
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {
  uint at = id.x * DRAW_WORDS;
  arguments[at] = INDICES;
  arguments[at + 1] = INSTANCES;
  arguments[at + 2] = 0;
  arguments[at + 3] = mesh * VERTICES;
  arguments[at + 4] = 0;
}
float4 vs_pass(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps_pass(float4 pos : SV_Position) : SV_Target {
  return texture0.SampleLevel(point_clamp, pos.xy / float2(WIDTH, HEIGHT), 0) * (tint / scale);
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  using Pixel = std::array<UINT8, 4>;
  struct Vertex { float x, y, u, v; };
  struct Object { UINT mesh, column, mip; float depth, scale; };
  const UINT frames = 200, pooled = 2, meshes = 3, cell = 8, instances = 3;
  UINT levels = 1;
  for (UINT side = cell; side > 1; side >>= 1)
    levels++;
  const UINT columns = meshes * levels, width = columns * cell, height = (instances + 1) * cell;
  const UINT16 indices[] = {0, 1, 2, 2, 1, 3}, adjacent[] = {0, 0, 1, 1, 2, 2, 2, 2, 1, 1, 3, 3};
  const Vertex corners[] = {{0, 0, 0, 0}, {1, 0, 1, 0}, {0, 1, 0, 1}, {1, 1, 1, 1}};
  const UINT argument_bytes = levels * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), descriptors = columns + 1;
  const UINT constant_stride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  std::array<float, 4> tints[] = {{2, 2, 2, 2}, {1, 1, 1, 1}};
  const std::vector<std::string> defines = {
      "CELL=" + std::to_string(cell), "WIDTH=" + std::to_string(width), "HEIGHT=" + std::to_string(height),
      "INSTANCES=" + std::to_string(instances), "INDICES=" + std::to_string(std::size(indices)),
      "VERTICES=" + std::to_string(std::size(corners)),
      "DRAW_WORDS=" + std::to_string(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) / sizeof(UINT))};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines), vs_pass = compiler.compile(hlsl, "vs_pass", "vs", defines),
       ps_pass = compiler.compile(hlsl, "ps_pass", "ps", defines);
  if (vs.empty() || ps.empty() || cs.empty() || vs_pass.empty() || ps_pass.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1};
  D3D12_ROOT_PARAMETER params[] = {{D3D12_ROOT_PARAMETER_TYPE_CBV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                  {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[1].Constants = {1, 0, sizeof(Object) / sizeof(UINT)};
  params[2].DescriptorTable = {1, &range};
  D3D12_STATIC_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                   D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER, sampler.MaxLOD = D3D12_FLOAT32_MAX;
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params, 1, &sampler,
                                         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  if (!expect(rs != nullptr, "root signature"))
    return verdict();
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  const D3D12_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, u)}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.RasterizerState.DepthClipEnable = TRUE;
  graphics.DepthStencilState = {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS};
  graphics.InputLayout = {elements, (UINT)std::size(elements)};
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1, graphics.RTVFormats[0] = format, graphics.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  graphics.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> scene, pass, writes;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&scene)));
  graphics.VS = bytecode(vs_pass), graphics.PS = bytecode(ps_pass);
  graphics.InputLayout = {}, graphics.DepthStencilState = {}, graphics.DSVFormat = DXGI_FORMAT_UNKNOWN;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pass)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&writes)));
  D3D12_INDIRECT_ARGUMENT_DESC indirect{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &indirect};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocators[pooled];
  ComPtr<ID3D12GraphicsCommandList> lists[pooled];
  for (UINT i = 0; i < pooled; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                  IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  CHECK(lists[0]->Reset(allocators[0].Get(), nullptr));
  auto list = lists[0].Get();
  ComPtr<ID3D12DescriptorHeap> sources, views, rtvs, dsvs;
  D3D12_DESCRIPTOR_HEAP_DESC source_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, meshes + pooled},
      view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, frames * descriptors,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * pooled}, dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, pooled};
  CHECK(device->CreateDescriptorHeap(&source_desc, IID_PPV_ARGS(&sources)));
  CHECK(device->CreateDescriptorHeap(&view_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  const UINT view_step = device->GetDescriptorHandleIncrementSize(source_desc.Type),
             rtv_step = device->GetDescriptorHandleIncrementSize(rtv_desc.Type),
             dsv_step = device->GetDescriptorHandleIncrementSize(dsv_desc.Type);
  auto source = [&](UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{sources->GetCPUDescriptorHandleForHeapStart().ptr + i * view_step};
  };
  auto view = [&](UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{views->GetCPUDescriptorHandleForHeapStart().ptr + i * view_step};
  };
  auto gpu_view = [&](UINT i) {
    return D3D12_GPU_DESCRIPTOR_HANDLE{views->GetGPUDescriptorHandleForHeapStart().ptr + i * view_step};
  };
  auto rtv = [&](UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{rtvs->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_step};
  };
  auto dsv = [&](UINT i) {
    return D3D12_CPU_DESCRIPTOR_HANDLE{dsvs->GetCPUDescriptorHandleForHeapStart().ptr + i * dsv_step};
  };

  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, format, {1, 0},
                                 D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> targets[2 * pooled], depths[pooled], textures[meshes];
  for (UINT i = 0; i < std::size(targets); i++) {
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&targets[i])));
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv(i));
  }
  auto depth_desc = target_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT, depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  for (UINT i = 0; i < pooled; i++) {
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depths[i])));
    device->CreateDepthStencilView(depths[i].Get(), nullptr, dsv(i));
  }
  std::vector<std::vector<Pixel>> texels(meshes * levels);
  std::vector<ComPtr<ID3D12Resource>> uploads;
  for (UINT mesh = 0; mesh < meshes; mesh++) {
    auto desc = target_desc;
    desc.Width = desc.Height = cell, desc.MipLevels = levels, desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&textures[mesh])));
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels);
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, levels, 0, footprints.data(), nullptr, nullptr, &bytes);
    auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!expect(upload != nullptr, "texture upload"))
      return verdict();
    char *data;
    CHECK(upload->Map(0, nullptr, (void **)&data));
    for (UINT mip = 0; mip < levels; mip++) {
      auto &f = footprints[mip];
      auto &pixels = texels[mesh * levels + mip];
      pixels.resize(f.Footprint.Width * f.Footprint.Height);
      for (UINT y = 0; y < f.Footprint.Height; y++)
        for (UINT x = 0; x < f.Footprint.Width; x++) {
          auto &pixel = pixels[y * f.Footprint.Width + x];
          pixel = {UINT8((mesh + 1) * 32 + mip + 1), UINT8(x * 17 + 1), UINT8(y * 19 + 1), 255};
          memcpy(data + f.Offset + y * f.Footprint.RowPitch + x * sizeof(Pixel), pixel.data(), sizeof(Pixel));
        }
      D3D12_TEXTURE_COPY_LOCATION from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = f}},
          to{textures[mesh].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}};
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    upload->Unmap(0, nullptr);
    uploads.push_back(upload);
    transition(list, textures[mesh].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }
  auto srv = [&](ID3D12Resource *resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{
        format, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    desc.Texture2D.MipLevels = resource->GetDesc().MipLevels;
    device->CreateShaderResourceView(resource, &desc, handle);
  };
  for (UINT i = 0; i < meshes + pooled; i++)
    srv(i < meshes ? textures[i].Get() : targets[2 * (i - meshes)].Get(), source(i));
  // a GPU descriptor copy that kept its old words reads a different, valid texture
  for (UINT frame = 0; frame < frames; frame++)
    for (UINT i = 0; i < descriptors; i++)
      srv(textures[(i / levels + 1) % meshes].Get(), view(frame * descriptors + i));

  auto vertices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, meshes * sizeof(corners),
                         D3D12_RESOURCE_STATE_GENERIC_READ),
       index_buffer = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(indices) + sizeof(adjacent),
                             D3D12_RESOURCE_STATE_GENERIC_READ),
       constants = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (frames + 1) * std::size(tints) * constant_stride,
                          D3D12_RESOURCE_STATE_GENERIC_READ),
       zeros = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (frames + 1) * argument_bytes,
                      D3D12_RESOURCE_STATE_GENERIC_READ),
       arguments = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, (frames + 1) * argument_bytes,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!expect(vertices && index_buffer && constants && zeros && arguments, "scene buffers"))
    return verdict();
  Vertex *vertex;
  char *index, *constant, *zero;
  CHECK(vertices->Map(0, nullptr, (void **)&vertex));
  CHECK(index_buffer->Map(0, nullptr, (void **)&index));
  CHECK(constants->Map(0, nullptr, (void **)&constant));
  CHECK(zeros->Map(0, nullptr, (void **)&zero));
  for (UINT mesh = 0; mesh < meshes; mesh++)
    for (UINT i = 0; i < std::size(corners); i++) {
      auto v = corners[i];
      if (mesh & 1)
        v.u = 1 - v.u;
      if (mesh & 2)
        v.v = 1 - v.v;
      vertex[mesh * std::size(corners) + i] = v;
    }
  memcpy(index, indices, sizeof(indices)), memcpy(index + sizeof(indices), adjacent, sizeof(adjacent));
  memset(constant, 0, constants->GetDesc().Width), memset(zero, 0, zeros->GetDesc().Width);
  // the preceding slices hold frame 0 too, so a stale slice first becomes wrong in frame 1
  for (UINT i = 0; i < std::size(tints); i++)
    memcpy(constant + i * constant_stride, tints[i].data(), sizeof(tints[i]));
  vertices->Unmap(0, nullptr), index_buffer->Unmap(0, nullptr), zeros->Unmap(0, nullptr);
  list->CopyResource(arguments.Get(), zeros.Get());
  transition(list, arguments.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  CHECK(list->Close());
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  std::vector<ComPtr<ID3D12Resource>> readbacks(frames);
  for (auto &readback : readbacks) {
    readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(readback != nullptr, "frame readback"))
      return verdict();
    CHECK(forget(readback.Get()));
  }
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(event != nullptr, "fence event"))
    return verdict();
  auto wait = [&](UINT64 value) {
    auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
      return expect(false, "device removed at fence %llu: %08lx", value, device->GetDeviceRemovedReason());
    if (completed >= value)
      return true;
    HRESULT hr = fence->SetEventOnCompletion(value, event);
    DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, INFINITE) : WAIT_FAILED;
    completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
      return expect(false, "device removed at fence %llu: %08lx", value, device->GetDeviceRemovedReason());
    return expect(result == WAIT_OBJECT_0 && completed >= value,
                  "fence %llu did not complete: HRESULT %08lx, wait %lu", value, hr, result);
  };
  ID3D12CommandList *setup[] = {list};
  queue->ExecuteCommandLists(1, setup);
  CHECK(queue->Signal(fence.Get(), 1));
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  const auto addresses = std::count_if(std::begin(params), std::end(params), [](auto &p) {
    return p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  });
  // every draw uploads at least its root constants and addresses (allocator's EncodeRootArgument)
  const UINT retained = system.dwAllocationGranularity /
                            ((2 * columns + std::size(tints) + 1) *
                             (sizeof(Object) + addresses * sizeof(D3D12_GPU_VIRTUAL_ADDRESS))) + 1;
  UINT used[pooled] = {};
  UINT64 last[pooled] = {};
  last[0] = 1;
  const D3D12_VERTEX_BUFFER_VIEW vbv{vertices->GetGPUVirtualAddress(), UINT(vertices->GetDesc().Width), sizeof(Vertex)};
  const D3D12_INDEX_BUFFER_VIEW ibv{
      index_buffer->GetGPUVirtualAddress(), UINT(index_buffer->GetDesc().Width), DXGI_FORMAT_R16_UINT};
  const D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
  const D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
  const float clear[4] = {};
  for (UINT frame = 0; frame < frames; frame++) {
    UINT pool = frame % pooled, base = frame * descriptors;
    step("frame %u of %u, allocator %u (%u retained recordings)", frame, frames, pool, used[pool]);
    if (used[pool] == retained) {
      if (!wait(last[pool]))
        ExitProcess(1);
      CHECK(allocators[pool]->Reset());
      used[pool] = 0;
    }
    list = lists[pool].Get();
    CHECK(list->Reset(allocators[pool].Get(), nullptr));
    used[pool]++;
    UINT64 constant_offset = (frame + 1) * std::size(tints) * constant_stride,
           argument_offset = (frame + 1) * argument_bytes;
    const float scale = frame + 1;
    for (UINT i = 0; i < std::size(tints); i++) {
      tints[i].fill((std::size(tints) - i) * scale);
      memcpy(constant + constant_offset + i * constant_stride, tints[i].data(), sizeof(tints[i]));
    }
    for (UINT column = 0; column < columns; column++)
      device->CopyDescriptorsSimple(1, view(base + column), source(column / levels), source_desc.Type);
    device->CopyDescriptorsSimple(1, view(base + columns), source(meshes + pool), source_desc.Type);
    transition(list, arguments.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(writes.Get());
    list->SetComputeRootSignature(rs.Get());
    const Object written{meshes - 1, 0, 0, 0, scale};
    list->SetComputeRoot32BitConstants(1, sizeof(written) / sizeof(UINT), &written, 0);
    list->SetComputeRootUnorderedAccessView(3, arguments->GetGPUVirtualAddress() + argument_offset);
    list->Dispatch(levels, 1, 1);
    transition(list, arguments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto near_rtv = rtv(2 * pool), final_rtv = rtv(2 * pool + 1), depth = dsv(pool);
    list->ClearRenderTargetView(near_rtv, clear, 0, nullptr);
    list->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    list->OMSetRenderTargets(1, &near_rtv, FALSE, &depth);
    list->RSSetViewports(1, &viewport), list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootConstantBufferView(
        0, constants->GetGPUVirtualAddress() + constant_offset + (std::size(tints) - 1) * constant_stride);
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->IASetVertexBuffers(0, 1, &vbv), list->IASetIndexBuffer(&ibv);
    list->SetPipelineState(scene.Get());
    for (bool behind : {false, true})
      for (UINT column = 0; column < columns; column++) {
        Object object{column / levels, column, column % levels, behind ? 0.75f : 0.25f, scale};
        UINT sampled_column = behind ? ((object.mesh + 1) % meshes) * levels + object.mip : column;
        list->SetGraphicsRoot32BitConstants(1, sizeof(object) / sizeof(UINT), &object, 0);
        list->SetGraphicsRootDescriptorTable(2, gpu_view(base + sampled_column));
        bool adjacency = object.mesh == 1;
        list->IASetPrimitiveTopology(
            adjacency ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (!behind && object.mesh == meshes - 1)
          list->ExecuteIndirect(
              signature.Get(), 1, arguments.Get(),
              argument_offset + object.mip * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), nullptr, 0);
        else
          list->DrawIndexedInstanced(adjacency ? std::size(adjacent) : std::size(indices), instances,
                                     adjacency ? std::size(indices) : 0, object.mesh * std::size(corners), 0);
      }
    transition(list, targets[2 * pool].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ClearRenderTargetView(final_rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &final_rtv, FALSE, nullptr);
    list->SetPipelineState(pass.Get());
    list->SetGraphicsRootDescriptorTable(2, gpu_view(base + columns));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT i = 0; i < std::size(tints); i++) {
      list->SetGraphicsRootConstantBufferView(
          0, constants->GetGPUVirtualAddress() + constant_offset + i * constant_stride);
      list->DrawInstanced(3, 1, 0, 0);
    }
    transition(list, targets[2 * pool].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list, targets[2 * pool + 1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{targets[2 * pool + 1].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
        to{readbacks[frame].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, targets[2 * pool + 1].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(list->Close());
    ID3D12CommandList *recorded[] = {list};
    queue->ExecuteCommandLists(1, recorded);
    CHECK(queue->Signal(fence.Get(), last[pool] = frame + 2));
  }
  step("every frame compared with frame 0, and frame 0 with the scene's texels");
  if (!wait(frames + 1))
    ExitProcess(1);
  CHECK(device->GetDeviceRemovedReason());
  constants->Unmap(0, nullptr);
  // row padding is not part of the frame: CopyTextureRegion leaves those bytes unspecified
  std::vector<Pixel> first(width * height);
  UINT different_frames = 0, different_pixels = 0, wrong_first = 0, untouched_frames = 0;
  for (UINT frame = 0; frame < frames; frame++) {
    const char *data;
    CHECK(readbacks[frame]->Map(0, nullptr, (void **)&data));
    UINT changed = 0, untouched = 0;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        Pixel got;
        memcpy(got.data(), data + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(Pixel),
               sizeof(Pixel));
        untouched += std::all_of(got.begin(), got.end(), [](UINT8 b) { return b == 0xff; });
        auto &reference = first[y * width + x];
        if (!frame) {
          reference = got;
          Pixel want{};
          if (y < instances * cell) {
            UINT mesh = x / cell / levels, mip = x / cell % levels, side = cell >> mip;
            UINT u = x % cell * side / cell, v = y % cell * side / cell;
            if (mesh & 1)
              u = side - 1 - u;
            if (mesh & 2)
              v = side - 1 - v;
            want = texels[mesh * levels + mip][v * side + u];
          }
          if (got != want && !wrong_first++)
            expect(false, "frame 0 pixel %u,%u: %u %u %u %u, want %u %u %u %u", x, y,
                   got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        } else if (got != reference) {
          if (!different_pixels && !changed)
            expect(false, "first difference: frame %u pixel %u,%u: %u %u %u %u, frame 0: %u %u %u %u", frame, x, y,
                   got[0], got[1], got[2], got[3], reference[0], reference[1], reference[2], reference[3]);
          changed++;
        }
      }
    readbacks[frame]->Unmap(0, nullptr);
    different_frames += changed != 0, different_pixels += changed;
    if (untouched == width * height) {
      if (!untouched_frames++)
        expect(false, "frame %u was not copied: every pixel still holds the readback's fill bytes", frame);
    }
  }
  printf("%u/%u frames differ from frame 0, %u pixels differ; %u wrong pixels in frame 0; %u untouched frames\n",
         different_frames, frames - 1, different_pixels, wrong_first, untouched_frames);
  CloseHandle(event);
  return verdict();
}
