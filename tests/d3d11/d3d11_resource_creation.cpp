// contract: creation rejects incompatible descriptions and null data in any supplied subresource, including
// validation-only calls; accepted descriptions retain their fields, with full mip chains and ignored strides
// normalized. D3D11.3 5.4.2: "StrideInBytes must be non-zero and ByteWidth must be evenly divisible by
// StructureByteStride." "The StructureByteStride can be at most 2048 bytes." 5.3.8: "A TextureCube has 6 faces,
// each of which is like a square Texture2D, including mipmaps." 5.3.5.3/5.3.6.3: "Resources created with this
// Pipeline Bind flag cannot also be used as a RenderTarget (the two flags are mutually exclusive)."
// Microsoft Learn, CreateTexture2D: "If the resource is multisampled, pInitialData must be NULL";
// CreateBuffer: "you must set the ByteWidth value of D3D11_BUFFER_DESC in multiples of 16".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createbuffer
// D3D11_SUBRESOURCE_DATA, pSysMem: "Pointer to the initialization data." invalid parameters return E_INVALIDARG.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_subresource_data
// Wine's test_create_buffer and test_create_texture{1,2,3}d in d3d11.c and d3d10core.c record Windows' refusals.
// two details contradict the specification's restrictions: Windows ignores non-structured strides and accepts
// trailing cube slices at feature level 10.1 and above. format/bind enumerations come from the SDK; expectations
// for typed formats use the queried support contract, not a second format table. typeless parents permit typed
// views; a depth stencil plane's format is fully typed (Microsoft Learn, format support for feature level 11.0
// hardware), so the same contract holds for it. trailing cube slices are
// initialized distinctly, sampled through cube and array views, and read back with blocking maps.
// Wine's test_resource_access records rejection of dynamic buffers without any bind flags in D3D10 and D3D11.
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <d3d11_3.h>
#include <bit>
#include <algorithm>
#include <limits>
#include <type_traits>
#include "../resource_cases.hpp"

template <typename Desc>
static void
texture(
    ID3D11Device *device, ID3D11Device3 *device3, ID3D10Device *device10, UINT route, Desc desc,
    const D3D11_SUBRESOURCE_DATA *data, bool output, HRESULT wanted
) {
  constexpr bool one = std::is_same_v<Desc, D3D11_TEXTURE1D_DESC>;
  constexpr bool two = std::is_same_v<Desc, D3D11_TEXTURE2D_DESC>;
  using Texture11 = std::conditional_t<one, ID3D11Texture1D, std::conditional_t<two, ID3D11Texture2D, ID3D11Texture3D>>;
  using Texture10 = std::conditional_t<one, ID3D10Texture1D, std::conditional_t<two, ID3D10Texture2D, ID3D10Texture3D>>;
  using Desc10 = std::conditional_t<
      one, D3D10_TEXTURE1D_DESC, std::conditional_t<two, D3D10_TEXTURE2D_DESC, D3D10_TEXTURE3D_DESC>>;
  auto sentinel11 = reinterpret_cast<Texture11 *>(device);
  auto sentinel10 = reinterpret_cast<Texture10 *>(device10);
  auto object11 = sentinel11;
  auto object10 = sentinel10;
  HRESULT hr;
  static_assert(sizeof(D3D10_SUBRESOURCE_DATA) == sizeof(D3D11_SUBRESOURCE_DATA));
  if (route == 10) {
    auto desc10 = std::bit_cast<Desc10>(desc);
    auto data10 = reinterpret_cast<const D3D10_SUBRESOURCE_DATA *>(data);
    if constexpr (one)
      hr = device10->CreateTexture1D(&desc10, data10, output ? &object10 : nullptr);
    else if constexpr (two)
      hr = device10->CreateTexture2D(&desc10, data10, output ? &object10 : nullptr);
    else
      hr = device10->CreateTexture3D(&desc10, data10, output ? &object10 : nullptr);
  } else if (route == 3 && !one) {
    if constexpr (two) {
      D3D11_TEXTURE2D_DESC1 desc1{};
      memcpy(&desc1, &desc, sizeof(desc));
      hr = device3->CreateTexture2D1(&desc1, data, output ? reinterpret_cast<ID3D11Texture2D1 **>(&object11) : nullptr);
    } else if constexpr (!one) {
      D3D11_TEXTURE3D_DESC1 desc1{};
      memcpy(&desc1, &desc, sizeof(desc));
      hr = device3->CreateTexture3D1(&desc1, data, output ? reinterpret_cast<ID3D11Texture3D1 **>(&object11) : nullptr);
    }
  } else {
    if constexpr (one)
      hr = device->CreateTexture1D(&desc, data, output ? &object11 : nullptr);
    else if constexpr (two)
      hr = device->CreateTexture2D(&desc, data, output ? &object11 : nullptr);
    else
      hr = device->CreateTexture3D(&desc, data, output ? &object11 : nullptr);
  }
  expect(hr == wanted, "creation returned %#lx, want %#lx", hr, wanted);
  if (!output)
    return;
  expect(
      SUCCEEDED(wanted) ? (route == 10 ? object10 && object10 != sentinel10 : object11 && object11 != sentinel11)
                        : (route == 10 ? !object10 : !object11),
      "creation output is incorrect"
  );
  if (!desc.MipLevels) {
    UINT extent = desc.Width;
    if constexpr (!one)
      extent = std::max(extent, desc.Height);
    if constexpr (!one && !two)
      extent = std::max(extent, desc.Depth);
    desc.MipLevels = 1;
    while (extent > 1) {
      extent /= 2;
      ++desc.MipLevels;
    }
  }
  if (route == 10 && object10 && object10 != sentinel10) {
    if (wanted == S_OK) {
      Desc10 got{};
      object10->GetDesc(&got);
      auto expected = std::bit_cast<Desc10>(desc);
      expect(!memcmp(&got, &expected, sizeof(got)), "description changed");
    }
    object10->Release();
  } else if (route != 10 && object11 && object11 != sentinel11) {
    if (wanted == S_OK) {
      Desc got{};
      object11->GetDesc(&got);
      expect(!memcmp(&got, &desc, sizeof(got)), "description changed");
    }
    object11->Release();
  }
}

int
main() {
  constexpr UINT constant_bytes = sizeof(float) * D3D11_COMMONSHADER_CONSTANT_BUFFER_COMPONENTS;
  constexpr UINT structure_limit = D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES;
  constexpr UINT cube_faces = 6; // D3D11.3 5.3.8
  const char shader[] = R"(
TextureCubeArray<float4> cubemap : register(t0);
Texture2DArray<float4> slices : register(t1);
SamplerState faceSampler : register(s0);
RWStructuredBuffer<uint2> result : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height, cubes;
  cubemap.GetDimensions(width, height, cubes);
  uint face = id.x % CUBE_FACES;
  float3 direction = (float3(0, 1, 2) == face / 2) * ((face & 1) ? -1.0 : 1.0);
  uint sampled = 0;
  if (id.x < cubes * CUBE_FACES)
    sampled = (uint)round(cubemap.SampleLevel(faceSampler, float4(direction, id.x / CUBE_FACES), 0).r * CHANNEL_MAX);
  uint loaded = (uint)round(slices.Load(int4(0, 0, id.x, 0)).r * CHANNEL_MAX);
  result[id.x] = uint2(sampled, loaded);
})";
  step("cube and array readback shader");
  auto code = compile(
      shader, "main", "cs",
      {"CUBE_FACES=" + std::to_string(cube_faces), "CHANNEL_MAX=" + std::to_string(std::numeric_limits<uint8_t>::max())}
  );
  if (!expect(!!code, "cube shader did not compile"))
    return verdict();
  auto module = LoadLibraryW(L"d3d10_1.dll");
  if (!expect(!!module, "d3d10_1.dll did not load"))
    return verdict();
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(GetProcAddress(module, "D3D10CreateDevice1"));
  if (!expect(!!create10, "D3D10CreateDevice1 is absent")) {
    FreeLibrary(module);
    return verdict();
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device3> device3;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D10Device1> device10;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context
  ));
  CHECK(device.As(&device3));
  CHECK(create10(
      nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_0, D3D10_1_SDK_VERSION, &device10
  ));
  ComPtr<ID3D11ComputeShader> compute;
  CHECK(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &compute));
  auto map = [&](ID3D11Resource *resource, UINT subresource, D3D11_MAPPED_SUBRESOURCE &mapped) {
    return context->Map(resource, subresource, D3D11_MAP_READ, 0, &mapped);
  };
  for (UINT route : {11u, 10u}) {
    auto check_buffer = [&](D3D11_BUFFER_DESC desc, bool output, HRESULT wanted) {
      auto sentinel11 = reinterpret_cast<ID3D11Buffer *>(device.Get());
      auto sentinel10 = reinterpret_cast<ID3D10Buffer *>(device10.Get());
      auto object11 = sentinel11;
      auto object10 = sentinel10;
      HRESULT hr;
      if (route == 10) {
        D3D10_BUFFER_DESC desc10{
            desc.ByteWidth, D3D10_USAGE(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags
        };
        hr = device10->CreateBuffer(&desc10, nullptr, output ? &object10 : nullptr);
      } else {
        hr = device->CreateBuffer(&desc, nullptr, output ? &object11 : nullptr);
      }
      expect(hr == wanted, "buffer returned %#lx, want %#lx", hr, wanted);
      if (!output)
        return;
      expect(
          wanted == S_OK ? (route == 10 ? object10 && object10 != sentinel10 : object11 && object11 != sentinel11)
                         : (route == 10 ? !object10 : !object11),
          "buffer output is incorrect"
      );
      if (route == 10 && object10 && object10 != sentinel10) {
        if (wanted == S_OK) {
          D3D10_BUFFER_DESC got{};
          object10->GetDesc(&got);
          D3D10_BUFFER_DESC expected{
              desc.ByteWidth, D3D10_USAGE(desc.Usage), desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags
          };
          expect(!memcmp(&got, &expected, sizeof(got)), "D3D10 description changed");
        }
        object10->Release();
      } else if (route == 11 && object11 && object11 != sentinel11) {
        if (wanted == S_OK) {
          D3D11_BUFFER_DESC got{};
          object11->GetDesc(&got);
          auto expected = desc;
          if (!(expected.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
            expected.StructureByteStride = 0;
          expect(!memcmp(&got, &expected, sizeof(got)), "buffer description changed");
        }
        object11->Release();
      }
    };
    for (bool output : {true, false}) {
      for (UINT use :
           {UINT(D3D11_USAGE_DEFAULT), UINT(D3D11_USAGE_IMMUTABLE), UINT(D3D11_USAGE_DYNAMIC),
            UINT(D3D11_USAGE_STAGING), ~0u}) {
        step("D3D%u buffer usage %u output %u", route, use, output);
        UINT cpu = use == D3D11_USAGE_DYNAMIC   ? D3D11_CPU_ACCESS_WRITE
                   : use == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ
                                                : 0;
        D3D11_BUFFER_DESC desc{constant_bytes, D3D11_USAGE(use), 0, cpu, 0, structure_limit + 1};
        check_buffer(
            desc, output,
            use == D3D11_USAGE_IMMUTABLE || use == D3D11_USAGE_DYNAMIC || use > D3D11_USAGE_STAGING
                ? E_INVALIDARG : (output ? S_OK : S_FALSE)
        );
      }
      for (UINT size = 0; size <= 2 * constant_bytes + 1; ++size) {
        step("D3D%u constant bytes %u output %u", route, size, output);
        D3D11_BUFFER_DESC desc{size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
        check_buffer(desc, output, size && !(size % constant_bytes) ? (output ? S_OK : S_FALSE) : E_INVALIDARG);
      }
      UINT valid10 = 0;
      for (UINT misc : resource_misc10)
        valid10 |= misc;
      for (UINT bind : resource_binds) {
        for (UINT flags :
             {0u, UINT(D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS), UINT(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED),
              UINT(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)}) {
          if (bind > D3D11_BIND_UNORDERED_ACCESS)
            continue;
          // the bit has another meaning in Direct3D 10, which the rule below does not describe
          if (route == 10 && (flags & valid10))
            continue;
          for (UINT size :
               {structure_limit - 1, structure_limit, structure_limit + 1, structure_limit + UINT(sizeof(UINT))}) {
            for (UINT stride :
                 {0u, UINT(sizeof(UINT) - 1), UINT(sizeof(UINT)), UINT(sizeof(UINT) + 1), structure_limit - 1,
                  structure_limit, structure_limit + 1, structure_limit + UINT(sizeof(UINT)), ~0u}) {
              step(
                  "D3D%u buffer bytes %u bind %#x misc %#x stride %u output %u", route, size, bind, flags, stride,
                  output
              );
              bool structured = flags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
              bool raw = flags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
              bool valid = !(bind & D3D11_BIND_CONSTANT_BUFFER) || !(size % constant_bytes);
              valid &= bind != D3D11_BIND_DEPTH_STENCIL;
              if (structured)
                valid &= stride && stride <= structure_limit && !(stride % sizeof(UINT)) &&
                         !(size % (stride ? stride : 1)) &&
                         !(bind & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) && !raw;
              if (raw)
                valid &= !!(bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS));
              if (route == 10)
                valid &= !(flags & ~valid10);
              D3D11_BUFFER_DESC desc{size, D3D11_USAGE_DEFAULT, bind, 0, flags, stride};
              check_buffer(desc, output, valid ? (output ? S_OK : S_FALSE) : E_INVALIDARG);
            }
          }
        }
        if (bind && bind != D3D11_BIND_CONSTANT_BUFFER) {
          step("D3D%u constant bind %#x output %u", route, bind, output);
          D3D11_BUFFER_DESC desc{constant_bytes, D3D11_USAGE_DEFAULT, bind | D3D11_BIND_CONSTANT_BUFFER};
          check_buffer(desc, output, E_INVALIDARG);
        }
      }
    }
  }

  for (UINT route : {11u, 10u, 3u}) {
    for (UINT dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D; dimension <= D3D11_RESOURCE_DIMENSION_TEXTURE3D;
         ++dimension) {
      auto check = [&](UINT width, UINT height, UINT depth, UINT mips, UINT array, DXGI_FORMAT format, UINT bind,
                       UINT misc, D3D11_USAGE usage, DXGI_SAMPLE_DESC samples, const D3D11_SUBRESOURCE_DATA *data,
                       bool output, HRESULT wanted) {
        UINT cpu = usage == D3D11_USAGE_DYNAMIC   ? D3D11_CPU_ACCESS_WRITE
                   : usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ
                                                  : 0;
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
          D3D11_TEXTURE1D_DESC desc{width, mips, array, format, usage, bind, cpu, misc};
          texture(device.Get(), device3.Get(), device10.Get(), route, desc, data, output, wanted);
        } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
          D3D11_TEXTURE2D_DESC desc{width, height, mips, array, format, samples, usage, bind, cpu, misc};
          texture(device.Get(), device3.Get(), device10.Get(), route, desc, data, output, wanted);
        } else {
          D3D11_TEXTURE3D_DESC desc{width, height, depth, mips, format, usage, bind, cpu, misc};
          texture(device.Get(), device3.Get(), device10.Get(), route, desc, data, output, wanted);
        }
      };
      for (bool output : {true, false}) {
        for (UINT mips : {0u, 1u, UINT(std::bit_width(8u)), UINT(std::bit_width(8u) + 1)}) {
          step("D3D%u dimension %u mip boundary %u output %u", route, dimension, mips, output);
          check(
              1, dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? 8 : 1,
              dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 8 : 1, mips, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
              D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DEFAULT, {1, 0}, nullptr, output,
              mips > (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1u : UINT(std::bit_width(8u)))
                  ? E_INVALIDARG
                  : (output ? S_OK : S_FALSE)
          );
        }
        for (UINT use : {UINT(D3D11_USAGE_STAGING), ~0u}) {
          step("D3D%u dimension %u invalid usage/bind %u output %u", route, dimension, use, output);
          check(
              4, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE(use), {1, 0},
              nullptr, output, E_INVALIDARG
          );
        }
        step("D3D%u dimension %u invalid cube shape output %u", route, dimension, output);
        check(
            4, 2, 4, 1, cube_faces, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
            D3D11_RESOURCE_MISC_TEXTURECUBE, D3D11_USAGE_DEFAULT, {1, 0}, nullptr, output, E_INVALIDARG
        );
        for (UINT bind :
             {UINT(D3D11_BIND_DEPTH_STENCIL), UINT(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE),
              UINT(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_RENDER_TARGET),
              UINT(D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS)}) {
          bool valid = dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D &&
                       !(bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS));
          step("D3D%u dimension %u typeless depth bind %#x output %u", route, dimension, bind, output);
          check(
              4, 4, 4, 1, 1, DXGI_FORMAT_R32_TYPELESS, bind, 0, D3D11_USAGE_DEFAULT, {1, 0}, nullptr, output,
              valid ? (output ? S_OK : S_FALSE) : E_INVALIDARG
          );
        }
        for (UINT use = D3D11_USAGE_DEFAULT; use <= D3D11_USAGE_STAGING; ++use) {
          auto usage = D3D11_USAGE(use);
          UINT bind = usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
          for (UINT mips : {0u, 1u, 3u}) {
            if (usage == D3D11_USAGE_DYNAMIC && mips != 1)
              continue;
            const UINT width = 4, height = 4, depth = 4;
            UINT levels = mips ? mips : std::bit_width(width | height | depth);
            UINT array = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D || usage == D3D11_USAGE_DYNAMIC ? 1 : 2;
            std::vector<UINT> bytes(width * height * depth);
            std::vector<D3D11_SUBRESOURCE_DATA> data(
                levels * array, {bytes.data(), width * sizeof(UINT), width * height * sizeof(UINT)}
            );
            step("D3D%u dimension %u usage %u mips %u output %u: valid data", route, dimension, use, mips, output);
            check(
                width, height, depth, mips, array, DXGI_FORMAT_R8G8B8A8_UNORM, bind, 0, usage, {1, 0}, data.data(),
                output, output ? S_OK : S_FALSE
            );
            for (UINT missing = 0; missing < data.size(); ++missing) {
              step(
                  "D3D%u dimension %u usage %u mips %u null subresource %u output %u", route, dimension, use, mips,
                  missing, output
              );
              auto saved = data[missing].pSysMem;
              data[missing].pSysMem = nullptr;
              check(
                  width, height, depth, mips, array, DXGI_FORMAT_R8G8B8A8_UNORM, bind, 0, usage, {1, 0}, data.data(),
                  output, E_INVALIDARG
              );
              data[missing].pSysMem = saved;
            }
            step("D3D%u dimension %u usage %u mips %u omitted data output %u", route, dimension, use, mips, output);
            check(
                width, height, depth, mips, array, DXGI_FORMAT_R8G8B8A8_UNORM, bind, 0, usage, {1, 0}, nullptr, output,
                usage == D3D11_USAGE_IMMUTABLE ? E_INVALIDARG : (output ? S_OK : S_FALSE)
            );
          }
        }
        UINT limit = route == 10 ? D3D10_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION : D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
        if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
          for (UINT array :
               {0u, 1u, cube_faces - 1, cube_faces, cube_faces + 1, 2 * cube_faces, limit - 1, limit, limit + 1}) {
            for (UINT misc : {0u, UINT(D3D11_RESOURCE_MISC_TEXTURECUBE)}) {
              if (misc && dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
                continue;
              bool valid =
                  array && array <= limit &&
                  (!misc || (array >= cube_faces && route != 10) || (misc && route == 10 && array == cube_faces));
              step("D3D%u dimension %u array %u misc %#x output %u", route, dimension, array, misc, output);
              check(
                  1, 1, 1, 1, array, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, misc, D3D11_USAGE_DEFAULT,
                  {1, 0}, nullptr, output, valid ? (output ? S_OK : S_FALSE) : E_INVALIDARG
              );
            }
          }
        }
        for (const auto &format : resource_formats) {
          const auto suffix = strchr(format.name + strlen("DXGI_FORMAT_"), '_');
          bool parent = suffix && !strcmp(suffix, "_TYPELESS");
          UINT support = 0;
          HRESULT supported = route == 10 ? device10->CheckFormatSupport(format.value, &support)
                                          : device->CheckFormatSupport(format.value, &support);
          UINT needed = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D   ? D3D11_FORMAT_SUPPORT_TEXTURE1D
                        : dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? D3D11_FORMAT_SUPPORT_TEXTURE2D
                                                                          : D3D11_FORMAT_SUPPORT_TEXTURE3D;
          for (UINT bind : resource_binds) {
            if (route == 10 && bind >= D3D11_BIND_UNORDERED_ACCESS)
              continue;
            // a typeless parent's depth binding is determined by its typed view, exercised above
            if (parent && bind == D3D11_BIND_DEPTH_STENCIL && !(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL))
              continue;
            bool valid = SUCCEEDED(supported) && (support & needed);
            if (bind == D3D11_BIND_RENDER_TARGET)
              valid &= !!(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
            else if (bind == D3D11_BIND_DEPTH_STENCIL)
              valid &=
                  dimension != D3D11_RESOURCE_DIMENSION_TEXTURE3D && !!(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL);
            else if (bind == D3D11_BIND_UNORDERED_ACCESS)
              valid &= !!(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
            // a typeless format's own row lists no shader operations (Format support for Direct3D Feature Level 11.0
            // hardware), yet Windows makes its textures for shader resources, read through its family's typed views
            // (Wine's d3d11 tests, test_create_texture2d: R32G32B32A32_TYPELESS and R24G8_TYPELESS succeed)
            else if (bind == D3D11_BIND_SHADER_RESOURCE)
              valid &= parent || (!!(support & D3D11_FORMAT_SUPPORT_SHADER_LOAD) &&
                                  !(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL));
            else if (bind)
              valid = false;
            // D3D11.3 19.1.4: shared-exponent data is for shader reads, even on Metal formats that render it
            if (format.value == DXGI_FORMAT_R9G9B9E5_SHAREDEXP && bind && bind != D3D11_BIND_SHADER_RESOURCE)
              valid = false;
            // the XR format has no views of its own: it is bound through the formats it casts to (its row: "Cast
            // Within Bit Layout", "BackBuffer Castable Even Fully Typed"), which the row's other targets do not decide
            if (format.value == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM && bind && (support & needed))
              continue;
            step("D3D%u dimension %u format %s bind %#x output %u", route, dimension, format.name, bind, output);
            check(
                4, 4, 4, 1, 1, format.value, bind, 0, D3D11_USAGE_DEFAULT, {1, 0}, nullptr, output,
                valid ? (output ? S_OK : S_FALSE) : E_INVALIDARG
            );
          }
        }
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
          for (UINT count = 0; count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT + 1; ++count) {
            // of the device that makes the texture: the Direct3D 10 one, of feature level 10_0, need not have the 8
            // samples a feature level 11_0 device has whatever the GPU
            UINT qualities = 0;
            if (route == 10)
              device10->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, count, &qualities);
            else
              device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, count, &qualities);
            for (UINT quality :
                 {0u, qualities, qualities + 1, UINT(D3D11_STANDARD_MULTISAMPLE_PATTERN),
                  UINT(D3D11_CENTER_MULTISAMPLE_PATTERN)}) {
              for (UINT mips : {0u, 1u, 2u}) {
                bool pattern =
                    quality == D3D11_STANDARD_MULTISAMPLE_PATTERN || quality == D3D11_CENTER_MULTISAMPLE_PATTERN;
                bool valid =
                    qualities && (quality < qualities || (pattern && route != 10)) && (count == 1 || mips == 1);
                step("D3D%u samples %u quality %u mips %u output %u", route, count, quality, mips, output);
                check(
                    4, 4, 1, mips, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, 0, D3D11_USAGE_DEFAULT,
                    {count, quality}, nullptr, output, valid ? (output ? S_OK : S_FALSE) : E_INVALIDARG
                );
              }
            }
            if (count > 1 && qualities) {
              UINT bytes[4 * 4]{};
              D3D11_SUBRESOURCE_DATA data{bytes, 4 * sizeof(UINT), sizeof(bytes)};
              for (UINT use = D3D11_USAGE_DEFAULT; use <= D3D11_USAGE_STAGING; ++use) {
                step("D3D%u samples %u usage %u output %u", route, count, use, output);
                check(
                    4, 4, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, 0, D3D11_USAGE(use),
                    {count, 0}, nullptr, output, use == D3D11_USAGE_DEFAULT ? (output ? S_OK : S_FALSE) : E_INVALIDARG
                );
              }
              step("D3D%u samples %u supplied data output %u", route, count, output);
              check(
                  4, 4, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, 0, D3D11_USAGE_DEFAULT,
                  {count, 0}, &data, output, E_INVALIDARG
              );
              step("D3D%u samples %u cube output %u", route, count, output);
              check(
                  4, 4, 1, 1, cube_faces, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                  D3D11_RESOURCE_MISC_TEXTURECUBE, D3D11_USAGE_DEFAULT, {count, 0}, nullptr, output, E_INVALIDARG
              );
            }
          }
        }
      }
    }
  }
  for (UINT array : {cube_faces, cube_faces + 1, 2 * cube_faces - 1, 2 * cube_faces, 2 * cube_faces + 1}) {
    step("cube array %u: initialized faces and view", array);
    std::vector<UINT> values(array);
    std::vector<D3D11_SUBRESOURCE_DATA> data(array);
    for (UINT slice = 0; slice < array; ++slice) {
      values[slice] = slice + 1;
      data[slice] = {&values[slice], sizeof(UINT), sizeof(UINT)};
    }
    D3D11_TEXTURE2D_DESC desc{
        1,
        1,
        1,
        array,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        {1, 0},
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE,
        0,
        D3D11_RESOURCE_MISC_TEXTURECUBE
    };
    ComPtr<ID3D11Texture2D> cube, staging;
    CHECK(device->CreateTexture2D(&desc, data.data(), &cube));
    D3D11_SHADER_RESOURCE_VIEW_DESC view{desc.Format, D3D11_SRV_DIMENSION_TEXTURECUBEARRAY};
    view.TextureCubeArray = {0, desc.MipLevels, 0, ~0u};
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK(device->CreateShaderResourceView(cube.Get(), &view, &srv));
    srv->GetDesc(&view);
    expect(view.TextureCubeArray.NumCubes == array / cube_faces, "view includes an incomplete cube");
    D3D11_SHADER_RESOURCE_VIEW_DESC array_view{desc.Format, D3D11_SRV_DIMENSION_TEXTURE2DARRAY};
    array_view.Texture2DArray = {0, desc.MipLevels, 0, array};
    ComPtr<ID3D11ShaderResourceView> slices;
    CHECK(device->CreateShaderResourceView(cube.Get(), &array_view, &slices));
    struct Pixel {
      UINT cube, array;
    };
    D3D11_BUFFER_DESC result_desc{UINT(array * sizeof(Pixel)),           D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_UNORDERED_ACCESS,           0,
                                  D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Pixel)};
    ComPtr<ID3D11Buffer> result, readback;
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateBuffer(&result_desc, nullptr, &result));
    CHECK(device->CreateUnorderedAccessView(result.Get(), nullptr, &uav));
    result_desc.Usage = D3D11_USAGE_STAGING;
    result_desc.BindFlags = result_desc.MiscFlags = result_desc.StructureByteStride = 0;
    result_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateBuffer(&result_desc, nullptr, &readback));
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.MaxAnisotropy = 1;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    CHECK(device->CreateSamplerState(&sampler_desc, &sampler));
    step("cube array %u: sample through cube and array views", array);
    ID3D11ShaderResourceView *views[] = {srv.Get(), slices.Get()};
    context->CSSetShaderResources(0, std::size(views), views);
    context->CSSetSamplers(0, 1, sampler.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    context->CSSetShader(compute.Get(), nullptr, 0);
    context->Dispatch(array, 1, 1);
    ID3D11UnorderedAccessView *unbound = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &unbound, nullptr);
    context->CopyResource(readback.Get(), result.Get());
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE sampled{};
    HRESULT sampled_hr = map(readback.Get(), 0, sampled);
    if (!expect(sampled_hr == S_OK && sampled.pData, "sample readback returned %#lx", sampled_hr))
      return verdict();
    for (UINT slice = 0; slice < array; ++slice) {
      Pixel got;
      memcpy(&got, static_cast<const char *>(sampled.pData) + slice * sizeof(Pixel), sizeof(got));
      UINT expected_cube = slice < array / cube_faces * cube_faces ? values[slice] : 0;
      expect(
          got.cube == expected_cube && got.array == values[slice], "slice %u sampled %u/%u, want %u/%u", slice,
          got.cube, got.array, expected_cube, values[slice]
      );
    }
    context->Unmap(readback.Get(), 0);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), cube.Get());
    context->Flush();
    for (UINT slice = 0; slice < array; ++slice) {
      step("cube array %u slice %u: read bytes", array, slice);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      HRESULT hr = map(staging.Get(), slice, mapped);
      if (!expect(hr == S_OK && mapped.pData, "Map returned %#lx", hr))
        return verdict();
      UINT got;
      memcpy(&got, mapped.pData, sizeof(got));
      expect(got == values[slice], "slice returned %#x, want %#x", got, values[slice]);
      context->Unmap(staging.Get(), slice);
    }
  }
  for (UINT use : {UINT(D3D11_USAGE_DEFAULT), UINT(D3D11_USAGE_DYNAMIC)}) {
    for (UINT misc : {0u, UINT(D3D11_RESOURCE_MISC_TEXTURECUBE)}) {
      step("cube view eligibility usage %u misc %#x", use, misc);
      D3D11_TEXTURE2D_DESC desc{
          1,
          1,
          1,
          cube_faces,
          DXGI_FORMAT_R8G8B8A8_UNORM,
          {1, 0},
          D3D11_USAGE(use),
          D3D11_BIND_SHADER_RESOURCE,
          use == D3D11_USAGE_DYNAMIC ? UINT(D3D11_CPU_ACCESS_WRITE) : 0,
          misc
      };
      ComPtr<ID3D11Texture2D> array;
      CHECK(device->CreateTexture2D(&desc, nullptr, &array));
      for (auto dimension : {D3D11_SRV_DIMENSION_TEXTURECUBE, D3D11_SRV_DIMENSION_TEXTURECUBEARRAY}) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{desc.Format, dimension};
        if (dimension == D3D11_SRV_DIMENSION_TEXTURECUBE)
          view.TextureCube = {0, desc.MipLevels};
        else
          view.TextureCubeArray = {0, desc.MipLevels, 0, 1};
        HRESULT hr = device->CreateShaderResourceView(array.Get(), &view, nullptr);
        expect(hr == (misc ? S_FALSE : E_INVALIDARG), "cube eligibility returned %#lx", hr);
      }
    }
  }
  device10.Reset();
  FreeLibrary(module);
  return verdict();
}
