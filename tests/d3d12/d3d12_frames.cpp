// contract: a dump is the current back buffer's bytes before presentation converts them; the selected presents
// alone are written, with or without a visible window. "Returns the index of the current back buffer."
// (IDXGISwapChain3::GetCurrentBackBufferIndex, Return value). channel bytes follow D3D11.3 19.1.3.1:
// "Channel ordering of R/G/B/A/D/S/X in format name, read from left to right indicates order of placement of the
// channel storage from \"first\" (left) to \"last\" (right)." the header and range follow the diagnostic's contract.
#include "d3d12_test.hpp"
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>

static constexpr UINT frames = 7, first = 1, count = 5, buffers = 3, height = 5;
static constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};

static UINT8
pixel(UINT frame, UINT row, UINT byte, DXGI_FORMAT format) {
  UINT8 value = frame + row + byte;
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT && byte % sizeof(UINT16) ? 0x30 + value % 0x10 : value;
}

static int
child(const char *directory_mode) {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  if (directory_mode) {
    ComPtr<IUnknown> debug;
    D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    SetEnvironmentVariableA("DXMT_LOG_PATH", !strcmp(directory_mode, "none") ? "none" : nullptr);
  }
  char setting[MAX_PATH];
  const bool enabled = GetEnvironmentVariableA("DXMT_D3D12_DUMP_FRAMES", setting, sizeof(setting));
  const std::string changed = "0:" + std::to_string(frames);
  SetEnvironmentVariableA("DXMT_D3D12_DUMP_FRAMES", enabled ? nullptr : changed.c_str());
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  ComPtr<IDXGIFactory4> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  WNDCLASSA wc{0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "d3d12_frames"};
  RegisterClassA(&wc);
  ComPtr<IDXGISwapChain3> chains[std::size(formats)];
  HWND windows[std::size(formats)];
  for (UINT i = 0; i < std::size(formats); i++) {
    windows[i] = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, 0, 0, 160, 120,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!expect(windows[i] != nullptr, "no hidden window"))
      return verdict();
    const UINT texel = formats[i] == DXGI_FORMAT_R8G8B8A8_UNORM ? sizeof(UINT8[4]) : sizeof(UINT16[4]);
    DXGI_SWAP_CHAIN_DESC1 desc{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / texel - 1, height, formats[i], FALSE, {1, 0},
                                DXGI_USAGE_RENDER_TARGET_OUTPUT, buffers};
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), windows[i], &desc, nullptr, nullptr, &chain));
    CHECK(chain.As(&chains[i]));
    CHECK(chains[i]->SetSourceSize(desc.Width / 2, desc.Height / 2));
  }
  ComPtr<ID3D12Fence> held;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held)));
  for (UINT frame = 0; frame < frames; frame++) {
    for (UINT chain = 0; chain < std::size(chains); chain++) {
      step("child: frame %u of chain %u, format %u", frame, chain, formats[chain]);
      const UINT texel = formats[chain] == DXGI_FORMAT_R8G8B8A8_UNORM ? sizeof(UINT8[4]) : sizeof(UINT16[4]);
      const UINT width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / texel + (frame + 1) / buffers - 1;
      if (frame && (frame + 1) % buffers == 0) {
        CHECK(chains[chain]->ResizeBuffers(buffers, width, height, DXGI_FORMAT_UNKNOWN, 0));
        CHECK(chains[chain]->SetSourceSize(width / 2, height / 2));
      }
      ComPtr<ID3D12Resource> back;
      CHECK(chains[chain]->GetBuffer(chains[chain]->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)));
      auto desc = back->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
      UINT64 size, row;
      device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, &row, &size);
      auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST);
      if (!expect(upload && readback, "no copy buffers"))
        return verdict();
      UINT8 *bytes;
      CHECK(upload->Map(0, nullptr, (void **)&bytes));
      memset(bytes, 0xff, size);
      for (UINT y = 0; y < height; y++)
        for (UINT b = 0; b < row; b++)
          bytes[y * footprint.Footprint.RowPitch + b] = pixel(frame, y, b, formats[chain]);
      upload->Unmap(0, nullptr);
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                          {.PlacedFootprint = footprint}},
          target{back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
      transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION into{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                        {.PlacedFootprint = footprint}};
      list->CopyTextureRegion(&into, 0, 0, 0, &target, nullptr);
      transition(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      CHECK(readback->Map(0, nullptr, (void **)&bytes));
      bool same = true;
      for (UINT y = 0; y < height; y++)
        for (UINT b = 0; b < row; b++)
          same &= bytes[y * footprint.Footprint.RowPitch + b] == pixel(frame, y, b, formats[chain]);
      readback->Unmap(0, nullptr);
      expect(same, "the presented back buffer has different bytes");
      CHECK(chains[chain]->Present(0, DXGI_PRESENT_TEST));
      if (frame == first && !chain)
        CHECK(queue->Wait(held.Get(), 1));
      CHECK(chains[chain]->Present(0, 0));
      if (frame == first && !chain)
        CHECK(held->Signal(1));
    }
  }
  queue.Reset();
  for (auto &chain : chains)
    chain.Reset();
  for (auto window : windows)
    DestroyWindow(window);
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[2], "child"))
    return child(argc > 3 ? argv[3] : nullptr);
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  const std::string requested = std::to_string(first) + ":" + std::to_string(count);
  const std::string beyond = std::to_string(frames) + ":1";
  const std::string maximum = std::to_string(std::numeric_limits<UINT64>::max());
  const std::string all = "0:" + maximum;
  std::string overflow = maximum;
  overflow.back()++;
  const std::string first_overflow = overflow + ":1", count_overflow = "0:" + overflow;
  const std::pair<const char *, const char *> cases[] = {
      {requested.c_str(), nullptr}, {nullptr, nullptr}, {"0:1", nullptr}, {"0:0", nullptr}, {beyond.c_str(), nullptr},
      {"bad", nullptr}, {"1", nullptr}, {"1/5", nullptr}, {"1:5x", nullptr},
      {first_overflow.c_str(), nullptr}, {count_overflow.c_str(), nullptr}, {all.c_str(), nullptr},
      {"0:1", "unset"}, {"0:1", "none"},
  };
  for (auto [range, directory_mode] : cases) {
    step("child with DXMT_D3D12_DUMP_FRAMES=%s", range ? range : "unset");
    if (!expect(GetTempFileNameA(temporary, "frm", 0, directory), "no temporary name"))
      return verdict();
    DeleteFileA(directory);
    if (!expect(CreateDirectoryA(directory, nullptr), "no temporary directory"))
      return verdict();
    SetEnvironmentVariableA("DXMT_LOG_PATH", directory);
    SetEnvironmentVariableA("DXMT_LOG_LEVEL", "info");
    SetEnvironmentVariableA("DXMT_D3D12_DUMP_FRAMES", range);
    std::string command = std::string("\"") + exe + "\" dxbc child";
    if (directory_mode)
      command += std::string(" ") + directory_mode;
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!expect(CreateProcessA(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
                "child did not start"))
      return verdict();
    auto waited = WaitForSingleObject(process.hProcess, 60000);
    if (waited != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1);
      WaitForSingleObject(process.hProcess, 60000);
    }
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    expect(waited == WAIT_OBJECT_0 && status == 0, "child ended with %lu after wait %lu", status, waited);
    const UINT begin = range && (!strcmp(range, "0:1") || range == all) ? 0 : first;
    const UINT selected = directory_mode || !range ? 0 : range == requested ? count : range == all ? frames
                                                                       : !strcmp(range, "0:1") ? 1 : 0;
    std::map<UINT, std::set<UINT>> seen;
    std::map<UINT, std::map<UINT, std::string>> names;
    std::string logs;
    UINT files = 0;
    for (auto &entry : std::filesystem::directory_iterator(directory)) {
      std::ifstream file(entry.path(), std::ios::binary);
      if (entry.path().extension() == ".log") {
        logs.append(std::istreambuf_iterator<char>(file), {});
        continue;
      }
      if (!expect(entry.path().extension() == ".frame", "unexpected file %s", entry.path().string().c_str()))
        continue;
      files++;
      std::string header, name, kind, extra;
      std::getline(file, header);
      std::istringstream parsed(header);
      UINT number = 0, width = 0, got_height = 0, format = 0, row = 0;
      parsed >> name >> kind >> number >> width >> got_height >> format >> row;
      const bool known = format == formats[0] || format == formats[1];
      const UINT texel = format == formats[0] ? sizeof(UINT8[4]) : sizeof(UINT16[4]);
      if (!expect(parsed && !(parsed >> extra) && name == "mullion" && kind == "frame" && known &&
                      number >= begin && number - begin < selected && got_height == height &&
                      width == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / texel + (number + 1) / buffers - 1 &&
                      row == width * texel,
                  "wrong header: %s", header.c_str()))
        continue;
      expect(seen[format].insert(number).second, "duplicate frame %u of format %u", number, format);
      names[format][number] = entry.path().filename().string();
      std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
      bool same = bytes.size() == row * height;
      if (same)
        for (UINT y = 0; y < height; y++)
          for (UINT b = 0; b < row; b++)
            same &= UINT8(bytes[y * row + b]) == pixel(number, y, b, DXGI_FORMAT(format));
      expect(same, "wrong bytes in %s", entry.path().string().c_str());
    }
    expect(files == selected * std::size(formats), "%u files, want %zu", files, selected * std::size(formats));
    for (auto format : formats)
      expect(seen[format].size() == selected, "format %u has %zu frames, want %u", format,
             seen[format].size(), selected);
    for (auto &[format, frames] : names) {
      size_t after = 0;
      std::string previous;
      for (auto &[number, name] : frames) {
        expect(previous.empty() || previous < name, "names do not sort in present order: %s", name.c_str());
        auto at = logs.find(name, after);
        if (expect(at != std::string::npos, "frame %u of format %u was not written in log order", number, format))
          after = at + name.size();
        previous = name;
      }
    }
    if (!range)
      expect(logs.find("D3D12 frame") == std::string::npos, "diagnostic logged with its variable unset");
    else if (selected)
      expect(logs.find("D3D12 frame written") != std::string::npos, "no completed capture in the child's log");
    if (range && (!strcmp(range, "bad") || !strcmp(range, "1") || !strcmp(range, "1/5") ||
                  !strcmp(range, "1:5x") || range == first_overflow || range == count_overflow)) {
      auto at = logs.find("D3D12 frame range invalid");
      expect(at != std::string::npos && logs.find("D3D12 frame range invalid", at + 1) == std::string::npos,
             "invalid range was not logged exactly once");
    }
    if (directory_mode) {
      auto at = logs.find("D3D12 frame directory missing");
      expect(at != std::string::npos && logs.find("D3D12 frame directory missing", at + 1) == std::string::npos,
             "missing directory was not logged exactly once");
    }
    std::filesystem::remove_all(directory);
  }
  return verdict();
}
