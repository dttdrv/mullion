// contract: what the device reports, it does. every capability CheckFeatureSupport reports names calls and formats
// in its specification; an engine reads the report at start-up and makes those calls, and each one succeeds:
// - a feature level (D3D12_FEATURE_FEATURE_LEVELS) creates a device, as does every level below it, and has what its
//   column of the feature level table requires (Microsoft Learn, "Hardware Feature Levels"; DirectX-Specs,
//   "Direct3D feature level 12_2");
// - a shader model (D3D12_FEATURE_SHADER_MODEL) is the answer to every model asked for at or above it, a lower
//   model asked for is answered with itself (D3D12_FEATURE_DATA_SHADER_MODEL), and a compute shader of each model up
//   to it makes a pipeline;
// - command signatures (DirectX-Specs, "Indirect Drawing"): every argument type makes one. the ray dispatch comes
//   with ray tracing tier 1.1 (DirectX Raytracing, D3D12_RAYTRACING_TIER), the mesh dispatch with mesh shader tier 1
//   (Mesh Shader, "ExecuteIndirect"). what the runtime validates there is refused with E_INVALIDARG: no draw or
//   dispatch, one that is not last, an index buffer without an indexed draw, a stride that is not a multiple of four
//   or too small for the arguments, and a root signature given or left out against "if and only if the command
//   signature changes one of the root arguments";
// - query heaps: the types that need no capability (occlusion, timestamp, pipeline statistics, stream output
//   statistics) make a heap (ID3D12Device::CreateQueryHeap);
// - ray tracing tier 1.0 has ID3D12Device5 and sizes an acceleration structure of each vertex format the
//   specification lists, tier 1.1 has ID3D12Device7 and the formats it adds (D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC);
// - mesh shader tier 1 has ID3D12GraphicsCommandList6, enhanced barriers have ID3D12Device10 and
//   ID3D12GraphicsCommandList7 (Enhanced Barriers, "D3D12_FEATURE_DATA_D3D12_OPTIONS12");
// - typed UAV loads of additional formats are reported for the formats that are "supported as a set" (Microsoft
//   Learn, "Typed unordered access view (UAV) loads");
// - resource heap tier 2 has one heap for buffers, textures and render targets (D3D12_RESOURCE_HEAP_TIER), a tiled
//   resources tier has reserved resources (D3D12_TILED_RESOURCES_TIER), root signature 1.1 makes a 1.1 signature;
// - a buffer's GPU address and size lie inside the reported address bits;
// - output merger logic ops, a shader specified stencil reference, rasterizer ordered views and a render target
//   array index from the vertex shader each make a pipeline that uses them (D3D12_FEATURE_DATA_D3D12_OPTIONS).
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWBuffer<uint> out_ : register(u0);
[numthreads(1, 1, 1)] void cs() { out_[0] = 1; }
struct V { float4 pos : SV_Position; uint layer : SV_RenderTargetArrayIndex; };
V vs(uint id : SV_VertexID) { V o; o.pos = float4(id & 1, id >> 1, 0, 1); o.layer = id; return o; }
float4 vs_plain(uint id : SV_VertexID) : SV_Position { return float4(id & 1, id >> 1, 0, 1); }
uint ps() : SV_Target { return 1; }
uint ps_stencil(out uint ref : SV_StencilRef) : SV_Target { ref = 1; return 1; }
RasterizerOrderedTexture2D<uint> ordered : register(u1);
uint ps_ordered(float4 pos : SV_Position) : SV_Target { return ordered[uint2(pos.xy)]++; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
  D3D12_FEATURE_DATA_ROOT_SIGNATURE root_version{D3D_ROOT_SIGNATURE_VERSION_1_1};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  // a structure the device does not know leaves its capabilities at none
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2));
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6));
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12));
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root_version, sizeof(root_version))))
    root_version.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;

  step("shader models: every model asked for is answered with the highest one at or below it");
  D3D12_FEATURE_DATA_SHADER_MODEL highest{D3D_HIGHEST_SHADER_MODEL};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &highest, sizeof(highest)));
  const D3D_SHADER_MODEL models[] = {D3D_SHADER_MODEL_5_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_6_1,
                                     D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_4,
                                     D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_7};
  D3D12_DESCRIPTOR_RANGE uav_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
  D3D12_ROOT_PARAMETER uav_table{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
  uav_table.DescriptorTable = {1, &uav_range};
  auto uav_signature = root_signature(device.Get(), {1, &uav_table});
  for (auto model : models) {
    D3D12_FEATURE_DATA_SHADER_MODEL asked{model};
    auto want = std::min(model, highest.HighestShaderModel);
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &asked, sizeof(asked));
    expect(hr == S_OK && asked.HighestShaderModel == want, "asked for shader model %x: %08lx with %x, want %x", model,
           hr, asked.HighestShaderModel, want);
    if (model > highest.HighestShaderModel || (model == D3D_SHADER_MODEL_5_1) != !compiler.dxc)
      continue;
    step("shader model %x.%x: a compute shader of it makes a pipeline", model >> 4, model & 15);
    auto profile = "cs_" + std::to_string(model >> 4) + "_" + std::to_string(model & 15);
    auto cs = compiler.compile(hlsl, "cs", compiler.dxc ? profile.c_str() : "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{uav_signature.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> pso;
    hr = cs.empty() ? E_FAIL : device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(hr == S_OK, "CreateComputePipelineState: %08lx%s", hr, cs.empty() ? " (the shader did not compile)" : "");
  }

  step("feature levels: the highest one reported, and each below it, creates a device");
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
  D3D12_FEATURE_DATA_FEATURE_LEVELS level{(UINT)std::size(levels), levels};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &level, sizeof(level)));
  for (auto l : levels) {
    HRESULT hr = D3D12CreateDevice(nullptr, l, __uuidof(ID3D12Device), nullptr);
    // without a device to return, S_FALSE says the level is there (D3D12CreateDevice)
    expect((hr == S_FALSE) == (l <= level.MaxSupportedFeatureLevel), "feature level %x of at most %x: %08lx", l,
           level.MaxSupportedFeatureLevel, hr);
  }
  const struct {
    D3D_FEATURE_LEVEL level;
    bool has;
    const char *what;
  } required[] = {
      {D3D_FEATURE_LEVEL_12_0, options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2, "resource binding tier 2"},
      {D3D_FEATURE_LEVEL_12_0, options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2, "tiled resources tier 2"},
      {D3D_FEATURE_LEVEL_12_0, !!options.TypedUAVLoadAdditionalFormats, "typed UAV loads of additional formats"},
      {D3D_FEATURE_LEVEL_12_1, options.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_1,
       "conservative rasterization tier 1"},
      {D3D_FEATURE_LEVEL_12_1, !!options.ROVsSupported, "rasterizer ordered views"},
      {D3D_FEATURE_LEVEL_12_2, highest.HighestShaderModel >= D3D_SHADER_MODEL_6_5, "shader model 6.5"},
      {D3D_FEATURE_LEVEL_12_2, options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1, "ray tracing tier 1.1"},
      {D3D_FEATURE_LEVEL_12_2, options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2,
       "variable shading rate tier 2"},
      {D3D_FEATURE_LEVEL_12_2, options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1, "mesh shader tier 1"},
      {D3D_FEATURE_LEVEL_12_2, options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9, "sampler feedback tier 0.9"},
      {D3D_FEATURE_LEVEL_12_2, options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3, "resource binding tier 3"},
      {D3D_FEATURE_LEVEL_12_2, options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_3, "tiled resources tier 3"},
      {D3D_FEATURE_LEVEL_12_2, options.ConservativeRasterizationTier >= D3D12_CONSERVATIVE_RASTERIZATION_TIER_3,
       "conservative rasterization tier 3"},
      {D3D_FEATURE_LEVEL_12_2, root_version.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1, "root signature 1.1"},
      {D3D_FEATURE_LEVEL_12_2, !!options2.DepthBoundsTestSupported, "the depth bounds test"},
  };
  for (auto &r : required)
    expect(level.MaxSupportedFeatureLevel < r.level || r.has, "feature level %x reported without %s",
           level.MaxSupportedFeatureLevel, r.what);

  // a root signature with one parameter of each kind an indirect argument can change
  D3D12_ROOT_PARAMETER parameters[4] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_CBV},
                                        {D3D12_ROOT_PARAMETER_TYPE_SRV}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  parameters[0].Constants = {0, 0, 4};
  parameters[1].Descriptor = {1, 0};
  auto rooted = root_signature(device.Get(), {4, parameters});
  enum { Constant, Cbv, Srv, Uav };
  using Type = D3D12_INDIRECT_ARGUMENT_TYPE;
  auto argument = [](Type type, UINT parameter = 0) {
    D3D12_INDIRECT_ARGUMENT_DESC desc{type};
    if (type == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT)
      desc.Constant = {parameter, 0, 1};
    else if (type == D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW)
      desc.VertexBuffer.Slot = 0;
    else
      desc.ConstantBufferView.RootParameterIndex = parameter;
    return desc;
  };
  // the bytes of an argument in the buffer: its structure (D3D12_INDIRECT_ARGUMENT_TYPE), one value for the
  // constant above, an address for a root view
  auto bytes = [](const D3D12_INDIRECT_ARGUMENT_DESC &desc) -> UINT {
    switch (desc.Type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: return sizeof(D3D12_DRAW_ARGUMENTS);
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: return sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: return sizeof(D3D12_DISPATCH_ARGUMENTS);
    case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: return sizeof(D3D12_VERTEX_BUFFER_VIEW);
    case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: return sizeof(D3D12_INDEX_BUFFER_VIEW);
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: return sizeof(UINT);
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: return sizeof(D3D12_DISPATCH_RAYS_DESC);
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: return sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
    default: return sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
    }
  };
  auto signature = [&](const char *what, std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments, bool rooted_one,
                       HRESULT want, int stride_change = 0) {
    UINT stride = stride_change;
    for (auto &a : arguments)
      stride += bytes(a);
    step("command signature: %s, stride %u, %s a root signature", what, stride, rooted_one ? "with" : "without");
    D3D12_COMMAND_SIGNATURE_DESC desc{stride, (UINT)arguments.size(), arguments.data()};
    ComPtr<ID3D12CommandSignature> made;
    HRESULT hr = device->CreateCommandSignature(&desc, rooted_one ? rooted.Get() : nullptr, IID_PPV_ARGS(&made));
    expect(hr == want && !made == FAILED(want), "CreateCommandSignature: %08lx and %s, want %08lx", hr,
           made ? "a signature" : "none", want);
  };
  auto draw = argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW), indexed = argument(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED),
       dispatch = argument(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH),
       vertices = argument(D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW),
       indices = argument(D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW),
       constant = argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT, Constant);
  signature("a draw", {draw}, false, S_OK);
  signature("an indexed draw", {indexed}, false, S_OK);
  signature("a dispatch", {dispatch}, false, S_OK);
  signature("a vertex buffer and a draw", {vertices, draw}, false, S_OK);
  signature("an index buffer and an indexed draw", {indices, indexed}, false, S_OK);
  signature("a vertex buffer, an index buffer and an indexed draw", {vertices, indices, indexed}, false, S_OK);
  signature("a constant and a draw", {constant, draw}, true, S_OK);
  signature("a constant buffer and a draw", {argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, Cbv), draw}, true, S_OK);
  signature("a shader resource and an indexed draw", {argument(D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW, Srv), indexed}, true, S_OK);
  signature("an unordered access and a dispatch", {argument(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW, Uav), dispatch}, true, S_OK);
  signature("every root argument and a dispatch",
            {constant, argument(D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW, Cbv),
             argument(D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW, Srv),
             argument(D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW, Uav), dispatch}, true, S_OK);
  signature("a draw with room after it", {draw}, false, S_OK, 64);
  if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1)
    signature("a ray dispatch", {argument(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS)}, false, S_OK);
  if (options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1)
    signature("a mesh dispatch", {argument(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH)}, false, S_OK);
  signature("a vertex buffer and no draw", {vertices}, false, E_INVALIDARG);
  signature("a draw that is not last", {draw, vertices}, false, E_INVALIDARG);
  signature("two draws", {draw, draw}, false, E_INVALIDARG);
  signature("an index buffer and a draw without indices", {indices, draw}, false, E_INVALIDARG);
  signature("a draw, stride not a multiple of four", {draw}, false, E_INVALIDARG, 2);
  signature("a draw, stride too small", {draw}, false, E_INVALIDARG, -4);
  signature("a constant and a draw", {constant, draw}, false, E_INVALIDARG);
  signature("a draw", {draw}, true, E_INVALIDARG);

  for (auto [type, name] : {std::pair{D3D12_QUERY_HEAP_TYPE_OCCLUSION, "occlusion"},
                            std::pair{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, "timestamp"},
                            std::pair{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, "pipeline statistics"},
                            std::pair{D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, "stream output statistics"}}) {
    step("query heap: %s", name);
    D3D12_QUERY_HEAP_DESC desc{type, 16};
    ComPtr<ID3D12QueryHeap> heap;
    HRESULT hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap));
    expect(hr == S_OK, "CreateQueryHeap: %08lx", hr);
  }

  // the interfaces a capability's calls are on
  auto has = [&](IUnknown *object, REFIID iid, const char *name, bool obliged, const char *by) {
    step("%s, by %s", name, by);
    ComPtr<IUnknown> got;
    HRESULT hr = object->QueryInterface(iid, &got);
    expect(!obliged || hr == S_OK, "QueryInterface: %08lx", hr);
  };
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  bool rays = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0, rays11 = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
  has(device.Get(), __uuidof(ID3D12Device5), "ID3D12Device5", rays, "ray tracing tier 1.0");
  has(list.Get(), __uuidof(ID3D12GraphicsCommandList4), "ID3D12GraphicsCommandList4", rays, "ray tracing tier 1.0");
  has(device.Get(), __uuidof(ID3D12Device7), "ID3D12Device7", rays11, "ray tracing tier 1.1");
  has(device.Get(), __uuidof(ID3D12Device2), "ID3D12Device2", options7.MeshShaderTier, "mesh shader tier 1");
  has(list.Get(), __uuidof(ID3D12GraphicsCommandList6), "ID3D12GraphicsCommandList6", options7.MeshShaderTier, "mesh shader tier 1");
  has(device.Get(), __uuidof(ID3D12Device8), "ID3D12Device8", options7.SamplerFeedbackTier, "sampler feedback");
  has(device.Get(), __uuidof(ID3D12Device10), "ID3D12Device10", options12.EnhancedBarriersSupported, "enhanced barriers");
  has(list.Get(), __uuidof(ID3D12GraphicsCommandList7), "ID3D12GraphicsCommandList7", options12.EnhancedBarriersSupported, "enhanced barriers");
  has(list.Get(), __uuidof(ID3D12GraphicsCommandList5), "ID3D12GraphicsCommandList5", options6.VariableShadingRateTier, "variable rate shading");

  ComPtr<ID3D12Device5> device5;
  if (rays && SUCCEEDED(device.As(&device5))) {
    const struct {
      DXGI_FORMAT format;
      bool tier11;
    } formats[] = {{DXGI_FORMAT_R32G32_FLOAT}, {DXGI_FORMAT_R32G32B32_FLOAT}, {DXGI_FORMAT_R16G16_FLOAT},
                   {DXGI_FORMAT_R16G16B16A16_FLOAT}, {DXGI_FORMAT_R16G16_SNORM}, {DXGI_FORMAT_R16G16B16A16_SNORM},
                   {DXGI_FORMAT_R16G16B16A16_UNORM, true}, {DXGI_FORMAT_R16G16_UNORM, true},
                   {DXGI_FORMAT_R10G10B10A2_UNORM, true}, {DXGI_FORMAT_R8G8B8A8_UNORM, true},
                   {DXGI_FORMAT_R8G8_UNORM, true}, {DXGI_FORMAT_R8G8B8A8_SNORM, true}, {DXGI_FORMAT_R8G8_SNORM, true}};
    for (auto &f : formats) {
      if (f.tier11 && !rays11)
        continue;
      step("acceleration structure of triangles with vertex format %u, ray tracing tier 1.%u", f.format, f.tier11);
      D3D12_RAYTRACING_GEOMETRY_DESC geometry{D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES};
      // sizing a structure reads no address, it only looks whether one is null (DXR,
      // "GetRaytracingAccelerationStructurePrebuildInfo")
      geometry.Triangles = {0, DXGI_FORMAT_UNKNOWN, f.format, 0, 3, 0, {0x10000, 16}};
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE,
          1, D3D12_ELEMENTS_LAYOUT_ARRAY};
      inputs.pGeometryDescs = &geometry;
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
      device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
      expect(info.ResultDataMaxSizeInBytes && info.ScratchDataSizeInBytes, "prebuild info: %llu bytes, %llu of scratch",
             info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes);
    }
  }

  if (options.TypedUAVLoadAdditionalFormats) {
    for (auto format : {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT,
                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT,
                        DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R8_UNORM,
                        DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT}) {
      step("typed UAV load of format %u, one of the set", format);
      D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
      HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
      expect(hr == S_OK && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD), "format support: %08lx, Support2 %x",
             hr, support.Support2);
    }
  }

  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 1 << 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
  D3D12_RESOURCE_DESC texture_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}};
  if (options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2) {
    step("resource heap tier 2: one heap for a buffer, a texture and a render target");
    D3D12_HEAP_DESC heap_desc{3 << 16, default_heap, 0, D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES};
    ComPtr<ID3D12Heap> heap;
    HRESULT hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (expect(hr == S_OK, "CreateHeap: %08lx", hr)) {
      auto target_desc = texture_desc;
      target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      UINT64 offset = 0;
      for (auto *desc : {&buffer_desc, &texture_desc, &target_desc}) {
        ComPtr<ID3D12Resource> placed;
        hr = device->CreatePlacedResource(heap.Get(), offset, desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placed));
        expect(hr == S_OK, "CreatePlacedResource of dimension %u with flags %x at %llu: %08lx", desc->Dimension,
               desc->Flags, offset, hr);
        offset += 1 << 16;
      }
    }
  }
  if (options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_1) {
    step("tiled resources tier %u: a reserved texture", options.TiledResourcesTier);
    auto reserved_desc = texture_desc;
    reserved_desc.Width = reserved_desc.Height = 1024;
    reserved_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    ComPtr<ID3D12Resource> reserved;
    HRESULT hr = device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&reserved));
    expect(hr == S_OK, "CreateReservedResource: %08lx", hr);
  }
  if (root_version.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1) {
    step("root signature 1.1: a signature of that version, with a static descriptor range");
    D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC, 0};
    D3D12_ROOT_PARAMETER1 parameter{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
    parameter.DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{D3D_ROOT_SIGNATURE_VERSION_1_1};
    desc.Desc_1_1 = {1, &parameter};
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D12RootSignature> made;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr);
    if (expect(hr == S_OK, "D3D12SerializeVersionedRootSignature: %08lx", hr))
      hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&made));
    expect(hr == S_OK, "CreateRootSignature: %08lx", hr);
  }

  step("GPU addresses: a buffer lies inside the address bits reported");
  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT address_bits{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &address_bits, sizeof(address_bits)));
  auto addressed = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, buffer_desc.Width, D3D12_RESOURCE_STATE_COMMON);
  auto last = addressed->GetGPUVirtualAddress() + buffer_desc.Width - 1;
  expect(addressed->GetGPUVirtualAddress() && !(last >> address_bits.MaxGPUVirtualAddressBitsPerProcess),
         "a buffer's last byte at %llx, with %u bits a process", last, address_bits.MaxGPUVirtualAddressBitsPerProcess);

  // pipelines that use an optional capability of the output merger and the rasterizer
  auto plain = compiler.compile(hlsl, "vs_plain", "vs");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC base{nullptr, bytecode(plain)};
  base.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  base.SampleMask = ~0u;
  base.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  base.NumRenderTargets = 1;
  base.RTVFormats[0] = DXGI_FORMAT_R8_UINT;
  base.SampleDesc = {1, 0};
  auto empty = root_signature(device.Get(), {});
  auto pipeline = [&](const char *what, bool reported, const char *vs_entry, const char *ps_entry,
                      ID3D12RootSignature *rs, auto change) {
    if (!reported)
      return;
    step("a pipeline with %s", what);
    auto vs = compiler.compile(hlsl, vs_entry, "vs"), ps = compiler.compile(hlsl, ps_entry, "ps");
    auto desc = base;
    desc.pRootSignature = rs;
    desc.VS = bytecode(vs);
    desc.PS = bytecode(ps);
    change(desc);
    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = vs.empty() || ps.empty() ? E_FAIL : device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    expect(hr == S_OK, "CreateGraphicsPipelineState: %08lx%s", hr, vs.empty() || ps.empty() ? " (a shader did not compile)" : "");
  };
  pipeline("a logic operation", options.OutputMergerLogicOp, "vs_plain", "ps", empty.Get(), [](auto &desc) {
    desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
  });
  pipeline("a stencil reference from the pixel shader", options.PSSpecifiedStencilRefSupported, "vs_plain", "ps_stencil",
           empty.Get(), [](auto &desc) {
             desc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
             desc.DepthStencilState.StencilEnable = TRUE;
             desc.DepthStencilState.StencilWriteMask = 0xff;
             desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
                 D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
           });
  pipeline("a rasterizer ordered view", options.ROVsSupported, "vs_plain", "ps_ordered", uav_signature.Get(), [](auto &) {});
  pipeline("a render target array index from the vertex shader",
           options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation, "vs", "ps", empty.Get(),
           [](auto &) {});
  return verdict();
}
