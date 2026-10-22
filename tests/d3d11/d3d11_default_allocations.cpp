// contract: whole default-buffer updates preserve the bytes of every pending draw and share storage as dynamic
// discards do. "The CPU copies data from memory to a subresource created in non-mappable memory" (Microsoft Learn,
// ID3D11DeviceContext::UpdateSubresource):
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource
// each list holds every name alive until measurement, then every draw's integer pixel is read. a profile buffer
// record means one successful Metal buffer creation by BufferAllocation, not bytes charged or names reused. the
// bound is the count for identical writes using dynamic DISCARD; both start with a fresh deferred context and
// retain every name. MULLION_PAGE_SIZE is the allocator's page. sizes cross its minimum and page branches; twice the
// number of uint4 constants in the larger of it and GetSystemInfo's page plus one updates cross every
// suballocation boundary. the name FIFO has no fixed cap. scratch and query buffers are outside this count.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

using Row = std::array<UINT, 4>;

static const char hlsl[] = R"hlsl(
cbuffer Constants : register(b0) { uint4 value; };
float4 vs() : SV_Position { return float4(0, 0, 0, 1); }
uint4 ps() : SV_Target { return value; }
)hlsl";

static UINT64
created(const char *directory) {
  UINT64 count = 0;
  for (auto &entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".diag") {
      std::ifstream file(entry.path());
      for (std::string line; std::getline(file, line);)
        count += line.starts_with("buffer\t");
    }
  return count;
}

int
main(int argc, char **argv) {
  step("compile every shader before creating a device");
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!vs_code || !ps_code)
    return 1;
  if (argc > 1 && !strcmp(argv[1], "shaders"))
    return verdict();
  if (argc != 3 || strcmp(argv[1], "child")) {
    char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
    GetTempPathA(sizeof(temporary), temporary);
    modulePath(exe, sizeof(exe));
    CHECK(GetTempFileNameA(temporary, "alc", 0, directory) ? S_OK : E_FAIL);
    DeleteFileA(directory);
    CHECK(CreateDirectoryA(directory, nullptr) ? S_OK : E_FAIL);
    auto unix_name =
        (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    CHECK(unix_name ? S_OK : E_FAIL);
    WCHAR wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, std::size(wide));
    char *path = unix_name(wide);
    CHECK(path ? S_OK : E_FAIL);
    SetEnvironmentVariableA("MULLION_DIAG_PATH", path);
    HeapFree(GetProcessHeap(), 0, path);
    SetEnvironmentVariableA("MULLION_DIAG", "profile");
    std::string command = std::string("\"") + exe + "\" child \"" + directory + "\"";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)
              ? S_OK : E_FAIL);
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
    expect(status == 0, "allocation test child failed with %lu", status);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    expect(!error, "temporary directory removal failed");
    return verdict();
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const uint64_t page = system.dwPageSize;
  CHECK(page ? S_OK : E_FAIL);
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  const UINT updates = 2 * std::max<uint64_t>(page, MULLION_PAGE_SIZE) / sizeof(Row) + 1;
  std::vector<UINT> sizes;
  for (UINT bytes = sizeof(Row); bytes < MULLION_PAGE_SIZE; bytes *= 2)
    sizes.push_back(bytes);
  sizes.insert(sizes.end(), {MULLION_PAGE_SIZE - sizeof(Row), MULLION_PAGE_SIZE, MULLION_PAGE_SIZE + sizeof(Row)});
  for (UINT bytes : sizes)
    for (bool initialized : {false, true}) {
      step("%u bytes, initial data %u, %u updates (%llu bytes written)", bytes, initialized, updates,
           uint64_t(bytes) * updates);
      D3D11_TEXTURE2D_DESC td{updates, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> target, staging;
      ComPtr<ID3D11RenderTargetView> rtv;
      CHECK(device->CreateTexture2D(&td, nullptr, &target));
      CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
      td.Usage = D3D11_USAGE_STAGING;
      td.BindFlags = 0;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&td, nullptr, &staging));
      ComPtr<ID3D11Buffer> inputs[2];
      ComPtr<ID3D11CommandList> lists[2];
      UINT64 counts[2]{};
      for (UINT mode = 0; mode < std::size(inputs); mode++) {
        ComPtr<ID3D11DeviceContext> deferred;
        CHECK(device->CreateDeferredContext(0, &deferred));
        std::vector<Row> data(bytes / sizeof(Row));
        D3D11_BUFFER_DESC bd{bytes, mode ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                               mode ? 0u : D3D11_CPU_ACCESS_WRITE};
        D3D11_SUBRESOURCE_DATA initial{data.data()};
        const auto initial_count = created(argv[2]);
        CHECK(device->CreateBuffer(&bd, initialized ? &initial : nullptr, &inputs[mode]));
        if (!expect(created(argv[2]) == initial_count + 1, "CreateBuffer did not record one resource buffer"))
          return verdict();
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        deferred->VSSetShader(vs.Get(), nullptr, 0);
        deferred->PSSetShader(ps.Get(), nullptr, 0);
        deferred->PSSetConstantBuffers(0, 1, inputs[mode].GetAddressOf());
        deferred->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const auto before = created(argv[2]);
        for (UINT n = 0; n < updates; n++) {
          for (UINT c = 0; c < data.front().size(); c++)
            data.front()[c] = n * data.front().size() + c + 1;
          if (mode) {
            deferred->UpdateSubresource(inputs[mode].Get(), 0, nullptr, data.data(), 0, 0);
          } else {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            CHECK(deferred->Map(inputs[mode].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, data.data(), bytes);
            deferred->Unmap(inputs[mode].Get(), 0);
          }
          D3D11_VIEWPORT viewport{float(n), 0, 1, 1, 0, 1};
          deferred->RSSetViewports(1, &viewport);
          deferred->Draw(1, 0);
        }
        CHECK(deferred->FinishCommandList(FALSE, &lists[mode]));
        const auto after = created(argv[2]);
        if (!expect(after > before, "no Metal buffer creations recorded for mode %u", mode))
          return verdict();
        counts[mode] = after - before;
      }
      printf("dynamic %llu, default %llu resource buffers created\n", counts[0], counts[1]);
      expect(counts[1] <= counts[0], "whole default updates create %llu Metal buffers, dynamic creates %llu",
             counts[1], counts[0]);
      for (UINT mode = 0; mode < std::size(lists); mode++) {
        immediate->ExecuteCommandList(lists[mode].Get(), FALSE);
        immediate->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        auto pixels = static_cast<const Row *>(mapped.pData);
        for (UINT n = 0; n < updates; n++)
          for (UINT c = 0; c < pixels[n].size(); c++)
            if (!expect(pixels[n][c] == n * pixels[n].size() + c + 1, "mode %u pixel %u channel %u: %#x", mode, n,
                        c, pixels[n][c])) {
              immediate->Unmap(staging.Get(), 0);
              return verdict();
            }
        immediate->Unmap(staging.Get(), 0);
      }
    }
  return verdict();
}
