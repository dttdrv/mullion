// contract: DispatchMesh runs a mesh shader's threadgroups, each setting its output counts and writing vertices,
// indices and per-primitive values that reach the pixel shader: an interpolated and a constant vertex attribute, a
// per-primitive attribute, SV_PrimitiveID as the mesh shader set it, and no pixels from a primitive it culled
// (SV_CullPrimitive) or whose vertices' SV_ClipDistance are negative; SV_RenderTargetArrayIndex chooses the slice of
// an array target per primitive, and a line mesh shader's two-vertex primitives rasterize as lines. with an amplification shader, DispatchMesh runs its threadgroups, and each launches the mesh
// threadgroups it asks for with its payload (D3D12 mesh shader specification). every mesh threadgroup draws triangles
// that each cover one pixel of its own, so the target shows which ran, with what IDs and payload, and nothing else.
// ExecuteIndirect with a DISPATCH_MESH argument does the same from a buffer; it is drawn `shift` rows lower.
#include "d3d12_test.hpp"
#include <map>

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; float weight : WEIGHT; nointerpolation uint value : VALUE; float clip : SV_ClipDistance; };
struct P { uint tag : TAG; uint id : SV_PrimitiveID; bool culled : SV_CullPrimitive; uint slice : SV_RenderTargetArrayIndex; };
// vertex `corner` of a triangle that covers the center of pixel (x, y) and no other
float4 corner(uint x, uint y, uint corner) {
  float2 at = float2(x, y) + float2(corner == 1 ? 1.4 : 0, corner == 2 ? 1.4 : 0);
  return float4(at / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
}
// group g: thread t draws a triangle on pixel (2g + t, 0); every third group culls its second, group CLIPPED's are
// clipped away, and group SLICED's go to slice 1
[numthreads(2, 1, 1)] [outputtopology("triangle")]
void ms(uint g : SV_GroupID, uint t : SV_GroupThreadID, out vertices V v[6], out indices uint3 i[2],
        out primitives P p[2]) {
  SetMeshOutputCounts(6, 2);
  for (uint c = 0; c < 3; c++) {
    v[t * 3 + c].pos = corner(2 * g + t, 0, c);
    v[t * 3 + c].weight = 3;
    v[t * 3 + c].value = g + 1;
    v[t * 3 + c].clip = g == CLIPPED ? -1 : 1;
  }
  i[t] = uint3(t * 3, t * 3 + 1, t * 3 + 2);
  p[t].tag = t + 5;
  p[t].id = g * 2 + t + 40;
  p[t].culled = t == 1 && g % 3 == 0;
  p[t].slice = g == SLICED;
}
// a line along row LINE_ROW, from the center of pixel 1 to the center of pixel 1 + LINE_PIXELS, which it leaves out
[numthreads(1, 1, 1)] [outputtopology("line")]
void ms_line(out vertices V v[2], out indices uint2 i[1], out primitives P p[1]) {
  SetMeshOutputCounts(2, 1);
  for (uint c = 0; c < 2; c++) {
    v[c].pos = float4((float2(1.5 + c * LINE_PIXELS, LINE_ROW + 0.5)) / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
    v[c].weight = 3;
    v[c].value = 77;
    v[c].clip = 1;
  }
  i[0] = uint2(0, 1);
  p[0].tag = 2;
  p[0].id = 3;
  p[0].culled = false;
  p[0].slice = 0;
}
struct Payload { uint row; uint first; uint pad[30]; };
// group a asks for a + 1 mesh threadgroups on row a + 2
[numthreads(1, 1, 1)]
void as(uint a : SV_GroupID) {
  Payload payload;
  payload.row = a + 2;
  payload.first = 100 * (a + 1);
  for (uint k = 0; k < 30; k++)
    payload.pad[k] = k;
  DispatchMesh(a + 1, 1, 1, payload);
}
[numthreads(1, 1, 1)] [outputtopology("triangle")]
void ms_payload(uint g : SV_GroupID, in payload Payload payload, out vertices V v[3], out indices uint3 i[1],
                out primitives P p[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint c = 0; c < 3; c++) {
    v[c].pos = corner(g, payload.row, c);
    v[c].weight = 3;
    v[c].value = payload.first + g + payload.pad[29];
    v[c].clip = 1;
  }
  i[0] = uint3(0, 1, 2);
  p[0].tag = 1;
  p[0].id = 0;
  p[0].culled = false;
  p[0].slice = 0;
}
uint ps(V v, uint tag : TAG, uint id : SV_PrimitiveID) : SV_Target {
  return v.value + (uint)v.weight * 1000 + (tag << 16) + (id << 24);
}
)hlsl";

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T> struct alignas(void *) Subobject {
  D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
  T data{};
};

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const UINT size = 16, groups = 7, amplified = 3, clipped = 4, sliced = 5, slices = 2, line_row = 14, line_pixels = 3;
  std::vector<std::string> defines = {
      "SIZE=" + std::to_string(size), "CLIPPED=" + std::to_string(clipped), "SLICED=" + std::to_string(sliced),
      "LINE_ROW=" + std::to_string(line_row), "LINE_PIXELS=" + std::to_string(line_pixels)
  };
  auto ms_line = compiler.compile(hlsl, "ms_line", "ms_6_5", defines);
  auto ms = compiler.compile(hlsl, "ms", "ms_6_5", defines), as = compiler.compile(hlsl, "as", "as_6_5", defines),
       ms_payload = compiler.compile(hlsl, "ms_payload", "ms_6_5", defines), ps = compiler.compile(hlsl, "ps", "ps_6_5", defines);
  if (ms.empty() || ms_line.empty() || as.empty() || ms_payload.empty() || ps.empty()) {
    printf("skipped: mesh shaders are shader model 6.5\n");
    return 77;
  }
  ComPtr<ID3D12Device2> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)));
  if (options.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
    printf("failed: mesh shaders are not supported\n");
    return 1;
  }
  auto rs = root_signature(device.Get(), {});

  struct {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature *> rs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
  } stream;
  stream.rs.data = rs.Get();
  stream.ms.data = bytecode(ms);
  stream.ps.data = bytecode(ps);
  stream.raster.data = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  stream.raster.data.DepthClipEnable = TRUE;
  stream.targets.data.NumRenderTargets = 1;
  stream.targets.data.RTFormats[0] = DXGI_FORMAT_R32_UINT;
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  ComPtr<ID3D12PipelineState> mesh_pso, amplified_pso, line_pso;
  CHECK(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&mesh_pso)));
  stream.ms.data = bytecode(ms_line);
  CHECK(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&line_pso)));
  stream.as.data = bytecode(as);
  stream.ms.data = bytecode(ms_payload);
  CHECK(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&amplified_pso)));

  // what the shaders draw
  const UINT untouched = ~0u;
  auto pixel = [](UINT value, UINT tag, UINT id) { return value + 3 * 1000 + (tag << 16) + (id << 24); };
  // by (x, y + size * slice)
  std::map<std::pair<UINT, UINT>, UINT> want;
  for (UINT g = 0; g < groups; g++)
    for (UINT t = 0; t < 2; t++)
      if (!(t == 1 && g % 3 == 0) && g != clipped)
        want[{2 * g + t, size * (g == sliced)}] = pixel(g + 1, t + 5, g * 2 + t + 40);
  for (UINT x = 1; x < 1 + line_pixels; x++)
    want[{x, line_row}] = pixel(77, 2, 3);
  const UINT shift = 8;
  for (UINT a = 0; a < amplified; a++)
    for (UINT g = 0; g < a + 1; g++)
      want[{g, a + 2}] = want[{g, a + 2 + shift}] = pixel(100 * (a + 1) + g + 29, 1, 0);
  ComPtr<ID3D12CommandSignature> signature;
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_MESH_ARGUMENTS), 1, &argument};
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_DISPATCH_MESH_ARGUMENTS), D3D12_RESOURCE_STATE_GENERIC_READ);
  D3D12_DISPATCH_MESH_ARGUMENTS *mapped_arguments;
  CHECK(arguments->Map(0, nullptr, (void **)&mapped_arguments));
  *mapped_arguments = {amplified, 1, 1};

  const UINT64 row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, slices, 1, DXGI_FORMAT_R32_UINT, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, row * size * slices, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList6> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), mesh_pso.Get(), IID_PPV_ARGS(&list)));
  const float clear[4] = {(float)untouched, 0, 0, 0};
  list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  list->SetGraphicsRootSignature(rs.Get());
  D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
  D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->DispatchMesh(groups, 1, 1);
  list->SetPipelineState(line_pso.Get());
  list->DispatchMesh(1, 1, 1);
  list->SetPipelineState(amplified_pso.Get());
  list->DispatchMesh(amplified, 1, 1);
  viewport.TopLeftY = shift;
  list->RSSetViewports(1, &viewport);
  list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT},
      src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  for (UINT slice = 0; slice < slices; slice++) {
    dst.PlacedFootprint = {row * size * slice, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
    src.SubresourceIndex = slice;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  uint8_t *out;
  CHECK(readback->Map(0, nullptr, (void **)&out));

  unsigned failures = 0;
  for (UINT y = 0; y < size * slices; y++)
    for (UINT x = 0; x < size; x++) {
      auto value = reinterpret_cast<const UINT *>(out + row * y)[x];
      auto it = want.find({x, y});
      auto expected = it == want.end() ? untouched : it->second;
      if (value != expected && failures++ < 12)
        printf("pixel %u,%u: %#x, want %#x\n", x, y, value, expected);
    }
  if (failures) {
    printf("failed: %u wrong pixels\n", failures);
    return 1;
  }
  printf("passed: %zu pixels drawn of %u\n", want.size(), size * size * slices);
  return 0;
}
