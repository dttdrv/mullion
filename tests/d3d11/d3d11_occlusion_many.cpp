// contract: every occlusion query returns the visible samples between Begin and End, however many other queries
// share its frame. "number of multisamples which passed depth and stencil testing"; the driver calculates "the
// difference between two requests (one request for Issue( BEGIN ), and one request for Issue( END ))" (D3D11.3
// 20.4.6). "If the OCCLUSION Query for the same bracketed range would return 0, the OCCLUSION Predicate would return
// FALSE. Otherwise, the OCCLUSION Predicate would return TRUE" (20.4.8). disabled depth and stencil tests pass
// (20.4.6), so a single-sample integer rectangle counts (right - left) * (bottom - top), and its predicate is nonzero.
// "Queries can be wrapped around Command List execution" (D3D11.3 6.3.6).
// "Bracketings of Queries are allowed to overlap and nest." (D3D11.3 20.3.4).
// Metal stores results "at offset, which needs to be a multiple of 8"; "You can set a specific offset value only
// once per render pass" (MTLRenderCommandEncoder::setVisibilityResultMode(_:offset:), Apple Developer Documentation).
// "Maximum visibility query offset": 65,528 B through Apple6, 256 KB from Apple7 (Apple, Metal Feature Set Tables,
// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf). the native Metal probes of 2026-10-08
// (tests/native/visibility.swift and visibility.txt) find that only the first 4096 counting segments store their
// counts; later segments add to old contents, and from segment 16383 they return zero on Apple10. 64-bit chunks
// allocate new Metal buffers, so zero-filled memory cannot distinguish a 4096 bound from an 8191 bound. the i386
// backing allocation can be recycled after completion; stress and reuse use the same sizes.
#include "d3d11_test.hpp"
#include <algorithm>
#include <fstream>
#include <utility>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
cbuffer Params { uint4 values; };
uint ps(float4 position : SV_Position) : SV_Target {
  return values.x + uint(position.y) * WIDTH + uint(position.x) + 1;
}
)hlsl";

int
main(int argc, char **) {
  step("create a single-sample target and a scissored triangle");
  const UINT width = 8, height = 4, query_width = width / 2;
  const UINT window_segments = 4096; // counting segments stored per pass by the native probes cited above
  const UINT control_queries = 256, boundaries = 8;
  const UINT stress_queries = boundaries * window_segments + control_queries, report_limit = 8;
  const std::vector<std::string> defines{"WIDTH=" + std::to_string(width)};
  auto vs_code = compile(hlsl, "vs", "vs", defines), ps_code = compile(hlsl, "ps", "ps", defines);
  if (!expect(vs_code && ps_code, "HLSL did not compile"))
    return verdict();
  char directory[MAX_PATH], exe[MAX_PATH];
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  if (argc == 1) {
    GetTempPathA(sizeof(directory), directory);
    const std::string path = std::string(directory) + "d3d11_occlusion_many-" + std::to_string(GetCurrentProcessId());
    if (!expect(CreateDirectoryA(path.c_str(), nullptr), "diagnostic directory could not be created"))
      return verdict();
    SetEnvironmentVariableA("DXMT_LOG_PATH", path.c_str());
    SetEnvironmentVariableA("DXMT_LOG_LEVEL", "trace");
    std::string command = std::string("\"") + exe + "\" diagnostics";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
                "diagnostic child could not be created")) {
      RemoveDirectoryA(path.c_str());
      return verdict();
    }
    expect(WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0, "diagnostic child wait failed");
    DWORD code = 1;
    expect(GetExitCodeProcess(process.hProcess, &code), "diagnostic child exit code could not be read");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const auto base = std::string(exe).substr(std::string(exe).find_last_of("\\/") + 1);
    for (const char *library : {"d3d11", "dxgi"})
      DeleteFileA((path + "/" + base.substr(0, base.find_last_of('.')) + "_" + library + ".log").c_str());
    RemoveDirectoryA(path.c_str());
    return code ? code : verdict();
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                        &device, nullptr, &context));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  D3D11_TEXTURE2D_DESC texture_desc{
      width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET
  };
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &staging));
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.ScissorEnable = raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  for (auto *record : {context.Get(), deferred.Get()}) {
    record->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    record->VSSetShader(vs.Get(), nullptr, 0);
    record->PSSetShader(ps.Get(), nullptr, 0);
    record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    record->RSSetState(raster.Get());
    record->RSSetViewports(1, &viewport);
  }
  constexpr D3D11_BUFFER_DESC params_desc{sizeof(UINT) * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  ComPtr<ID3D11Buffer> params;
  CHECK(device->CreateBuffer(&params_desc, nullptr, &params));
  for (auto *record : {context.Get(), deferred.Get()})
    record->PSSetConstantBuffers(0, 1, params.GetAddressOf());
  GetEnvironmentVariableA("DXMT_LOG_PATH", directory, sizeof(directory));
  const auto base = std::string(exe).substr(std::string(exe).find_last_of("\\/") + 1);
  const auto log = std::string(directory) + "/" + base.substr(0, base.find_last_of('.')) + "_d3d11.log";
  auto statistics = [&]() {
    std::ifstream input(log);
    std::string line;
    std::pair<UINT, UINT> result{};
    for (UINT passes, merged; std::getline(input, line);)
      if (sscanf(line.c_str(), "trace: render passes: %u, merged: %u", &passes, &merged) == 2)
        result = {passes, merged};
    return result;
  };
  ID3D11DeviceContext *record = context.Get();
  UINT bias = 0;

  const D3D11_RECT marker{LONG(query_width), 0, LONG(width), LONG(height)};
  auto rectangle = [&](UINT i, bool before_only_first, bool second_use) {
    const UINT n = i / 2;
    D3D11_RECT rect{0, 0, LONG(second_use ? query_width - n % (query_width + 1) : n % (query_width + 1)),
                    LONG(n / (query_width + 1) % height + 1)};
    for (UINT b = 0; b < boundaries; b++) {
      const UINT boundary = (b + 1) * window_segments;
      if (i >= boundary - 3 && i < boundary + 3)
        rect = (b == 2 ? i < boundary : (b || before_only_first) && i >= boundary && i < boundary + 2)
                   ? D3D11_RECT{} : D3D11_RECT{0, 0, LONG(query_width), LONG(height)};
    }
    return rect;
  };
  std::vector<UINT> want(width * height);
  auto draw = [&](const D3D11_RECT &rect) {
    record->RSSetScissorRects(1, &rect);
    record->Draw(3, 0);
    for (LONG y = rect.top; y < rect.bottom; y++)
      for (LONG x = rect.left; x < rect.right; x++)
        want[y * width + x] = bias + y * width + x + 1;
  };
  auto picture = [&]() {
    context->CopyResource(staging.Get(), target.Get());
    context->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (!expect(hr == S_OK, "pixel readback returned %#lx", hr))
      return false;
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const UINT got = ((const UINT *)((const char *)mapped.pData + y * mapped.RowPitch))[x];
        expect(got == want[y * width + x], "pixel %u,%u is %u, want %u", x, y, got, want[y * width + x]);
      }
    context->Unmap(staging.Get(), 0);
    return expect(device->GetDeviceRemovedReason() == S_OK, "device was removed during the pixel readback");
  };
  const D3D11_QUERY_DESC counter_desc{D3D11_QUERY_OCCLUSION}, predicate_desc{D3D11_QUERY_OCCLUSION_PREDICATE};
  const float clear[4] = {};
  enum { Control, Stress, Isolated, Reuse, Deferred, Flushed, ReuseFlushed, Frames };
  const char *names[Frames] = {
      "control", "stress", "isolated immediate stress", "reuse after stress", "deferred stress", "stress with Flush",
      "reuse after Flush"
  };
  for (UINT frame = 0; frame < Frames; frame++) {
    const UINT count = frame == Deferred ? stress_queries
                       : frame == Stress || frame == Isolated || frame == Reuse ? 4 * window_segments + control_queries
                       : frame == Flushed ? 2 * window_segments + control_queries
                       : control_queries;
    const char *name = names[frame];
    bias = (frame + 1) * width * height;
    const UINT values[params_desc.ByteWidth / sizeof(UINT)] = {bias};
    context->UpdateSubresource(params.Get(), 0, nullptr, values, 0, 0);
    step("%s: create %u alternating counter and predicate queries", name, count);
    std::vector<ComPtr<ID3D11Query>> queries(count);
    ComPtr<ID3D11Query> total, buffer_cross, across[boundaries];
    ComPtr<ID3D11Predicate> buffer_predicate;
    ComPtr<ID3D11Predicate> across_predicate[boundaries];
    CHECK(device->CreateQuery(&counter_desc, &total));
    CHECK(device->CreateQuery(&counter_desc, &buffer_cross));
    CHECK(device->CreatePredicate(&predicate_desc, &buffer_predicate));
    for (UINT b = 0; b < std::size(across); b++) {
      CHECK(device->CreateQuery(&counter_desc, &across[b]));
      CHECK(device->CreatePredicate(&predicate_desc, &across_predicate[b]));
    }
    for (UINT i = 0; i < count; i++) {
      if (i % 2) {
        ComPtr<ID3D11Predicate> predicate;
        CHECK(device->CreatePredicate(&predicate_desc, &predicate));
        queries[i] = predicate;
      } else {
        CHECK(device->CreateQuery(&counter_desc, &queries[i]));
      }
    }
    step("%s: pixels before the frame", name);
    context->ClearRenderTargetView(rtv.Get(), clear);
    std::fill(want.begin(), want.end(), 0);
    draw(marker);
    if (!picture())
      return verdict();

    step("%s: %u query segments", name, count);
    record = frame == Deferred ? deferred.Get() : context.Get();
    record->ClearRenderTargetView(rtv.Get(), clear);
    std::fill(want.begin(), want.end(), 0);
    UINT64 sum = 0, buffer_sum = 0, across_sum[boundaries] = {};
    BOOL across_visible[boundaries] = {};
    if (frame != Deferred && frame != Isolated)
      record->Begin(total.Get());
    for (UINT i = 0; i < count; i++) {
      if (frame != Isolated && count > 2 * control_queries && i == control_queries) {
        record->Begin(buffer_cross.Get());
        record->Begin(buffer_predicate.Get());
      }
      if (frame != Isolated && count > 2 * control_queries && i == count - control_queries) {
        record->End(buffer_predicate.Get());
        record->End(buffer_cross.Get());
      }
      for (UINT b = 0; b < std::size(across); b++) {
        if (frame == Isolated)
          break;
        const UINT boundary = (b + 1) * window_segments;
        if (i == boundary - 3) {
          record->Begin(across[b].Get());
          record->Begin(across_predicate[b].Get());
        }
        if (i == boundary + 2)
          record->End(across_predicate[b].Get());
        if (i == boundary + 3)
          record->End(across[b].Get());
      }
      if (frame == Flushed && i == window_segments)
        context->Flush();
      const auto rect = rectangle(i, frame == Deferred, frame == Reuse);
      record->Begin(queries[i].Get());
      draw(rect);
      record->End(queries[i].Get());
      UINT64 samples = UINT64(rect.right - rect.left) * (rect.bottom - rect.top);
      if (frame == Deferred) {
        draw(marker);
        samples += UINT64(marker.right - marker.left) * height;
      }
      sum += samples;
      if (i >= control_queries && i < count - control_queries)
        buffer_sum += samples;
      for (UINT b = 0; b < std::size(across); b++) {
        const UINT boundary = (b + 1) * window_segments;
        if (i >= boundary - 3 && i < boundary + 3)
          across_sum[b] += samples;
        if (i >= boundary - 3 && i < boundary + 2)
          across_visible[b] |= samples != 0;
      }
    }
    if (frame != Deferred && frame != Isolated)
      record->End(total.Get());
    draw(marker);
    if (frame == Deferred) {
      sum += UINT64(marker.right - marker.left) * height;
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(TRUE, &list));
      context->Begin(total.Get());
      context->ExecuteCommandList(list.Get(), TRUE);
      context->End(total.Get());
    }
    record = context.Get();
    step("%s: pixels after the frame", name);
    if (!picture())
      return verdict();

    step("%s: GetData for every query and the enclosing counter", name);
    auto data = [&](ID3D11Query *query, void *out, UINT size) {
      HRESULT hr;
      while ((hr = context->GetData(query, out, size, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
        Sleep(0);
      return hr;
    };
    UINT wrong = 0;
    for (UINT i = 0; i < count; i++) {
      const auto rect = rectangle(i, frame == Deferred, frame == Reuse);
      UINT64 expected = UINT64(rect.right - rect.left) * (rect.bottom - rect.top), got = ~UINT64(0);
      BOOL visible = -1;
      HRESULT hr;
      if (i % 2) {
        hr = data(queries[i].Get(), &visible, sizeof(visible));
        got = visible;
        expected = expected != 0;
      } else {
        hr = data(queries[i].Get(), &got, sizeof(got));
      }
      if (i % window_segments >= window_segments - 2 || i % window_segments == 0)
        expect(hr == S_OK && got == expected, "%s boundary query %u: HRESULT %#lx, got %llu, want %llu", name, i,
               hr, (unsigned long long)got, (unsigned long long)expected);
      if ((hr != S_OK || got != expected) && wrong++ < report_limit)
        printf("%s query %u (%s): HRESULT %#lx, got %llu, want %llu\n", name, i,
               i % 2 ? "predicate" : "counter", hr, (unsigned long long)got, (unsigned long long)expected);
    }
    printf("%s: %u wrong of %u individual queries\n", name, wrong, count);
    expect(wrong == 0, "%s has %u wrong individual queries", name, wrong);
    UINT64 got = ~UINT64(0);
    if (frame != Isolated) {
      const HRESULT hr = data(total.Get(), &got, sizeof(got));
      expect(hr == S_OK && got == sum, "%s enclosing counter: HRESULT %#lx, got %llu, want %llu", name, hr,
             (unsigned long long)got, (unsigned long long)sum);
    }
    if (frame != Isolated && count > 2 * control_queries) {
      UINT64 across_buffers = ~UINT64(0);
      BOOL visible = -1;
      expect(data(buffer_cross.Get(), &across_buffers, sizeof(across_buffers)) == S_OK && across_buffers == buffer_sum,
             "%s buffer crossing counter is %llu, want %llu", name,
             (unsigned long long)across_buffers, (unsigned long long)buffer_sum);
      expect(data(buffer_predicate.Get(), &visible, sizeof(visible)) == S_OK && visible == (buffer_sum != 0),
             "%s buffer crossing predicate is %d, want %d", name, visible, buffer_sum != 0);
    }
    for (UINT b = 0; b < std::size(across); b++) {
      if (frame == Isolated || count <= (b + 1) * window_segments + 3)
        continue;
      got = ~UINT64(0);
      const HRESULT counter_hr = data(across[b].Get(), &got, sizeof(got));
      expect(counter_hr == S_OK && got == across_sum[b],
             "%s crossing counter %u: HRESULT %#lx, got %llu, want %llu", name, b, counter_hr,
             (unsigned long long)got, (unsigned long long)across_sum[b]);
      BOOL visible = -1;
      const HRESULT predicate_hr = data(across_predicate[b].Get(), &visible, sizeof(visible));
      expect(predicate_hr == S_OK && visible == across_visible[b],
             "%s crossing predicate %u: HRESULT %#lx, got %d, want %d", name, b, predicate_hr,
             visible, across_visible[b]);
    }
    if (!expect(device->GetDeviceRemovedReason() == S_OK, "%s device was removed", name))
      return verdict();
  }
  step("one counting segment and a query-free pass, each with draws past two segment boundaries");
  context->ClearRenderTargetView(rtv.Get(), clear);
  std::fill(want.begin(), want.end(), 0);
  ComPtr<ID3D11Query> long_query, empty_query;
  CHECK(device->CreateQuery(&counter_desc, &long_query));
  CHECK(device->CreateQuery(&counter_desc, &empty_query));
  const UINT draws = 2 * window_segments + control_queries;
  for (UINT i = 0; i < draws; i++) {
    context->Begin(empty_query.Get());
    context->End(empty_query.Get());
  }
  context->Begin(long_query.Get());
  for (UINT i = 0; i < draws; i++)
    draw(marker);
  context->End(long_query.Get());
  if (!picture())
    return verdict();
  for (UINT i = 0; i < draws; i++)
    draw(marker);
  if (!picture())
    return verdict();
  for (auto query : {empty_query.Get(), long_query.Get()}) {
    UINT64 got = ~UINT64(0);
    HRESULT hr;
    while ((hr = context->GetData(query, &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
      Sleep(0);
    const UINT64 expected = query == empty_query.Get() ? 0 : UINT64(draws) * (marker.right - marker.left) * height;
    expect(hr == S_OK && got == expected, "long or empty query: HRESULT %#lx, got %llu, want %llu", hr,
           (unsigned long long)got, (unsigned long long)expected);
  }
  step("a counting pass followed by a query-free pass on the same target");
  record = deferred.Get();
  record->Begin(long_query.Get());
  draw(marker);
  record->End(long_query.Get());
  record->OMSetRenderTargets(0, nullptr, nullptr);
  record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  draw(marker);
  ComPtr<ID3D11CommandList> list;
  CHECK(deferred->FinishCommandList(TRUE, &list));
  context->ExecuteCommandList(list.Get(), TRUE);
  record = context.Get();
  if (!picture())
    return verdict();
  UINT64 got = ~UINT64(0);
  HRESULT hr;
  while ((hr = context->GetData(long_query.Get(), &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
    Sleep(0);
  const UINT64 expected = UINT64(marker.right - marker.left) * height;
  expect(hr == S_OK && got == expected, "query-free pass: HRESULT %#lx, got %llu, want %llu", hr,
         (unsigned long long)got, (unsigned long long)expected);
  step("overlapping deferred queries close consecutively, with and without intervening draws");
  for (bool separate_draws : {false, true}) {
    std::vector<ComPtr<ID3D11Query>> overlapping(window_segments);
    record = deferred.Get();
    for (auto &query : overlapping) {
      CHECK(device->CreateQuery(&counter_desc, &query));
      record->Begin(query.Get());
    }
    if (!separate_draws)
      draw(marker);
    for (auto &query : overlapping) {
      if (separate_draws)
        draw(marker);
      record->End(query.Get());
    }
    draw(marker);
    list.Reset();
    CHECK(deferred->FinishCommandList(TRUE, &list));
    const auto passes = statistics().first;
    context->ExecuteCommandList(list.Get(), TRUE);
    record = context.Get();
    if (!picture())
      return verdict();
    if (!separate_draws)
      expect(statistics().first - passes == 1, "consecutive Ends recorded %u passes, want 1",
             statistics().first - passes);
    for (UINT i = 0; i < overlapping.size(); i++) {
      got = ~UINT64(0);
      while ((hr = context->GetData(overlapping[i].Get(), &got, sizeof(got),
                                   D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
        Sleep(0);
      const UINT64 wanted = expected * (separate_draws ? i + 1 : 1);
      expect(hr == S_OK && got == wanted, "overlapping query %u returned %#lx, %llu, want %llu", i, hr,
             (unsigned long long)got, (unsigned long long)wanted);
    }
  }
  step("empty deferred queries do not split a query-free pass, including when an immediate query encloses the list");
  record = deferred.Get();
  for (UINT i = 0; i < draws; i++) {
    draw(marker);
    record->Begin(empty_query.Get());
    record->End(empty_query.Get());
  }
  list.Reset();
  CHECK(deferred->FinishCommandList(TRUE, &list));
  for (bool enclosed : {false, true}) {
    const auto passes = statistics().first;
    if (enclosed)
      context->Begin(long_query.Get());
    context->ExecuteCommandList(list.Get(), TRUE);
    if (enclosed)
      context->End(long_query.Get());
    record = context.Get();
    if (!picture())
      return verdict();
    expect(statistics().first - passes == 1, "empty deferred brackets recorded %u passes, want 1",
           statistics().first - passes);
    UINT64 empty = ~UINT64(0);
    while ((hr = context->GetData(empty_query.Get(), &empty, sizeof(empty), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
      Sleep(0);
    expect(hr == S_OK && empty == 0, "empty deferred query returned %#lx, %llu", hr, (unsigned long long)empty);
    if (enclosed) {
      got = ~UINT64(0);
      while ((hr = context->GetData(long_query.Get(), &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
        Sleep(0);
      expect(hr == S_OK && got == UINT64(draws) * expected, "enclosing empty brackets returned %#lx, %llu, want %llu",
             hr, (unsigned long long)got, (unsigned long long)(UINT64(draws) * expected));
    }
  }
  step("empty brackets inside one deferred counting query remain one segment");
  record = deferred.Get();
  record->Begin(long_query.Get());
  for (UINT i = 0; i < draws; i++) {
    draw(marker);
    record->Begin(empty_query.Get());
    record->End(empty_query.Get());
  }
  record->End(long_query.Get());
  ComPtr<ID3D11CommandList> counted;
  CHECK(deferred->FinishCommandList(TRUE, &counted));
  const auto counted_passes = statistics().first;
  context->ExecuteCommandList(counted.Get(), TRUE);
  record = context.Get();
  if (!picture())
    return verdict();
  expect(statistics().first - counted_passes == 1, "empty brackets split one counting segment into %u passes",
         statistics().first - counted_passes);
  got = ~UINT64(0);
  while ((hr = context->GetData(long_query.Get(), &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
    Sleep(0);
  expect(hr == S_OK && got == UINT64(draws) * expected, "deferred counting segment returned %#lx, %llu, want %llu",
         hr, (unsigned long long)got, (unsigned long long)(UINT64(draws) * expected));
  step("a deferred query counts the draws of a nested command list");
  ComPtr<ID3D11CommandList> nested;
  deferred->Begin(long_query.Get());
  deferred->ExecuteCommandList(list.Get(), TRUE);
  deferred->End(long_query.Get());
  CHECK(deferred->FinishCommandList(TRUE, &nested));
  context->ExecuteCommandList(nested.Get(), TRUE);
  if (!picture())
    return verdict();
  got = ~UINT64(0);
  while ((hr = context->GetData(long_query.Get(), &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
    Sleep(0);
  expect(hr == S_OK && got == UINT64(draws) * expected, "nested list returned %#lx, %llu, want %llu",
         hr, (unsigned long long)got, (unsigned long long)(UINT64(draws) * expected));
  step("compatible counting passes merge only when their combined segments fit");
  for (UINT segments : {window_segments / 2 - 1, window_segments / 2, window_segments / 2 + 1}) {
    record = deferred.Get();
    for (UINT pass = 0; pass < 2; pass++) {
      record->OMSetRenderTargets(0, nullptr, nullptr);
      record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      for (UINT i = 0; i < segments; i++) {
        record->Begin(long_query.Get());
        draw(marker);
        record->End(long_query.Get());
      }
    }
    list.Reset();
    CHECK(deferred->FinishCommandList(TRUE, &list));
    const auto [passes, merged] = statistics();
    context->ExecuteCommandList(list.Get(), TRUE);
    record = context.Get();
    if (!picture())
      return verdict();
    const auto after = statistics();
    const UINT recorded = after.first - passes, optimized = after.second - merged;
    const UINT wanted = 2 * segments <= window_segments ? 1 : 2;
    expect(recorded - optimized == wanted, "%u + %u segments: %u passes, %u merged, want %u remaining",
           segments, segments, recorded, optimized, wanted);
    got = ~UINT64(0);
    while ((hr = context->GetData(long_query.Get(), &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
      Sleep(0);
    expect(hr == S_OK && got == expected, "merged passes' final query returned %#lx, %llu, want %llu",
           hr, (unsigned long long)got, (unsigned long long)expected);
  }
  step("sparse compatible passes across a visibility buffer boundary keep distinct results");
  ComPtr<ID3D11Texture2D> targets[16];
  ComPtr<ID3D11RenderTargetView> views[std::size(targets)];
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  texture_desc.CPUAccessFlags = 0;
  for (UINT i = 0; i < std::size(targets); i++) {
    CHECK(device->CreateTexture2D(&texture_desc, nullptr, &targets[i]));
    CHECK(device->CreateRenderTargetView(targets[i].Get(), nullptr, &views[i]));
    context->ClearRenderTargetView(views[i].Get(), clear);
  }
  context->ClearRenderTargetView(rtv.Get(), clear);
  std::fill(want.begin(), want.end(), 0);
  if (!picture())
    return verdict();
  ComPtr<ID3D11Query> sparse_total;
  CHECK(device->CreateQuery(&counter_desc, &sparse_total));
  const UINT filler = stress_queries - control_queries - std::size(targets) - 2;
  UINT64 sparse_sum = 0;
  record = deferred.Get();
  for (auto &view : views) {
    record->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    draw(marker);
    sparse_sum += expected;
  }
  record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  for (UINT i = 0; i < filler; i++) {
    auto rect = rectangle(i, false, false);
    record->Begin(long_query.Get());
    draw(rect);
    record->End(long_query.Get());
    sparse_sum += UINT64(rect.right - rect.left) * (rect.bottom - rect.top);
  }
  for (auto &view : views) {
    record->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    draw(marker);
    sparse_sum += expected;
  }
  record->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  draw(marker);
  sparse_sum += expected;
  list.Reset();
  CHECK(deferred->FinishCommandList(TRUE, &list));
  context->Begin(sparse_total.Get());
  context->ExecuteCommandList(list.Get(), TRUE);
  context->End(sparse_total.Get());
  record = context.Get();
  if (!picture())
    return verdict();
  got = ~UINT64(0);
  while ((hr = context->GetData(sparse_total.Get(), &got, sizeof(got), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
    Sleep(0);
  expect(hr == S_OK && got == sparse_sum, "sparse ranges returned %#lx, %llu, want %llu", hr,
         (unsigned long long)got, (unsigned long long)sparse_sum);
  return verdict();
}
