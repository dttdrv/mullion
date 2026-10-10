// contract: DXMT_DIAG=profile records the final Metal passes, their targets, pipeline runs and CPU/GPU times,
// and real presents; unset or any other value writes nothing and leaves rendered results unchanged.
// "When the GPU starts or finishes a stage, it samples the counters" (Apple, Sampling GPU data into counter
// sample buffers, Sample counters at stage boundaries, https://developer.apple.com/documentation/metal/
// sampling-gpu-data-into-counter-sample-buffers). "The value should be sampled at the instant that the GPU is
// finished with all the preceding workload." (D3D11.3 functional specification, 20.4.3).
// vertex and index counts include instances: "same set repeated for each Instance" (D3D11.3 8.4 and 8.6).
// a triangle covers the target, each shader writes its chosen value, and dispatches write that value into a buffer.
// the profile's common records are D3D12's; cmdbuf adds submission time and counts of passes, presents and timestamps.
// "Check this value after the waitUntilCompleted() method returns" (Apple, MTLCommandBuffer.gpuStartTime).
// event-only chunks have no GPU encoder; their clocks may be zero, but every chunk with GPU work must be timed.
#include "d3d11_test.hpp"
#include "../../src/winemetal/winemetal.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

static constexpr UINT width = 8, height = 4, frames = 40;
static constexpr UINT indices[] = {0, 1, 2}, direct_instances[] = {1, 2};
static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint ps() : SV_Target { return VALUE; }
RWBuffer<uint> result : register(u0);
[numthreads(1, 1, 1)] void cs() { result[0] = VALUE; }
)hlsl";

static std::vector<std::string> fields(const std::string &line, char separator = '\t') {
  std::vector<std::string> out;
  std::istringstream stream(line);
  for (std::string field; std::getline(stream, field, separator);)
    out.push_back(field);
  return out;
}

static UINT64 number(const std::string &text) {
  UINT64 value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  expect(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "invalid number %s", text.c_str());
  return value;
}

static int child(const char *directory, ComPtr<ID3DBlob> vs_code, const ComPtr<ID3DBlob> (&codes)[2],
                 ComPtr<ID3DBlob> cs_code, ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> context) {
  ComPtr<ID3D11DeviceContext> deferred;
  CHECK(device->CreateDeferredContext(0, &deferred));
  step("submit an empty deferred list: event signalling without a GPU encoder");
  ComPtr<ID3D11CommandList> empty;
  CHECK(deferred->FinishCommandList(FALSE, &empty));
  context->ExecuteCommandList(empty.Get(), FALSE);
  context->Flush();
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps[2];
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  for (UINT i = 0; i < std::size(ps); i++)
    CHECK(device->CreatePixelShader(codes[i]->GetBufferPointer(), codes[i]->GetBufferSize(), nullptr, &ps[i]));
  ComPtr<ID3D11ComputeShader> cs;
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
  auto window = CreateWindowA("STATIC", "d3d11_profile", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              GetModuleHandleA(nullptr), nullptr);
  if (!expect(window != nullptr, "window creation failed"))
    return verdict();
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory> factory;
  ComPtr<IDXGISwapChain> chain;
  CHECK(device.As(&dxgi));
  CHECK(dxgi->GetAdapter(&adapter));
  CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC swap{{width, height, {}, DXGI_FORMAT_R8G8B8A8_UNORM},
                            {1, 0},
                            DXGI_USAGE_RENDER_TARGET_OUTPUT,
                            2,
                            window,
                            TRUE,
                            DXGI_SWAP_EFFECT_DISCARD};
  CHECK(factory->CreateSwapChain(device.Get(), &swap, &chain));
  D3D11_TEXTURE2D_DESC desc{
      width, height, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> targets[2], staging;
  ComPtr<ID3D11RenderTargetView> views[2];
  for (UINT i = 0; i < std::size(targets); i++) {
    CHECK(device->CreateTexture2D(&desc, nullptr, &targets[i]));
    CHECK(device->CreateRenderTargetView(targets[i].Get(), nullptr, &views[i]));
  }
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
  auto output = buffer(device.Get(), sizeof(UINT), D3D11_BIND_UNORDERED_ACCESS);
  if (!expect(output != nullptr, "output buffer creation failed"))
    return verdict();
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER};
  uav_desc.Buffer.NumElements = 1;
  ComPtr<ID3D11UnorderedAccessView> uav;
  CHECK(device->CreateUnorderedAccessView(output.Get(), &uav_desc, &uav));
  D3D11_BUFFER_DESC index_desc{sizeof(indices), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER};
  D3D11_SUBRESOURCE_DATA index_data{indices};
  ComPtr<ID3D11Buffer> index_buffer;
  CHECK(device->CreateBuffer(&index_desc, &index_data, &index_buffer));
  const UINT arguments[] = {std::size(indices), 1, 0, 0, 0, 1, 1, 1};
  D3D11_BUFFER_DESC argument_desc{sizeof(arguments), D3D11_USAGE_DEFAULT};
  argument_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  D3D11_SUBRESOURCE_DATA argument_data{arguments};
  ComPtr<ID3D11Buffer> indirect;
  CHECK(device->CreateBuffer(&argument_desc, &argument_data, &indirect));
  UINT quality = 0;
  CHECK(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 2, &quality));
  if (!expect(quality > 0, "two-sample RGBA8 target unavailable"))
    return verdict();
  D3D11_TEXTURE2D_DESC ms_desc{
      width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {2, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> multisampled, resolved, color_staging;
  ComPtr<ID3D11RenderTargetView> ms_view;
  CHECK(device->CreateTexture2D(&ms_desc, nullptr, &multisampled));
  CHECK(device->CreateRenderTargetView(multisampled.Get(), nullptr, &ms_view));
  ms_desc.SampleDesc.Count = 1;
  CHECK(device->CreateTexture2D(&ms_desc, nullptr, &resolved));
  ms_desc.Usage = D3D11_USAGE_STAGING;
  ms_desc.BindFlags = 0;
  ms_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  CHECK(device->CreateTexture2D(&ms_desc, nullptr, &color_staging));
  auto bridge = LoadLibraryA("winemetal.dll");
  auto devices = (UINT64 (*)())GetProcAddress(bridge, "WMTCopyAllDevices");
  auto object = (UINT64 (*)(UINT64, UINT64))GetProcAddress(bridge, "NSArray_object");
  auto make = (UINT64 (*)(UINT64, UINT, bool))GetProcAddress(bridge, "MTLCounterSampleBuffer_newTimestampBuffer");
  auto release = (void (*)(UINT64))GetProcAddress(bridge, "NSObject_release");
  if (!expect(devices && object && make && release, "counter buffer bridge unavailable"))
    return verdict();
  auto all = devices(), metal = object(all, 0);
  UINT capacity = 1;
  for (;;) {
    auto samples = make(metal, capacity * 2, true);
    if (!samples)
      break;
    release(samples);
    capacity *= 2;
  }
  release(all);
  FreeLibrary(bridge);
  const UINT burst = capacity / WMTTimestampSamplesPerStage + 1;
  std::ofstream(std::string(directory) + "\\facts") << burst << '\n';
  D3D11_RASTERIZER_DESC raster_desc{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> raster;
  CHECK(device->CreateRasterizerState(&raster_desc, &raster));
  D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
  std::ofstream pixels(std::string(directory) + "\\pixels", std::ios::binary);
  for (UINT frame = 0; frame < frames; frame++) {
    step("frame %u: %s context, two targets, repeated and changed pipelines, a dispatch and readback", frame,
         frame % 2 ? "deferred" : "immediate");
    auto draw = frame % 2 ? deferred.Get() : context.Get();
    draw->VSSetShader(vs.Get(), nullptr, 0);
    draw->RSSetState(raster.Get());
    draw->RSSetViewports(1, &viewport);
    draw->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    draw->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT target = 0; target < std::size(targets); target++) {
      auto rtv = views[target].Get();
      draw->OMSetRenderTargets(1, &rtv, nullptr);
      const float clear[4]{};
      draw->ClearRenderTargetView(rtv, clear);
      draw->PSSetShader(ps[0].Get(), nullptr, 0);
      draw->Draw(0, 0);
      draw->Draw(std::size(indices), 0);
      draw->DrawInstanced(std::size(indices), direct_instances[1], 0, 0);
      draw->DrawInstancedIndirect(indirect.Get(), 0);
      draw->PSSetShader(ps[1].Get(), nullptr, 0);
      draw->DrawIndexed(std::size(indices), 0, 0);
      draw->DrawIndexedInstanced(std::size(indices), direct_instances[1], 0, 0, 0);
      draw->DrawIndexedInstancedIndirect(indirect.Get(), 0);
    }
    auto unordered = uav.Get();
    draw->CSSetShader(cs.Get(), nullptr, 0);
    draw->CSSetUnorderedAccessViews(0, 1, &unordered, nullptr);
    draw->Dispatch(1, 1, 1);
    draw->DispatchIndirect(indirect.Get(), 5 * sizeof(UINT));
    if (frame % 2) {
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(FALSE, &list));
      context->ExecuteCommandList(list.Get(), FALSE);
    }
    for (UINT target = 0; target < std::size(targets); target++) {
      context->CopyResource(staging.Get(), targets[target].Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      for (UINT y = 0; y < height; y++) {
        auto row = (const UINT *)((const char *)mapped.pData + y * mapped.RowPitch);
        for (UINT x = 0; x < width; x++)
          expect(row[x] == 2, "wrong target %u pixel %u/%u", target, x, y);
        pixels.write((const char *)row, width * sizeof(UINT));
      }
      context->Unmap(staging.Get(), 0);
    }
    auto words = read(device.Get(), context.Get(), output.Get());
    expect(words == std::vector<uint32_t>{3}, "wrong compute output");
    const float clear[4]{};
    context->ClearRenderTargetView(views[0].Get(), clear);
    context->CopyResource(staging.Get(), targets[0].Get());
    D3D11_MAPPED_SUBRESOURCE cleared;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &cleared));
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++)
        expect(((const UINT *)((const char *)cleared.pData + y * cleared.RowPitch))[x] == 0,
               "standalone clear missed pixel %u/%u", x, y);
    context->Unmap(staging.Get(), 0);
    const float red[4] = {1, 0, 0, 1};
    context->ClearRenderTargetView(ms_view.Get(), red);
    context->ResolveSubresource(resolved.Get(), 0, multisampled.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    context->CopyResource(color_staging.Get(), resolved.Get());
    CHECK(context->Map(color_staging.Get(), 0, D3D11_MAP_READ, 0, &cleared));
    for (UINT y = 0; y < height; y++)
      for (UINT x = 0; x < width; x++) {
        const UINT8 want[] = {255, 0, 0, 255};
        expect(!memcmp((const char *)cleared.pData + y * cleared.RowPitch + x * sizeof(want), want, sizeof(want)),
               "resolve missed pixel %u/%u", x, y);
      }
    context->Unmap(color_staging.Get(), 0);
    CHECK(chain->Present(0, DXGI_PRESENT_TEST));
    CHECK(chain->Present(0, 0));
    context->CopyResource(staging.Get(), targets[0].Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    context->Unmap(staging.Get(), 0);
  }
  step("more clear passes than one sample buffer holds: %u", burst);
  const float black[4]{};
  for (UINT i = 0; i < burst; i++)
    context->ClearRenderTargetView(views[0].Get(), black);
  context->CopyResource(staging.Get(), targets[0].Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
  expect(*(const UINT *)mapped.pData == 0, "overflow clear wrong");
  context->Unmap(staging.Get(), 0);
  context->ClearState();
  deferred->ClearState();
  DestroyWindow(window);
  return verdict();
}

int main(int argc, char **argv) {
  auto vs = compile(hlsl, "vs", "vs", {"VALUE=1"});
  ComPtr<ID3DBlob> ps[] = {compile(hlsl, "ps", "ps", {"VALUE=1"}), compile(hlsl, "ps", "ps", {"VALUE=2"})};
  auto cs = compile(hlsl, "cs", "cs", {"VALUE=3"});
  if (!expect(vs && ps[0] && ps[1] && cs, "HLSL did not compile"))
    return verdict();
  if (argc > 1 && !strcmp(argv[1], "shaders"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  if (argc > 2)
    return child(argv[2], vs, ps, cs, device, context);
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  auto unix_name =
      (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
  if (!expect(unix_name != nullptr, "Wine path conversion unavailable"))
    return verdict();
  std::string baseline;
  const char *settings[] = {nullptr, "profiles", "profile"};
  for (const char *setting : settings) {
    step("DXMT_DIAG=%s", setting ? setting : "unset");
    if (!expect(GetTempFileNameA(temporary, "prf", 0, directory), "temporary name unavailable"))
      return verdict();
    DeleteFileA(directory);
    if (!expect(CreateDirectoryA(directory, nullptr), "temporary directory unavailable"))
      return verdict();
    WCHAR wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, std::size(wide));
    char *path = unix_name(wide);
    if (!expect(path != nullptr, "Wine path conversion failed"))
      return verdict();
    SetEnvironmentVariableA("DXMT_DIAG_PATH", path);
    HeapFree(GetProcessHeap(), 0, path);
    SetEnvironmentVariableA("DXMT_DIAG", setting);
    std::string command = std::string("\"") + exe + "\" child \"" + directory + "\"";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
                "child creation failed"))
      return verdict();
    auto waited = WaitForSingleObject(process.hProcess, 60000);
    if (waited != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 60000);
    }
    expect(waited == WAIT_OBJECT_0, "child timed out");
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(status == 0, "child failed with %lu", status);
    std::ifstream pixels(std::string(directory) + "\\pixels", std::ios::binary);
    std::string output((std::istreambuf_iterator<char>(pixels)), {});
    if (!setting)
      baseline = output;
    expect(!output.empty() && output == baseline, "profiling changed pixels");
    pixels.close();
    std::ifstream facts(std::string(directory) + "\\facts");
    UINT burst = 0;
    facts >> burst;
    expect(bool(facts) && burst > 0, "counter capacity facts missing");
    facts.close();
    using Key = std::pair<std::string, UINT64>;
    std::map<Key, std::vector<std::string>> buffers;
    std::vector<std::vector<std::string>> passes, targets, presented;
    UINT files = 0, presents = 0, submits = 0, present_spans = 0, encodes = 0;
    UINT64 last_frame = 0;
    for (auto &entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() != ".diag")
        continue;
      files++;
      std::ifstream file(entry.path());
      for (std::string line; std::getline(file, line);) {
        auto row = fields(line);
        if (!expect(!row.empty(), "empty profile row"))
          continue;
        if (row[0] == "cmdbuf" && expect(row.size() == 11, "bad cmdbuf row")) {
          step("command buffer %s/%s: %s passes, %s presents, %s timestamps, GPU %s..%s", row[1].c_str(),
               row[2].c_str(), row[8].c_str(), row[9].c_str(), row[10].c_str(), row[5].c_str(), row[6].c_str());
          expect(row[1].starts_with("d3d11:") && number(row[1].substr(row[1].find(':') + 1)) && number(row[2]),
                 "invalid command buffer identity");
          expect(buffers.emplace(Key{row[1], number(row[2])}, row).second, "duplicate command buffer");
          expect(number(row[3]) > 0 && number(row[4]) >= number(row[3]) && number(row[7]) <= number(row[3]),
                 "command buffer CPU times reversed");
          const auto start = number(row[5]), end = number(row[6]);
          const bool work = number(row[8]) || number(row[9]) || number(row[10]);
          expect(work ? start > 0 && end > start : start <= end && (start == 0) == (end == 0),
                 "command buffer GPU time does not match its work");
        } else if (row[0] == "pass")
          passes.push_back(row);
        else if (row[0] == "target")
          targets.push_back(row);
        else if (row[0] == "frame" && expect(row.size() == 4, "bad frame row")) {
          presents++;
          presented.push_back(row);
          expect(number(row[3]) > last_frame, "frame times reversed");
          last_frame = number(row[3]);
        } else if (row[0] == "span" && expect(row.size() == 6, "bad span row")) {
          submits += row[1] == "execute";
          present_spans += row[1] == "present";
          encodes += row[1] == "encode";
          expect(row[1] == "execute" || row[1] == "present" || row[1] == "encode", "unknown CPU span");
          expect(number(row[3]) && number(row[4]) && number(row[5]), "empty CPU span");
        } else if (row[0] != "clock")
          expect(false, "unexpected record %s", row[0].c_str());
      }
    }
    const bool profiling = setting && !strcmp(setting, "profile");
    expect(files == UINT(profiling), "got %u profile files", files);
    if (profiling) {
      expect(presents == frames && present_spans == frames && submits == buffers.size() && encodes == buffers.size(),
             "missing or extra frames/submissions");
      for (auto &present : presented) {
        Key key{present[1], number(present[2])};
        if (expect(buffers.count(key), "present has no command buffer"))
          expect(number(buffers[key][7]) <= number(present[3]) && number(present[3]) <= number(buffers[key][3]),
                 "present outside submission interval");
      }
      UINT renders = 0, computes = 0, blits = 0, clears = 0, resolves = 0;
      std::vector<UINT> frame_renders(frames), frame_computes(frames);
      std::set<std::string> render_pipelines, compute_pipelines;
      std::set<std::tuple<Key, UINT64>> seen;
      std::map<Key, UINT64> encoding;
      std::map<Key, UINT> pass_counts;
      std::map<std::tuple<Key, UINT64>, std::string> kinds;
      for (auto &row : passes) {
        if (!expect(row.size() == 9 || row.size() == 11, "bad pass row"))
          continue;
        Key key{row[1], number(row[2])};
        if (!expect(buffers.count(key), "pass has no command buffer"))
          continue;
        auto &buffer = buffers[key];
        pass_counts[key]++;
        UINT frame = 0;
        while (frame < presented.size() && number(buffer[7]) > number(presented[frame][3]))
          frame++;
        step("profile: frame %u, chunk %llu, pass %s %s", frame, (unsigned long long)key.second,
             row[3].c_str(), row[4].c_str());
        expect(number(row[3]) && seen.emplace(key, number(row[3])).second, "invalid or duplicate pass");
        kinds[{key, number(row[3])}] = row[4];
        expect(number(row[5]) > 0, "empty encoding time");
        encoding[key] += number(row[5]);
        UINT64 gpu = 0;
        auto finish = number(buffer[6]);
        const UINT64 rounding =
            UINT64(std::ceil(std::nextafter(double(finish) / 1e9, std::numeric_limits<double>::infinity()) * 1e9 -
                             finish)) +
            1;
        for (size_t i = 7; i < row.size(); i += 2) {
          auto start = number(row[i]), end = number(row[i + 1]);
          expect(start != ~UINT64(0) && end >= start && start + rounding >= number(buffer[5]) &&
                     end <= finish + rounding,
                 "GPU stage missing or outside command buffer");
          gpu += end - start;
        }
        expect(gpu > 0, "empty pass GPU time");
        auto runs = fields(row[6], '|');
        if (row[4] == "Render") {
          renders++;
          if (expect(frame < frames, "render attributed after final present"))
            frame_renders[frame]++;
          expect(row.size() == 11 && runs.size() == 2, "render stages or pipeline runs missing");
          for (UINT i = 0; i < runs.size(); i++) {
            auto run = fields(runs[i], '*');
            if (!expect(run.size() == 4, "bad pipeline run"))
              continue;
            expect(run[0].starts_with("Render:") && number(run[0].substr(run[0].find(':') + 1)),
                   "invalid render pipeline identity");
            render_pipelines.insert(run[0]);
            expect(!run[0].empty() && number(run[1]) == std::size(direct_instances) + 1 &&
                       number(run[2]) == (i ? 0u : std::size(indices) * (direct_instances[0] + direct_instances[1])) &&
                       number(run[3]) == (i ? std::size(indices) * (direct_instances[0] + direct_instances[1]) : 0u),
                   "wrong draw or vertex count");
          }
        } else if (row[4] == "Compute") {
          computes++;
          if (expect(frame < frames, "compute attributed after final present"))
            frame_computes[frame]++;
          expect(runs.size() == 1, "compute pipeline missing");
          if (runs.size() == 1) {
            auto run = fields(runs[0], '*');
            if (expect(run.size() == 4, "bad compute run")) {
              expect(run[0].starts_with("Compute:") && number(run[0].substr(run[0].find(':') + 1)),
                     "invalid compute pipeline identity");
              compute_pipelines.insert(run[0]);
              expect(number(run[1]) == 2 && number(run[2]) == 0 && number(run[3]) == 0, "wrong dispatch count");
            }
          }
        } else if (row[4] == "Blit") {
          blits++;
          expect(runs.empty(), "blit has a pipeline");
        } else if (row[4] == "Clear" || row[4] == "Resolve") {
          clears += row[4] == "Clear";
          resolves += row[4] == "Resolve";
          expect(row.size() == 9 && runs.empty(), "clear or resolve has a pipeline or wrong stages");
        } else
          expect(false, "unexpected pass kind %s", row[4].c_str());
      }
      expect(renders == frames * 2 && computes == frames && blits >= frames && clears == frames * 2 + burst &&
                 resolves == frames,
             "passes lost or misclassified");
      for (UINT frame = 0; frame < frames; frame++)
        expect(frame_renders[frame] == 2 && frame_computes[frame] == 1, "frame %u lost its passes", frame);
      expect(render_pipelines.size() == 2 && compute_pipelines.size() == 1, "pipelines changed on reuse");
      for (auto &[key, total] : encoding)
        expect(total <= number(buffers[key][4]) - number(buffers[key][7]), "encoding exceeds submission interval");
      UINT empty_buffers = 0;
      for (auto &[key, buffer] : buffers) {
        expect(number(buffer[8]) == pass_counts[key] && number(buffer[10]) == 0, "wrong command buffer work counts");
        const bool presents = std::any_of(presented.begin(), presented.end(), [&](auto &row) {
          return row[1] == key.first && number(row[2]) == key.second;
        });
        expect(number(buffer[9]) == UINT(presents), "command buffer present count wrong");
        empty_buffers += !pass_counts[key] && !presents;
      }
      expect(empty_buffers > 0, "empty deferred list did not reach an event-only chunk");
      std::set<std::string> textures;
      std::map<std::tuple<Key, UINT64>, UINT> attachments;
      std::map<std::tuple<Key, UINT64>, std::set<std::string>> bindings;
      for (auto &row : targets)
        if (expect(row.size() == 12, "bad target row")) {
          auto pass = std::tuple{Key{row[1], number(row[2])}, number(row[3])};
          expect(seen.count(pass), "target has no pass");
          attachments[pass]++;
          bindings[pass].insert(row[4]);
          expect((kinds[pass] == "Render" || kinds[pass] == "Clear") ? row[4] == "color" : kinds[pass] == "Resolve",
                 "target bound to wrong pass kind");
          expect(kinds[pass] != "Render" || number(row[9]) == WMTPixelFormatR32Uint, "wrong render target format");
          expect(kinds[pass] != "Resolve" || number(row[9]) == WMTPixelFormatRGBA8Unorm, "wrong resolve target format");
          expect((row[4] == "color" || row[4] == "resolve") && number(row[5]) == 0 && number(row[6]) &&
                     number(row[7]) == width && number(row[8]) == height &&
                     (number(row[9]) == WMTPixelFormatR32Uint || number(row[9]) == WMTPixelFormatRGBA8Unorm) &&
                     number(row[10]) == 0 && number(row[11]) == 0,
                 "target metadata wrong");
          textures.insert(row[6]);
        }
      expect(targets.size() == frames * 6 + burst && textures.size() == 4, "targets lost or mixed");
      for (auto &[pass, kind] : kinds) {
        expect(attachments[pass] == (kind == "Resolve" ? 2u : kind == "Render" || kind == "Clear" ? 1u : 0u),
               "pass lost or gained targets");
        if (kind == "Resolve")
          expect(bindings[pass] == std::set<std::string>{"color", "resolve"}, "resolve lost source or destination");
      }
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    expect(!error, "temporary directory removal failed");
  }
  return verdict();
}
