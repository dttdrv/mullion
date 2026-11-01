// contract: a shared object stays openable, in its own process and in another, while a handle to it is open and the
// object lives, and the names Mullion registers with launchd for sharing go when the objects that registered them do.
// Microsoft Learn, ID3D12Device::CreateSharedHandle: "Creates a shared handle to a heap, resource, or fence object."
// ID3D12Device::OpenSharedHandle: "Opens a handle for shared resources, shared heaps, and shared fences".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle
// ID3D11Fence::CreateSharedHandle: "Creates a shared handle to a fence object."; IDXGIResource1::CreateSharedHandle:
// "Creates a handle to a shared resource"; ID3D11Device1::OpenSharedResource1 opens it in another device or process.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-createsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1
// the leak bound is Mullion's own: macOS has no shared NT handles, so a shared fence, texture or keyed mutex is
// published to other processes under a launchd name (MULLION_shared_..., winemetal_unix.c: shared_name), and launchd
// keeps a name until the port registered under it dies. names that outlive their objects pile up for as long as the
// user stays logged in, and some 20,000 of them stopped every Direct3D 12 command queue from being created. so every
// name the objects here registered must be gone once the objects are released. launchd's names are read with
// launchctl, through a unix shell (ntdll's __wine_unix_spawnvp); a name another process registered meanwhile is not
// counted. the objects open in this process and in a child, and each opened object shows its creator's value: an
// initial fence value, or the texels a texture was created with. Windows keeps a shared object while any NT handle to it is open; Mullion keeps it openable
// while an object of the creating process made from it lives, so an open with an NT handle but no such object fails:
// the test checks that it fails at once, as the known difference a fuller lifetime would have to change on purpose.
// a process that ends without releasing its objects leaves no names either.
#include "d3d12_test.hpp"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <set>

namespace {

const UINT fence_count = 8, texture_count = 8, planar_count = 2, side = 4;
// names each object registers: its own, its keyed mutex's, and a planar texture's second plane's
const size_t wanted_names = 2 * fence_count + texture_count + texture_count / 2 + planar_count * 3;
const DWORD wait_ms = 10000;

// the names in launchd's domain for this user that Mullion registers
std::set<std::string>
names() {
  std::set<std::string> found;
  using Spawn = LONG(WINAPI *)(char *const *, int);
  using UnixName = char *(CDECL *)(const WCHAR *);
  auto spawn = (Spawn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_unix_spawnvp");
  auto unix_name = (UnixName)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
  WCHAR dir[MAX_PATH], path[MAX_PATH];
  if (!expect(spawn && unix_name, "Wine's unix process and path calls are missing") || !GetTempPathW(MAX_PATH, dir) ||
      !GetTempFileNameW(dir, L"names", 0, path))
    return found;
  char *unix_path = unix_name(path);
  if (!expect(!!unix_path, "the temporary file has no unix path"))
    return found;
  std::string command = std::string("/bin/launchctl print user/$(/usr/bin/id -u) > '") + unix_path + "'";
  HeapFree(GetProcessHeap(), 0, unix_path);
  char *argv[] = {(char *)"/bin/sh", (char *)"-c", command.data(), nullptr};
  LONG status = spawn(argv, 1);
  if (FILE *file = _wfopen(path, L"r")) {
    char line[512];
    while (fgets(line, sizeof(line), file))
      if (char *name = strstr(line, "MULLION_shared_"))
        found.insert(std::string(name, strcspn(name, " \t\r\n")));
    fclose(file);
  }
  DeleteFileW(path);
  expect(!status, "launchctl failed: %#lx", status);
  return found;
}

// the names of `of` that `in` has
std::set<std::string>
among(const std::set<std::string> &of, const std::set<std::string> &in) {
  std::set<std::string> found;
  for (auto &name : of)
    if (in.count(name))
      found.insert(name);
  return found;
}

// the names of `of` still registered after up to wait_ms
std::set<std::string>
remaining(const std::set<std::string> &of) {
  std::set<std::string> left;
  for (DWORD start = GetTickCount(); GetTickCount() - start < wait_ms; Sleep(100))
    if ((left = among(of, names())).empty())
      break;
  return left;
}

UINT
color(UINT i) {
  return 0x10203040u + i * 0x01010101u;
}

// whether every texel of a side x side RGBA texture holds `value`
bool
holds(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture, UINT value) {
  D3D11_TEXTURE2D_DESC desc{side, side, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                            D3D11_CPU_ACCESS_READ};
  ComPtr<ID3D11Texture2D> staging;
  ComPtr<IDXGIKeyedMutex> mutex;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
    return false;
  bool keyed = SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&mutex)));
  if (keyed && mutex->AcquireSync(0, wait_ms) != S_OK)
    return false;
  context->CopyResource(staging.Get(), texture);
  if (keyed)
    mutex->ReleaseSync(0);
  D3D11_MAPPED_SUBRESOURCE map;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
    return false;
  bool same = true;
  for (UINT y = 0; y < side; y++)
    for (UINT x = 0; x < side; x++)
      same &= ((UINT *)((char *)map.pData + y * map.RowPitch))[x] == value;
  context->Unmap(staging.Get(), 0);
  return same;
}

struct Devices {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11Device1> device1;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext> context;

  bool
  create() {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
           SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &device11, nullptr, &context)) &&
           SUCCEEDED(device11.As(&device1)) && SUCCEEDED(device11.As(&device5));
  }
};

// the handles, in order: D3D12 fences, D3D11 fences, RGBA textures, planar textures
struct Opened {
  std::vector<ComPtr<ID3D12Fence>> fences12;
  std::vector<ComPtr<ID3D11Fence>> fences11;
  std::vector<ComPtr<ID3D11Texture2D>> textures;

  // opens every handle and checks what each holds; false at the first that does not open
  bool
  open(Devices &d, const std::vector<HANDLE> &handles, const char *where) {
    UINT at = 0;
    for (UINT i = 0; i < fence_count; i++, at++) {
      ComPtr<ID3D12Fence> fence;
      if (!expect(SUCCEEDED(d.device->OpenSharedHandle(handles[at], IID_PPV_ARGS(&fence))),
                  "%s: D3D12 fence %u did not open", where, i))
        return false;
      expect(fence->GetCompletedValue() == 1000 + i, "%s: D3D12 fence %u is at %llu, want %u", where, i,
             fence->GetCompletedValue(), 1000 + i);
      fences12.push_back(fence);
    }
    for (UINT i = 0; i < fence_count; i++, at++) {
      ComPtr<ID3D11Fence> fence;
      if (!expect(SUCCEEDED(d.device5->OpenSharedFence(handles[at], IID_PPV_ARGS(&fence))),
                  "%s: D3D11 fence %u did not open", where, i))
        return false;
      expect(fence->GetCompletedValue() == 2000 + i, "%s: D3D11 fence %u is at %llu, want %u", where, i,
             fence->GetCompletedValue(), 2000 + i);
      fences11.push_back(fence);
    }
    for (UINT i = 0; i < texture_count + planar_count; i++, at++) {
      ComPtr<ID3D11Texture2D> texture;
      if (!expect(SUCCEEDED(d.device1->OpenSharedResource1(handles[at], IID_PPV_ARGS(&texture))),
                  "%s: texture %u did not open", where, i))
        return false;
      D3D11_TEXTURE2D_DESC desc;
      texture->GetDesc(&desc);
      if (i < texture_count)
        expect(holds(d.device11.Get(), d.context.Get(), texture.Get(), color(i)),
               "%s: texture %u does not hold its creator's texels", where, i);
      else
        expect(desc.Format == DXGI_FORMAT_NV12 && desc.Width == 2 * side, "%s: planar texture %u changed", where, i);
      textures.push_back(texture);
    }
    return true;
  }
};

} // namespace

int
main(int argc, char **argv) {
  Devices d;
  if (!expect(d.create(), "devices were not created"))
    return verdict();

  if (argc > 2 && !strcmp(argv[1], "--open")) {
    step("another process opens every handle");
    std::vector<HANDLE> handles;
    for (int i = 2; i < argc; i++)
      handles.push_back(reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[i], nullptr, 10))));
    Opened opened;
    opened.open(d, handles, "child");
    return verdict();
  }

  if (argc == 4 && !strcmp(argv[1], "--exit")) {
    step("another process makes shared objects and ends without releasing them");
    HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
    HANDLE go = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D11Texture2D> texture;
    HANDLE handle;
    D3D11_TEXTURE2D_DESC desc{side, side, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE, 0,
                              D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX};
    CHECK(d.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    CHECK(d.device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
    CHECK(d.device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)));
    CHECK(d.device11->CreateTexture2D(&desc, nullptr, &texture));
    SetEvent(ready);
    WaitForSingleObject(go, wait_ms);
    TerminateProcess(GetCurrentProcess(), 0);
  }

  step("names before the objects");
  auto before = names();

  step("create %u D3D12 and %u D3D11 shared fences, and %u shared textures", fence_count, fence_count,
       texture_count + planar_count);
  std::vector<HANDLE> handles;
  std::vector<ComPtr<ID3D12Fence>> fences12;
  std::vector<ComPtr<ID3D11Fence>> fences11;
  std::vector<ComPtr<ID3D11Texture2D>> created;
  for (UINT i = 0; i < fence_count; i++) {
    ComPtr<ID3D12Fence> fence;
    HANDLE handle;
    CHECK(d.device->CreateFence(1000 + i, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    CHECK(d.device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
    fences12.push_back(fence);
    handles.push_back(handle);
  }
  for (UINT i = 0; i < fence_count; i++) {
    ComPtr<ID3D11Fence> fence;
    HANDLE handle;
    CHECK(d.device5->CreateFence(2000 + i, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    CHECK(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle));
    fences11.push_back(fence);
    handles.push_back(handle);
  }
  for (UINT i = 0; i < texture_count + planar_count; i++) {
    bool nv12 = i >= texture_count;
    // odd textures and the planar ones have a keyed mutex, which has a name of its own
    UINT mode = i % 2 || nv12 ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
    D3D11_TEXTURE2D_DESC desc{nv12 ? 2 * side : side, nv12 ? 2 * side : side, 1, 1,
                              nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_NTHANDLE | mode};
    std::vector<UINT> texels(4 * side * side, color(i));
    D3D11_SUBRESOURCE_DATA data{texels.data(), desc.Width * (nv12 ? 1 : 4)};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIResource1> resource;
    HANDLE handle;
    CHECK(d.device11->CreateTexture2D(&desc, &data, &texture));
    CHECK(texture.As(&resource));
    CHECK(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                       &handle));
    created.push_back(texture);
    handles.push_back(handle);
  }
  auto during = names();
  std::set<std::string> registered;
  for (auto &name : during)
    if (!before.count(name))
      registered.insert(name);
  printf("names: %zu before, %zu with the objects, %zu new for %u objects\n", before.size(), during.size(),
         registered.size(), 2 * fence_count + texture_count + planar_count);
  expect(registered.size() >= wanted_names, "the objects registered %zu names, want %zu", registered.size(),
         wanted_names);

  step("this process opens every handle");
  Opened opened;
  if (!opened.open(d, handles, "same process"))
    return verdict();

  step("another process opens every handle");
  char executable[MAX_PATH];
  if (!expect(!!GetModuleFileNameA(nullptr, executable, sizeof(executable)), "GetModuleFileName: %lu", GetLastError()))
    return verdict();
  std::string command = "\"" + std::string(executable) + "\" --open";
  for (HANDLE handle : handles) {
    expect(!!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT), "SetHandleInformation: %lu",
           GetLastError());
    command += " " + std::to_string(reinterpret_cast<uintptr_t>(handle));
  }
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &child),
              "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(child.hThread);
  if (!expect(WaitForSingleObject(child.hProcess, 6 * wait_ms) == WAIT_OBJECT_0, "the child did not exit")) {
    TerminateProcess(child.hProcess, 1);
    WaitForSingleObject(child.hProcess, wait_ms);
  }
  DWORD code = 1;
  GetExitCodeProcess(child.hProcess, &code);
  expect(code == 0, "the child exited %lu", code);
  CloseHandle(child.hProcess);

  step("an object opened in this process keeps the handle openable after its creator's release");
  fences12[0].Reset();
  fences11[0].Reset();
  created[0].Reset();
  created[1].Reset();
  created[texture_count].Reset();
  ComPtr<ID3D12Fence> again12;
  ComPtr<ID3D11Fence> again11;
  ComPtr<ID3D11Texture2D> again, again_keyed, again_planar;
  ComPtr<IDXGIKeyedMutex> again_mutex;
  expect(SUCCEEDED(d.device->OpenSharedHandle(handles[0], IID_PPV_ARGS(&again12))) &&
             again12->GetCompletedValue() == 1000,
         "the D3D12 fence did not open again");
  expect(SUCCEEDED(d.device5->OpenSharedFence(handles[fence_count], IID_PPV_ARGS(&again11))) &&
             again11->GetCompletedValue() == 2000,
         "the D3D11 fence did not open again");
  expect(SUCCEEDED(d.device1->OpenSharedResource1(handles[2 * fence_count], IID_PPV_ARGS(&again))) &&
             holds(d.device11.Get(), d.context.Get(), again.Get(), color(0)),
         "the texture did not open again");
  // the keyed mutex and the second plane have names of their own
  expect(SUCCEEDED(d.device1->OpenSharedResource1(handles[2 * fence_count + 1], IID_PPV_ARGS(&again_keyed))) &&
             SUCCEEDED(again_keyed.As(&again_mutex)) &&
             holds(d.device11.Get(), d.context.Get(), again_keyed.Get(), color(1)),
         "the keyed texture did not open again with its mutex");
  D3D11_TEXTURE2D_DESC planar_desc{};
  if (expect(SUCCEEDED(d.device1->OpenSharedResource1(handles[2 * fence_count + texture_count],
                                                      IID_PPV_ARGS(&again_planar))),
             "the planar texture did not open again"))
    again_planar->GetDesc(&planar_desc);
  expect(planar_desc.Format == DXGI_FORMAT_NV12, "the planar texture opened again as format %u", planar_desc.Format);

  step("release every object, and every handle but three");
  // one of each kind stays open for the late open
  const size_t late[] = {1, fence_count + 1, 2 * fence_count + 2};
  again12.Reset();
  again11.Reset();
  again.Reset();
  again_mutex.Reset();
  again_keyed.Reset();
  again_planar.Reset();
  opened = {};
  fences12.clear();
  fences11.clear();
  created.clear();
  for (size_t i = 0; i < handles.size(); i++)
    if (std::find(std::begin(late), std::end(late), i) == std::end(late))
      CloseHandle(handles[i]);
  d.context->ClearState();
  d.context->Flush();

  step("every name the objects registered is gone, with their devices alive");
  auto left = remaining(registered);
  expect(left.empty(), "%zu of the %zu names the objects registered remain after their release", left.size(),
         registered.size());

  step("an open after every object made from the handle was released fails at once: Mullion's known difference");
  for (size_t i : late) {
    DWORD start = GetTickCount();
    HRESULT hr;
    if (i < fence_count) {
      ComPtr<ID3D12Fence> fence;
      hr = d.device->OpenSharedHandle(handles[i], IID_PPV_ARGS(&fence));
      expect(!fence, "the late D3D12 fence open returned an object");
    } else if (i < 2 * fence_count) {
      ComPtr<ID3D11Fence> fence;
      hr = d.device5->OpenSharedFence(handles[i], IID_PPV_ARGS(&fence));
      expect(!fence, "the late D3D11 fence open returned an object");
    } else {
      ComPtr<ID3D11Texture2D> texture;
      hr = d.device1->OpenSharedResource1(handles[i], IID_PPV_ARGS(&texture));
      expect(!texture, "the late texture open returned an object");
    }
    DWORD took = GetTickCount() - start;
    expect(FAILED(hr), "the late open of handle %zu returned %#lx", i, hr);
    expect(took < 1000, "the late open of handle %zu took %lu ms", i, took);
    CloseHandle(handles[i]);
  }

  step("a process that ends without releasing its objects leaves no names");
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE ready = CreateEventA(&inherit, TRUE, FALSE, nullptr), go = CreateEventA(&inherit, TRUE, FALSE, nullptr);
  if (!expect(ready && go, "CreateEvent: %lu", GetLastError()))
    return verdict();
  auto quiet = names();
  command = "\"" + std::string(executable) + "\" --exit " + std::to_string(reinterpret_cast<uintptr_t>(ready)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(go));
  child = {};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &child),
              "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(child.hThread);
  std::set<std::string> ended;
  if (expect(WaitForSingleObject(ready, wait_ms) == WAIT_OBJECT_0, "the ending child made no objects")) {
    for (auto &name : names())
      if (!quiet.count(name))
        ended.insert(name);
    // a D3D12 and a D3D11 fence, and a keyed texture
    expect(ended.size() >= 4, "the ending child registered %zu names, want 4", ended.size());
  }
  SetEvent(go);
  if (!expect(WaitForSingleObject(child.hProcess, wait_ms) == WAIT_OBJECT_0, "the ending child did not end"))
    TerminateProcess(child.hProcess, 1);
  CloseHandle(child.hProcess);
  CloseHandle(ready);
  CloseHandle(go);
  left = remaining(ended);
  expect(left.empty(), "%zu of the %zu names of the ended process remain", left.size(), ended.size());
  return verdict();
}
