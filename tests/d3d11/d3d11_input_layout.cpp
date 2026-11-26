// contract: input layouts must cover every signature element except SV_VertexID and SV_InstanceID, including when
// the signature comes from a pixel shader. D3D11.3 8.16: "The mere presence of VertexID in a Vertex Shaders' input
// declarations activates the feature"; 8.18 gives InstanceID the same rule.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// CreateInputLayout: "The compiled shader code contains a input signature which is validated against the array of
// elements." Wine's test_create_input_layout records the same two exceptions for pixel signatures, where
// SV_InstanceID has D3D_NAME_UNDEFINED and SV_Position has D3D_NAME_POSITION but still requires an element.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createinputlayout
// https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/d3d11/tests/d3d11.c
// a zero-element layout covers an empty signature. InputSlotClass "must be same for all Elements at same InputSlot"
// (D3D11.3 8.1.3), including unused elements; different slots may have different classifications.
// InstanceDataStepRate "must be the same for all Elements at same InputSlot" and must be zero for per-vertex data.
// "The alignment of an Element in a structure in memory must be, in bytes, the nearest power of 2 greater or equal to
// the width of the Element's format, or 4, whichever is less" (D3D11.3 4.4.6), and a layout with an element that is
// not is refused, whether the shader reads it or not: Wine's test_input_layout_alignment has Windows refuse R16 at 1,
// R32 at 2 and R32G32B32A32 at 17, 18 and 19, and take R8 at 1 and R32G32B32A32 at 16 and 20. the spec's examples give
// the rest: R11G11B10_FLOAT and R8G8B8A8 align to 4, R8G8 to 2.
#define INITGUID
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <cctype>

static const char hlsl[] = R"hlsl(
struct Inputs {
  float4 values[COUNT] : POSITION;
  float4 position : SV_Position;
  nointerpolation uint instance : SV_InstanceID;
#if VERTEX
  uint vertex : SV_VertexID;
  float4 clip : SV_ClipDistance;
#endif
};
float4 main(Inputs value) : OUTPUT {
  float4 result = value.position + value.instance;
  for (uint i = 0; i < COUNT; i++)
    result += value.values[i];
#if VERTEX
  result += value.vertex + value.clip;
#endif
  return result;
}
)hlsl";

int
main() {
  struct Signature {
    bool d3d10, pixel;
    UINT count;
    ComPtr<ID3DBlob> code;
    ComPtr<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC desc;
  };
  std::vector<Signature> signatures;
  step("compile and reflect every signature before creating a device");
  for (bool d3d10 : {false, true})
    for (bool pixel : {false, true}) {
      const UINT elements_limit =
          d3d10 ? D3D10_1_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT : D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT;
      const UINT registers = pixel ? (d3d10 ? D3D10_PS_INPUT_REGISTER_COUNT : D3D11_PS_INPUT_REGISTER_COUNT)
                                   : (d3d10 ? D3D10_1_VS_INPUT_REGISTER_COUNT : elements_limit);
      UINT maximum = 0;
      for (UINT count = 0;; count = count == 0 ? 1 : count == 1 ? maximum - 1 : maximum) {
        Signature sig{d3d10, pixel, count};
        auto number = std::to_string(count);
        D3D_SHADER_MACRO macros[] = {{"COUNT", number.c_str()},
                                     {"VERTEX", pixel ? "0" : "1"},
                                     {"OUTPUT", pixel ? "SV_Target" : "SV_Position"},
                                     {}};
        std::string profile = std::string(pixel ? "ps" : "vs") + (d3d10 ? "_4_1" : "_5_0");
        const char *source = count ? hlsl : "float4 main() : OUTPUT { return float4(0, 0, 0, 1); }";
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(source, strlen(source), nullptr, macros, nullptr, "main", profile.c_str(), 0, 0,
                                &sig.code, &errors);
        if (FAILED(hr)) {
          printf("failed: %s COUNT=%u did not compile: %08lx\n", profile.c_str(), count, hr);
          if (errors)
            printf("%.*s\n", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
          return 1;
        }
        CHECK(D3DReflect(sig.code->GetBufferPointer(), sig.code->GetBufferSize(), IID_ID3D11ShaderReflection,
                         reinterpret_cast<void **>(sig.reflection.GetAddressOf())));
        CHECK(sig.reflection->GetDesc(&sig.desc));
        if (count == 1)
          maximum = std::min(registers, elements_limit) - (sig.desc.InputParameters - count);
        signatures.push_back(sig);
        if (count && count == maximum)
          break;
      }
    }

  ComPtr<ID3D11Device> device11;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device11,
                          nullptr, nullptr));
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create10 != nullptr, "no D3D10CreateDevice1 in d3d10_1.dll"))
    return verdict();
  ComPtr<ID3D10Device1> device10;
  CHECK(create10(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION,
                 &device10));

  for (auto &sig : signatures) {
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> parameters(sig.desc.InputParameters);
    step("D3D%u %s COUNT=%u: reflect the signature", sig.d3d10 ? 10 : 11, sig.pixel ? "ps" : "vs", sig.count);
    for (UINT i = 0; i < parameters.size(); i++) {
      CHECK(sig.reflection->GetInputParameterDesc(i, &parameters[i]));
      if (sig.pixel && !_stricmp(parameters[i].SemanticName, "SV_InstanceID"))
        expect(parameters[i].SystemValueType == D3D_NAME_UNDEFINED, "pixel SV_InstanceID is not an ordinary semantic");
    }
    auto id = [](const char *name) { return !_stricmp(name, "SV_VertexID") || !_stricmp(name, "SV_InstanceID"); };
    const UINT slots =
        sig.d3d10 ? D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT : D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    const UINT missing_all = parameters.size() + 2, mixed_slot = missing_all + 1, separate_slot = mixed_slot + 1,
               mixed_rate = separate_slot + 1;
    for (bool lower : {false, true})
      for (bool reverse : {false, true})
        for (auto classification : {D3D11_INPUT_PER_VERTEX_DATA, D3D11_INPUT_PER_INSTANCE_DATA})
          for (UINT slot : {0u, slots - 1})
            for (UINT omitted = 0; omitted <= (sig.count == 1 ? mixed_rate : missing_all); omitted++) {
              const char *excluded = omitted == 0                    ? "none"
                                     : omitted == missing_all       ? "all"
                                     : omitted == mixed_slot        ? "unused element, mixed class in same slot"
                                     : omitted == separate_slot     ? "unused element, mixed class in different slot"
                                     : omitted == mixed_rate        ? "unused element, different rate in same slot"
                                     : omitted > parameters.size()  ? "both IDs"
                                                                    : parameters[omitted - 1].SemanticName;
              step("D3D%u %s COUNT=%u lower=%u reverse=%u class=%u slot=%u omitted=%u (%s)", sig.d3d10 ? 10 : 11,
                   sig.pixel ? "ps" : "vs", sig.count, lower, reverse, classification, slot, omitted, excluded);
              std::vector<std::string> names(parameters.size());
              std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
              HRESULT wanted = S_OK;
              for (UINT i = 0; i < parameters.size(); i++) {
                auto &p = parameters[i];
                const bool generated = id(p.SemanticName);
                if (omitted == i + 1 || (omitted == parameters.size() + 1 && generated)) {
                  if (!generated)
                    wanted = E_INVALIDARG;
                  continue;
                }
                names[i] = p.SemanticName;
                if (lower)
                  std::transform(names[i].begin(), names[i].end(), names[i].begin(),
                                 [](unsigned char c) { return std::tolower(c); });
                elements.push_back({names[i].c_str(), p.SemanticIndex,
                                    p.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                                                                      : DXGI_FORMAT_R32G32B32A32_UINT,
                                    slot, D3D11_APPEND_ALIGNED_ELEMENT, classification, 0});
              }
              D3D11_INPUT_ELEMENT_DESC unused{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, slot,
                                               D3D11_APPEND_ALIGNED_ELEMENT, classification, 0};
              if (omitted == missing_all) {
                elements.clear();
                wanted = sig.count ? E_INVALIDARG : S_OK;
              } else if (omitted == mixed_slot || omitted == separate_slot) {
                unused.InputSlotClass = classification == D3D11_INPUT_PER_VERTEX_DATA ? D3D11_INPUT_PER_INSTANCE_DATA
                                                                                    : D3D11_INPUT_PER_VERTEX_DATA;
                if (omitted == separate_slot)
                  unused.InputSlot = slot ? 0 : slots - 1;
                else
                  wanted = E_INVALIDARG;
                elements.push_back(unused);
              } else if (omitted == mixed_rate) {
                unused.InstanceDataStepRate = 1;
                wanted = E_INVALIDARG;
                elements.push_back(unused);
              }
              const UINT num_elements = elements.size();
              if (elements.empty())
                elements.push_back(unused);
              if (reverse)
                std::reverse(elements.begin(), elements.end());
              IUnknown *device = sig.d3d10 ? static_cast<IUnknown *>(device10.Get()) : device11.Get();
              device->AddRef();
              const ULONG before = device->Release();
              ComPtr<ID3D11InputLayout> layout11;
              ComPtr<ID3D10InputLayout> layout10;
              HRESULT hr;
              if (sig.d3d10) {
                std::vector<D3D10_INPUT_ELEMENT_DESC> elements10;
                for (auto &e : elements)
                  elements10.push_back({e.SemanticName, e.SemanticIndex, e.Format, e.InputSlot, e.AlignedByteOffset,
                                        D3D10_INPUT_CLASSIFICATION(e.InputSlotClass), e.InstanceDataStepRate});
                hr = device10->CreateInputLayout(elements10.data(), num_elements, sig.code->GetBufferPointer(),
                                                 sig.code->GetBufferSize(), &layout10);
              } else {
                hr = device11->CreateInputLayout(elements.data(), num_elements, sig.code->GetBufferPointer(),
                                                 sig.code->GetBufferSize(), &layout11);
              }
              expect(hr == wanted, "CreateInputLayout returned %08lx, expected %08lx", hr, wanted);
              expect(bool(layout10 || layout11) == (wanted == S_OK), "wrong returned layout");
              device->AddRef();
              const ULONG held = device->Release();
              const ULONG required = before + (hr == S_OK);
              expect(sig.d3d10 ? held >= required : held == required, "device refcount %lu, expected %s%lu", held,
                     sig.d3d10 ? "at least " : "", required);
              layout10.Reset();
              layout11.Reset();
              device->AddRef();
              expect(device->Release() == before, "layout release did not restore the device refcount");
            }
  }

  // POSITION is the shader's, COLOR is not
  static const char one_input[] = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
  auto aligned_code = compile(one_input, "main", "vs_4_0");
  if (!expect(aligned_code, "the one-input shader did not compile"))
    return verdict();
  struct Aligned {
    DXGI_FORMAT first, second;
    UINT offset;
    HRESULT hr;
  } aligned[] = {
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R16_UINT, 2, S_OK},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R16_UINT, 1, E_INVALIDARG},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT, 1, S_OK},
      {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32_UINT, 2, E_INVALIDARG},
      {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, E_INVALIDARG},
      {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, S_OK},
      {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 17, E_INVALIDARG},
      {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 18, E_INVALIDARG},
      {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 19, E_INVALIDARG},
      {DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 20, S_OK},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R11G11B10_FLOAT, 2, E_INVALIDARG},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R11G11B10_FLOAT, 4, S_OK},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, 2, E_INVALIDARG},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, 4, S_OK},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UNORM, 1, E_INVALIDARG},
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UNORM, 2, S_OK},
  };
  for (auto &c : aligned) {
    // the element the shader reads is the misaligned one too
    for (bool read : {false, true}) {
      D3D11_INPUT_ELEMENT_DESC elements[] = {
          {read ? "COLOR" : "POSITION", 0, c.first, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {read ? "POSITION" : "COLOR", 0, c.second, 0, c.offset, D3D11_INPUT_PER_VERTEX_DATA, 0}};
      for (bool d3d10 : {false, true}) {
        step("D3D%u: format %u at 0, format %u at %u, %s", d3d10 ? 10 : 11, c.first, c.second, c.offset,
             read ? "the shader reads the second" : "the shader reads the first");
        HRESULT hr;
        if (d3d10) {
          D3D10_INPUT_ELEMENT_DESC elements10[std::size(elements)];
          for (UINT i = 0; i < std::size(elements); i++)
            elements10[i] = {elements[i].SemanticName, 0, elements[i].Format, 0, elements[i].AlignedByteOffset,
                             D3D10_INPUT_PER_VERTEX_DATA, 0};
          ComPtr<ID3D10InputLayout> layout;
          hr = device10->CreateInputLayout(elements10, std::size(elements10), aligned_code->GetBufferPointer(),
                                           aligned_code->GetBufferSize(), &layout);
        } else {
          ComPtr<ID3D11InputLayout> layout;
          hr = device11->CreateInputLayout(elements, std::size(elements), aligned_code->GetBufferPointer(),
                                           aligned_code->GetBufferSize(), &layout);
        }
        expect(hr == c.hr, "CreateInputLayout returned %08lx, expected %08lx", hr, c.hr);
      }
    }
  }
  return verdict();
}
