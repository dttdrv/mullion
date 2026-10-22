// contract: whole default updates and dynamic discards preserve each pending reader while replacement storage
// grows only after all names in an allocation are pending. "The entire operation can be converted to be analogous
// to the UpdateSubresource scenario on the same Deferred Context." (D3D11.3 6.3.8). "When returning a pointer to
// the mapped resource, the pointer must be 16-byte aligned." (5.6.1). words at both ends give independent pixels;
// a staging copy checks every byte of the last name. a profile buffer record (time, handle, length) counts one
// successful BufferAllocation construction. shaders are compiled before any device is made.
// the storage contract is a page (MULLION_PAGE_SIZE from the build) below a page, otherwise one name, followed by
// twice the names when the preceding allocation filled with all names pending, capped at the queue's block.
// stride s rounds a name to Map's alignment. a page holds c = floor(page / s) names, or one above a page;
// a deferred list starts with c;
// the immediate context has c - 1 unused names initially, then another c before its queue can justify growth.
// sum these capacities until n fits, doubling c up to floor(block / s), then adding that many each time. this
// derives the allocation count and lengths at every first and last name, without using allocator headers.
// an unexecuted list gives exact counts; immediate counts are upper bounds because names may finish meanwhile.
// the geometric sum is below twice its final term: total created bytes <= twice peak name bytes + page + name.
// n unexecuted draws make n names pending; the extra page and name cover initial storage and page rounding.
// one rewrite per resource uses the original page or name;
// one rewrite per completed frame must never exceed that length. 10,000 distinct resources check the first rule.
// the block is observed from a page-sized buffer with two maximum-width target rows plus one pending rewrite
// (D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, D3D11.3 21). its last three allocations must plateau after growing;
// otherwise calibration fails, rather than accepting an unbounded allocation as the block. other sizes include
// a word past a page and three nearly equal names in that observed block, crossing 16-byte rounding separately.
// a name above the block stays alone at its original length. all other workloads pass capacities 1 + 2 + 4 + 8
// of their first allocation, or the block cap if reached first. page-sized workloads also pass every doubling.
#include "d3d11_test.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

using Row = std::array<UINT, 4>;

static const char hlsl[] = R"hlsl(
Buffer<uint> words : register(t0);
float4 vs() : SV_Position { return float4(0, 0, 0, 1); }
uint4 ps() : SV_Target {
  uint last = words[0];
  return uint4(words[1], words[2], words[last - 1], words[last]);
}
)hlsl";

static std::vector<UINT64>
created(const char *directory) {
  std::vector<std::pair<UINT64, UINT64>> records;
  for (auto &entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".diag") {
      std::ifstream file(entry.path());
      for (std::string line; std::getline(file, line);)
        if (line.starts_with("buffer\t")) {
          std::istringstream fields(line);
          std::string kind, handle;
          UINT64 time = 0, length = 0;
          fields >> kind >> time >> handle >> length;
          expect(!fields.fail() && length, "profile buffer record is malformed: %s", line.c_str());
          records.emplace_back(time, length);
        }
    }
  std::stable_sort(records.begin(), records.end(), [](auto a, auto b) { return a.first < b.first; });
  std::vector<UINT64> lengths;
  for (auto [time, length] : records)
    lengths.push_back(length);
  return lengths;
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
    CHECK(GetTempPathA(sizeof(temporary), temporary) ? S_OK : E_FAIL);
    CHECK(modulePath(exe, sizeof(exe)) ? S_OK : E_FAIL);
    CHECK(GetTempFileNameA(temporary, "grw", 0, directory) ? S_OK : E_FAIL);
    CHECK(DeleteFileA(directory) ? S_OK : E_FAIL);
    CHECK(CreateDirectoryA(directory, nullptr) ? S_OK : E_FAIL);
    auto unix_name =
        (char *(__cdecl *)(const WCHAR *))GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    CHECK(unix_name ? S_OK : E_FAIL);
    WCHAR wide[MAX_PATH];
    CHECK(MultiByteToWideChar(CP_ACP, 0, directory, -1, wide, std::size(wide)) ? S_OK : E_FAIL);
    char *path = unix_name(wide);
    CHECK(path ? S_OK : E_FAIL);
    CHECK(SetEnvironmentVariableA("MULLION_DIAG_PATH", path) ? S_OK : E_FAIL);
    HeapFree(GetProcessHeap(), 0, path);
    CHECK(SetEnvironmentVariableA("MULLION_DIAG", "profile") ? S_OK : E_FAIL);
    std::string command = std::string("\"") + exe + "\" child \"" + directory + "\"";
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)
              ? S_OK : E_FAIL);
    auto waited = WaitForSingleObject(process.hProcess, 600000);
    if (waited != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 60000);
    }
    expect(waited == WAIT_OBJECT_0, "child timed out");
    DWORD status = 1;
    expect(GetExitCodeProcess(process.hProcess, &status), "child exit status is unavailable");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(status == 0, "growth test child failed with %lu", status);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    expect(!error, "temporary directory removal failed");
    return verdict();
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                          nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  const UINT page = MULLION_PAGE_SIZE, map_alignment = 16, word = sizeof(UINT);
  UINT64 block = 0;
  auto exercise = [&](UINT bytes, UINT rewrites, bool dynamic, bool recorded, bool many, bool completed,
                      bool calibration) -> int {
    step("%s %s: %u bytes, %u %s%s", recorded ? "deferred" : "immediate", dynamic ? "dynamic" : "default",
         bytes, rewrites, many ? "buffers rewritten once" : "rewrites", completed ? " with completion" : "");
    immediate->ClearState();
    deferred->ClearState();
    auto context = recorded ? deferred.Get() : immediate.Get();
    const UINT width = std::min(rewrites, UINT(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));
    D3D11_TEXTURE2D_DESC td{width, (rewrites + width - 1) / width, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0},
                            D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK(device->CreateTexture2D(&td, nullptr, &target));
    CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&td, nullptr, &staging));
    const float clear[4]{};
    immediate->ClearRenderTargetView(rtv.Get(), clear);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    std::vector<UINT> data(bytes / word);
    std::vector<ComPtr<ID3D11Buffer>> inputs;
    ComPtr<ID3D11ShaderResourceView> srv;
    const UINT64 stride = (UINT64(bytes) + map_alignment - 1) / map_alignment * map_alignment;
    const UINT64 first_length = std::max(page, bytes), first_names = bytes > page ? 1 : page / stride;
    const UINT64 cap = calibration ? UINT64(rewrites) : std::max<UINT64>(1, block / stride);
    UINT64 covered = recorded ? 0 : first_names - 1, names = first_names, length = first_length;
    std::vector<UINT64> expected;
    const auto before = created(argv[2]).size();
    for (UINT n = 1; n <= rewrites; n++) {
      for (UINT i = 0; i < data.size(); i++)
        data[i] = n * data.size() + i + 1;
      data[0] = data.size() - 1;
      if (many || n == 1) {
        inputs.emplace_back();
        D3D11_BUFFER_DESC bd{bytes, dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE, dynamic ? D3D11_CPU_ACCESS_WRITE : 0u};
        CHECK(device->CreateBuffer(&bd, nullptr, &inputs.back()));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{DXGI_FORMAT_R32_UINT, D3D11_SRV_DIMENSION_BUFFER};
        sd.Buffer.NumElements = data.size();
        srv.Reset();
        CHECK(device->CreateShaderResourceView(inputs.back().Get(), &sd, &srv));
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());
        expected.push_back(first_length);
        if (many && (recorded || first_names == 1))
          expected.push_back(first_length);
      }
      if (dynamic) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(context->Map(inputs.back().Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        expect(uintptr_t(mapped.pData) % map_alignment == 0, "Map alignment");
        memcpy(mapped.pData, data.data(), bytes);
        context->Unmap(inputs.back().Get(), 0);
      } else {
        context->UpdateSubresource(inputs.back().Get(), 0, nullptr, data.data(), 0, 0);
      }
      D3D11_VIEWPORT viewport{float((n - 1) % width), float((n - 1) / width), 1, 1, 0, 1};
      context->RSSetViewports(1, &viewport);
      context->Draw(1, 0);
      const bool boundary = n == covered + 1;
      if (!many && !completed && n > covered) {
        expected.push_back(length);
        covered += names;
        names = std::min(2 * names, cap);
        length = stride > block && !calibration ? first_length : std::max<UINT64>(page, names * stride);
      }
      if (completed) {
        immediate->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const auto pixels = reinterpret_cast<const Row *>(static_cast<const char *>(mapped.pData) +
                                                         ((n - 1) / width) * mapped.RowPitch);
        expect(pixels[(n - 1) % width] == Row{data[1], data[2], data[data.size() - 2], data.back()},
               "completed frame reads its last bytes");
        immediate->Unmap(staging.Get(), 0);
      }
      if ((!many && (boundary || n == covered || completed)) || n == rewrites) {
        auto all = created(argv[2]);
        std::vector<UINT64> lengths(all.begin() + before, all.end());
        if (calibration) {
          if (!expect(!lengths.empty(), "profile records resource buffer creations"))
            return 1;
          block = *std::max_element(lengths.begin(), lengths.end());
        } else if (many || completed) {
          for (auto size : lengths)
            if (!expect(size == first_length, completed ? "completed-frame allocation never grows"
                                                       : "single-rewrite allocation keeps its original length"))
              return 1;
          if (many && !expect(lengths == expected, "single-rewrite count and bytes match the original allocator"))
            return 1;
        } else {
          if (!expect(recorded ? lengths == expected : lengths.size() <= expected.size(),
                      recorded ? "deferred names fit recorded lengths at rewrite %u"
                               : "immediate allocation count bound at rewrite %u", n))
            return 1;
          UINT64 total = 0;
          for (auto size : lengths) {
            total += size;
            if (!expect(size <= std::max<UINT64>(block, first_length), "allocation length stays within the block"))
              return 1;
          }
          if (!expect(total <= 2 * UINT64(n) * stride + page + stride,
                      "created bytes bound at rewrite %u: %llu", n, total))
            return 1;
        }
        if (calibration && n == rewrites) {
          if (!expect(lengths.size() >= 4 && block > page && lengths[lengths.size() - 2] == block &&
                        lengths[lengths.size() - 3] == block, "bounded doubling reaches a plateau"))
            return 1;
          printf("observed block: %llu bytes\n", block);
        }
      }
    }
    ComPtr<ID3D11CommandList> list;
    if (recorded) {
      CHECK(deferred->FinishCommandList(FALSE, &list));
      immediate->ExecuteCommandList(list.Get(), FALSE);
    }
    immediate->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    UINT wrong = 0;
    for (UINT n = 1; n <= rewrites; n++) {
      const auto pixels = reinterpret_cast<const Row *>(static_cast<const char *>(mapped.pData) +
                                                       ((n - 1) / width) * mapped.RowPitch);
      const UINT base = n * data.size();
      const Row want{base + 2, base + 3, base + UINT(data.size()) - 1, base + UINT(data.size())};
      const Row got = pixels[(n - 1) % width];
      if (got != want && !wrong++)
        printf("rewrite %u reads %u %u %u %u, not %u %u %u %u\n", n, got[0], got[1], got[2], got[3], want[0], want[1],
               want[2], want[3]);
    }
    immediate->Unmap(staging.Get(), 0);
    if (!expect(!wrong, "pending-reader pixels: %u wrong", wrong))
      return 1;
    auto got = read(device.Get(), immediate.Get(), inputs.back().Get());
    expect(got == data, "whole-buffer bytes at the last name");
    return 0;
  };
  const UINT calibration_rewrites = 2 * D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1;
  CHECK(exercise(page, calibration_rewrites, true, true, false, false, true) ? E_FAIL : S_OK);
  const UINT few = block / 3 / word * word;
  const UINT sizes[] = {page / 2, page, page + word, 4 * page, few, UINT(block), UINT(block) + word};
  for (UINT bytes : {page / 2, page, page + word})
    for (bool dynamic : {false, true})
      CHECK(exercise(bytes, 3 * std::max(1u, page / bytes) + 1, dynamic, false, false, true, false) ? E_FAIL : S_OK);
  for (UINT bytes : sizes)
    for (bool dynamic : {false, true})
      for (bool recorded : {true, false})
        CHECK(exercise(bytes, 1, dynamic, recorded, true, false, false) ? E_FAIL : S_OK);
  for (UINT bytes : sizes)
    for (bool dynamic : {false, true})
      for (bool recorded : {true, false}) {
        const UINT names = bytes > page ? 1 : page / bytes;
        const UINT rewrites = bytes == page ? calibration_rewrites : (1 + 2 + 4 + 8) * names + 1;
        CHECK(exercise(bytes, rewrites, dynamic, recorded, false, false, false) ? E_FAIL : S_OK);
      }
  for (UINT bytes : {page / 2, page, page + word})
    for (bool dynamic : {false, true})
      for (bool recorded : {false, true})
        CHECK(exercise(bytes, 10000, dynamic, recorded, true, false, false) ? E_FAIL : S_OK);
  return verdict();
}
