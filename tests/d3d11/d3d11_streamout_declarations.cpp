// contract: stream output selects components of the preceding stage's output, and invalid declarations return
// E_INVALIDARG. D3D11.3 14.3: "This must be a subset of the mask for the "register" in the source Pipeline
// Stage's output" and "RegisterMask cannot be empty."
// repeated entries may select disjoint masks: "RegisterMask does not overlap for repeated registers within a
// Stream." (D3D11.3 14.3).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#GSTiedToSO
// Wine test_stream_output records Windows rejecting a non-null declaration with zero entries in D3D11, and
// a nonzero stride with zero entries in D3D10 (dlls/d3d11/tests/d3d11.c:27084, d3d10core/tests/d3d10core.c:15647).
// D3D11.3 14.3 requires a GS when no stream output is declared.
// below feature level 11_0 a declaration has Direct3D 10's limits, which Windows keeps in Wine's
// test_fl10_stream_output_desc and test_stream_output: stream 0 is rasterized, there is at most one stride and none
// without entries, each of several slots takes one entry ("If you are streaming data into multiple buffers, each
// buffer can only capture a single element", "Stream-Output Stage"), and Direct3D 10's stride is only for a
// declaration all on slot 0. a declaration or strides counted without their array are refused (Microsoft Learn:
// pSODeclaration "Cannot be NULL if NumEntries > 0"; NumStrides "ranges from 0 to D3D11_SO_BUFFER_SLOT_COUNT").
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

  // each D3D11 check goes to a device of each level
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  ComPtr<ID3D11Device> devices[std::size(levels)];
  for (UINT d = 0; d < std::size(levels); d++)
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &levels[d], 1, D3D11_SDK_VERSION,
                            &devices[d], nullptr, nullptr));
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
      // `valid` at 11_0 and `valid10` at 10_0, each with its rasterized stream
      auto check11 = [&](const D3D11_SO_DECLARATION_ENTRY *entries, UINT count, const UINT *strides, UINT stride_count,
                         bool valid, bool valid10, UINT rasterized = D3D11_SO_NO_RASTERIZED_STREAM,
                         UINT rasterized10 = 0) {
        for (UINT d = 0; d < std::size(devices); d++) {
          auto sentinel = reinterpret_cast<ID3D11GeometryShader *>(uintptr_t(1));
          auto out = sentinel;
          bool ok = d ? valid10 : valid;
          auto hr = devices[d]->CreateGeometryShaderWithStreamOutput(
              code->GetBufferPointer(), code->GetBufferSize(), entries, count, strides, stride_count,
              d ? rasterized10 : rasterized, nullptr, validate ? nullptr : &out);
          auto wanted = ok ? (validate ? S_FALSE : S_OK) : E_INVALIDARG;
          expect(hr == wanted, "D3D11 at level %#x returned %#lx, expected %#lx", (unsigned)levels[d], (long)hr,
                 (long)wanted);
          if (!validate) {
            expect(ok ? out && out != sentinel : !out, "D3D11 shader output at level %#x is wrong",
                   (unsigned)levels[d]);
            if (hr == S_OK && out && out != sentinel)
              out->Release();
          }
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
      check11(split, std::size(split), &bytes, 1, true, true);
      check10(split10, std::size(split10), bytes, true);
      for (UINT count : {0u, 1u})
        for (UINT stride : {0u, bytes - word_bytes, bytes, bytes + word_bytes}) {
          step("shader %s, validate %u, entries %u, stride %u", stages[shader], validate, count, stride);
          check11(count ? &entry : nullptr, count, &stride, 1, count ? stride >= bytes : shader != 0,
                  count && stride >= bytes);
          check10(count ? &entry10 : nullptr, count, stride, count ? stride >= bytes : shader != 0 && !stride);
        }
      step("shader %s, validate %u, non-null declaration with no entries", stages[shader], validate);
      check11(&entry, 0, &bytes, 1, false, false);
      check10(&entry10, 0, bytes, false);
      step("shader %s, validate %u, entries or strides without their array", stages[shader], validate);
      check11(nullptr, 1, &bytes, 1, false, false);
      check11(&entry, 1, nullptr, 1, false, false);
      check10(nullptr, 1, bytes, false);
      const UINT strides[D3D11_SO_BUFFER_SLOT_COUNT + 1] = {bytes, bytes, bytes, bytes, bytes};
      for (UINT count = 2; count <= std::size(strides); count++) {
        step("shader %s, validate %u, %u strides", stages[shader], validate, count);
        check11(&entry, 1, strides, count, count <= D3D11_SO_BUFFER_SLOT_COUNT, false);
      }
      // the geometry shader's streams
      if (shader)
        for (UINT stream : {0u, 1u, 2u, 3u, 4u, UINT(D3D11_SO_NO_RASTERIZED_STREAM)}) {
          step("shader %s, validate %u, rasterized stream %#x", stages[shader], validate, stream);
          check11(&entry, 1, nullptr, 0, stream < D3D11_SO_STREAM_COUNT || stream == D3D11_SO_NO_RASTERIZED_STREAM,
                  !stream, stream, stream);
        }
      // the position's halves on slots 0 and 1; then its first half as two entries on slot 0; then slot 1 alone
      auto to10 = [](const D3D11_SO_DECLARATION_ENTRY &e) {
        return D3D10_SO_DECLARATION_ENTRY{e.SemanticName, e.SemanticIndex, e.StartComponent, e.ComponentCount,
                                          e.OutputSlot};
      };
      D3D11_SO_DECLARATION_ENTRY halves[] = {split[0], split[1]}, thirds[] = {split[0], split[0], split[1]},
                                 alone = entry;
      halves[1].OutputSlot = thirds[2].OutputSlot = alone.OutputSlot = 1;
      thirds[0].ComponentCount = thirds[1].ComponentCount = thirds[1].StartComponent = 1;
      D3D10_SO_DECLARATION_ENTRY halves10[] = {to10(halves[0]), to10(halves[1])},
                                 thirds10[] = {to10(thirds[0]), to10(thirds[1]), to10(thirds[2])},
                                 alone10 = to10(alone);
      step("shader %s, validate %u, a slot an entry", stages[shader], validate);
      check11(halves, std::size(halves), nullptr, 0, true, true);
      check10(halves10, std::size(halves10), 0, true);
      check10(halves10, std::size(halves10), bytes, false);
      step("shader %s, validate %u, two entries on a slot beside another slot", stages[shader], validate);
      check11(thirds, std::size(thirds), nullptr, 0, true, false);
      check10(thirds10, std::size(thirds10), 0, false);
      step("shader %s, validate %u, slot 1 alone", stages[shader], validate);
      check11(&alone, 1, nullptr, 0, true, true);
      check10(&alone10, 1, 0, true);
      check10(&alone10, 1, bytes, false);
      for (UINT stream = 0; stream <= D3D11_SO_STREAM_COUNT; stream++)
        for (UINT slot = 0; slot <= D3D11_SO_BUFFER_SLOT_COUNT; slot++) {
          step("shader %s, validate %u, stream %u, slot %u", stages[shader], validate, stream, slot);
          entry.Stream = stream;
          entry.OutputSlot = BYTE(slot);
          bool valid = !stream && slot < D3D11_SO_BUFFER_SLOT_COUNT;
          check11(&entry, 1, nullptr, 0, valid, valid);
        }
      entry.Stream = entry.OutputSlot = 0;
      for (UINT start = 0; start <= components; start++)
        for (UINT count = 0; count <= components + 1; count++) {
          step("shader %s, validate %u, start %u, components %u", stages[shader], validate, start, count);
          entry.StartComponent = entry10.StartComponent = BYTE(start);
          entry.ComponentCount = entry10.ComponentCount = BYTE(count);
          bool valid = count && start + count <= components;
          check11(&entry, 1, &bytes, 1, valid, valid);
          check10(&entry10, 1, bytes, valid);
        }
    }
  }
  return verdict();
}
