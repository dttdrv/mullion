// contract: format layout metadata does not depend on Metal storage (FORMAT_INFO: "The number of planes to
// provide information about", Microsoft Learn, D3D12_FEATURE_DATA_FORMAT_INFO, PlaneCount). one sample has one
// quality level ("D3D11 requires support for 1x(trivial)", D3D11.3 19.2.5).
// plane expectations follow
// Microsoft's DirectX-Headers, D3D12_PROPERTY_LAYOUT_FORMAT_TABLE::GetPlaneCount and GetParentFormat; R1_UNORM
// is rejected as Windows records in vkd3d-proton:test_check_feature_support. unsupported storage reports E_FAIL
// with zero support bits, not E_INVALIDARG; the latter rejects a feature data size mismatch.
// D16_UNORM depth/stencil and integer shader loads are required by D3D11.3 19.1.4.
#include "d3d12_test.hpp"
#include "../depth_formats.hpp"
#include <limits>

// SDK declarations absent from the toolchain (DirectX-Headers/include/directx/d3d12.idl and dxgiformat.h)
namespace reference {
constexpr auto DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = (DXGI_FORMAT)189;
constexpr auto DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = (DXGI_FORMAT)190;
constexpr auto D3D_ROOT_SIGNATURE_VERSION_1_2 = (D3D_ROOT_SIGNATURE_VERSION)3;
constexpr auto D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE = (D3D12_BARRIER_LAYOUT)31;
constexpr UINT D3D12_FEATURE_PREDICATION = 50;
constexpr UINT D3D12_FEATURE_HARDWARE_COPY = 52;
constexpr UINT D3D12_FEATURE_APPLICATION_SPECIFIC_DRIVER_STATE = 56;
constexpr UINT D3D12_FEATURE_BYTECODE_BYPASS_HASH_SUPPORTED = 57;
constexpr UINT D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT = 61;
constexpr UINT D3D12_FEATURE_BARRIER_LAYOUT = 64;
constexpr UINT D3D12_FEATURE_D3D12_OPTIONS22 = 65;
struct D3D12_FEATURE_DATA_D3D12_OPTIONS22 {
  BOOL ShaderExecutionReorderingActuallyReorders;
  BOOL CreateByteOffsetViewsSupported;
  UINT Max1DDispatchSize;
  UINT Max1DDispatchMeshSize;
};
union D3D12_VERSION_NUMBER {
  UINT64 Version;
  UINT16 VersionParts[4];
};
struct D3D12_FEATURE_DATA_SHADERCACHE_ABI_SUPPORT {
  WCHAR szAdapterFamily[128];
  UINT64 MinimumABISupportVersion;
  UINT64 MaximumABISupportVersion;
  D3D12_VERSION_NUMBER CompilerVersion;
  D3D12_VERSION_NUMBER ApplicationProfileVersion;
};
struct D3D12_FEATURE_DATA_BARRIER_LAYOUT {
  D3D12_COMMAND_LIST_TYPE CommandListType;
  D3D12_BARRIER_LAYOUT Layout;
  BOOL Supported;
};
} // namespace reference

int
main(int argc, char **argv) {
  printf("variant: %s; inputs: exhaustive\n", argc > 1 ? argv[1] : "api");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const struct { DXGI_FORMAT format; const char *name; } formats[] = {DXGI_FORMATS};
  for (auto [format, name] : formats) {
    step("format %#x: support and plane layout", format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1(~0u), D3D12_FORMAT_SUPPORT2(~0u)};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (format != DXGI_FORMAT_NV12)
      expect(hr == S_OK || (hr == E_FAIL && !support.Support1 && !support.Support2),
             "support hr %08lx, bits %#x, %#x", hr, support.Support1, support.Support2);
    if (format == DXGI_FORMAT_UNKNOWN)
      expect(hr == S_OK && support.Support1 == D3D12_FORMAT_SUPPORT1_BUFFER && !support.Support2,
             "UNKNOWN is not a buffer-only format");
    std::string type = name;
    if ((type.ends_with("_UINT") || type.ends_with("_SINT")) && !type.starts_with("DXGI_FORMAT_D")) {
      expect(!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE), "integer format reports shader sampling");
      expect(hr == S_OK && (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD),
             "integer format has no shader loads");
    }
    if (format == DXGI_FORMAT_D16_UNORM)
      expect(hr == S_OK && (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL),
             "required depth format has no depth/stencil support");
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
      expect(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
             "required normalized format has no shader sampling");
    UINT planes = 1;
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_NV11:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
    case DXGI_FORMAT_P208:
      planes = 2;
      break;
    case DXGI_FORMAT_V208:
    case DXGI_FORMAT_V408:
      planes = 3;
      break;
    default:
      break;
    }
    D3D12_FEATURE_DATA_FORMAT_INFO info{format, UINT8(~0u)};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info));
    expect(format == DXGI_FORMAT_R1_UNORM ? hr == E_INVALIDARG : hr == S_OK && info.PlaneCount == planes,
           "plane hr %08lx, count %u, want %u", hr, info.PlaneCount, planes);
    for (UINT size : {0u, UINT(sizeof(info) - 1), UINT(sizeof(info) + 1)})
      expect(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, size) == E_INVALIDARG,
             "format info accepts size %u", size);
    for (UINT size : {0u, UINT(sizeof(support) - 1), UINT(sizeof(support) + 1)})
      expect(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, size) == E_INVALIDARG,
             "format support accepts size %u", size);
    for (UINT count = 0; count <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT + 1; count++) {
      for (auto flags : {D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,
                         D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE}) {
        step("format %#x, samples %u, flags %#x", format, count, flags);
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS query{format, count, flags, ~0u};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &query, sizeof(query));
        HRESULT want = !count || count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ? E_FAIL
                       : format == DXGI_FORMAT_R1_UNORM ? E_INVALIDARG : S_OK;
        expect(hr == want, "multisample hr %08lx, want %08lx", hr, want);
        expect(query.Format == format && query.SampleCount == count && query.Flags == flags, "query inputs changed");
        if (want == E_FAIL)
          expect(!query.NumQualityLevels, "invalid count has %u levels", query.NumQualityLevels);
        else if (want == S_OK && count == 1)
          expect(query.NumQualityLevels == 1, "one sample has %u levels", query.NumQualityLevels);
        else if (want == S_OK && (flags || !(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET)))
          expect(!query.NumQualityLevels, "non-multisample format or tiled resource grants %u levels",
                 query.NumQualityLevels);
        if (count == 4 && !flags && format == DXGI_FORMAT_R8G8B8A8_UNORM)
          expect(query.NumQualityLevels > 0, "required 4x MSAA has no quality levels");
      }
    }
  }
  // DirectX-Specs, Sampler Feedback, Format Support: these opaque formats have only TEXTURE2D, MIP and SAMPLER_FEEDBACK
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 capabilities{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &capabilities, sizeof(capabilities)));
  for (auto format : {reference::DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE,
                      reference::DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE}) {
    step("sampler feedback format %#x", format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (capabilities.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9)
      expect(hr == S_OK && support.Support1 == (D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_MIP) &&
                 support.Support2 == D3D12_FORMAT_SUPPORT2_SAMPLER_FEEDBACK,
             "feedback hr %08lx, bits %#x, %#x", hr, support.Support1, support.Support2);
    else
      expect((hr == S_OK || hr == E_FAIL) && !support.Support1 && !support.Support2,
             "feedback format support disagrees with the feedback tier");
    D3D12_FEATURE_DATA_FORMAT_INFO info{format};
    expect(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)) == S_OK && info.PlaneCount == 1,
           "sampler feedback has no plane layout");
    for (UINT count = 0; count <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT + 1; count++) {
      for (auto flags : {D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,
                         D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE}) {
        step("sampler feedback format %#x, samples %u, flags %#x", format, count, flags);
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, count, flags, ~0u};
        HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
        HRESULT want = !count || count > D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT ? E_FAIL : S_OK;
        expect(hr == want && quality.NumQualityLevels == UINT(count == 1),
               "feedback quality hr %08lx, levels %u", hr, quality.NumQualityLevels);
        expect(quality.Format == format && quality.SampleCount == count && quality.Flags == flags,
               "feedback query inputs changed");
      }
    }
  }
  for (auto format : {DXGI_FORMAT(DXGI_FORMAT_B4G4R4A4_UNORM + 1), DXGI_FORMAT(DXGI_FORMAT_V408 + 1),
                      DXGI_FORMAT_FORCE_UINT}) {
    step("invalid format %#x", format);
    D3D12_FEATURE_DATA_FORMAT_INFO info{format};
    expect(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)) == E_INVALIDARG,
           "invalid format has plane metadata");
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, D3D12_FORMAT_SUPPORT1(~0u), D3D12_FORMAT_SUPPORT2(~0u)};
    expect(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)) == E_FAIL &&
               !support.Support1 && !support.Support2,
           "invalid format did not fail with zero support bits");
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{format, 1};
    expect(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality,
                                      sizeof(quality)) == E_INVALIDARG,
           "invalid format has quality levels");
  }
  step("multisample data sizes");
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS query{DXGI_FORMAT_R8G8B8A8_UNORM, 1};
  for (UINT size : {0u, UINT(sizeof(query) - 1), UINT(sizeof(query) + 1)}) {
    auto before = query;
    expect(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &query, size) == E_INVALIDARG,
           "multisample query accepts size %u", size);
    expect(query.Format == before.Format && query.SampleCount == before.SampleCount && query.Flags == before.Flags &&
               query.NumQualityLevels == before.NumQualityLevels,
           "rejected size %u changed multisample data", size);
  }
  // D3D11.3 19.2.3: "the reported set of quality levels for each sample count for any one
  // format in the family must be the same for the rest of the formats in the family."
  step("R32 family: four samples and typeless depth storage");
  D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS depth_quality{DXGI_FORMAT_D32_FLOAT, 4};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &depth_quality, sizeof(depth_quality)));
  for (auto format : {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT}) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS family_quality{format, depth_quality.SampleCount};
    CHECK(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &family_quality,
                                     sizeof(family_quality)));
    expect(family_quality.NumQualityLevels == depth_quality.NumQualityLevels,
           "R32 family format %#x has %u quality levels, depth has %u", format, family_quality.NumQualityLevels,
           depth_quality.NumQualityLevels);
  }
  if (depth_quality.NumQualityLevels) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1, DXGI_FORMAT_R32_TYPELESS,
                             {depth_quality.SampleCount, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    ComPtr<ID3D12Resource> resource;
    expect(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                           nullptr, IID_PPV_ARGS(&resource)) == S_OK,
           "R32_TYPELESS cannot use the reported depth storage");
  }
  // HighestVersion is "the highest version, up to the input version specified, actually available"
  // (Microsoft Learn, D3D12_FEATURE_DATA_ROOT_SIGNATURE); unrecognized versions leave the input alone (Windows witness)
  for (UINT version = 0; version <= UINT(reference::D3D_ROOT_SIGNATURE_VERSION_1_2) + 1; version++) {
    step("root signature version %u", version);
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root{D3D_ROOT_SIGNATURE_VERSION(version)};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root, sizeof(root));
    bool valid = version >= D3D_ROOT_SIGNATURE_VERSION_1_0 && version <= reference::D3D_ROOT_SIGNATURE_VERSION_1_2;
    expect(valid ? hr == S_OK && root.HighestVersion <= version && root.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_0
                 : hr == E_INVALIDARG && root.HighestVersion == version, "root signature hr %08lx, version %u", hr,
           root.HighestVersion);
  }
  // these queries are recognized even without driver state, hash bypass or a shader-cache ABI (Windows witness)
  for (auto feature : {reference::D3D12_FEATURE_PREDICATION, reference::D3D12_FEATURE_HARDWARE_COPY}) {
    step("reserved feature %u", feature);
    BOOL supported = ~0u;
    expect(device->CheckFeatureSupport(D3D12_FEATURE(feature), &supported, sizeof(supported)) == E_NOTIMPL,
           "reserved query did not return E_NOTIMPL");
  }
  for (auto feature : {reference::D3D12_FEATURE_APPLICATION_SPECIFIC_DRIVER_STATE,
                       reference::D3D12_FEATURE_BYTECODE_BYPASS_HASH_SUPPORTED}) {
    step("feature %u and sizes", feature);
    BOOL supported = ~0u, repeated = FALSE;
    expect(device->CheckFeatureSupport(D3D12_FEATURE(feature), &supported, sizeof(supported)) == S_OK,
           "driver facility query is not recognized");
    expect(device->CheckFeatureSupport(D3D12_FEATURE(feature), &repeated, sizeof(repeated)) == S_OK &&
               !supported == !repeated, "support depends on the output's initial value");
    for (UINT size : {0u, UINT(sizeof(supported) - 1), UINT(sizeof(supported) + 1)})
      expect(device->CheckFeatureSupport(D3D12_FEATURE(feature), &supported, size) == E_INVALIDARG,
             "accepts size %u", size);
  }
  step("shader-cache ABI");
  reference::D3D12_FEATURE_DATA_SHADERCACHE_ABI_SUPPORT abi{};
  HRESULT abi_hr = device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT), &abi,
                                               sizeof(abi));
  expect(abi_hr == S_OK || abi_hr == E_FAIL, "shader-cache ABI query hr %08lx", abi_hr);
  for (UINT size : {0u, UINT(sizeof(abi) - 1), UINT(sizeof(abi) + 1)})
    expect(device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_SHADER_CACHE_ABI_SUPPORT), &abi,
                                       size) == E_INVALIDARG,
           "shader-cache ABI accepts size %u", size);
  step("dispatch limits");
  reference::D3D12_FEATURE_DATA_D3D12_OPTIONS22 options{};
  expect(device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_D3D12_OPTIONS22), &options,
                                     sizeof(options)) == S_OK,
         "dispatch limit query failed");
  expect(options.Max1DDispatchSize >= D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, "compute limit is too small");
  // DirectX-Specs, Mesh Shader, DispatchMesh API: each count "must be less than 64k"
  expect(capabilities.MeshShaderTier
             ? options.Max1DDispatchMeshSize >= std::numeric_limits<UINT16>::max()
             : !options.Max1DDispatchMeshSize, "mesh limit disagrees with mesh support");
  for (UINT size : {0u, UINT(sizeof(options) - 1), UINT(sizeof(options) + 1)})
    expect(device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_D3D12_OPTIONS22), &options,
                                       size) == E_INVALIDARG,
           "dispatch limits accept size %u", size);
  // "Resource Layouts MUST be compatible with the type of Queue performing the layout transition"
  // (DirectX-Specs, Enhanced Barriers, Command Queue Layout Compatibility)
  for (UINT kind = D3D12_COMMAND_LIST_TYPE_DIRECT; kind <= D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE + 1; kind++) {
    for (UINT layout = D3D12_BARRIER_LAYOUT_COMMON;
         layout <= reference::D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE; layout++) {
      step("command list type %u, layout %u", kind, layout);
      reference::D3D12_FEATURE_DATA_BARRIER_LAYOUT query{
          D3D12_COMMAND_LIST_TYPE(kind), D3D12_BARRIER_LAYOUT(layout), ~0};
      bool want = layout == D3D12_BARRIER_LAYOUT_COMMON;
      if (layout >= D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON && layout <= D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST)
        want = kind == D3D12_COMMAND_LIST_TYPE_DIRECT;
      else if (layout >= D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON &&
               layout <= D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST)
        want = kind == D3D12_COMMAND_LIST_TYPE_COMPUTE;
      else if (layout >= D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ && layout <= D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE)
        want = kind == UINT(D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE) +
                           (layout - D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ) /
                               (D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ - D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ);
      else if (layout == D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON)
        want = kind >= D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE && kind <= D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
      else if (layout != D3D12_BARRIER_LAYOUT_COMMON) {
        bool compute = layout == D3D12_BARRIER_LAYOUT_GENERIC_READ || layout == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS ||
                       layout == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE || layout == D3D12_BARRIER_LAYOUT_COPY_SOURCE ||
                       layout == D3D12_BARRIER_LAYOUT_COPY_DEST ||
                       layout == reference::D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE;
        want = kind == D3D12_COMMAND_LIST_TYPE_DIRECT || (compute && kind == D3D12_COMMAND_LIST_TYPE_COMPUTE);
      }
      HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_BARRIER_LAYOUT), &query,
                                               sizeof(query));
      expect(hr == S_OK && !!query.Supported == want, "layout hr %08lx, supported %u, want %u", hr,
             query.Supported, want);
      expect(query.CommandListType == kind && query.Layout == layout, "layout query inputs changed");
      for (UINT size : {0u, UINT(sizeof(query) - 1), UINT(sizeof(query) + 1)})
        expect(device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_BARRIER_LAYOUT), &query,
                                           size) == E_INVALIDARG,
               "layout query accepts size %u", size);
    }
    step("command list type %u, undefined layout", kind);
    reference::D3D12_FEATURE_DATA_BARRIER_LAYOUT undefined{
        D3D12_COMMAND_LIST_TYPE(kind), D3D12_BARRIER_LAYOUT_UNDEFINED};
    expect(device->CheckFeatureSupport(D3D12_FEATURE(reference::D3D12_FEATURE_BARRIER_LAYOUT), &undefined,
                                       sizeof(undefined)) == S_OK && undefined.Supported,
           "undefined layout is not recognized");
  }
  return verdict();
}
