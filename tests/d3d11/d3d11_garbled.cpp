// contract: Direct3D 11 refuses a shader that is not what a compiler wrote, and the process goes on. a shader comes
// in a container whose header has a hash of everything after it (tests/garbled.hpp), and the Create*Shader methods
// return "one of the Direct3D 11 Return Codes", of which E_INVALIDARG is "An invalid parameter was passed to the
// returning function" (ID3D11Device::CreateVertexShader; Direct3D 11 Return Codes). with no pointer for the shader
// the method only validates: "if all parameters pass validation this API will return S_FALSE instead of S_OK".
// a vertex, a pixel, a geometry and a compute shader are garbled in every way their layout gives
// (tests/garbled.hpp) and given to their method, with a pointer for the shader and without; the geometry shader
// also to CreateGeometryShaderWithStreamOutput, with a declaration of what it puts out. an answer other than E_INVALIDARG is a wrong result;
// a fault ends the test with the garbling it was given (tests/trace.hpp). every garbling is also given with its
// hash made anew, which takes it past the hash to whatever reads the parts: some of those are whole shaders of
// another kind (a flipped constant), so they are held only to an answer, S_OK or E_INVALIDARG.
#include "d3d11_test.hpp"
#include "../garbled.hpp"
#include <functional>

static const char hlsl[] = R"hlsl(
Texture2D<float4> colors : register(t0);
SamplerState point_sampler : register(s0);
cbuffer Scale : register(b0) { float4 scale; };
struct V { float4 pos : SV_Position; float2 uv : UV; };
V vs(float3 pos : POSITION, float2 uv : UV) { V v; v.pos = float4(pos * scale.xyz, 1); v.uv = uv; return v; }
float4 ps(V v) : SV_Target { return colors.Sample(point_sampler, v.uv) * scale; }
RWStructuredBuffer<uint> numbers : register(u0);
[maxvertexcount(3)] void gs(triangle V corners[3], inout TriangleStream<V> stream) {
  for (int i = 0; i < 3; i++)
    stream.Append(corners[i]);
}
[numthreads(8, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { numbers[id.x] = numbers[id.x] * 3 + uint(scale.x); }
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  // a shader's method, with a pointer for the shader or to validate only
  struct Use {
    const char *call, *entry, *stage;
    std::function<HRESULT(const std::string &, bool)> make;
  };
  const Use uses[] = {
      {"CreateVertexShader", "vs", "vs",
       [&](auto &code, bool wanted) {
         ComPtr<ID3D11VertexShader> made;
         return device->CreateVertexShader(code.data(), code.size(), nullptr, wanted ? made.GetAddressOf() : nullptr);
       }},
      {"CreatePixelShader", "ps", "ps",
       [&](auto &code, bool wanted) {
         ComPtr<ID3D11PixelShader> made;
         return device->CreatePixelShader(code.data(), code.size(), nullptr, wanted ? made.GetAddressOf() : nullptr);
       }},
      {"CreateGeometryShader", "gs", "gs",
       [&](auto &code, bool wanted) {
         ComPtr<ID3D11GeometryShader> made;
         return device->CreateGeometryShader(code.data(), code.size(), nullptr, wanted ? made.GetAddressOf() : nullptr);
       }},
      {"CreateGeometryShaderWithStreamOutput", "gs", "gs",
       [&](auto &code, bool wanted) {
         // the shader's two outputs, whole, into one buffer
         const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "UV", 0, 0, 2, 0}};
         const UINT stride = 6 * sizeof(float);
         ComPtr<ID3D11GeometryShader> made;
         return device->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), entries, std::size(entries), &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM,
                                                             nullptr, wanted ? made.GetAddressOf() : nullptr);
       }},
      {"CreateComputeShader", "cs", "cs",
       [&](auto &code, bool wanted) {
         ComPtr<ID3D11ComputeShader> made;
         return device->CreateComputeShader(code.data(), code.size(), nullptr, wanted ? made.GetAddressOf() : nullptr);
       }},
  };
  unsigned made = 0, taken = 0, past = 0, past_taken = 0;
  for (auto &use : uses) {
    auto blob = compile(hlsl, use.entry, use.stage);
    if (!expect(!!blob, "%s did not compile", use.entry))
      continue;
    const std::string whole((const char *)blob->GetBufferPointer(), blob->GetBufferSize());
    // what the compiler wrote is taken: without that, a refusal says nothing
    step("%s: the container as it was written", use.call);
    HRESULT created = use.make(whole, true), validated = use.make(whole, false);
    if (!expect(created == S_OK && validated == S_FALSE, "the whole shader: %08lx, and %08lx without a pointer for it", created, validated))
      continue;
    for (auto &[what, code] : garbled(whole))
      for (bool wanted : {true, false}) {
        step("%s%s: %s", use.call, wanted ? "" : " to validate only", what.c_str());
        HRESULT hr = use.make(code, wanted);
        made++, taken += SUCCEEDED(hr);
        expect(hr == E_INVALIDARG, SUCCEEDED(hr) ? "taken as if it were whole (%08lx)" : "refused with %08lx, want E_INVALIDARG", hr);
        std::string signed_anew = code;
        dxbc::sign(signed_anew);
        step("%s%s: %s, with its hash made anew", use.call, wanted ? "" : " to validate only", what.c_str());
        hr = use.make(signed_anew, wanted);
        past++, past_taken += SUCCEEDED(hr);
        expect(hr == E_INVALIDARG || hr == (wanted ? S_OK : S_FALSE), "answered %08lx", hr);
      }
  }
  printf("%u garbled containers, %u taken; %u more with their hash made anew, %u of them taken\n", made, taken, past, past_taken);
  return verdict();
}
