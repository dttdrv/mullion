// contract: an occlusion query counts every sample its draws pass, in 64 bits, however many draws add up past 2^32.
// it returns the "number of multisamples which passed depth and stencil testing" as a UINT64 (D3D11.3 20.4.6), and
// its predicate is FALSE only where that count would be 0 (20.4.8); with no depth or stencil test every sample of a
// draw passes. Metal counts a counting segment's samples in 32 bits (tests/native/visibility_width.swift): 64 triangles
// over an 8192x8192 target, 2^26 samples each, count 0 in one segment, and 100 of them 0x90000000, which Wine's
// test_occlusion_query caught (it wants 0x190000000, or 0xffffffff from a 32-bit counter). a pass stores the counts of
// its first 4096 segments, adds later ones to what the buffer holds and counts nothing from segment 16383 on
// (tests/native/visibility.swift): the many-draws query counts more draws than that in one pass of a small target.
// each query runs on the immediate context and in a deferred context's list
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  id %= 3;
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps() : SV_Target { return 1; }
)hlsl";

int
main(int argc, char **argv) {
  step("compile shaders and create the device");
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!expect(vs_code && ps_code, "HLSL did not compile"))
    return verdict();
  if (argc > 1 && !strcmp(argv[1], "shaders"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));

  struct Target {
    UINT side;
    ComPtr<ID3D11RenderTargetView> rtv;
  } wide{D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION}, small{8};
  for (auto *target : {&wide, &small}) {
    D3D11_TEXTURE2D_DESC desc{target->side, target->side, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> texture;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &target->rtv));
  }
  ComPtr<ID3D11Query> count, predicate;
  D3D11_QUERY_DESC count_desc{D3D11_QUERY_OCCLUSION}, predicate_desc{D3D11_QUERY_OCCLUSION_PREDICATE};
  CHECK(device->CreateQuery(&count_desc, &count));
  CHECK(device->CreatePredicate(&predicate_desc, (ID3D11Predicate **)predicate.GetAddressOf()));

  // a query of `draws` triangles over the target, each draw `instances` of them
  auto run = [&](ID3D11DeviceContext *context, ID3D11Query *query, Target &target, UINT draws, UINT instances) {
    D3D11_VIEWPORT viewport{0, 0, (float)target.side, (float)target.side, 0, 1};
    context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->Begin(query);
    for (UINT i = 0; i < draws; i++)
      context->DrawInstanced(3, instances, 0, 0);
    context->End(query);
    context->ClearState();
    if (context == deferred.Get()) {
      ComPtr<ID3D11CommandList> commands;
      if (FAILED(deferred->FinishCommandList(FALSE, &commands)))
        return false;
      immediate->ExecuteCommandList(commands.Get(), FALSE);
    }
    return true;
  };
  // the query's result once it is there
  auto result = [&](ID3D11Query *query, void *data, UINT size) {
    HRESULT hr;
    while ((hr = immediate->GetData(query, data, size, 0)) == S_FALSE)
      Sleep(1);
    return hr;
  };
  const UINT64 wide_samples = UINT64(wide.side) * wide.side, small_samples = UINT64(small.side) * small.side;
  const UINT many = 4 * 4096 + 100;
  for (auto *context : {immediate.Get(), deferred.Get()}) {
    const char *name = context == immediate.Get() ? "immediate" : "deferred";
    struct {
      const char *what;
      Target &target;
      UINT draws, instances;
      UINT64 samples;
    } counts[] = {
        {"one triangle", wide, 1, 1, wide_samples},
        {"100 triangles, as Wine's test", wide, 100, 1, 100 * wide_samples},
        {"40 draws of 3 instances", wide, 40, 3, 120 * wide_samples},
        {"16484 draws in one pass", small, many, 1, many * small_samples},
    };
    for (auto &c : counts) {
      step("%s: %s", name, c.what);
      if (!expect(run(context, count.Get(), c.target, c.draws, c.instances), "the list could not be finished"))
        continue;
      UINT64 got = 0;
      CHECK(result(count.Get(), &got, sizeof(got)));
      expect(got == c.samples, "%llu samples (0x%llx), want %llu (0x%llx)", got, got, c.samples, c.samples);
    }
    step("%s: the predicate of 64 triangles, 2^32 samples", name);
    if (!expect(run(context, predicate.Get(), wide, 64, 1), "the list could not be finished"))
      continue;
    BOOL visible = FALSE;
    CHECK(result(predicate.Get(), &visible, sizeof(visible)));
    expect(visible, "the predicate is FALSE");
  }
  return verdict();
}
