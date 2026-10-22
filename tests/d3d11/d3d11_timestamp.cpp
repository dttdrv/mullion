// contract: timestamps measure completed preceding GPU work, including in a replayed command list.
// "The value should be sampled at the instant that the GPU is finished with all the preceding workload."
// the disjoint frequency must match that clock and be "greater than 10 MHz" (D3D11.3 specification 20.4.3-4).
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// "How frequently the GPU counter increments in Hz." (Microsoft Learn, D3D11_QUERY_DATA_TIMESTAMP_DISJOINT).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_timestamp_disjoint
// "When using this type of query, ID3D11DeviceContext::Begin is disabled." (Microsoft Learn, D3D11_QUERY).
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query
// independently sampled Metal pass time bounds the enclosing query interval from below; CPU time from
// before submission until all results are ready bounds it from above, on fast and slow GPUs alike. timestamps'
// unspecified initial value is not compared. the profile run checks that query positions share a command buffer.
// "The number of nanoseconds for a point in absolute time or Mach absolute time." (Apple, MTLTimestamp).
// https://developer.apple.com/documentation/metal/mtltimestamp
// sample capacity is discovered through Metal; more distinct positions than that exercise rollover. exhaustion
// returns E_FAIL without writing data; releasing the external sample buffers allows queries again.
#include "d3d11_test.hpp"
#include <d3d10.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <ratio>
#include <sstream>

int
main(int argc, char **argv) {
  const UINT seed = argc > 1 ? strtoul(argv[1], nullptr, 0) : 11;
  const char shader[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint ps() : SV_Target { return 1; }
RWBuffer<uint> result : register(u0);
[numthreads(1, 1, 1)] void cs() { result[0] = 1; }
)";
  auto vs_code = compile(shader, "vs", "vs"), ps_code = compile(shader, "ps", "ps");
  auto cs_code = compile(shader, "cs", "cs");
  if (!vs_code || !ps_code || !cs_code)
    return 1;
  if (argc < 3) {
    char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
    GetTempPathA(sizeof(temporary), temporary);
    modulePath(exe, sizeof(exe));
    auto unix_name =
        (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    if (!expect(unix_name != nullptr, "Wine path conversion unavailable"))
      return verdict();
    for (const char *mode : {"plain", "profile", "exhaust", "profile-exhaust"}) {
      step("seed %u mode %s", seed, mode);
      if (!expect(GetTempFileNameA(temporary, "tsq", 0, directory), "temporary name unavailable"))
        return verdict();
      DeleteFileA(directory);
      if (!expect(CreateDirectoryA(directory, nullptr), "temporary directory unavailable"))
        return verdict();
      WCHAR wide[MAX_PATH];
      MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, std::size(wide));
      char *path = unix_name(wide);
      if (!expect(path != nullptr, "Wine path conversion failed"))
        return verdict();
      SetEnvironmentVariableA("MULLION_DIAG_PATH", path);
      HeapFree(GetProcessHeap(), 0, path);
      SetEnvironmentVariableA("MULLION_DIAG", strstr(mode, "profile") ? "profile" : nullptr);
      std::string command =
          std::string("\"") + exe + "\" " + std::to_string(seed) + " " + mode + " \"" + directory + "\"";
      STARTUPINFOA startup{sizeof(startup)};
      PROCESS_INFORMATION process{};
      if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
                  "child creation failed"))
        return verdict();
      DWORD waited = WaitForSingleObject(process.hProcess, 120000);
      if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
      }
      DWORD status = 1;
      GetExitCodeProcess(process.hProcess, &status);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      expect(waited == WAIT_OBJECT_0 && status == 0, "child wait %lu exit %lu", waited, status);
      std::filesystem::remove_all(directory);
    }
    return verdict();
  }
  const bool profile = strstr(argv[2], "profile"), exhaust = strstr(argv[2], "exhaust");
  std::minstd_rand next(seed);
  auto bridge = LoadLibraryA("winemetal.dll");
  auto devices = (UINT64 (*)())GetProcAddress(bridge, "WMTCopyAllDevices");
  auto object = (UINT64 (*)(UINT64, UINT64))GetProcAddress(bridge, "NSArray_object");
  auto make = (UINT64 (*)(UINT64, UINT, bool))GetProcAddress(bridge, "MTLCounterSampleBuffer_newTimestampBuffer");
  auto release = (void (*)(UINT64))GetProcAddress(bridge, "NSObject_release");
  if (!expect(devices && object && make && release, "Metal counter bridge unavailable"))
    return verdict();
  const auto all = devices(), metal = object(all, 0);
  UINT capacity = 1;
  while (auto samples = make(metal, capacity * 2, true)) {
    release(samples);
    capacity *= 2;
  }
  std::vector<UINT64> held;
  if (exhaust)
    while (auto samples = make(metal, 1, true))
      held.push_back(samples);
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
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
  std::vector<uint32_t> expected(D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT);
  auto output = buffer(device.Get(), expected.size() * sizeof(expected[0]), D3D11_BIND_UNORDERED_ACCESS);
  if (!expect(output != nullptr, "work buffer unavailable"))
    return verdict();
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER};
  uav_desc.Buffer.NumElements = expected.size();
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateUnorderedAccessView(output.Get(), &uav_desc, &uav));
  auto work = [&](ID3D11DeviceContext *context) {
    const UINT words = 1 + next() % expected.size();
    for (UINT i = 0; i < words; i++)
      expected[i] = next();
    D3D11_BOX box{0, 0, 0, UINT(words * sizeof(expected[0])), 1, 1};
    context->UpdateSubresource(output.Get(), 0, &box, expected.data(), 0, 0);
  };
  auto ready = [&](ID3D11Query *query, void *data, UINT size) {
    HRESULT hr;
    const ULONGLONG until = GetTickCount64() + 30000;
    do {
      hr = immediate->GetData(query, data, size, 0);
      if (hr == S_FALSE)
        Sleep(1);
    } while (hr == S_FALSE && GetTickCount64() < until);
    return hr;
  };
  work(immediate.Get());
  expect(read(device.Get(), immediate.Get(), output.Get()) == expected, "warmup bytes differ");
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  std::map<std::filesystem::path, std::streamoff> offsets;
  ComPtr<ID3D11Query> failed_query;
  for (UINT variant = 0; variant < 12; variant++) {
    if (variant == 5) {
      for (auto samples : held)
        release(samples);
      held.clear();
    }
    const bool exhausted = exhaust && variant < 5;
    step("seed %u mode %s variant %u capacity %u", seed, argv[2], variant, capacity);
    if (profile)
      for (auto &entry : std::filesystem::directory_iterator(argv[3]))
        if (entry.path().extension() == ".diag")
          offsets[entry.path()] = std::filesystem::file_size(entry.path());
    next.seed(seed);
    next.discard(variant);
    const bool many = variant == 2 || variant == 5 || variant >= 9;
    const bool replay = variant == 3 || variant == 5 || variant == 6 || variant == 11;
    const UINT count = variant == 9 ? capacity - 1 : variant == 10 ? capacity
                                  : variant == 11 ? 2 * capacity + 2 : many ? capacity + 2 : 3;
    const UINT work_count = many || variant >= 7 ? count - 1 : 1;
    ComPtr<ID3D11Texture2D> target, staging;
    if (variant == 7) {
      D3D11_TEXTURE2D_DESC desc{work_count, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET};
      CHECK(device->CreateTexture2D(&desc, nullptr, &target));
      ComPtr<ID3D11RenderTargetView> view;
      CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &view));
      immediate->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
      const float clear[4]{};
      immediate->ClearRenderTargetView(view.Get(), clear);
      immediate->VSSetShader(vs.Get(), nullptr, 0);
      immediate->PSSetShader(ps.Get(), nullptr, 0);
      immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    }
    std::vector<ComPtr<ID3D11Query>> queries(count);
    std::vector<ComPtr<ID3D10Query>> ten(count);
    D3D11_QUERY_DESC timestamp{D3D11_QUERY_TIMESTAMP}, disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT};
    ComPtr<ID3D11Query> disjoint;
    CHECK(device->CreateQuery(&disjoint_desc, &disjoint));
    ComPtr<ID3D10Query> disjoint10;
    if (variant == 4)
      CHECK(disjoint.As(&disjoint10));
    for (UINT i = 0; i < count; i++) {
      CHECK(device->CreateQuery(&timestamp, &queries[i]));
      if (variant == 4)
        CHECK(queries[i].As(&ten[i]));
    }
    ComPtr<ID3D11CommandList> inner;
    if (variant == 6) {
      deferred->End(queries[1].Get());
      CHECK(deferred->FinishCommandList(FALSE, &inner));
    }
    auto context = replay ? deferred.Get() : immediate.Get();
    if (variant == 8) {
      context->CSSetShader(cs.Get(), nullptr, 0);
      context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
      expected[0] = 1;
    }
    LARGE_INTEGER began, finished, waited_from{}, waited_to{};
    QueryPerformanceCounter(&began);
    if (disjoint10)
      disjoint10->Begin();
    else
      context->Begin(disjoint.Get());
    for (UINT i = 0; i < count; i++) {
      if (ten[i])
        ten[i]->End();
      else
        context->End(queries[i].Get());
      if (variant == 1 && !i) {
        immediate->Flush();
        expect(ready(queries[i].Get(), nullptr, 0) == (exhausted ? E_FAIL : S_OK), "flush query status differs");
        QueryPerformanceCounter(&waited_from);
        Sleep(20);
        QueryPerformanceCounter(&waited_to);
      }
      if (inner && !i)
        deferred->ExecuteCommandList(inner.Get(), TRUE);
      if (i < work_count) {
        if (target) {
          D3D11_VIEWPORT viewport{float(i), 0, 1, 1, 0, 1};
          context->RSSetViewports(1, &viewport);
          context->Draw(3, 0);
        } else if (variant == 8) {
          context->Dispatch(1, 1, 1);
        } else {
          work(context);
        }
      }
    }
    if (replay) {
      context->End(disjoint.Get());
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(FALSE, &list));
      immediate->ExecuteCommandList(list.Get(), TRUE);
      immediate->ExecuteCommandList(list.Get(), TRUE);
    } else if (disjoint10) {
      disjoint10->End();
    } else {
      immediate->End(disjoint.Get());
    }
    immediate->Flush();
    std::vector<UINT64> values(count, ~UINT64(0));
    for (UINT i = 0; i < count; i++) {
      HRESULT hr = ready(queries[i].Get(), &values[i], sizeof(values[i]));
      expect(hr == (exhausted ? E_FAIL : S_OK), "query %u status %#lx", i, hr);
      if (exhausted) {
        expect(values[i] == ~UINT64(0), "failed query %u wrote data", i);
        expect(immediate->GetData(queries[i].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == E_FAIL,
               "failed query %u status-only read differs", i);
        failed_query = queries[i];
      }
      if (ten[i]) {
        UINT64 value = ~UINT64(0);
        expect(ten[i]->GetData(&value, sizeof(value), 0) == hr && value == values[i], "D3D10 query %u differs", i);
      }
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT range{};
    expect(ready(disjoint.Get(), &range, sizeof(range)) == S_OK, "disjoint query not ready");
    if (disjoint10) {
      D3D10_QUERY_DATA_TIMESTAMP_DISJOINT range10{};
      expect(disjoint10->GetData(&range10, sizeof(range10), 0) == S_OK &&
                 range10.Frequency == range.Frequency && range10.Disjoint == range.Disjoint,
             "D3D10 disjoint result differs");
    }
    QueryPerformanceCounter(&finished);
    expect(range.Frequency > 10 * std::mega::num && range.Disjoint == FALSE, "frequency %llu disjoint %d",
           range.Frequency, range.Disjoint);
    if (!exhausted) {
      for (UINT i = 0; i < count; i++) {
        if (i)
          expect(many ? values[i - 1] < values[i] : values[i - 1] <= values[i],
                 "query %u did not follow its preceding work", i);
      }
      const double seconds = double(finished.QuadPart - began.QuadPart) / frequency.QuadPart;
      const double elapsed = double(values.back() - values.front()) / range.Frequency;
      expect(values[1] > values[0] && elapsed <= seconds, "elapsed %.9g outside (0, %.9g]", elapsed, seconds);
      if (variant == 1)
        expect(elapsed >= double(waited_to.QuadPart - waited_from.QuadPart) / frequency.QuadPart,
               "elapsed did not include CPU wait across flush");
    }
    if (profile && !exhausted) {
      UINT64 lower = 0;
      std::vector<std::pair<UINT64, UINT64>> passes;
      bool together = false;
      for (auto &entry : std::filesystem::directory_iterator(argv[3])) {
        if (entry.path().extension() != ".diag")
          continue;
        std::ifstream file(entry.path());
        file.seekg(offsets[entry.path()]);
        for (std::string line; std::getline(file, line);) {
          std::istringstream stream(line);
          std::vector<std::string> fields;
          for (std::string field; std::getline(stream, field, '\t');)
            fields.push_back(field);
          if (fields.size() == 11 && fields[0] == "cmdbuf")
            together |= std::stoul(fields[10]) >= work_count + 1;
          if (!fields.empty() && fields[0] == "pass" &&
              ((fields.size() == 9 && fields[4] == (variant == 8 ? "Compute" : "Blit") && !target) ||
               (fields.size() == 11 && fields[4] == "Render" && target))) {
            UINT64 start = std::stoull(fields[fields.size() - 2]), end = std::stoull(fields.back());
            passes.push_back({start, end});
          }
        }
      }
      if (expect(passes.size() == work_count * (replay ? 2 : 1), "Metal work pass count %zu", passes.size()))
        for (UINT i = 0; i < work_count; i++) {
          auto [start, end] = passes[passes.size() - work_count + i];
          expect(start < end && values[i] < values[i + 1] &&
                     double(values[i + 1] - values[i]) / range.Frequency >= double(end - start) / std::nano::den,
                 "query interval %u [%llu, %llu] is shorter than Metal work [%llu, %llu]", i,
                 values[i], values[i + 1], start, end);
          lower += end - start;
        }
      expect(lower > 0 && double(values.back() - values.front()) / range.Frequency >= double(lower) / std::nano::den,
             "elapsed is shorter than Metal work %llu ns", lower);
      if (variant != 1)
        expect(together, "query positions did not share one command buffer");
    }
    expect(read(device.Get(), immediate.Get(), output.Get()) == expected, "work bytes differ");
    if (target) {
      immediate->CopyResource(staging.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      for (UINT i = 0; i < work_count; i++)
        expect(static_cast<const UINT *>(mapped.pData)[i] == 1, "render pixel %u lost across timestamp", i);
      immediate->Unmap(staging.Get(), 0);
    }
  }
  for (auto samples : held)
    release(samples);
  if (exhaust) {
    step("seed %u recover after counter exhaustion", seed);
    immediate->End(failed_query.Get());
    UINT64 value = 0;
    expect(ready(failed_query.Get(), &value, sizeof(value)) == S_OK && value != ~UINT64(0), "recovery query failed");
  }
  release(all);
  FreeLibrary(bridge);
  return verdict();
}
