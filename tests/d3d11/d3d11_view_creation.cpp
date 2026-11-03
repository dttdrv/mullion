// contract: valid views select the requested mip and slices, and all accesses
// and clears use that selection. D3D11.3 5.2.1: "Out of range behaviors are
// also always with respect to the view properties where views are used."
// 5.2.3.1: "The full extent of the resource view is always cleared.  Viewport
// and scissor are not applied."
// 22.4.7: "ld2dms operates identically to ld except on 2D multsample resources
// with one or more samples". 22.4.21: "Returns the number of samples for the
// given resource or the rasterizer."
// 22.4.8: "Out of bounds addressing is the same as the ld instruction."
// 22.4.6: "ld returns 0 in all non-missing components of the format of the SrcResource".
// 22.4.9: "Out of bounds addressing means nothing gets written to memory."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// D3D11_TEX3D_RTV, WSize: "A value of -1 indicates all of the slices along the
// w axis, starting from FirstWSlice."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_tex3d_rtv
// Wine's test_create_shader_resource_view (16), test_create_depthstencil_view
// (20), and test_create_unordered_access_view (24) record Windows accepting
// these sample-count-one and partial-W views. Every valid W interval at every
// mip is generated; words outside it retain their initial value. The first
// out-of-view shader coordinate reads zero and cannot write, even when it is
// inside the underlying texture.
// the read precedes the store because 22.19.2 permits "the compiler or drivers
// can optimize away the read from memory" after a store, even outside the view.
// Direct3D 11 Return Codes: E_INVALIDARG means "An invalid parameter was passed
// to the returning function."
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-returnvalues
// creation contract: a view has a compatible type and format and selects a nonempty interval inside its resource.
// D3D11.3 5.2.1: "the effective dimensions and array sizes of the Views will, naturally,
// always be less than or equal to the original Resource." 5.1.6: "Resource Views created
// against Prestructured+Typed Resources must have indentical Resource Formats as the
// Prestructured+Typed Resource." 5.2.1's DDI declarations require MostDetailedMip < Resource MipLevels and
// FirstArraySlice < Resource ArraySize. their counts cannot exceed the remaining interval. -1 selects the rest
// (D3D11_TEX2D_SRV, D3D11_TEX2D_ARRAY_RTV, D3D11_TEX3D_RTV, Microsoft Learn). Wine's four test_create_*_view
// functions record Windows accepting MS views at sample count one and refusing incompatible resource dimensions.
// validation-only calls perform the same checks (CreateShaderResourceView, Microsoft Learn: "validate the other
// input parameters"). buffer RTVs clear and draw exactly their elements; 5.2.3.1: "Buffers are treated as 1D."
// 5.2.1 limits buffer RTV width to D3D11_REQ_TEXTURE1D_U_DIMENSION; 19.1.4's "Buffer" and "RenderTarget"
// support columns determine typed buffer RTV formats. D32_FLOAT's "Shader ld" entry is "-".
// Wine's test_device_interfaces requires E_NOINTERFACE for ID3D10Device on a D3D11 device; its
// test_create_shader_resource_view exercises structured buffers only at feature level 11.0 and above.
#include "d3d11_test.hpp"
#include "../resource_cases.hpp"
#include <d3d10_1.h>
#include <d3d11_3.h>
#include <algorithm>
#include <bit>
#include <iterator>
#include <map>
#include <type_traits>

static void CALLBACK
timeout(void *, BOOLEAN) {
  puts("failed: view creation, readback or teardown exceeded five minutes");
  TerminateProcess(GetCurrentProcess(), 1);
}

template <typename Desc>
static void
creation(ID3D11Device *device, ID3D11Resource *resource, const Desc *requested, const Desc *expected,
         ID3D10Device *device10_without_resource = nullptr) {
  constexpr bool srv = std::is_same_v<Desc, D3D11_SHADER_RESOURCE_VIEW_DESC>;
  constexpr bool rtv = std::is_same_v<Desc, D3D11_RENDER_TARGET_VIEW_DESC>;
  constexpr bool dsv = std::is_same_v<Desc, D3D11_DEPTH_STENCIL_VIEW_DESC>;
  using View = std::conditional_t<
      srv, ID3D11ShaderResourceView,
      std::conditional_t<rtv, ID3D11RenderTargetView,
                         std::conditional_t<dsv, ID3D11DepthStencilView, ID3D11UnorderedAccessView>>>;
  ComPtr<ID3D11Device3> device3;
  if (!expect(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device3))), "no device3 for view creation"))
    return;
  for (UINT route : {11u, 10u, 3u}) {
    for (bool output : {false, true}) {
      auto sentinel = reinterpret_cast<View *>(device);
      View *object = sentinel;
      HRESULT hr;
      if (route == 10) {
        if constexpr (srv || rtv || dsv) {
          if (resource) {
            D3D11_RESOURCE_DIMENSION dimension;
            resource->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
              D3D11_BUFFER_DESC desc;
              static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
              if (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)
                continue;
            }
          }
          ComPtr<ID3D10Resource> resource10;
          if (resource && !expect(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&resource10))), "no D3D10 resource"))
            return;
          ComPtr<ID3D10Device> device10;
          if (resource10)
            resource10->GetDevice(&device10);
          else
            device10 = device10_without_resource;
          // a resource of a Direct3D 11 device has no Direct3D 10 device to make its views
          if (!device10)
            continue;
          using Desc10 =
              std::conditional_t<srv, D3D10_SHADER_RESOURCE_VIEW_DESC1,
                                 std::conditional_t<rtv, D3D10_RENDER_TARGET_VIEW_DESC, D3D10_DEPTH_STENCIL_VIEW_DESC>>;
          using View10 = std::conditional_t<srv, ID3D10ShaderResourceView1,
                                            std::conditional_t<rtv, ID3D10RenderTargetView, ID3D10DepthStencilView>>;
          Desc10 desc10{};
          if (requested) {
            if constexpr (dsv) {
              desc10.Format = requested->Format;
              desc10.ViewDimension = D3D10_DSV_DIMENSION(requested->ViewDimension);
              memcpy(&desc10.Texture1D, &requested->Texture1D, sizeof(desc10.Texture2DArray));
            } else {
              desc10 = std::bit_cast<Desc10>(*requested);
            }
          }
          auto sentinel10 = reinterpret_cast<View10 *>(device10.Get());
          auto view10 = sentinel10;
          if constexpr (srv) {
            ComPtr<ID3D10Device1> device10_1;
            if (!expect(SUCCEEDED(device10.As(&device10_1)), "no D3D10.1 device"))
              return;
            hr = device10_1->CreateShaderResourceView1(resource10.Get(), requested ? &desc10 : nullptr,
                                                       output ? &view10 : nullptr);
          } else if constexpr (rtv) {
            hr = device10->CreateRenderTargetView(resource10.Get(), requested ? &desc10 : nullptr,
                                                  output ? &view10 : nullptr);
          } else {
            hr = device10->CreateDepthStencilView(resource10.Get(), requested ? &desc10 : nullptr,
                                                  output ? &view10 : nullptr);
          }
          object = nullptr;
          if (output && view10 && view10 != sentinel10) {
            expect(SUCCEEDED(view10->QueryInterface(IID_PPV_ARGS(&object))), "no D3D11 view interface");
            view10->Release();
          } else if (output && view10)
            object = sentinel;
        } else {
          continue;
        }
      } else if (route == 3 && !dsv) {
        using Desc1 = std::conditional_t<
            srv, D3D11_SHADER_RESOURCE_VIEW_DESC1,
            std::conditional_t<rtv, D3D11_RENDER_TARGET_VIEW_DESC1, D3D11_UNORDERED_ACCESS_VIEW_DESC1>>;
        Desc1 desc1{};
        if (requested)
          memcpy(&desc1, requested, sizeof(*requested));
        if constexpr (srv)
          hr = device3->CreateShaderResourceView1(resource, requested ? &desc1 : nullptr,
                                                  output ? reinterpret_cast<ID3D11ShaderResourceView1 **>(&object)
                                                         : nullptr);
        else if constexpr (rtv)
          hr = device3->CreateRenderTargetView1(resource, requested ? &desc1 : nullptr,
                                                output ? reinterpret_cast<ID3D11RenderTargetView1 **>(&object)
                                                       : nullptr);
        else if constexpr (!dsv)
          hr = device3->CreateUnorderedAccessView1(resource, requested ? &desc1 : nullptr,
                                                   output ? reinterpret_cast<ID3D11UnorderedAccessView1 **>(&object)
                                                          : nullptr);
      } else {
        if constexpr (srv)
          hr = device->CreateShaderResourceView(resource, requested, output ? &object : nullptr);
        else if constexpr (rtv)
          hr = device->CreateRenderTargetView(resource, requested, output ? &object : nullptr);
        else if constexpr (dsv)
          hr = device->CreateDepthStencilView(resource, requested, output ? &object : nullptr);
        else
          hr = device->CreateUnorderedAccessView(resource, requested, output ? &object : nullptr);
      }
      expect(hr == (expected ? (output ? S_OK : S_FALSE) : E_INVALIDARG), "view route=%u output=%u returned %#lx",
             route, output, hr);
      if (output) {
        expect(expected ? object && object != sentinel : !object, "view route=%u output differs", route);
        if (object && object != sentinel) {
          if (expected) {
            Desc actual{};
            object->GetDesc(&actual);
            const UINT kind = UINT(expected->ViewDimension);
            constexpr UINT one_array =
                dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE1DARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE1DARRAY);
            constexpr UINT two_array =
                dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
            constexpr UINT ms = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DMS) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DMS);
            constexpr UINT ms_array =
                dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY);
            size_t bytes = sizeof(actual.Texture1D);
            if (kind == one_array || kind == two_array || (!srv && !dsv && kind == D3D11_RTV_DIMENSION_TEXTURE3D))
              bytes = sizeof(actual.Texture1DArray);
            if (kind == ms)
              bytes = 0;
            if constexpr (srv || rtv || dsv)
              if (kind == ms_array)
                bytes = sizeof(actual.Texture2DMSArray);
            if constexpr (srv) {
              if (kind == D3D11_SRV_DIMENSION_BUFFER || kind == D3D11_SRV_DIMENSION_TEXTURECUBE)
                bytes = sizeof(actual.Texture1D);
              if (kind == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY)
                bytes = sizeof(actual.TextureCubeArray);
            } else if constexpr (!dsv) {
              if (kind == D3D11_RTV_DIMENSION_BUFFER)
                bytes = sizeof(actual.Buffer);
            }
            bool same = actual.Format == expected->Format && actual.ViewDimension == expected->ViewDimension &&
                        !memcmp(&actual.Texture1D, &expected->Texture1D, bytes);
            if constexpr (dsv)
              same &= actual.Flags == expected->Flags;
            expect(same, "view route=%u description differs", route);
            ComPtr<ID3D11Resource> owner;
            object->GetResource(&owner);
            expect(owner.Get() == resource, "view route=%u resource differs", route);
            ComPtr<ID3D11Device> parent;
            object->GetDevice(&parent);
            expect(parent.Get() == device, "view route=%u device differs", route);
          }
          object->Release();
        }
      }
    }
  }
}

template <typename Desc>
static void
intervals(ID3D11Device *device, ID3D11Resource *resource, D3D11_RESOURCE_DIMENSION dimension, UINT mips, UINT slices,
          DXGI_FORMAT format, bool cube = false) {
  constexpr bool srv = std::is_same_v<Desc, D3D11_SHADER_RESOURCE_VIEW_DESC>;
  constexpr bool dsv = std::is_same_v<Desc, D3D11_DEPTH_STENCIL_VIEW_DESC>;
  constexpr UINT one = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE1D) : UINT(D3D11_SRV_DIMENSION_TEXTURE1D);
  constexpr UINT one_array = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE1DARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE1DARRAY);
  constexpr UINT two = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2D) : UINT(D3D11_SRV_DIMENSION_TEXTURE2D);
  constexpr UINT two_array = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DARRAY);
  constexpr UINT ms = dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DMS) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DMS);
  constexpr UINT ms_array =
      dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY) : UINT(D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY);
  constexpr UINT last = srv   ? UINT(D3D11_SRV_DIMENSION_TEXTURECUBEARRAY)
                        : dsv ? UINT(D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY)
                              : UINT(D3D11_RTV_DIMENSION_TEXTURE3D);
  constexpr UINT faces = 6; // D3D11.3 5.3.8: "A TextureCube has 6 faces"
  if (!cube) {
    Desc whole{};
    whole.Format = format;
    const UINT kind = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D   ? (slices > 1 ? one_array : one)
                      : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? (slices > 1 ? two_array : two)
                                                                        : UINT(D3D11_SRV_DIMENSION_TEXTURE3D);
    whole.ViewDimension = decltype(whole.ViewDimension)(kind);
    if constexpr (srv)
      whole.Texture1DArray = {0, mips, 0, slices};
    else
      whole.Texture1DArray = {0, 0, slices};
    step("default view dimension=%u mips=%u slices=%u", dimension, mips, slices);
    creation(device, resource, static_cast<const Desc *>(nullptr), &whole);
  }
  std::vector<UINT> mip_indices;
  for (UINT mip = 0; mip <= mips + 1; ++mip)
    mip_indices.push_back(mip);
  mip_indices.push_back(~0u);
  for (UINT kind = 0; kind <= last + 1; ++kind) {
    const bool array = kind == one_array || kind == two_array || kind == ms_array;
    const bool multisample = kind == ms || kind == ms_array;
    const bool cubemap =
        srv && (kind == D3D11_SRV_DIMENSION_TEXTURECUBE || kind == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY);
    const bool cube_array = srv && kind == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    bool compatible = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D   ? kind == one || kind == one_array
                      : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? kind >= two && kind <= ms_array
                                                                        : !dsv && kind == D3D11_SRV_DIMENSION_TEXTURE3D;
    if (cubemap)
      compatible = cube;
    if constexpr (std::is_same_v<Desc, D3D11_UNORDERED_ACCESS_VIEW_DESC>)
      if (multisample)
        compatible = false;
    std::vector<UINT> starts{0};
    if (array || cube_array || (!srv && dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)) {
      for (UINT first = 1; first <= slices + 1; ++first)
        starts.push_back(first);
      starts.push_back(~0u);
    }
    for (UINT mip : mip_indices) {
      for (UINT first : starts) {
        const UINT extent = !srv && dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D && mip < mips
                                ? std::max(1u, slices >> mip)
                                : slices;
        for (UINT length : {0u, 1u, extent, extent + 1, ~0u}) {
          for (UINT levels : {0u, 1u, mips, mips + 1, ~0u}) {
            if (!compatible && (mip || first || length != 1 || levels != 1))
              continue;
            if ((!srv && levels != 1) || (multisample && (mip || levels != 1)) ||
                (!(array || cube_array || (!srv && dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)) && length != 1))
              continue;
            Desc desc{};
            desc.Format = format;
            desc.ViewDimension = decltype(desc.ViewDimension)(kind);
            if constexpr (srv)
              desc.Texture1DArray = {mip, levels, first, length};
            else
              desc.Texture1DArray = {mip, first, length};
            if constexpr (!std::is_same_v<Desc, D3D11_UNORDERED_ACCESS_VIEW_DESC>)
              if (multisample)
                desc.Texture2DMSArray = {first, length};
            auto expected = desc;
            bool valid = compatible && mip < mips;
            if constexpr (srv) {
              const UINT count = levels == ~0u && mip < mips ? mips - mip : levels;
              if (!multisample)
                expected.Texture1D.MipLevels = count;
              valid &= multisample || (count && mip < mips && count <= mips - mip);
            }
            if (array || cube_array || (!srv && dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)) {
              const UINT unit = cube_array ? faces : 1;
              const UINT count = length == ~0u && first < extent ? (extent - first) / unit : length;
              valid &= first < extent && count && count <= (extent - std::min(first, extent)) / unit;
              if constexpr (!std::is_same_v<Desc, D3D11_UNORDERED_ACCESS_VIEW_DESC>) {
                if (multisample)
                  expected.Texture2DMSArray.ArraySize = count;
                else
                  expected.Texture1DArray.ArraySize = count;
              } else {
                expected.Texture1DArray.ArraySize = count;
              }
            }
            step("creation type=%u dimension=%u mips=%u slices=%u kind=%u mip=%u first=%u length=%u levels=%u",
                 UINT(srv                                                   ? D3D11_BIND_SHADER_RESOURCE
                      : dsv                                                 ? D3D11_BIND_DEPTH_STENCIL
                      : std::is_same_v<Desc, D3D11_RENDER_TARGET_VIEW_DESC> ? D3D11_BIND_RENDER_TARGET
                                                                            : D3D11_BIND_UNORDERED_ACCESS),
                 dimension, mips, slices, kind, mip, first, length, levels);
            creation(device, resource, &desc, valid ? &expected : nullptr);
          }
        }
      }
    }
  }
}

int
main() {
  HANDLE watchdog;
  if (!expect(CreateTimerQueueTimer(&watchdog, nullptr, timeout, nullptr, 5 * 60 * 1000, 0, WT_EXECUTEONLYONCE),
              "could not bound view creation and teardown"))
    return 1;
  std::map<std::string, std::pair<unsigned, std::string>> failures;
  auto expect = [&](bool ok, const char *format, auto... args) {
    if (ok)
      return true;
    trace::wrong++;
    auto &[count, first] = failures[format];
    if (!count++) {
      char message[sizeof(trace::doing)];
      if constexpr (sizeof...(args))
        snprintf(message, sizeof(message), format, args...);
      else
        snprintf(message, sizeof(message), "%s", format);
      first = std::string(message) + " (while: " + trace::doing + ")";
    }
    return false;
  };
  const UINT width = 9, height = 5, depth = 9, word_bytes = sizeof(UINT);
  const UINT unwritten[] = {~0u, ~0u, ~0u, ~0u};
  const UINT mips = std::bit_width(std::max({width, height, depth}));
  const UINT mips_2d = std::bit_width(std::max(width, height));
  const char hlsl[] = R"(
RWTexture3D<uint> volume : register(u0);
RWBuffer<uint> result : register(u1);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, depth;
  volume.GetDimensions(width, height, depth);
  result[0] = width; result[1] = height; result[2] = depth;
  result[3] = volume[uint3(0, 0, depth)];
#if !READONLY
  volume[uint3(0, 0, depth)] = 99;
  for (uint z = 0; z < depth; ++z) volume[uint3(0, 0, z)] = z + 1;
#endif
}
[numthreads(1, 1, 1)] void write_only(uint3 id : SV_DispatchThreadID) {
  volume[uint3(0, 0, id.x)] = id.x + 1;
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4((id & 1) * 2.0 - 1.0, (id >> 1) * 2.0 - 1.0, 0, 1);
}
float4 ps() : SV_Target { return 7; }
)";
  auto cs_code = compile(hlsl, "cs", "cs"), vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  auto read_code = compile(hlsl, "cs", "cs", {"READONLY=1"});
  auto write_code = compile(hlsl, "write_only", "cs");
  const char pixel_hlsl[] = R"(
RWTexture3D<uint> volume : register(u1);
RWBuffer<uint> result : register(u2);
float4 ps() : SV_Target {
  uint width, height, depth;
  volume.GetDimensions(width, height, depth);
  result[0] = width; result[1] = height; result[2] = depth;
  result[3] = volume[uint3(0, 0, depth)];
#if !READONLY
  volume[uint3(0, 0, depth)] = 99;
  for (uint z = 0; z < depth; ++z) volume[uint3(0, 0, z)] = z + 1;
#endif
  return 7;
}
float4 write_only() : SV_Target {
  for (uint z = 0; z < DEPTH; ++z) volume[uint3(0, 0, z)] = z + 1;
  return 7;
}
)";
  const std::string depth_define = "DEPTH=" + std::to_string(depth + 1);
  auto pixel_code = compile(pixel_hlsl, "ps", "ps", {depth_define});
  auto pixel_read_code = compile(pixel_hlsl, "ps", "ps", {depth_define, "READONLY=1"});
  auto pixel_write_code = compile(pixel_hlsl, "write_only", "ps", {depth_define});
  const char ms_hlsl[] = R"(
#if ARRAY
Texture2DMSArray<float> source : register(t0);
#else
Texture2DMS<float> source : register(t0);
#endif
RWBuffer<uint> result : register(u0);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, samples, arrays = 1;
#if ARRAY
  source.GetDimensions(width, height, arrays, samples);
  float value = source.Load(int3(width - 1, height - 1, arrays - 1), samples - 1);
#else
  source.GetDimensions(width, height, samples);
  float value = source.Load(int2(width - 1, height - 1), samples - 1);
#endif
  result[0] = width; result[1] = height; result[2] = arrays;
  result[3] = samples; result[4] = asuint(value);
}
)";
  auto ms_code = compile(ms_hlsl, "cs", "cs", {"ARRAY=0"});
  auto ms_array_code = compile(ms_hlsl, "cs", "cs", {"ARRAY=1"});
  const char plane_hlsl[] = R"(
#if ARRAY
RWTexture2DArray<uint> plane : register(u0);
#else
RWTexture2D<uint> plane : register(u0);
#endif
RWBuffer<uint> result : register(u1);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, arrays = 1;
#if ARRAY
  plane.GetDimensions(width, height, arrays);
  for (uint z = 0; z < arrays; ++z) plane[uint3(0, 0, z)] = z + 1;
#else
  plane.GetDimensions(width, height);
  plane[uint2(0, 0)] = 1;
#endif
  result[0] = width; result[1] = height; result[2] = arrays; result[3] = 0;
}
)";
  auto plane_code = compile(plane_hlsl, "cs", "cs", {"ARRAY=0"});
  auto plane_array_code = compile(plane_hlsl, "cs", "cs", {"ARRAY=1"});
  if (!cs_code || !read_code || !write_code || !vs_code || !ps_code || !pixel_code || !pixel_read_code ||
      !pixel_write_code || !ms_code || !ms_array_code || !plane_code || !plane_array_code)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context
  ));
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!create10) {
    printf("failed: no D3D10CreateDevice1 in d3d10_1.dll\n");
    return 1;
  }
  ComPtr<ID3D10Device1> device10;
  CHECK(create10(
      nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device10
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> context1, deferred1;
  CHECK(context.As(&context1));
  CHECK(deferred.As(&deferred1));
  ComPtr<ID3D11ComputeShader> cs, read_only, write_only;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps, pixel_uav, pixel_read_only, pixel_write_only;
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
  CHECK(device->CreateComputeShader(read_code->GetBufferPointer(), read_code->GetBufferSize(), nullptr, &read_only));
  CHECK(device->CreateComputeShader(write_code->GetBufferPointer(), write_code->GetBufferSize(), nullptr, &write_only));
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &pixel_uav));
  CHECK(device->CreatePixelShader(
      pixel_read_code->GetBufferPointer(), pixel_read_code->GetBufferSize(), nullptr, &pixel_read_only
  ));
  CHECK(device->CreatePixelShader(
      pixel_write_code->GetBufferPointer(), pixel_write_code->GetBufferSize(), nullptr, &pixel_write_only
  ));
  ComPtr<ID3D11RasterizerState> rasterizer;
  D3D11_RASTERIZER_DESC rasterizer_desc{};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  CHECK(device->CreateRasterizerState(&rasterizer_desc, &rasterizer));
  ComPtr<ID3D11ComputeShader> ms, ms_array;
  CHECK(device->CreateComputeShader(ms_code->GetBufferPointer(), ms_code->GetBufferSize(), nullptr, &ms));
  CHECK(
      device->CreateComputeShader(ms_array_code->GetBufferPointer(), ms_array_code->GetBufferSize(), nullptr, &ms_array)
  );
  D3D11_BUFFER_DESC ms_result_desc{5 * word_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
  ComPtr<ID3D11Buffer> ms_result;
  CHECK(device->CreateBuffer(&ms_result_desc, nullptr, &ms_result));
  ComPtr<ID3D11UnorderedAccessView> ms_result_view;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ms_result_view_desc{};
  ms_result_view_desc.Format = DXGI_FORMAT_R32_UINT;
  ms_result_view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ms_result_view_desc.Buffer.NumElements = ms_result_desc.ByteWidth / word_bytes;
  CHECK(device->CreateUnorderedAccessView(ms_result.Get(), &ms_result_view_desc, &ms_result_view));

  for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D;
       ++dimension) {
    for (bool dynamic : {false, true}) {
      for (bool depth_format : {false, true}) {
        if (depth_format && (dynamic || dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D))
          continue;
        for (UINT slices : {1u, 3u}) {
          for (UINT mip_count : {1u, mips}) {
            UINT actual_mips = mip_count, actual_slices = slices;
            DXGI_FORMAT format = depth_format ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            const D3D11_USAGE usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
            const UINT bind = depth_format ? UINT(D3D11_BIND_DEPTH_STENCIL)
                                           : D3D11_BIND_SHADER_RESOURCE |
                                                 (dynamic ? 0u : UINT(D3D11_BIND_RENDER_TARGET |
                                                                      D3D11_BIND_UNORDERED_ACCESS));
            const UINT cpu = dynamic ? UINT(D3D11_CPU_ACCESS_WRITE) : 0;
            ComPtr<ID3D11Resource> resource;
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
              D3D11_TEXTURE1D_DESC desc{width, mip_count, slices, format, usage, bind, cpu};
              ComPtr<ID3D11Texture1D> texture;
              CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
              texture->GetDesc(&desc);
              actual_mips = desc.MipLevels;
              actual_slices = desc.ArraySize;
              resource = texture;
            } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
              D3D11_TEXTURE2D_DESC desc{width, height, mip_count, slices, format, {1, 0}, usage, bind, cpu};
              ComPtr<ID3D11Texture2D> texture;
              CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
              texture->GetDesc(&desc);
              actual_mips = desc.MipLevels;
              actual_slices = desc.ArraySize;
              resource = texture;
            } else {
              D3D11_TEXTURE3D_DESC desc{width, height, slices, mip_count, format, usage, bind, cpu};
              ComPtr<ID3D11Texture3D> texture;
              CHECK(device->CreateTexture3D(&desc, nullptr, &texture));
              texture->GetDesc(&desc);
              actual_mips = desc.MipLevels;
              actual_slices = desc.Depth;
              resource = texture;
            }
            if (!depth_format)
              intervals<D3D11_SHADER_RESOURCE_VIEW_DESC>(device.Get(), resource.Get(),
                                                         D3D11_RESOURCE_DIMENSION(dimension), actual_mips,
                                                         actual_slices, format);
            if (!dynamic) {
              if (depth_format)
                intervals<D3D11_DEPTH_STENCIL_VIEW_DESC>(device.Get(), resource.Get(),
                                                         D3D11_RESOURCE_DIMENSION(dimension), actual_mips,
                                                         actual_slices, format);
              else {
                intervals<D3D11_RENDER_TARGET_VIEW_DESC>(device.Get(), resource.Get(),
                                                         D3D11_RESOURCE_DIMENSION(dimension), actual_mips,
                                                         actual_slices, format);
                intervals<D3D11_UNORDERED_ACCESS_VIEW_DESC>(device.Get(), resource.Get(),
                                                            D3D11_RESOURCE_DIMENSION(dimension), actual_mips,
                                                            actual_slices, format);
              }
            }
          }
        }
      }
    }
  }

  constexpr UINT cube_faces = 6; // D3D11.3 5.3.8: "A TextureCube has 6 faces"
  for (UINT slices = cube_faces; slices <= cube_faces * 2 + 1; ++slices) {
    D3D11_TEXTURE2D_DESC desc{width,
                              width,
                              2,
                              slices,
                              DXGI_FORMAT_R8G8B8A8_UNORM,
                              {1, 0},
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE,
                              0,
                              D3D11_RESOURCE_MISC_TEXTURECUBE};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    texture->GetDesc(&desc);
    intervals<D3D11_SHADER_RESOURCE_VIEW_DESC>(device.Get(), texture.Get(), D3D11_RESOURCE_DIMENSION_TEXTURE2D,
                                               desc.MipLevels, desc.ArraySize, desc.Format, true);
    D3D11_SHADER_RESOURCE_VIEW_DESC expected{};
    expected.Format = desc.Format;
    if (desc.ArraySize / cube_faces > 1) {
      expected.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
      expected.TextureCubeArray = {0, desc.MipLevels, 0, desc.ArraySize / cube_faces};
    } else {
      expected.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
      expected.TextureCube = {0, desc.MipLevels};
    }
    creation(device.Get(), texture.Get(), static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr), &expected);
  }

  for (UINT parent = DXGI_FORMAT_R8G8B8A8_TYPELESS; parent <= DXGI_FORMAT_R8G8B8A8_SINT; ++parent) {
    D3D11_TEXTURE2D_DESC desc{width,
                              height,
                              mips,
                              1,
                              DXGI_FORMAT(parent),
                              {1, 0},
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    UINT parent_support = 0;
    CHECK(device->CheckFormatSupport(desc.Format, &parent_support));
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        (parent_support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    std::vector<UINT> children;
    for (UINT child = DXGI_FORMAT_R8G8B8A8_TYPELESS; child <= DXGI_FORMAT_R8G8B8A8_SINT; ++child)
      children.push_back(child);
    children.push_back(DXGI_FORMAT_R16_FLOAT);
    for (UINT child : children) {
      for (bool unknown : {false, true}) {
        auto format = unknown ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT(child);
        const auto effective = unknown ? desc.Format : format;
        const bool compatible = effective >= DXGI_FORMAT_R8G8B8A8_UNORM && effective <= DXGI_FORMAT_R8G8B8A8_SINT &&
                                (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || effective == desc.Format);
        UINT support = 0;
        CHECK(device->CheckFormatSupport(effective, &support));
        step("view formats parent=%u child=%u unknown=%u", parent, child, unknown);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D = {0, ~0u};
        auto srv_expected = srv_desc;
        srv_expected.Format = effective;
        srv_expected.Texture2D.MipLevels = desc.MipLevels;
        creation(device.Get(), texture.Get(), &srv_desc, compatible ? &srv_expected : nullptr);
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        auto rtv_expected = rtv_desc;
        rtv_expected.Format = effective;
        creation(device.Get(), texture.Get(), &rtv_desc,
                 compatible && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? &rtv_expected : nullptr);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = format;
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        auto uav_expected = uav_desc;
        uav_expected.Format = effective;
        creation(device.Get(), texture.Get(), &uav_desc,
                 compatible && (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) &&
                         (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
                     ? &uav_expected
                     : nullptr);
      }
    }
  }

  // D3D11.3 5.3.9.5: a texture of these 32-bit typeless formats takes R32_UINT, R32_SINT and R32_FLOAT unordered
  // access views, and only those views, though R32 is not of its type group; a typed texture of the group takes none
  for (DXGI_FORMAT parent : {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                             DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_R16G16_TYPELESS,
                             DXGI_FORMAT_R8G8B8A8_UNORM}) {
    D3D11_TEXTURE2D_DESC desc{4, 4, 1, 1, parent, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS};
    ComPtr<ID3D11Texture2D> texture;
    // Mullion gives B8G8R8A8 and B8G8R8X8 no unordered access, so no texture of theirs is made for it
    const bool bgr = parent == DXGI_FORMAT_B8G8R8A8_TYPELESS || parent == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    if (bgr && FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
      continue;
    if (!bgr)
      CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    const bool typeless = parent != DXGI_FORMAT_R8G8B8A8_UNORM;
    for (DXGI_FORMAT child : {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32_FLOAT}) {
      step("R32 views parent=%u child=%u", parent, child);
      D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
      uav_desc.Format = child;
      uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      creation(device.Get(), texture.Get(), &uav_desc, typeless ? &uav_desc : nullptr);
      D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
      srv_desc.Format = child;
      srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      srv_desc.Texture2D = {0, 1};
      creation(device.Get(), texture.Get(), &srv_desc, static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr));
      D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
      rtv_desc.Format = child;
      rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      creation(device.Get(), texture.Get(), &rtv_desc, static_cast<const D3D11_RENDER_TARGET_VIEW_DESC *>(nullptr));
    }
    if (!typeless)
      continue;
    // the view writes the texture's bits as they are: a uint clear through it reads back as the same bytes
    step("R32 view bits parent=%u", parent);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = DXGI_FORMAT_R32_UINT;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav));
    const UINT bits[4] = {0x80402010u, 0, 0, 0};
    context->ClearUnorderedAccessViewUint(uav.Get(), bits);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    bool same = true;
    for (UINT y = 0; y < desc.Height; y++)
      for (UINT x = 0; x < desc.Width; x++)
        same &= reinterpret_cast<const UINT *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch)[x] ==
                bits[0];
    context->Unmap(staging.Get(), 0);
    expect(same, "an R32_UINT view of format %u does not write its bits", parent);
  }

  for (DXGI_FORMAT parent : {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT}) {
    D3D11_TEXTURE2D_DESC desc{width, height, mips, 3, parent, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    for (DXGI_FORMAT child : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
                             DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT}) {
      D3D11_DEPTH_STENCIL_VIEW_DESC view{};
      view.Format = child;
      view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
      view.Texture2DArray = {0, 0, desc.ArraySize};
      auto expected = view;
      expected.Format = child == DXGI_FORMAT_UNKNOWN ? parent : child;
      step("DSV formats parent=%u child=%u", parent, child);
      creation(device.Get(), texture.Get(), &view,
               expected.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ? &expected : nullptr);
    }
    if (parent == DXGI_FORMAT_D24_UNORM_S8_UINT)
      intervals<D3D11_DEPTH_STENCIL_VIEW_DESC>(device.Get(), texture.Get(), D3D11_RESOURCE_DIMENSION_TEXTURE2D,
                                              desc.MipLevels, desc.ArraySize, parent);
  }

  for (UINT parent = DXGI_FORMAT_R32_TYPELESS; parent <= DXGI_FORMAT_R32_FLOAT; ++parent) {
    D3D11_TEXTURE2D_DESC desc{
        width, height, mips, 3, DXGI_FORMAT(parent), {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
    if (parent == DXGI_FORMAT_R32_FLOAT)
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    for (UINT child = DXGI_FORMAT_R32_TYPELESS; child <= DXGI_FORMAT_R32_SINT; ++child) {
      D3D11_DEPTH_STENCIL_VIEW_DESC view{};
      view.Format = DXGI_FORMAT(child);
      view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
      view.Texture2DArray = {0, 0, desc.ArraySize};
      step("DSV formats parent=%u child=%u", parent, child);
      creation(device.Get(), texture.Get(), &view,
               child == DXGI_FORMAT_D32_FLOAT && (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? &view : nullptr);
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC expected{};
    expected.Format = desc.Format;
    expected.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    expected.Texture2DArray = {0, 0, desc.ArraySize};
    creation(device.Get(), texture.Get(), static_cast<const D3D11_DEPTH_STENCIL_VIEW_DESC *>(nullptr),
             desc.Format == DXGI_FORMAT_D32_FLOAT ? &expected : nullptr);
  }

  // a shader reads a depth texture in its color format and a depth stencil texture's planes in the formats named
  // for them: D3D11.3 19.1.4 gives R24_UNORM_X8_TYPELESS, X24_TYPELESS_G8_UINT, R32_FLOAT_X8X24_TYPELESS and
  // X32_TYPELESS_G8X24_UINT shader loads, and each family's D format the depth stencil target
  const struct {
    DXGI_FORMAT parent, target;
    std::vector<DXGI_FORMAT> reads;
  } depth_families[] = {
      {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, {DXGI_FORMAT_R16_UNORM}},
      {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, {DXGI_FORMAT_R32_FLOAT}},
      {DXGI_FORMAT_R24G8_TYPELESS,
       DXGI_FORMAT_D24_UNORM_S8_UINT,
       {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_X24_TYPELESS_G8_UINT}},
      {DXGI_FORMAT_R32G8X24_TYPELESS,
       DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
       {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT}},
  };
  for (const auto &family : depth_families) {
    D3D11_TEXTURE2D_DESC desc{width,   height, 1, 1, family.parent, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    D3D11_DEPTH_STENCIL_VIEW_DESC target{};
    target.Format = family.target;
    target.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    step("depth family parent=%u target=%u", family.parent, family.target);
    creation(device.Get(), texture.Get(), &target, &target);
    D3D11_SHADER_RESOURCE_VIEW_DESC read{};
    read.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    read.Texture2D = {0, 1};
    for (DXGI_FORMAT format : family.reads) {
      read.Format = format;
      step("depth family parent=%u read=%u", family.parent, format);
      creation(device.Get(), texture.Get(), &read, &read);
    }
    read.Format = family.parent;
    step("depth family parent=%u read as itself", family.parent);
    creation(device.Get(), texture.Get(), &read, static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr));
  }

  for (UINT bind :
       {0u, UINT(D3D11_BIND_SHADER_RESOURCE), UINT(D3D11_BIND_UNORDERED_ACCESS), UINT(D3D11_BIND_RENDER_TARGET)}) {
    D3D11_BUFFER_DESC desc{width * sizeof(float) * 4, D3D11_USAGE_DEFAULT, bind};
    ComPtr<ID3D11Buffer> resource;
    CHECK(device->CreateBuffer(&desc, nullptr, &resource));
    resource->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer = {0, desc.ByteWidth / UINT(sizeof(float) * 4)};
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = srv_desc.Format;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer = {srv_desc.Buffer.FirstElement, srv_desc.Buffer.NumElements, 0};
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = srv_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
    rtv_desc.Buffer = {srv_desc.Buffer.FirstElement, srv_desc.Buffer.NumElements};
    step("buffer views bind=%u", bind);
    creation(device.Get(), resource.Get(), &srv_desc, bind & D3D11_BIND_SHADER_RESOURCE ? &srv_desc : nullptr);
    creation(device.Get(), resource.Get(), &uav_desc, bind & D3D11_BIND_UNORDERED_ACCESS ? &uav_desc : nullptr);
    creation(device.Get(), resource.Get(), &rtv_desc, bind & D3D11_BIND_RENDER_TARGET ? &rtv_desc : nullptr);
    creation(device.Get(), resource.Get(), static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr),
             static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr));
    creation(device.Get(), resource.Get(), static_cast<const D3D11_UNORDERED_ACCESS_VIEW_DESC *>(nullptr),
             static_cast<const D3D11_UNORDERED_ACCESS_VIEW_DESC *>(nullptr));
    creation(device.Get(), resource.Get(), static_cast<const D3D11_RENDER_TARGET_VIEW_DESC *>(nullptr),
             static_cast<const D3D11_RENDER_TARGET_VIEW_DESC *>(nullptr));
    creation(device.Get(), nullptr, &srv_desc, static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr),
             device10.Get());
    creation(device.Get(), nullptr, &rtv_desc, static_cast<const D3D11_RENDER_TARGET_VIEW_DESC *>(nullptr),
             device10.Get());
    creation(device.Get(), nullptr, &uav_desc, static_cast<const D3D11_UNORDERED_ACCESS_VIEW_DESC *>(nullptr));
  }

  for (bool wide : {false, true}) {
    const float clear[] = {3, 3, 3, 3};
    const UINT element_bytes = wide ? sizeof(clear) : sizeof(clear[0]);
    D3D11_BUFFER_DESC desc{width * element_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Buffer> resource;
    CHECK(device->CreateBuffer(&desc, nullptr, &resource));
    resource->GetDesc(&desc);
    const UINT elements = desc.ByteWidth / element_bytes;
    for (UINT first = 0; first <= elements + 1; ++first) {
      for (UINT count = 0; count <= elements + 1; ++count) {
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = wide ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
        view.Buffer = {first, count};
        const bool valid = first < elements && count && count <= elements - std::min(first, elements);
        step("buffer RTV bytes=%u first=%u count=%u", element_bytes, first, count);
        creation(device.Get(), resource.Get(), &view, valid ? &view : nullptr);
        if (!valid)
          continue;
        ComPtr<ID3D11RenderTargetView> rtv;
        CHECK(device->CreateRenderTargetView(resource.Get(), &view, &rtv));
        for (bool listed : {false, true}) {
          auto run = listed ? deferred.Get() : context.Get();
          std::vector<float> initial(desc.ByteWidth / sizeof(float), 2);
          context->UpdateSubresource(resource.Get(), 0, nullptr, initial.data(), 0, 0);
          run->ClearRenderTargetView(rtv.Get(), clear);
          if (listed) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            context->ExecuteCommandList(commands.Get(), FALSE);
          }
          auto verify = [&](UINT written, float changed, float untouched) {
            auto words = read(device.Get(), context.Get(), resource.Get());
            if (!expect(words.size() == initial.size(), "buffer RTV readback size differs"))
              return;
            for (UINT i = 0; i < words.size(); ++i) {
              float actual;
              memcpy(&actual, &words[i], sizeof(actual));
              const UINT element = i / (element_bytes / sizeof(float));
              const float wanted = element >= first && element - first < written ? changed : untouched;
              expect(actual == wanted, "buffer RTV listed=%u word=%u got=%g expected=%g", listed, i, actual, wanted);
            }
          };
          step("buffer RTV clear bytes=%u first=%u count=%u listed=%u", element_bytes, first, count, listed);
          verify(count, clear[0], initial[0]);
          std::fill(initial.begin(), initial.end(), 5);
          context->UpdateSubresource(resource.Get(), 0, nullptr, initial.data(), 0, 0);
          const UINT drawn = std::max(1u, count / 2);
          run->ClearState();
          run->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
          run->VSSetShader(vs.Get(), nullptr, 0);
          run->PSSetShader(ps.Get(), nullptr, 0);
          run->RSSetState(rasterizer.Get());
          D3D11_VIEWPORT viewport{0, 0, float(drawn), 1, 0, 1};
          run->RSSetViewports(1, &viewport);
          ID3D11RenderTargetView *target = rtv.Get();
          run->OMSetRenderTargets(1, &target, nullptr);
          run->Draw(4, 0);
          if (listed) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            context->ExecuteCommandList(commands.Get(), FALSE);
          }
          step("buffer RTV draw bytes=%u first=%u count=%u listed=%u", element_bytes, first, count, listed);
          verify(drawn, 7, initial[0]);
          context->UpdateSubresource(resource.Get(), 0, nullptr, initial.data(), 0, 0);
          const float rect_color[] = {6, 6, 6, 6};
          D3D11_RECT rect{0, 0, LONG(drawn), 1};
          (listed ? deferred1.Get() : context1.Get())->ClearView(rtv.Get(), rect_color, &rect, 1);
          run->ClearState();
          if (listed) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            rtv.Reset();
            context->ExecuteCommandList(commands.Get(), FALSE);
          }
          step("buffer RTV rect bytes=%u first=%u count=%u listed=%u", element_bytes, first, count, listed);
          verify(drawn, rect_color[0], initial[0]);
          context->ClearState();
        }
      }
    }
    D3D11_RENDER_TARGET_VIEW_DESC invalid{};
    invalid.Format = wide ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_FLOAT;
    invalid.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
    invalid.Buffer = {~0u, 1};
    creation(device.Get(), resource.Get(), &invalid, static_cast<const D3D11_RENDER_TARGET_VIEW_DESC *>(nullptr));
    invalid.Buffer = {0, ~0u};
    creation(device.Get(), resource.Get(), &invalid, static_cast<const D3D11_RENDER_TARGET_VIEW_DESC *>(nullptr));
  }

  for (const auto &format : resource_formats) {
    D3D11_BUFFER_DESC desc{UINT(sizeof(unwritten)), D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Buffer> resource;
    CHECK(device->CreateBuffer(&desc, nullptr, &resource));
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = format.value;
    view.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
    view.Buffer = {0, 1};
    UINT support = 0;
    const UINT required = D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    const bool valid = SUCCEEDED(device->CheckFormatSupport(format.value, &support)) &&
                       (support & required) == required && !strstr(format.name, "TYPELESS");
    step("buffer RTV format=%s support=%#x", format.name, support);
    creation(device.Get(), resource.Get(), &view, valid ? &view : nullptr);
  }

  for (UINT count : {UINT(D3D11_REQ_TEXTURE1D_U_DIMENSION - 1), UINT(D3D11_REQ_TEXTURE1D_U_DIMENSION),
                     UINT(D3D11_REQ_TEXTURE1D_U_DIMENSION + 1)}) {
    D3D11_BUFFER_DESC desc{count * UINT(sizeof(float)), D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Buffer> resource;
    CHECK(device->CreateBuffer(&desc, nullptr, &resource));
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
    view.Buffer = {0, desc.ByteWidth / UINT(sizeof(float))};
    step("buffer RTV width boundary count=%u", count);
    creation(device.Get(), resource.Get(), &view, count <= D3D11_REQ_TEXTURE1D_U_DIMENSION ? &view : nullptr);
  }
  for (UINT count : {UINT(D3D10_REQ_TEXTURE1D_U_DIMENSION - 1), UINT(D3D10_REQ_TEXTURE1D_U_DIMENSION),
                     UINT(D3D10_REQ_TEXTURE1D_U_DIMENSION + 1)}) {
    D3D10_BUFFER_DESC desc{count * word_bytes, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
    ComPtr<ID3D10Buffer> resource;
    CHECK(device10->CreateBuffer(&desc, nullptr, &resource));
    D3D10_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D10_RTV_DIMENSION_BUFFER;
    view.Buffer = {0, count};
    const bool valid = count <= D3D10_REQ_TEXTURE1D_U_DIMENSION;
    for (bool output : {false, true}) {
      step("D3D10 buffer RTV width boundary count=%u output=%u", count, output);
      auto sentinel = reinterpret_cast<ID3D10RenderTargetView *>(device10.Get());
      auto rtv = sentinel;
      HRESULT hr = device10->CreateRenderTargetView(resource.Get(), &view, output ? &rtv : nullptr);
      expect(hr == (valid ? output ? S_OK : S_FALSE : E_INVALIDARG), "D3D10 width returned %#lx", hr);
      if (output) {
        expect(valid ? rtv && rtv != sentinel : !rtv, "D3D10 width output differs");
        if (rtv && rtv != sentinel)
          rtv->Release();
      }
    }
  }

  D3D11_BUFFER_DESC structured_desc{width * word_bytes,
                                    D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                    0,
                                    D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                    word_bytes};
  ComPtr<ID3D11Buffer> structured;
  CHECK(device->CreateBuffer(&structured_desc, nullptr, &structured));
  structured->GetDesc(&structured_desc);
  D3D11_SHADER_RESOURCE_VIEW_DESC structured_srv{};
  structured_srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  structured_srv.Buffer = {0, structured_desc.ByteWidth / structured_desc.StructureByteStride};
  creation(device.Get(), structured.Get(), static_cast<const D3D11_SHADER_RESOURCE_VIEW_DESC *>(nullptr),
           &structured_srv);
  D3D11_UNORDERED_ACCESS_VIEW_DESC structured_uav{};
  structured_uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  structured_uav.Buffer = {0, structured_srv.Buffer.NumElements, 0};
  creation(device.Get(), structured.Get(), static_cast<const D3D11_UNORDERED_ACCESS_VIEW_DESC *>(nullptr),
           &structured_uav);

  D3D11_TEXTURE2D_DESC boundary_desc{D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION,
                                     1,
                                     0,
                                     1,
                                     DXGI_FORMAT_R32_FLOAT,
                                     {1, 0},
                                     D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_SHADER_RESOURCE};
  ComPtr<ID3D11Texture2D> boundary;
  CHECK(device->CreateTexture2D(&boundary_desc, nullptr, &boundary));
  boundary->GetDesc(&boundary_desc);
  intervals<D3D11_SHADER_RESOURCE_VIEW_DESC>(device.Get(), boundary.Get(), D3D11_RESOURCE_DIMENSION_TEXTURE2D,
                                             boundary_desc.MipLevels, boundary_desc.ArraySize, boundary_desc.Format);
  boundary_desc.Width = 1;
  boundary_desc.MipLevels = 1;
  boundary_desc.ArraySize = D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
  boundary.Reset();
  CHECK(device->CreateTexture2D(&boundary_desc, nullptr, &boundary));
  boundary->GetDesc(&boundary_desc);
  for (UINT first : {0u, boundary_desc.ArraySize - 1, boundary_desc.ArraySize, ~0u}) {
    for (UINT count : {0u, 1u, boundary_desc.ArraySize, boundary_desc.ArraySize + 1, ~0u}) {
      D3D11_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = boundary_desc.Format;
      view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      view.Texture2DArray = {0, boundary_desc.MipLevels, first, count};
      auto expected = view;
      const UINT remaining = boundary_desc.ArraySize - std::min(first, boundary_desc.ArraySize);
      if (count == ~0u)
        expected.Texture2DArray.ArraySize = remaining;
      const bool valid = first < boundary_desc.ArraySize && expected.Texture2DArray.ArraySize &&
                         expected.Texture2DArray.ArraySize <= remaining;
      step("array metadata boundary first=%u count=%u", first, count);
      creation(device.Get(), boundary.Get(), &view, valid ? &expected : nullptr);
    }
  }

  for (bool depth_view : {false, true}) {
    for (UINT arrays : {1u, 3u}) {
      D3D11_TEXTURE1D_DESC desc{
          width,
          1,
          arrays,
          depth_view ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT,
          D3D11_USAGE_DEFAULT,
          UINT(depth_view ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET) | D3D11_BIND_SHADER_RESOURCE
      };
      ComPtr<ID3D11Texture1D> texture;
      CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
      step("unknown 1D view arrays=%u depth=%u", arrays, depth_view);
      if (depth_view) {
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        ComPtr<ID3D11DepthStencilView> dsv;
        HRESULT hr = device->CreateDepthStencilView(texture.Get(), &view, &dsv);
        expect(hr == E_INVALIDARG && !dsv, "unknown DSV dimension returned %#lx", hr);
      } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = desc.Format;
        ComPtr<ID3D11ShaderResourceView> srv;
        HRESULT hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
        expect(hr == E_INVALIDARG && !srv, "unknown SRV dimension returned %#lx", hr);
        D3D11_RENDER_TARGET_VIEW_DESC target_view{};
        target_view.Format = desc.Format;
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = device->CreateRenderTargetView(texture.Get(), &target_view, &rtv);
        expect(hr == E_INVALIDARG && !rtv, "unknown RTV dimension returned %#lx", hr);
      }
      for (bool array_view : {false, true}) {
        step("invalid 1D MS view arrays=%u depth=%u array-view=%u", arrays, depth_view, array_view);
        if (depth_view) {
          D3D11_DEPTH_STENCIL_VIEW_DESC view{};
          view.Format = DXGI_FORMAT_D32_FLOAT;
          view.ViewDimension = array_view ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
          if (array_view)
            view.Texture2DMSArray = {0, arrays};
          ComPtr<ID3D11DepthStencilView> dsv;
          HRESULT hr = device->CreateDepthStencilView(texture.Get(), &view, &dsv);
          expect(hr == E_INVALIDARG && !dsv, "1D MS DSV returned %#lx", hr);
        } else {
          D3D11_SHADER_RESOURCE_VIEW_DESC view{};
          view.Format = desc.Format;
          view.ViewDimension = array_view ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
          if (array_view)
            view.Texture2DMSArray = {0, arrays};
          ComPtr<ID3D11ShaderResourceView> srv;
          HRESULT hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
          expect(hr == E_INVALIDARG && !srv, "1D MS SRV returned %#lx", hr);
          D3D11_RENDER_TARGET_VIEW_DESC target_view{};
          target_view.Format = desc.Format;
          target_view.ViewDimension =
              array_view ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
          if (array_view)
            target_view.Texture2DMSArray = {0, arrays};
          ComPtr<ID3D11RenderTargetView> rtv;
          hr = device->CreateRenderTargetView(texture.Get(), &target_view, &rtv);
          expect(hr == E_INVALIDARG && !rtv, "1D MS RTV returned %#lx", hr);
        }
      }
    }
  }

  for (UINT arrays : {1u, 3u}) {
    step("unknown dynamic SRV dimension arrays=%u", arrays);
    D3D11_TEXTURE1D_DESC desc{width,
                              1,
                              arrays,
                              DXGI_FORMAT_R32_FLOAT,
                              D3D11_USAGE_DYNAMIC,
                              D3D11_BIND_SHADER_RESOURCE,
                              D3D11_CPU_ACCESS_WRITE};
    ComPtr<ID3D11Texture1D> texture;
    CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
    expect(hr == E_INVALIDARG && !srv, "unknown dynamic SRV dimension returned %#lx", hr);
    for (bool array_view : {false, true}) {
      step("invalid dynamic 1D MS view arrays=%u array-view=%u", arrays, array_view);
      view.ViewDimension = array_view ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
      if (array_view)
        view.Texture2DMSArray = {0, arrays};
      hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
      expect(hr == E_INVALIDARG && !srv, "dynamic 1D MS SRV returned %#lx", hr);
    }
  }

  for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2) {
    for (bool depth : {false, true}) {
      const DXGI_FORMAT format = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;
      UINT quality = 0;
      CHECK(device->CheckMultisampleQualityLevels(format, samples, &quality));
      if (!quality) {
        expect(samples > 1, "format %u has no sample-count-one support", format);
        step("format %u samples %u unsupported", format, samples);
        continue;
      }
      for (UINT arrays : {1u, 3u}) {
        for (UINT mip_count : {1u, samples == 1 ? mips_2d : 1u}) {
          D3D11_TEXTURE2D_DESC desc{
              width,
              height,
              mip_count,
              arrays,
              depth ? DXGI_FORMAT_R32_TYPELESS : format,
              {samples, 0},
              D3D11_USAGE_DEFAULT,
              UINT(depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET) | D3D11_BIND_SHADER_RESOURCE
          };
          ComPtr<ID3D11Texture2D> texture;
          CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
          D3D10_TEXTURE2D_DESC desc10{
              width,
              height,
              mip_count,
              arrays,
              desc.Format,
              desc.SampleDesc,
              D3D10_USAGE_DEFAULT,
              UINT(depth ? D3D10_BIND_DEPTH_STENCIL : D3D10_BIND_RENDER_TARGET) | D3D10_BIND_SHADER_RESOURCE
          };
          ComPtr<ID3D10Texture2D> texture10;
          CHECK(device10->CreateTexture2D(&desc10, nullptr, &texture10));
          for (UINT first : {0u, arrays, ~0u}) {
            for (UINT count : {0u, arrays + 1, ~0u}) {
              if (first == 0 && count == ~0u)
                continue;
              step(
                  "invalid MS array samples=%u arrays=%u depth=%u first=%u "
                  "count=%u",
                  samples, arrays, depth, first, count
              );
              if (depth) {
                D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                view.Texture2DMSArray = {first, count};
                ComPtr<ID3D11DepthStencilView> dsv;
                HRESULT hr = device->CreateDepthStencilView(texture.Get(), &view, &dsv);
                expect(hr == E_INVALIDARG && !dsv, "invalid MS-array DSV returned %#lx", hr);
              } else {
                D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                srv_desc.Format = format;
                srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                srv_desc.Texture2DMSArray = {first, count};
                ComPtr<ID3D11ShaderResourceView> srv;
                HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv);
                expect(hr == E_INVALIDARG && !srv, "invalid MS-array SRV returned %#lx", hr);
                D3D11_RENDER_TARGET_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                view.Texture2DMSArray = {first, count};
                ComPtr<ID3D11RenderTargetView> rtv;
                hr = device->CreateRenderTargetView(texture.Get(), &view, &rtv);
                expect(hr == E_INVALIDARG && !rtv, "invalid MS-array RTV returned %#lx", hr);
              }
            }
          }
          for (bool array_view : {false, true}) {
            if (!array_view && arrays > 1 && samples > 1)
              continue;
            for (UINT first = 0; first < (array_view ? arrays : 1); first++) {
              for (UINT requested : {1u, arrays - first, ~0u}) {
                if (!array_view && requested != 1)
                  continue;
                const UINT count = requested == ~0u ? arrays - first : requested;
                step(
                    "2D samples=%u mips=%u arrays=%u depth=%u array-view=%u "
                    "first=%u count=%u",
                    samples, mip_count, arrays, depth, array_view, first, requested
                );
                std::vector<UINT> zero(width * height);
                if (samples == 1)
                  for (UINT slice = 0; slice < arrays; slice++)
                    for (UINT mip = 0; mip < mip_count; mip++)
                      context->UpdateSubresource(
                          texture.Get(), D3D11CalcSubresource(mip, slice, mip_count), nullptr, zero.data(),
                          std::max(1u, width >> mip) * word_bytes, 0
                      );
                if (!depth) {
                  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                  srv_desc.Format = format;
                  srv_desc.ViewDimension =
                      array_view ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    srv_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D11ShaderResourceView> srv;
                  HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv);
                  expect(hr == S_OK && srv, "SRV returned %#lx", hr);
                  if (srv) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC actual{};
                    srv->GetDesc(&actual);
                    expect(
                        actual.Format == format && actual.ViewDimension == srv_desc.ViewDimension &&
                            (!array_view || (actual.Texture2DMSArray.FirstArraySlice == first &&
                                             actual.Texture2DMSArray.ArraySize == count)),
                        "SRV description differs"
                    );
                  }
                  D3D10_SHADER_RESOURCE_VIEW_DESC srv10_desc{};
                  srv10_desc.Format = format;
                  srv10_desc.ViewDimension =
                      array_view ? D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D10_SRV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    srv10_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D10ShaderResourceView> srv10;
                  hr = device10->CreateShaderResourceView(texture10.Get(), &srv10_desc, &srv10);
                  expect(hr == S_OK && srv10, "D3D10 SRV returned %#lx", hr);
                  D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
                  rtv_desc.Format = format;
                  rtv_desc.ViewDimension =
                      array_view ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    rtv_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D11RenderTargetView> rtv;
                  hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv);
                  expect(hr == S_OK && rtv, "RTV returned %#lx", hr);
                  if (rtv) {
                    D3D11_RENDER_TARGET_VIEW_DESC actual{};
                    rtv->GetDesc(&actual);
                    expect(
                        actual.Format == format && actual.ViewDimension == rtv_desc.ViewDimension &&
                            (!array_view || (actual.Texture2DMSArray.FirstArraySlice == first &&
                                             actual.Texture2DMSArray.ArraySize == count)),
                        "RTV description differs"
                    );
                    const float clear[] = {3, 3, 3, 3};
                    context->ClearRenderTargetView(rtv.Get(), clear);
                  }
                  D3D10_RENDER_TARGET_VIEW_DESC rtv10_desc{};
                  rtv10_desc.Format = format;
                  rtv10_desc.ViewDimension =
                      array_view ? D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D10_RTV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    rtv10_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D10RenderTargetView> rtv10;
                  hr = device10->CreateRenderTargetView(texture10.Get(), &rtv10_desc, &rtv10);
                  expect(hr == S_OK && rtv10, "D3D10 RTV returned %#lx", hr);
                  if (srv && rtv) {
                    context->ClearUnorderedAccessViewUint(ms_result_view.Get(), unwritten);
                    context->CSSetShader(array_view ? ms_array.Get() : ms.Get(), nullptr, 0);
                    ID3D11ShaderResourceView *source = srv.Get();
                    context->CSSetShaderResources(0, 1, &source);
                    ID3D11UnorderedAccessView *target = ms_result_view.Get();
                    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                    context->Dispatch(1, 1, 1);
                    auto words = read(device.Get(), context.Get(), ms_result.Get());
                    const float clear = 3;
                    UINT bits;
                    memcpy(&bits, &clear, sizeof(bits));
                    const UINT expected[] = {width, height, count, samples, bits};
                    if (expect(words.size() == std::size(expected), "MS readback has %zu words", words.size()))
                      for (UINT i = 0; i < std::size(expected); i++)
                        expect(words[i] == expected[i], "MS word=%u got=%#x expected=%#x", i, words[i], expected[i]);
                    context->ClearState();
                  }
                } else {
                  D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
                  dsv_desc.Format = format;
                  dsv_desc.ViewDimension =
                      array_view ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    dsv_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D11DepthStencilView> dsv;
                  HRESULT hr = device->CreateDepthStencilView(texture.Get(), &dsv_desc, &dsv);
                  expect(hr == S_OK && dsv, "DSV returned %#lx", hr);
                  if (dsv) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC actual{};
                    dsv->GetDesc(&actual);
                    expect(
                        actual.Format == format && actual.Flags == 0 &&
                            actual.ViewDimension == dsv_desc.ViewDimension &&
                            (!array_view || (actual.Texture2DMSArray.FirstArraySlice == first &&
                                             actual.Texture2DMSArray.ArraySize == count)),
                        "DSV description differs"
                    );
                    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
                  }
                  D3D10_DEPTH_STENCIL_VIEW_DESC dsv10_desc{};
                  dsv10_desc.Format = format;
                  dsv10_desc.ViewDimension =
                      array_view ? D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D10_DSV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    dsv10_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D10DepthStencilView> dsv10;
                  hr = device10->CreateDepthStencilView(texture10.Get(), &dsv10_desc, &dsv10);
                  expect(hr == S_OK && dsv10, "D3D10 DSV returned %#lx", hr);
                }
                if (samples == 1) {
                  auto staging_desc = desc;
                  staging_desc.Usage = D3D11_USAGE_STAGING;
                  staging_desc.BindFlags = 0;
                  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                  ComPtr<ID3D11Texture2D> staging;
                  CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
                  context->CopyResource(staging.Get(), texture.Get());
                  for (UINT slice = 0; slice < arrays; slice++) {
                    for (UINT mip = 0; mip < mip_count; mip++) {
                      const UINT subresource = D3D11CalcSubresource(mip, slice, mip_count);
                      D3D11_MAPPED_SUBRESOURCE mapped{};
                      CHECK(context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapped));
                      const float wanted =
                          mip == 0 && slice >= first && slice - first < count ? (depth ? 0.5f : 3.0f) : 0;
                      for (UINT y = 0; y < std::max(1u, height >> mip); y++) {
                        for (UINT x = 0; x < std::max(1u, width >> mip); x++) {
                          float actual;
                          memcpy(
                              &actual, (char *)mapped.pData + y * mapped.RowPitch + x * sizeof(float), sizeof(actual)
                          );
                          expect(
                              actual == wanted, "2D mip=%u slice=%u xy=%u,%u got=%g expected=%g", mip, slice, x, y,
                              actual, wanted
                          );
                        }
                      }
                      context->Unmap(staging.Get(), subresource);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  const UINT initial_value = 17;
  ComPtr<ID3D11Buffer> result;
  D3D11_BUFFER_DESC result_desc{4 * word_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
  CHECK(device->CreateBuffer(&result_desc, nullptr, &result));
  ComPtr<ID3D11UnorderedAccessView> result_view;
  D3D11_UNORDERED_ACCESS_VIEW_DESC result_view_desc{};
  result_view_desc.Format = DXGI_FORMAT_R32_UINT;
  result_view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  result_view_desc.Buffer.NumElements = result_desc.ByteWidth / word_bytes;
  CHECK(device->CreateUnorderedAccessView(result.Get(), &result_view_desc, &result_view));
  ComPtr<ID3D11ComputeShader> plane, plane_array;
  CHECK(device->CreateComputeShader(plane_code->GetBufferPointer(), plane_code->GetBufferSize(), nullptr, &plane));
  CHECK(device->CreateComputeShader(
      plane_array_code->GetBufferPointer(), plane_array_code->GetBufferSize(), nullptr, &plane_array
  ));
  for (UINT arrays : {1u, 3u}) {
    step("2D UAV metadata control arrays=%u", arrays);
    D3D11_TEXTURE2D_DESC desc{
        width, height, 1, arrays, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS
    };
    std::vector<UINT> zero(width * height);
    std::vector<D3D11_SUBRESOURCE_DATA> initial(arrays, {zero.data(), width * word_bytes, 0});
    ComPtr<ID3D11Texture2D> texture, staging;
    CHECK(device->CreateTexture2D(&desc, initial.data(), &texture));
    D3D11_UNORDERED_ACCESS_VIEW_DESC unknown{};
    unknown.Format = desc.Format;
    ComPtr<ID3D11UnorderedAccessView> invalid;
    HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), &unknown, &invalid);
    expect(hr == E_INVALIDARG && !invalid, "unknown UAV dimension returned %#lx", hr);
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav));
    context->ClearState();
    context->ClearUnorderedAccessViewUint(result_view.Get(), unwritten);
    context->CSSetShader(arrays > 1 ? plane_array.Get() : plane.Get(), nullptr, 0);
    ID3D11UnorderedAccessView *views[] = {uav.Get(), result_view.Get()};
    context->CSSetUnorderedAccessViews(0, 2, views, nullptr);
    context->Dispatch(1, 1, 1);
    auto words = read(device.Get(), context.Get(), result.Get());
    expect(
        words.size() == 4 && words[0] == width && words[1] == height && words[2] == arrays, "2D UAV dimensions differ"
    );
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture.Get());
    for (UINT slice = 0; slice < arrays; slice++) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      CHECK(context->Map(staging.Get(), slice, D3D11_MAP_READ, 0, &mapped));
      for (UINT y = 0; y < height; y++) {
        for (UINT x = 0; x < width; x++) {
          UINT actual;
          memcpy(&actual, (char *)mapped.pData + y * mapped.RowPitch + x * word_bytes, sizeof(actual));
          expect(
              actual == (x == 0 && y == 0 ? slice + 1 : 0), "2D UAV slice=%u xy=%u,%u value=%u", slice, x, y, actual
          );
        }
      }
      context->Unmap(staging.Get(), slice);
    }
  }
  ComPtr<ID3D11Texture2D> pixel_target;
  D3D11_TEXTURE2D_DESC pixel_desc{
      1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET
  };
  CHECK(device->CreateTexture2D(&pixel_desc, nullptr, &pixel_target));
  ComPtr<ID3D11RenderTargetView> pixel_target_view;
  CHECK(device->CreateRenderTargetView(pixel_target.Get(), nullptr, &pixel_target_view));
  for (bool floating : {false, true}) {
    const DXGI_FORMAT format = floating ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT;
    std::vector<std::vector<UINT>> initial(mips);
    std::vector<D3D11_SUBRESOURCE_DATA> data(mips);
    const float initial_float = float(initial_value);
    UINT initial_bits = initial_value;
    if (floating)
      memcpy(&initial_bits, &initial_float, sizeof(initial_bits));
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT w = std::max(1u, width >> mip), h = std::max(1u, height >> mip), d = std::max(1u, depth >> mip);
      initial[mip].assign(w * h * d, initial_bits);
      data[mip] = {initial[mip].data(), w * word_bytes, w * h * word_bytes};
    }
    D3D11_TEXTURE3D_DESC desc{width,
                              height,
                              depth,
                              mips,
                              format,
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS};
    ComPtr<ID3D11Texture3D> texture, staging;
    CHECK(device->CreateTexture3D(&desc, data.data(), &texture));
    ComPtr<ID3D10Texture3D> texture10;
    if (floating) {
      D3D10_TEXTURE3D_DESC desc10{width, height, depth, mips, format, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
      CHECK(device10->CreateTexture3D(&desc10, nullptr, &texture10));
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture3D(&desc, nullptr, &staging));
    auto reset = [&] {
      context->ClearState();
      for (UINT mip = 0; mip < mips; mip++)
        context->UpdateSubresource(
            texture.Get(), mip, nullptr, data[mip].pSysMem, data[mip].SysMemPitch, data[mip].SysMemSlicePitch
        );
    };
    auto verify = [&](UINT mip, UINT first, UINT count, UINT value, bool shader, UINT draw_plane = ~0u) {
      context->CopyResource(staging.Get(), texture.Get());
      for (UINT level_index = 0; level_index < mips; level_index++) {
        const UINT w = std::max(1u, width >> level_index), h = std::max(1u, height >> level_index);
        const UINT d = std::max(1u, depth >> level_index);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!expect(SUCCEEDED(context->Map(staging.Get(), level_index, D3D11_MAP_READ, 0, &mapped)), "readback failed"))
          continue;
        for (UINT z = 0; z < d; z++) {
          for (UINT y = 0; y < h; y++) {
            for (UINT x = 0; x < w; x++) {
              UINT wanted = initial_bits;
              const bool inside = level_index == mip && z >= first && z - first < count;
              if (inside && (!shader || (x == 0 && y == 0)))
                wanted = shader ? z - first + 1 : value;
              if (level_index == mip && z == draw_plane) {
                const float drawn = 7;
                memcpy(&wanted, &drawn, sizeof(wanted));
              }
              UINT actual;
              memcpy(
                  &actual, (char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch + x * sizeof(UINT),
                  sizeof(actual)
              );
              if (!expect(
                      actual == wanted, "mip=%u xyz=%u,%u,%u got=%#x expected=%#x", level_index, x, y, z, actual, wanted
                  )) {
                context->Unmap(staging.Get(), level_index);
                return;
              }
            }
          }
        }
        context->Unmap(staging.Get(), level_index);
      }
    };
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
      const UINT size = std::max(1u, depth >> mip);
      for (UINT bad_mip : {mip, mips, ~0u}) {
        for (UINT bad_first : {0u, size, ~0u}) {
          for (UINT bad_size : {0u, size + 1, ~0u}) {
            if (bad_mip == mip && bad_first == 0 && bad_size == ~0u)
              continue;
            step("invalid 3D float=%u mip=%u first=%u WSize=%u", floating, bad_mip, bad_first, bad_size);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
            uav_desc.Format = format;
            uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            uav_desc.Texture3D = {bad_mip, bad_first, bad_size};
            ComPtr<ID3D11UnorderedAccessView> uav;
            HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav);
            expect(hr == E_INVALIDARG && !uav, "invalid UAV returned %#lx", hr);
            if (floating) {
              D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
              rtv_desc.Format = format;
              rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
              rtv_desc.Texture3D = {bad_mip, bad_first, bad_size};
              ComPtr<ID3D11RenderTargetView> rtv;
              hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv);
              expect(hr == E_INVALIDARG && !rtv, "invalid RTV returned %#lx", hr);
            }
          }
        }
      }
      for (UINT first = 0; first < size; first++) {
        for (UINT requested = 1; requested <= size - first + 1; requested++) {
          const UINT wsize = requested == size - first + 1 ? ~0u : requested;
          const UINT count = wsize == ~0u ? size - first : wsize;
          step("3D float=%u mip=%u first=%u WSize=%u", floating, mip, first, wsize);
          D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
          uav_desc.Format = format;
          uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
          uav_desc.Texture3D = {mip, first, wsize};
          ComPtr<ID3D11UnorderedAccessView> uav;
          HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav);
          expect(hr == S_OK && uav, "3D UAV returned %#lx", hr);
          if (uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC actual{};
            uav->GetDesc(&actual);
            expect(
                actual.Texture3D.MipSlice == mip && actual.Texture3D.FirstWSlice == first &&
                    actual.Texture3D.WSize == count,
                "UAV W range differs"
            );
            for (bool raw : {false, true}) {
              if (!floating && !raw)
                continue;
              reset();
              UINT value = 5;
              if (raw) {
                const UINT clear[] = {5, 5, 5, 5};
                context->ClearUnorderedAccessViewUint(uav.Get(), clear);
              } else {
                const float clear[] = {5, 5, 5, 5};
                memcpy(&value, clear, sizeof(value));
                context->ClearUnorderedAccessViewFloat(uav.Get(), clear);
              }
              step("3D clear float=%u raw=%u mip=%u first=%u count=%u", floating, raw, mip, first, count);
              verify(mip, first, count, value, false);
            }
            if (!floating) {
              for (bool pixel : {false, true}) {
                for (UINT variant = 0; variant < 5; variant++) {
                  step("3D shader mip=%u first=%u count=%u pixel=%u variant=%u", mip, first, count, pixel, variant);
                  reset();
                  context->ClearUnorderedAccessViewUint(result_view.Get(), unwritten);
                  auto run = variant == 1 ? deferred.Get() : context.Get();
                  run->ClearState();
                  ID3D11UnorderedAccessView *views[] = {uav.Get(), result_view.Get()};
                  if (pixel) {
                    run->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                    run->VSSetShader(vs.Get(), nullptr, 0);
                    run->PSSetShader(
                        variant == 3   ? pixel_read_only.Get()
                        : variant == 4 ? pixel_write_only.Get()
                                       : pixel_uav.Get(),
                        nullptr, 0
                    );
                    run->RSSetState(rasterizer.Get());
                    D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
                    run->RSSetViewports(1, &viewport);
                    ID3D11RenderTargetView *target = pixel_target_view.Get();
                    run->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr, 1, 2, views, nullptr);
                  } else {
                    run->CSSetShader(
                        variant == 3   ? read_only.Get()
                        : variant == 4 ? write_only.Get()
                                       : cs.Get(),
                        nullptr, 0
                    );
                    run->CSSetUnorderedAccessViews(0, 2, views, nullptr);
                  }
                  if (variant == 2) {
                    deferred->ClearState();
                    ComPtr<ID3D11CommandList> empty;
                    CHECK(deferred->FinishCommandList(FALSE, &empty));
                    context->ExecuteCommandList(empty.Get(), TRUE);
                  }
                  if (pixel)
                    run->Draw(4, 0);
                  else
                    run->Dispatch(variant == 4 ? count + 1 : 1, 1, 1);
                  if (variant == 1) {
                    ComPtr<ID3D11CommandList> list;
                    CHECK(deferred->FinishCommandList(FALSE, &list));
                    context->ExecuteCommandList(list.Get(), TRUE);
                  }
                  if (variant != 4) {
                    auto words = read(device.Get(), context.Get(), result.Get());
                    const UINT expected[] = {w, h, count, 0};
                    if (expect(words.size() == std::size(expected), "3D readback has %zu words", words.size()))
                      for (UINT i = 0; i < std::size(expected); i++)
                        expect(words[i] == expected[i], "3D word=%u got=%#x expected=%#x", i, words[i], expected[i]);
                  }
                  verify(mip, first, count, initial_bits, variant != 3);
                }
              }
            }
          }
          if (floating) {
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rtv_desc.Texture3D = {mip, first, wsize};
            ComPtr<ID3D11RenderTargetView> rtv;
            hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv);
            expect(hr == S_OK && rtv, "3D RTV returned %#lx", hr);
            if (!rtv)
              continue;
            D3D11_RENDER_TARGET_VIEW_DESC actual{};
            rtv->GetDesc(&actual);
            expect(
                actual.Texture3D.MipSlice == mip && actual.Texture3D.FirstWSlice == first &&
                    actual.Texture3D.WSize == count,
                "RTV W range differs"
            );
            D3D10_RENDER_TARGET_VIEW_DESC rtv10_desc{};
            rtv10_desc.Format = format;
            rtv10_desc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE3D;
            rtv10_desc.Texture3D = {mip, first, wsize};
            ComPtr<ID3D10RenderTargetView> rtv10;
            hr = device10->CreateRenderTargetView(texture10.Get(), &rtv10_desc, &rtv10);
            expect(hr == S_OK && rtv10, "D3D10 3D RTV returned %#lx", hr);
            if (rtv10) {
              D3D10_RENDER_TARGET_VIEW_DESC actual10{};
              rtv10->GetDesc(&actual10);
              expect(
                  actual10.Texture3D.MipSlice == mip && actual10.Texture3D.FirstWSlice == first &&
                      actual10.Texture3D.WSize == count,
                  "D3D10 RTV W range differs"
              );
            }
            reset();
            const float clear[] = {5, 5, 5, 5};
            UINT clear_bits;
            memcpy(&clear_bits, clear, sizeof(clear_bits));
            context->ClearRenderTargetView(rtv.Get(), clear);
            verify(mip, first, count, clear_bits, false);
            context->ClearRenderTargetView(rtv.Get(), clear);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps.Get(), nullptr, 0);
            context->RSSetState(rasterizer.Get());
            D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
            context->RSSetViewports(1, &viewport);
            const UINT draw_plane = count == 1 && size > 1 ? (first + 1) % size : first;
            rtv_desc.Texture3D = {mip, draw_plane, count};
            ComPtr<ID3D11RenderTargetView> draw_view;
            CHECK(device->CreateRenderTargetView(texture.Get(), &rtv_desc, &draw_view));
            ID3D11RenderTargetView *target = draw_view.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->Draw(4, 0);
            verify(mip, first, count, clear_bits, false, draw_plane);
          }
        }
      }
    }
  }
  for (const auto &[kind, failure] : failures)
    printf("wrong: %u x %s\n", failure.first, failure.second.c_str());
  return verdict();
}
