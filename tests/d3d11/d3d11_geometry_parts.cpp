// contract: a direct draw through a geometry shader streams out every primitive, in the API's order (D3D11.3
// functional spec, 14.5), however many object threadgroups it takes. on an Apple7 or Apple8 GPU a draw of more than
// 2^21 mesh threadgroups does not run in one Metal draw, so Mullion runs it in parts, each told its first object
// threadgroup and instance. two draws need parts there: 96000 points of one instance (3000 object threadgroups of 1024
// mesh threadgroups, split along the points) and 64 points of 2000 instances (split along the instances). each point
// comes out once, from geometry instance id % 32, with its draw instance, vertex and geometry instance in its value.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint points; };
struct V { uint key : KEY; };
V vs(uint id : SV_VertexID, uint instance : SV_InstanceID) { V v; v.key = instance * points + id; return v; }
struct P { float4 pos : SV_Position; uint value : VALUE; };
[maxvertexcount(1)] [instance(32)]
void gs(point V v[1], uint i : SV_GSInstanceID, inout PointStream<P> s) {
  if (i != v[0].key % 32)
    return;
  P o;
  o.pos = float4(0, 0, 0, 1);
  o.value = v[0].key * 32 + i;
  s.Append(o);
}
)hlsl";

int
main() {
  auto vs = compile(hlsl, "vs", "vs"), gs = compile(hlsl, "gs", "gs");
  if (!vs || !gs) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
  const D3D11_SO_DECLARATION_ENTRY entries[] = {{0, "VALUE", 0, 0, 1, 0}};
  const UINT stride = 4;
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11GeometryShader> geometry;
  CHECK(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
  CHECK(device->CreateGeometryShaderWithStreamOutput(
      gs->GetBufferPointer(), gs->GetBufferSize(), entries, 1, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
      &geometry
  ));
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->GSSetShader(geometry.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

  const UINT sentinel = 0xa5a5a5a5u;
  unsigned failures = 0;
  struct Draw {
    const char *what;
    UINT points, instances;
  };
  for (auto [what, points, instances] : {Draw{"96000 points", 96000, 1}, Draw{"64 points x 2000 instances", 64, 2000}}) {
    D3D11_BUFFER_DESC constants_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    const UINT words[4] = {points};
    D3D11_SUBRESOURCE_DATA constants_data{words};
    ComPtr<ID3D11Buffer> constants;
    CHECK(device->CreateBuffer(&constants_desc, &constants_data, &constants));
    context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
    const UINT total = points * instances;
    auto out = buffer(device.Get(), stride * total + 4, D3D11_BIND_STREAM_OUTPUT, sentinel);
    UINT offset = 0;
    ID3D11Buffer *targets[] = {out.Get()};
    context->SOSetTargets(1, targets, &offset);
    context->DrawInstanced(points, instances, 0, 0);
    context->SOSetTargets(0, nullptr, nullptr);
    auto got = read(device.Get(), context.Get(), out.Get());
    for (UINT k = 0; k <= total; k++) {
      UINT want = k < total ? k * 32 + k % 32 : sentinel;
      if (got[k] != want && failures++ < 12)
        printf("%s, value %u: %#x, want %#x\n", what, k, got[k], want);
    }
  }
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: 96000 points and 64 points x 2000 instances streamed out in order\n");
  return 0;
}
