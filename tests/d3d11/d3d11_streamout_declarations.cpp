// contract: stream output selects components of the preceding stage's output, and invalid declarations return
// E_INVALIDARG. D3D11.3 14.3: "This must be a subset of the mask for the "register" in the source Pipeline
// Stage's output" and "RegisterMask cannot be empty."
// repeated entries may select disjoint masks: "RegisterMask does not overlap for repeated registers within a
// Stream." (D3D11.3 14.3).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#GSTiedToSO
// Wine test_stream_output records Windows rejecting a non-null declaration with zero entries in D3D11, and
// a nonzero stride with zero entries in D3D10 (dlls/d3d11/tests/d3d11.c:27084, d3d10core/tests/d3d10core.c:15647).
// D3D11.3 14.3 requires a GS when no stream output is declared.
// Microsoft Learn, CreateGeometryShaderWithStreamOutput: "if all parameters pass validation this API will return
// S_FALSE instead of S_OK" when the output pointer is null; E_INVALIDARG: "An invalid parameter was passed to
// the returning function." Each shader declares only stream 0, position index 0, and a float4 position.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-creategeometryshaderwithstreamoutput
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-returnvalues
#include "d3d11_test.hpp"
#include <d3d10_1.h>

static const char hlsl[] = R"hlsl(
struct V { float4 position : SV_Position; };
V vs(V vertex) { return vertex; }
[maxvertexcount(1)]
void gs(point V vertices[1], inout PointStream<V> result) { result.Append(vertices[0]); }
)hlsl";

int main() {
  const char *stages[] = {"vs", "gs"};
  ComPtr<ID3DBlob> codes[std::size(stages)];
  for (UINT shader = 0; shader < std::size(stages); shader++) {
    ComPtr<ID3DBlob> errors;
    auto profile = std::string(stages[shader]) + "_4_0";
    auto hr = D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, stages[shader], profile.c_str(), 0, 0,
                         &codes[shader], &errors);
    if (errors)
      printf("%s: %.*s\n", stages[shader], (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
    if (!expect(SUCCEEDED(hr) && codes[shader], "%s HLSL did not compile: %#lx", stages[shader], (long)hr))
      return verdict();
  }

  ComPtr<ID3D11Device> device;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          nullptr));
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1"));
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  ComPtr<ID3D10Device1> device10;
  CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION,
               &device10));
  const UINT components = D3D11_STANDARD_VECTOR_SIZE, word_bytes = sizeof(float), bytes = components * word_bytes;
  for (UINT shader = 0; shader < std::size(codes); shader++) {
    auto code = codes[shader].Get();
    for (bool validate : {false, true}) {
      auto check11 = [&](const D3D11_SO_DECLARATION_ENTRY *entries, UINT count, const UINT *strides, UINT stride_count,
                         bool valid) {
        auto sentinel = reinterpret_cast<ID3D11GeometryShader *>(uintptr_t(1));
        auto out = sentinel;
        auto hr = device->CreateGeometryShaderWithStreamOutput(
            code->GetBufferPointer(), code->GetBufferSize(), entries, count, strides, stride_count,
            D3D11_SO_NO_RASTERIZED_STREAM, nullptr, validate ? nullptr : &out);
        auto wanted = valid ? (validate ? S_FALSE : S_OK) : E_INVALIDARG;
        expect(hr == wanted, "D3D11 returned %#lx, expected %#lx", (long)hr, (long)wanted);
        if (!validate) {
          expect(valid ? out && out != sentinel : !out, "D3D11 shader output is wrong");
          if (hr == S_OK && out && out != sentinel)
            out->Release();
        }
      };
      auto check10 = [&](const D3D10_SO_DECLARATION_ENTRY *entries, UINT count, UINT stride, bool valid) {
        auto sentinel = reinterpret_cast<ID3D10GeometryShader *>(uintptr_t(1));
        auto out = sentinel;
        auto hr = device10->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(),
                                                                 entries, count, stride, validate ? nullptr : &out);
        auto wanted = valid ? (validate ? S_FALSE : S_OK) : E_INVALIDARG;
        expect(hr == wanted, "D3D10 returned %#lx, expected %#lx", (long)hr, (long)wanted);
        if (!validate) {
          expect(valid ? out && out != sentinel : !out, "D3D10 shader output is wrong");
          if (hr == S_OK && out && out != sentinel)
            out->Release();
        }
      };
      D3D11_SO_DECLARATION_ENTRY entry{0, "SV_Position", 0, 0, BYTE(components), 0};
      D3D10_SO_DECLARATION_ENTRY entry10{"SV_Position", 0, 0, BYTE(components), 0};
      D3D11_SO_DECLARATION_ENTRY split[] = {entry, entry};
      D3D10_SO_DECLARATION_ENTRY split10[] = {entry10, entry10};
      split[0].ComponentCount = split10[0].ComponentCount = BYTE(components / 2);
      split[1].StartComponent = split10[1].StartComponent = split[0].ComponentCount;
      split[1].ComponentCount = split10[1].ComponentCount = BYTE(components - split[1].StartComponent);
      step("shader %s, validate %u, two non-overlapping entries", stages[shader], validate);
      check11(split, std::size(split), &bytes, 1, true);
      check10(split10, std::size(split10), bytes, true);
      for (UINT count : {0u, 1u})
        for (UINT stride : {0u, bytes - word_bytes, bytes, bytes + word_bytes}) {
          step("shader %s, validate %u, entries %u, stride %u", stages[shader], validate, count, stride);
          check11(count ? &entry : nullptr, count, &stride, 1, count ? stride >= bytes : shader != 0);
          check10(count ? &entry10 : nullptr, count, stride, count ? stride >= bytes : shader != 0 && !stride);
        }
      step("shader %s, validate %u, non-null declaration with no entries", stages[shader], validate);
      check11(&entry, 0, &bytes, 1, false);
      check10(&entry10, 0, bytes, false);
      for (UINT stream = 0; stream <= D3D11_SO_STREAM_COUNT; stream++)
        for (UINT slot = 0; slot <= D3D11_SO_BUFFER_SLOT_COUNT; slot++) {
          step("shader %s, validate %u, stream %u, slot %u", stages[shader], validate, stream, slot);
          entry.Stream = stream;
          entry.OutputSlot = BYTE(slot);
          check11(&entry, 1, nullptr, 0, !stream && slot < D3D11_SO_BUFFER_SLOT_COUNT);
        }
      entry.Stream = entry.OutputSlot = 0;
      for (UINT start = 0; start <= components; start++)
        for (UINT count = 0; count <= components + 1; count++) {
          step("shader %s, validate %u, start %u, components %u", stages[shader], validate, start, count);
          entry.StartComponent = entry10.StartComponent = BYTE(start);
          entry.ComponentCount = entry10.ComponentCount = BYTE(count);
          bool valid = count && start + count <= components;
          check11(&entry, 1, &bytes, 1, valid);
          check10(&entry10, 1, bytes, valid);
        }
    }
  }
  return verdict();
}
