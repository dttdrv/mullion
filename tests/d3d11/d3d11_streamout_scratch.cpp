// contract: DrawInstancedIndirect streams out every primitive its arguments draw, in the API's order, as a direct draw
// of them does (D3D11.3 functional spec, 8.7 and 14.5), and the statistics query counts them (20.4). Mullion gives an
// indirect draw's stream output its scratch from an area the encoder's indirect draws share, sized on the GPU from the
// arguments; the area starts at 32 MB. a geometry shader of 32 instances needs about 16 KB a 32-point object
// threadgroup, so 96000 points need about 49 MB. a draw the area has no room for draws nothing (Mullion's limit, not
// D3D11's rule), the encoder's other draws still stream out whole, and the area grows so the same draw, once the GPU
// has finished the first, streams out whole.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
struct V { uint id : ID; };
V vs(uint id : SV_VertexID) { V v; v.id = id; return v; }
struct P { float4 pos : SV_Position; uint value : VALUE; };
// point p comes out once, from geometry instance p % 32
[maxvertexcount(1)] [instance(32)]
void gs(point V v[1], uint i : SV_GSInstanceID, inout PointStream<P> s) {
  if (i != v[0].id % 32)
    return;
  P o;
  o.pos = float4(0, 0, 0, 1);
  o.value = v[0].id * 32 + i;
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

  // a small draw, the big one, and the small one again; then the big one once more
  const UINT small = 1000, big = 96000;
  const UINT argument_words[] = {small, 1, 0, 0, big, 1, 0, 0};
  D3D11_BUFFER_DESC arguments_desc{sizeof(argument_words), D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS};
  D3D11_SUBRESOURCE_DATA arguments_data{argument_words};
  ComPtr<ID3D11Buffer> arguments;
  CHECK(device->CreateBuffer(&arguments_desc, &arguments_data, &arguments));
  const UINT sentinel = 0xa5a5a5a5u;
  auto target = [&](UINT points) { return buffer(device.Get(), stride * points + 4, D3D11_BIND_STREAM_OUTPUT, sentinel); };
  auto before = target(small), first = target(big), after = target(small), again = target(big);
  auto query = [&](D3D11_QUERY type) {
    ComPtr<ID3D11Query> q;
    D3D11_QUERY_DESC desc{type};
    device->CreateQuery(&desc, &q);
    return q;
  };
  auto draw = [&](ID3D11Buffer *out, UINT at) {
    UINT offset = 0;
    context->SOSetTargets(1, &out, &offset);
    context->DrawInstancedIndirect(arguments.Get(), at);
  };
  context->VSSetShader(vertex.Get(), nullptr, 0);
  context->GSSetShader(geometry.Get(), nullptr, 0);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  draw(before.Get(), 0);
  draw(first.Get(), 4 * sizeof(UINT));
  draw(after.Get(), 0);
  auto finished = query(D3D11_QUERY_EVENT);
  context->End(finished.Get());
  BOOL done = FALSE;
  for (int tries = 0; context->GetData(finished.Get(), &done, sizeof(done), 0) == S_FALSE && tries < 10000; tries++)
    Sleep(1);
  auto statistics = query(D3D11_QUERY_SO_STATISTICS);
  context->Begin(statistics.Get());
  draw(again.Get(), 4 * sizeof(UINT));
  context->End(statistics.Get());
  context->SOSetTargets(0, nullptr, nullptr);

  unsigned failures = 0;
  auto expect = [&](const char *what, uint64_t value, uint64_t want) {
    if (value != want && failures++ < 12)
      printf("%s: %#llx, want %#llx\n", what, (unsigned long long)value, (unsigned long long)want);
  };
  auto streamed = [&](const char *what, ID3D11Buffer *out, UINT points) {
    auto got = read(device.Get(), context.Get(), out);
    for (UINT p = 0; p < points; p++)
      expect(what, got[p], p * 32 + p % 32);
    expect(what, got[points], sentinel);
  };
  streamed("the small draw before the big one", before.Get(), small);
  streamed("the small draw after the big one", after.Get(), small);
  streamed("the big draw once the area grew", again.Get(), big);
  // the same draw in a later encoder, over the area the last one left its totals in
  auto over = target(big);
  draw(over.Get(), 4 * sizeof(UINT));
  context->SOSetTargets(0, nullptr, nullptr);
  streamed("the big draw over the area the last one used", over.Get(), big);
  auto got_first = read(device.Get(), context.Get(), first.Get());
  for (UINT p = 0; p <= big; p++)
    expect("the big draw the area had no room for", got_first[p], sentinel);
  D3D11_QUERY_DATA_SO_STATISTICS counted{};
  HRESULT hr;
  for (int tries = 0; (hr = context->GetData(statistics.Get(), &counted, sizeof(counted), 0)) == S_FALSE && tries < 1000; tries++)
    Sleep(1);
  expect("statistics query result", hr, S_OK);
  expect("primitives written", counted.NumPrimitivesWritten, big);
  expect("primitives needed", counted.PrimitivesStorageNeeded, big);
  if (failures) {
    printf("failed: %u wrong values\n", failures);
    return 1;
  }
  printf("passed: %u points, then the %u the area grew for\n", 2 * small, big);
  return 0;
}
