// contract: Direct3D 10 as Direct3D 10 applications reach it: a device from D3D10CreateDevice1 (Wine's d3d10_1 onto
// this d3d10core), whose objects are also their Direct3D 11 selves.
// - an object answers QueryInterface for its Direct3D 11 interface and that one for the Direct3D 10 one again, and a
//   view is gone when its one reference is released, as is then its resource.
// - getters of a device with nothing bound give null, whatever the pointer held (IAGetIndexBuffer, GetPredication,
//   whose predicate pointer may be left out).
// - ID3D10Device::CreateGeometryShaderWithStreamOutput gives a geometry shader that streams out: to one buffer at the
//   stride given ("only used when the output slot is 0 for all entries"), or to several, one element each, and the
//   stream is rasterized as well while a pixel shader is bound ("Stream-Output Stage").
// - CreatePredicate makes predicates of the queries that are predicates, which are ID3D10Asynchronous, and a draw is
//   skipped when its predicate's result is the value set with it.
// - queries and capabilities have Direct3D 10's shapes: pipeline statistics are D3D10_QUERY_DATA_PIPELINE_STATISTICS,
//   CheckFormatSupport answers with D3D10_FORMAT_SUPPORT bits only, the device has no counters (D3D10_COUNTER_INFO:
//   LastDeviceDependentCounter 0; CreateCounter: DXGI_ERROR_UNSUPPORTED for a well-known counter, E_INVALIDARG past
//   them), and the exception mode set is the one got.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x)                                                                                                       \
  if (FAILED(x)) {                                                                                                     \
    printf("failed: %s (line %d)\n", #x, __LINE__);                                                                    \
    return 1;                                                                                                          \
  }

static const char hlsl[] = R"hlsl(
struct V { float4 pos : SV_Position; float2 tag : TAG; };
// point `id` at the center of pixel `id` of a row of COUNT
V vs(uint id : SV_VertexID) {
  V o;
  o.pos = float4((id + 0.5) / COUNT * 2 - 1, 0, 0, 1);
  o.tag = float2(id, 2 * id + 1);
  return o;
}
[maxvertexcount(1)] void gs(point V i[1], inout PointStream<V> o) { o.Append(i[0]); }
float4 ps() : SV_Target { return 1; }
)hlsl";

static ComPtr<ID3DBlob>
compile(const char *entry, const char *profile, UINT count) {
  auto text = std::to_string(count);
  D3D_SHADER_MACRO macros[] = {{"COUNT", text.c_str()}, {}};
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(hlsl, sizeof(hlsl) - 1, nullptr, macros, nullptr, entry, profile, 0, 0, &code, &errors)) && errors)
    printf("%s: %.*s\n", entry, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
  return code;
}

int
main() {
  // from d3d10_1.dll itself: the toolchain has no import library for it
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!create) {
    printf("failed: no D3D10CreateDevice1 in d3d10_1.dll\n");
    return 1;
  }
  ComPtr<ID3D10Device1> device;
  CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device));
  unsigned wrong = 0, checks = 0;
  auto expect = [&](bool ok, const char *what) {
    checks++;
    if (!ok && wrong++ < 16)
      printf("%s\n", what);
  };
  const UINT count = 8;
  auto vs_code = compile("vs", "vs_4_0", count), gs_code = compile("gs", "gs_4_0", count), ps_code = compile("ps", "ps_4_0", count);
  if (!vs_code || !gs_code || !ps_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  // an object and its Direct3D 11 self; a view's reference
  D3D10_TEXTURE2D_DESC target_desc{count, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
  ComPtr<ID3D10Texture2D> target;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  {
    ComPtr<ID3D11Texture2D> as11;
    ComPtr<ID3D10Texture2D> as10;
    expect(SUCCEEDED(target.As(&as11)) && SUCCEEDED(as11.As(&as10)) && as10 == target,
           "a texture is not its Direct3D 11 self and back");
    ComPtr<ID3D10Texture2D> viewed;
    ID3D10RenderTargetView *view = nullptr;
    CHECK(device->CreateTexture2D(&target_desc, nullptr, &viewed));
    CHECK(device->CreateRenderTargetView(viewed.Get(), nullptr, &view));
    expect(view->Release() == 0, "a view is created with more than the one reference its creator gets");
    expect(viewed.Reset() == 0, "a texture outlives its released view and its one reference");
  }
  ComPtr<ID3D10RenderTargetView> rtv;
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));

  // getters with nothing bound
  {
    ID3D10Buffer *index = (ID3D10Buffer *)(uintptr_t)0x1;
    DXGI_FORMAT format;
    UINT offset;
    device->IAGetIndexBuffer(&index, &format, &offset);
    expect(!index, "IAGetIndexBuffer leaves the pointer as it was when no buffer is bound");
    ID3D10Predicate *predicate = (ID3D10Predicate *)(uintptr_t)0x1;
    BOOL value = TRUE;
    device->GetPredication(&predicate, &value);
    expect(!predicate, "GetPredication gives a predicate when none is set");
    device->GetPredication(nullptr, &value);
    ComPtr<ID3D10BlendState> blend;
    expect(device->CreateBlendState(nullptr, &blend) == E_INVALIDARG, "a blend state is made of no description");
  }

  ComPtr<ID3D10VertexShader> vs;
  ComPtr<ID3D10PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), &ps));
  D3D10_VIEWPORT viewport{0, 0, count, 1, 0, 1};
  device->RSSetViewports(1, &viewport);
  device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_POINTLIST);
  device->VSSetShader(vs.Get());
  device->PSSetShader(ps.Get());
  device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

  // a buffer stream output fills, every word of it `fill` before, and its words afterwards
  const UINT fill = 0xdeadbeef, words = count * 4;
  auto so_buffer = [&]() {
    std::vector<UINT> initial(words, fill);
    D3D10_BUFFER_DESC desc{words * 4, D3D10_USAGE_DEFAULT, D3D10_BIND_STREAM_OUTPUT};
    D3D10_SUBRESOURCE_DATA data{initial.data()};
    ComPtr<ID3D10Buffer> buffer;
    device->CreateBuffer(&desc, &data, &buffer);
    return buffer;
  };
  auto read = [&](ID3D10Buffer *buffer, std::vector<UINT> &out) -> HRESULT {
    D3D10_BUFFER_DESC desc{words * 4, D3D10_USAGE_STAGING, 0, D3D10_CPU_ACCESS_READ};
    ComPtr<ID3D10Buffer> staging;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &staging);
    if (FAILED(hr))
      return hr;
    device->CopyResource(staging.Get(), buffer);
    void *mapped;
    if (FAILED(hr = staging->Map(D3D10_MAP_READ, 0, &mapped)))
      return hr;
    out.assign((UINT *)mapped, (UINT *)mapped + words);
    staging->Unmap();
    return S_OK;
  };
  auto bits = [](float f) {
    UINT u;
    memcpy(&u, &f, 4);
    return u;
  };
  const float clear[4] = {};
  // how many of the target's pixels a draw's points covered
  auto covered = [&](UINT &pixels) -> HRESULT {
    D3D10_TEXTURE2D_DESC desc = target_desc;
    desc.Usage = D3D10_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    ComPtr<ID3D10Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr))
      return hr;
    device->CopyResource(staging.Get(), target.Get());
    D3D10_MAPPED_TEXTURE2D mapped;
    if (FAILED(hr = staging->Map(0, D3D10_MAP_READ, 0, &mapped)))
      return hr;
    pixels = 0;
    for (UINT x = 0; x < count; x++)
      pixels += ((const UINT *)mapped.pData)[x] == 0xffffffff;
    staging->Unmap(0);
    return S_OK;
  };

  // stream output to one buffer at a stride wider than its element, with the stream rasterized
  {
    const UINT stride = 16;
    D3D10_SO_DECLARATION_ENTRY declaration[] = {{"TAG", 0, 0, 2, 0}};
    ComPtr<ID3D10GeometryShader> gs;
    HRESULT hr = device->CreateGeometryShaderWithStreamOutput(
        gs_code->GetBufferPointer(), gs_code->GetBufferSize(), declaration, 1, stride, &gs
    );
    expect(SUCCEEDED(hr) && gs, "no geometry shader with stream output to one buffer");
    if (gs) {
      ComPtr<ID3D11GeometryShader> as11;
      ComPtr<ID3D10GeometryShader> as10;
      expect(SUCCEEDED(gs.As(&as11)) && SUCCEEDED(as11.As(&as10)) && as10 == gs,
             "a stream output shader is not its Direct3D 11 self and back");
      auto buffer = so_buffer();
      UINT offset = 0;
      device->GSSetShader(gs.Get());
      device->SOSetTargets(1, buffer.GetAddressOf(), &offset);
      device->ClearRenderTargetView(rtv.Get(), clear);
      device->Draw(count, 0);
      device->SOSetTargets(0, nullptr, nullptr);
      std::vector<UINT> got;
      CHECK(read(buffer.Get(), got));
      unsigned differ = 0;
      for (UINT i = 0; i < count; i++) {
        const UINT want[4] = {bits(float(i)), bits(float(2 * i + 1)), fill, fill};
        differ += memcmp(&got[i * stride / 4], want, sizeof(want)) != 0;
      }
      expect(!differ, "one buffer does not hold each point's element at the stride given");
      UINT pixels = 0;
      CHECK(covered(pixels));
      expect(pixels == count, "the stream is not rasterized while a pixel shader is bound");
    }
  }
  // to two buffers, an element each
  {
    D3D10_SO_DECLARATION_ENTRY declaration[] = {{"SV_Position", 0, 0, 4, 0}, {"TAG", 0, 0, 2, 1}};
    ComPtr<ID3D10GeometryShader> gs;
    HRESULT hr = device->CreateGeometryShaderWithStreamOutput(
        gs_code->GetBufferPointer(), gs_code->GetBufferSize(), declaration, 2, 0, &gs
    );
    expect(SUCCEEDED(hr) && gs, "no geometry shader with stream output to two buffers");
    if (gs) {
      ComPtr<ID3D10Buffer> buffers[2] = {so_buffer(), so_buffer()};
      ID3D10Buffer *bound[2] = {buffers[0].Get(), buffers[1].Get()};
      UINT offsets[2] = {};
      device->GSSetShader(gs.Get());
      device->SOSetTargets(2, bound, offsets);
      device->Draw(count, 0);
      device->SOSetTargets(0, nullptr, nullptr);
      std::vector<UINT> positions, tags;
      CHECK(read(buffers[0].Get(), positions));
      CHECK(read(buffers[1].Get(), tags));
      unsigned differ = 0;
      for (UINT i = 0; i < count; i++) {
        const UINT position[4] = {bits((i + 0.5f) / count * 2 - 1), 0, 0, bits(1)};
        const UINT tag[2] = {bits(float(i)), bits(float(2 * i + 1))};
        differ += memcmp(&positions[i * 4], position, sizeof(position)) != 0;
        differ += memcmp(&tags[i * 2], tag, sizeof(tag)) != 0;
      }
      expect(!differ, "two buffers do not hold each point's two elements, each at its element's size");
      expect(tags[count * 2] == fill, "the second buffer is written past its elements");
    }
    device->GSSetShader(nullptr);
  }

  // predicates
  {
    D3D10_QUERY_DESC occlusion{D3D10_QUERY_OCCLUSION}, is_predicate{D3D10_QUERY_OCCLUSION_PREDICATE};
    ComPtr<ID3D10Predicate> predicate, not_one;
    expect(device->CreatePredicate(&occlusion, &not_one) == E_INVALIDARG, "an occlusion query is made a predicate");
    expect(device->CreatePredicate(nullptr, &not_one) == E_INVALIDARG, "a predicate is made of no description");
    HRESULT hr = device->CreatePredicate(&is_predicate, &predicate);
    expect(SUCCEEDED(hr) && predicate, "no predicate of an occlusion predicate query");
    if (predicate) {
      ComPtr<ID3D10Asynchronous> asynchronous;
      expect(SUCCEEDED(predicate.As(&asynchronous)), "a predicate is not ID3D10Asynchronous");
      // nothing is drawn in the predicate: its result is false, and a draw predicated on false is skipped
      predicate->Begin();
      predicate->End();
      device->ClearRenderTargetView(rtv.Get(), clear);
      device->SetPredication(predicate.Get(), FALSE);
      ComPtr<ID3D10Predicate> set;
      BOOL value = TRUE;
      device->GetPredication(&set, &value);
      expect(set == predicate && value == FALSE, "GetPredication does not give what SetPredication set");
      device->Draw(count, 0);
      device->SetPredication(nullptr, FALSE);
      UINT pixels = ~0u;
      CHECK(covered(pixels));
      expect(pixels == 0, "a draw predicated on its predicate's result is drawn");
      device->Draw(count, 0);
      CHECK(covered(pixels));
      expect(pixels == count, "a draw without a predicate is not drawn");
    }
  }

  // Direct3D 10's shapes
  {
    D3D10_QUERY_DESC statistics{D3D10_QUERY_PIPELINE_STATISTICS};
    ComPtr<ID3D10Query> query;
    CHECK(device->CreateQuery(&statistics, &query));
    expect(query->GetDataSize() == sizeof(D3D10_QUERY_DATA_PIPELINE_STATISTICS),
           "pipeline statistics do not have Direct3D 10's size");
    D3D10_QUERY_DATA_PIPELINE_STATISTICS data;
    query->Begin();
    query->End();
    HRESULT hr = S_FALSE;
    for (int attempt = 0; attempt < 1000 && hr == S_FALSE; attempt++) {
      hr = query->GetData(&data, sizeof(data), 0);
      Sleep(1);
    }
    expect(hr == S_OK, "pipeline statistics are not read at Direct3D 10's size");
    UINT support = 0;
    CHECK(device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, &support));
    expect(support && !(support & ~(D3D10_FORMAT_SUPPORT_BACK_BUFFER_CAST * 2 - 1)),
           "CheckFormatSupport answers with bits Direct3D 10 has no names for");
    D3D10_COUNTER_INFO info{D3D10_COUNTER_DEVICE_DEPENDENT_0, 1, 1};
    device->CheckCounterInfo(&info);
    expect(info.LastDeviceDependentCounter == 0 && info.NumSimultaneousCounters == 0, "the device says it has counters");
    D3D10_COUNTER_DESC well_known{D3D10_COUNTER_GPU_IDLE}, past{D3D10_COUNTER_DEVICE_DEPENDENT_0};
    ComPtr<ID3D10Counter> counter;
    expect(device->CreateCounter(&well_known, &counter) == DXGI_ERROR_UNSUPPORTED, "a well-known counter is not unsupported");
    expect(device->CreateCounter(&past, &counter) == E_INVALIDARG, "a counter the device does not have is not out of range");
    expect(device->SetExceptionMode(D3D10_RAISE_FLAG_DRIVER_INTERNAL_ERROR) == S_OK &&
               device->GetExceptionMode() == D3D10_RAISE_FLAG_DRIVER_INTERNAL_ERROR,
           "the exception mode set is not the one got");
    expect(device->SetExceptionMode(~0u) == E_INVALIDARG, "an exception mode of no flag is set");
  }
  printf("%s: %u wrong of %u checks\n", wrong ? "failed" : "passed", wrong, checks);
  return wrong != 0;
}
