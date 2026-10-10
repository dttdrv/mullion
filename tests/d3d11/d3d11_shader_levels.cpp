// contract: shader creation is limited by the device's chosen feature level, including validation-only calls.
// "DirectX 11 only supports up to shader model 5.0" (Microsoft Learn, Direct3D feature levels, footnote 2).
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro
// "Only use legacy Direct3D 9 shader models with the Direct3D 9 API." (Microsoft Learn, Specifying Compiler
// Targets, note 3). its tables assign SM4.0 to FL10.0, SM4.1 to FL10.1 and SM5.0 to FL11.0; level 9 uses
// *_4_0_level_9_x containers. D3D11.3 18.7.2.4: "Beyond the VS/GS/PS, an additional shader type is available on
// downlevel HW: Compute Shader, via shader models CS_4_0 and CS_4_1."
// https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/specifying-compiler-targets
// Microsoft's shader reflection supplies each ordinary shader's minimum feature level. the simple level 9
// shaders are accepted at all three FL9 levels, as Wine's d3d11 test_create_shader records for Windows.
// every D3D11 stage in Microsoft's tokenized-program header is tried at each minimum and on either side,
// with and without an output, twice to exercise cached shaders too. VS and GS also go through stream output,
// which requires FL10.0 (Direct3D feature levels, Stream Out row); VS/GS/PS through D3D10, including level 9.
// the version token's whole domain is also generated in an otherwise unchanged, signed container; only the three
// ordinary versions listed by the specification are valid here; hull and domain shaders require SM5.0.
#define INITGUID
#include "d3d11_test.hpp"
#include <d3d10.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <set>
#include "../../libs/DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "../../libs/DXBCParser/BlobContainer.h"
#include "../dxbc_hash.hpp"

using namespace microsoft;

static const char legacy_hlsl[] = R"hlsl(
float4 vs(float4 pos : POSITION) : POSITION { return pos; }
float4 ps(float4 color : COLOR) : COLOR { return color; }
)hlsl";

static const char hlsl[] = R"hlsl(
struct Vertex { float4 pos : SV_Position; };
Vertex vs(float4 pos : POSITION) { Vertex v; v.pos = pos; return v; }
float4 ps(float4 color : COLOR) : SV_Target { return color; }
[maxvertexcount(1)]
void gs(point Vertex corners[1], inout PointStream<Vertex> stream) { stream.Append(corners[0]); }
struct Factors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors constants(InputPatch<Vertex, 3> vertices) {
  Factors f; f.edges[0] = f.edges[1] = f.edges[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("constants")]
Vertex hs(InputPatch<Vertex, 3> vertices, uint id : SV_OutputControlPointID) { return vertices[id]; }
[domain("tri")]
Vertex ds(Factors factors, float3 bary : SV_DomainLocation, const OutputPatch<Vertex, 3> vertices) {
  Vertex v; v.pos = bary.x * vertices[0].pos + bary.y * vertices[1].pos + bary.z * vertices[2].pos; return v;
}
RWStructuredBuffer<uint> result : register(u0);
[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { result[id.x] = id.x; }
)hlsl";

int
main() {
  struct Program {
    UINT stage;
    std::string profile;
    D3D_FEATURE_LEVEL minimum;
    bool supported;
    std::string code;
  };
  std::vector<Program> programs;
  std::set<D3D_FEATURE_LEVEL> levels{D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_11_1};
  for (UINT stage = D3D10_SB_PIXEL_SHADER; stage <= D3D11_SB_COMPUTE_SHADER; stage++) {
    const char *entry = nullptr;
    switch (stage) {
    case D3D10_SB_PIXEL_SHADER:
      entry = "ps";
      break;
    case D3D10_SB_VERTEX_SHADER:
      entry = "vs";
      break;
    case D3D10_SB_GEOMETRY_SHADER:
      entry = "gs";
      break;
    case D3D11_SB_HULL_SHADER:
      entry = "hs";
      break;
    case D3D11_SB_DOMAIN_SHADER:
      entry = "ds";
      break;
    case D3D11_SB_COMPUTE_SHADER:
      entry = "cs";
      break;
    }
    for (const char *model : {"4_0", "4_1", "5_0", "5_1", "4_0_level_9_1", "4_0_level_9_3", "2_0", "3_0"}) {
      const bool legacy = model[0] < '4', level9 = strstr(model, "level_9") != nullptr;
      if ((stage == D3D11_SB_HULL_SHADER || stage == D3D11_SB_DOMAIN_SHADER) && model[0] != '5')
        continue;
      if ((legacy || level9) && stage != D3D10_SB_PIXEL_SHADER && stage != D3D10_SB_VERTEX_SHADER)
        continue;
      Program p{stage, std::string(entry) + "_" + model, D3D_FEATURE_LEVEL_9_1,
                !legacy && strcmp(model, "5_1") != 0, {}};
      step("compile %s before device creation", p.profile.c_str());
      ComPtr<ID3DBlob> code, errors;
      HRESULT hr = D3DCompile(legacy ? legacy_hlsl : hlsl, legacy ? sizeof(legacy_hlsl) - 1 : sizeof(hlsl) - 1,
                              nullptr, nullptr, nullptr, entry, p.profile.c_str(), 0, 0, &code, &errors);
      if (errors)
        printf("%s: %.*s\n", p.profile.c_str(), int(errors->GetBufferSize()), (const char *)errors->GetBufferPointer());
      if (!expect(hr == S_OK && code, "%s did not compile: %08lx", p.profile.c_str(), hr))
        return verdict();
      if (p.supported && !level9) {
        ComPtr<ID3D11ShaderReflection> reflection;
        CHECK(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_ID3D11ShaderReflection,
                         reinterpret_cast<void **>(reflection.GetAddressOf())));
        CHECK(reflection->GetMinFeatureLevel(&p.minimum));
      }
      if (p.supported)
        levels.insert(p.minimum);
      p.code.assign((const char *)code->GetBufferPointer(), code->GetBufferSize());
      programs.push_back(std::move(p));
    }
  }

  const size_t compiled_count = programs.size();
  std::set<UINT> generated;
  for (size_t i = 0; i < compiled_count; i++) {
    const auto base = programs[i];
    if (!base.supported || !generated.insert(base.stage).second)
      continue;
    const auto *header = reinterpret_cast<const DXBCHeader *>(base.code.data());
    const auto *offsets = reinterpret_cast<const UINT *>(base.code.data() + sizeof(*header));
    bool found = false;
    for (UINT part = 0; part < header->BlobCount; part++) {
      const auto *chunk = reinterpret_cast<const DXBCBlobHeader *>(base.code.data() + offsets[part]);
      if (chunk->BlobFourCC != DXBC_GenericShader && chunk->BlobFourCC != DXBC_GenericShaderEx)
        continue;
      found = true;
      for (UINT major = 0; major <= DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(~0u); major++) {
        for (UINT minor = 0; minor <= DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(~0u); minor++) {
          UINT token = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(base.stage, major, minor);
          const bool tessellation = base.stage == D3D11_SB_HULL_SHADER || base.stage == D3D11_SB_DOMAIN_SHADER;
          if ((!tessellation &&
               (token == ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(base.stage, 4, 0) ||
                token == ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(base.stage, 4, 1))) ||
              token == ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(base.stage, 5, 0))
            continue;
          Program p = base;
          p.profile = base.profile.substr(0, base.profile.find('_')) + "_version_" + std::to_string(major) + "_" +
                      std::to_string(minor);
          p.supported = false;
          memcpy(p.code.data() + offsets[part] + sizeof(*chunk), &token, sizeof(token));
          dxbc::sign(p.code);
          programs.push_back(std::move(p));
        }
      }
    }
    if (!expect(found, "%s has no tokenized shader part for version generation", base.profile.c_str()))
      return verdict();
  }

  for (auto level : levels) {
    ComPtr<ID3D11Device> device;
    step("create device of feature level %04x", UINT(level));
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, nullptr));
    expect(device->GetFeatureLevel() == level, "device changed the requested feature level");
    ComPtr<ID3D10Device> device10;
    if (level <= D3D_FEATURE_LEVEL_10_1) {
      // a D3D11-created device gives its D3D10 interface once a context state exists (d3d11_device_interfaces)
      ComPtr<ID3D11Device1> device1;
      ComPtr<ID3DDeviceContextState> state;
      CHECK(device.As(&device1));
      CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device), nullptr,
                                              &state));
      CHECK(device.As(&device10));
    }
    for (const auto &p : programs) {
      const void *bytes = p.code.data();
      const SIZE_T size = p.code.size();
      for (UINT repeat = 0; repeat < 2; repeat++) {
        for (bool wanted : {false, true}) {
          const HRESULT required = p.supported && level >= p.minimum ? (wanted ? S_OK : S_FALSE) : E_INVALIDARG;
          const HRESULT so_required = level >= D3D_FEATURE_LEVEL_10_0 ? required : E_INVALIDARG;
          step("FL%04x %s output=%u repeat=%u", UINT(level), p.profile.c_str(), wanted, repeat);
          auto make = [&](auto method, auto shader) {
            HRESULT hr = (device.Get()->*method)(bytes, size, nullptr, wanted ? shader.GetAddressOf() : nullptr);
            expect(hr == required, "D3D11 returned %08lx, expected %08lx", hr, required);
            expect(bool(shader) == (wanted && hr == S_OK), "D3D11 shader output differs from HRESULT");
          };
          switch (p.stage) {
          case D3D10_SB_PIXEL_SHADER:
            make(&ID3D11Device::CreatePixelShader, ComPtr<ID3D11PixelShader>{});
            break;
          case D3D10_SB_VERTEX_SHADER:
            make(&ID3D11Device::CreateVertexShader, ComPtr<ID3D11VertexShader>{});
            break;
          case D3D10_SB_GEOMETRY_SHADER:
            make(&ID3D11Device::CreateGeometryShader, ComPtr<ID3D11GeometryShader>{});
            break;
          case D3D11_SB_HULL_SHADER:
            make(&ID3D11Device::CreateHullShader, ComPtr<ID3D11HullShader>{});
            break;
          case D3D11_SB_DOMAIN_SHADER:
            make(&ID3D11Device::CreateDomainShader, ComPtr<ID3D11DomainShader>{});
            break;
          case D3D11_SB_COMPUTE_SHADER:
            make(&ID3D11Device::CreateComputeShader, ComPtr<ID3D11ComputeShader>{});
            break;
          }
          if (p.stage == D3D10_SB_GEOMETRY_SHADER || p.stage == D3D10_SB_VERTEX_SHADER) {
            const D3D11_SO_DECLARATION_ENTRY declaration{0, "SV_Position", 0, 0, 4, 0};
            const UINT stride = 4 * sizeof(float);
            ComPtr<ID3D11GeometryShader> shader;
            HRESULT hr = device->CreateGeometryShaderWithStreamOutput(
                bytes, size, &declaration, 1, &stride, 1, 0, nullptr, wanted ? shader.GetAddressOf() : nullptr);
            expect(hr == so_required, "stream output returned %08lx, expected %08lx", hr, so_required);
            expect(bool(shader) == (wanted && hr == S_OK), "stream output shader differs from HRESULT");
          }
          if (device10) {
            auto make10 = [&](auto method, auto shader) {
              HRESULT hr = (device10.Get()->*method)(bytes, size, wanted ? shader.GetAddressOf() : nullptr);
              expect(hr == required, "D3D10 returned %08lx, expected %08lx", hr, required);
              expect(bool(shader) == (wanted && hr == S_OK), "D3D10 shader output differs from HRESULT");
            };
            switch (p.stage) {
            case D3D10_SB_PIXEL_SHADER:
              make10(&ID3D10Device::CreatePixelShader, ComPtr<ID3D10PixelShader>{});
              break;
            case D3D10_SB_VERTEX_SHADER:
              make10(&ID3D10Device::CreateVertexShader, ComPtr<ID3D10VertexShader>{});
              break;
            case D3D10_SB_GEOMETRY_SHADER:
              make10(&ID3D10Device::CreateGeometryShader, ComPtr<ID3D10GeometryShader>{});
              break;
            }
            if (p.stage == D3D10_SB_GEOMETRY_SHADER || p.stage == D3D10_SB_VERTEX_SHADER) {
              const D3D10_SO_DECLARATION_ENTRY declaration{"SV_Position", 0, 0, 4, 0};
              ComPtr<ID3D10GeometryShader> shader;
              HRESULT hr = device10->CreateGeometryShaderWithStreamOutput(
                  bytes, size, &declaration, 1, 4 * sizeof(float), wanted ? shader.GetAddressOf() : nullptr);
              expect(hr == so_required, "D3D10 stream output returned %08lx, expected %08lx", hr, so_required);
              expect(bool(shader) == (wanted && hr == S_OK), "D3D10 stream output shader differs from HRESULT");
            }
          }
        }
      }
      if (trace::wrong)
        return verdict();
    }
    device10.Reset();
    step("FL%04x release after shader creation and validation", UINT(level));
    expect(device.Detach()->Release() == 0, "device retained a public reference");
  }
  return verdict();
}
