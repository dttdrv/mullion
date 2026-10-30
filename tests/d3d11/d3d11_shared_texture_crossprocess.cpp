// contract: a shared texture and its keyed mutex reach another process, as shared fences do. Microsoft Learn,
// IDXGIResource::GetSharedHandle: "You can also marshal this handle to another process to share a resource with a
// device in another process. However, this handle is not an NT handle. Therefore, don't use the handle with
// CloseHandle, DuplicateHandle, and so on." the same page fixes its lifetime: "The validity of the handle is tied
// to the lifetime of the underlying video memory. If no resource objects exist on any devices that refer to this
// resource, the handle is no longer valid. To extend the lifetime of the handle and video memory, you must open
// the shared resource on a device." an NT handle crosses by inheritance (DuplicateHandle: "The duplicate handle
// refers to the same object as the original handle"). the keyed mutex travels with the texture: "the initial key
// is zero and subsequent keys are those released", "A keyed mutex does not support recursive calls to the
// AcquireSync method" (DXGI_ERROR_INVALID_CALL, as Wine's Windows test_keyed_mutex observes for recursive acquire
// and unowned release), only the owner releases, and destroying the owner abandons its acquired mutex. the child
// of this test is the test itself, as in d3d11_shared_fence.cpp. there are no shaders.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync
// https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <cstdlib>
#include <string>
#include <vector>

static const UINT width = 64, height = 1;
static const UINT bytes = width * height * 4;
static const UINT64 probe_salt = 0x5a5a5a5a5a5a5a5aULL;
static const DWORD wait_ms = 10000;
// the key a share is left at when its creator keeps nothing but the handle, and the keys a named share is left at
static const UINT64 late_key = 11, named_key = 23, held_key = 33;

// a second device of this process, as another application has one: an open has to work here as it does in a
// process of its own
struct second_device {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;

  bool
  open() {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
      return false;
    return SUCCEEDED(device.As(&device1));
  }
};

// the name a share publishes: one per test process, so a rerun of this test cannot collide with a name a previous
// run left in the session's namespace
static void
share_name(DWORD pid, const WCHAR *suffix, WCHAR (&out)[64]) {
  swprintf(out, 64, L"MULLION_shared_texture_crossprocess_%lu%s", static_cast<unsigned long>(pid), suffix);
}

// one byte pattern per turn, the same in both processes
static std::vector<BYTE>
pattern(unsigned turn) {
  std::vector<BYTE> out(bytes);
  for (UINT i = 0; i < bytes; i++)
    out[i] = BYTE((turn * 0x37 + i * 7 + 1) & 0xff);
  return out;
}

// the texture's bytes, read back through a staging copy
static std::vector<BYTE>
contents(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  std::vector<BYTE> out(bytes);
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
    return {};
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return {};
  for (UINT y = 0; y < height; y++)
    memcpy(out.data() + y * width * 4, (BYTE *)mapped.pData + y * mapped.RowPitch, width * 4);
  context->Unmap(staging.Get(), 0);
  return out;
}

// a raw shared texture carries no ordering of its own (that is what the keyed mutex is for), so a handover of
// its contents drains the writer's queue first: "the value of the BOOL is always TRUE" of an EVENT query says the
// earlier commands finished (D3D11.3 20.4.2)
static bool
drained(ID3D11Device *device, ID3D11DeviceContext *context) {
  const D3D11_QUERY_DESC event_desc{D3D11_QUERY_EVENT};
  ComPtr<ID3D11Query> completed;
  if (FAILED(device->CreateQuery(&event_desc, &completed)))
    return false;
  context->End(completed.Get());
  context->Flush();
  BOOL done = FALSE;
  HRESULT hr;
  while ((hr = context->GetData(completed.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE)
    Sleep(0);
  return hr == S_OK && done;
}

// the two ways a turn lands its bytes: straight into the texture, or through a staging upload copied into it
static void
write_update(ID3D11DeviceContext *context, ID3D11Texture2D *texture, const std::vector<BYTE> &words) {
  context->UpdateSubresource(texture, 0, nullptr, words.data(), width * 4, 0);
}

static bool
write_copy(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *texture, const std::vector<BYTE> &words) {
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
    return false;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped)))
    return false;
  for (UINT y = 0; y < height; y++)
    memcpy((BYTE *)mapped.pData + y * mapped.RowPitch, words.data() + y * width * 4, width * 4);
  context->Unmap(staging.Get(), 0);
  context->CopyResource(texture, staging.Get());
  return true;
}

// one keyed turn: the probes that hold whatever the other process is doing (an unowned release is never legal, and
// a wrong key never matches the released key), then the keyed handover, the recursive acquire that must be
// refused, the bytes of the turn before, and the release that publishes this turn's key
static int
keyed_turn(ID3D11Device *device, ID3D11DeviceContext *context, IDXGIKeyedMutex *mutex, ID3D11Texture2D *texture,
           unsigned turn, bool write_with_copy) {
  UINT64 key = turn - 1;
  if (!expect(mutex->ReleaseSync(key ^ probe_salt) == DXGI_ERROR_INVALID_CALL, "turn %u: unowned release accepted key %llu",
              turn, (unsigned long long)(key ^ probe_salt)))
    return 1;
  if (!expect(mutex->AcquireSync(key ^ probe_salt, 0) == WAIT_TIMEOUT, "turn %u: wrong key acquired the mutex", turn))
    return 1;
  if (!expect(mutex->AcquireSync(key, wait_ms) == S_OK, "turn %u: released key %llu did not acquire the mutex", turn,
              (unsigned long long)key))
    return 1;
  if (!expect(mutex->AcquireSync(key ^ probe_salt, 0) == DXGI_ERROR_INVALID_CALL, "turn %u: recursive acquire accepted key %llu",
              turn, (unsigned long long)(key ^ probe_salt)))
    return 1;
  if (turn > 1) {
    auto got = contents(device, context, texture);
    auto wanted = pattern(turn - 1);
    if (!expect(got == wanted, "turn %u: the other process's bytes did not arrive", turn))
      return 1;
  }
  auto words = pattern(turn);
  if (write_with_copy) {
    if (!expect(write_copy(device, context, texture, words), "turn %u: staging upload failed", turn))
      return 1;
  } else {
    write_update(context, texture, words);
  }
  if (!expect(mutex->ReleaseSync(turn) == S_OK, "turn %u: owner could not release key %llu", turn, (unsigned long long)turn))
    return 1;
  return 0;
}

// the keyed child: open the inherited NT handle, refuse every handle of the wrong kind, take the even turns, and
// end owning the mutex so its destroyed texture abandons it
static int
child_keyed(char **argv) {
  HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(device.As(&device1));

  ComPtr<ID3D11Texture2D> imported, wrong;
  HRESULT hr = device->OpenSharedResource(shared, IID_PPV_ARGS(&wrong));
  if (!expect(hr == E_INVALIDARG && !wrong, "the NT handle opened through the legacy API, hr %#lx", hr))
    return 1;
  // the legacy marker is the handle's own second-highest bit, so the foreign handle means the same in either width
  const uintptr_t foreign = (uintptr_t(1) << (sizeof(uintptr_t) * 8 - 2)) | 1u;
  hr = device1->OpenSharedResource1(HANDLE(foreign), IID_PPV_ARGS(&wrong));
  if (!expect(hr == E_INVALIDARG && !wrong, "a foreign legacy handle opened through the NT API, hr %#lx", hr))
    return 1;
  CHECK(device1->OpenSharedResource1(shared, IID_PPV_ARGS(&imported)));
  D3D11_TEXTURE2D_DESC got{};
  imported->GetDesc(&got);
  if (!expect(got.Width == width && got.Height == height && got.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "imported texture description changed"))
    return 1;
  ComPtr<IDXGIKeyedMutex> mutex;
  CHECK(imported.As(&mutex));
  CloseHandle(shared);
  if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(ready);

  for (unsigned turn : {2u, 4u})
    if (keyed_turn(device.Get(), context.Get(), mutex.Get(), imported.Get(), turn, true))
      ExitProcess(1);
  // end owning: the abandoned mutex is the parent's to observe
  if (!expect(mutex->AcquireSync(4, wait_ms) == S_OK, "final acquire did not take the mutex"))
    ExitProcess(1);
  return verdict();
}

// the crashing child: open the inherited NT handle, take the mutex, say so, and then die the way a crash or a kill
// does - with nothing unwound and no destructor run - so the mutex is left owned by a process that no longer exists
static int
child_keyed_crash(char **argv) {
  HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  HANDLE taken = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
  HANDLE die = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[4], nullptr, 10)));
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(device.As(&device1));
  ComPtr<ID3D11Texture2D> imported;
  CHECK(device1->OpenSharedResource1(shared, IID_PPV_ARGS(&imported)));
  ComPtr<IDXGIKeyedMutex> mutex;
  CHECK(imported.As(&mutex));
  CloseHandle(shared);
  // a fresh mutex is free with the initial key, so this is the child's to take at once
  if (!expect(mutex->AcquireSync(0, wait_ms) == S_OK, "the crashing child did not take the mutex"))
    ExitProcess(1);
  if (!expect(!!SetEvent(taken), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(taken);
  // the parent asks the same key while this owner is certainly alive before it lets the child die
  if (!expect(WaitForSingleObject(die, 60000) == WAIT_OBJECT_0, "the parent never let the child die"))
    ExitProcess(1);
  CloseHandle(die);
  ExitProcess(0);
}

// the shared child: open the marshalled legacy handle, verify the identity and the creator's first bytes, land its
// own, and hold its references until the parent says the handle's lifetime has been checked
static int
child_shared(char **argv) {
  HANDLE legacy = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
  HANDLE go = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[4], nullptr, 10)));
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(device.As(&device1));

  ComPtr<ID3D11Texture2D> imported, wrong;
  HRESULT hr = device1->OpenSharedResource1(legacy, IID_PPV_ARGS(&wrong));
  if (!expect(hr == E_INVALIDARG && !wrong, "the marshalled legacy handle opened through the NT API, hr %#lx", hr))
    return 1;
  CHECK(device->OpenSharedResource(legacy, IID_PPV_ARGS(&imported)));
  ComPtr<IDXGIResource1> imported_resource;
  CHECK(imported.As(&imported_resource));
  HANDLE identity = nullptr;
  CHECK(imported_resource->GetSharedHandle(&identity));
  if (!expect(identity == legacy, "imported sharing identity changed"))
    return 1;
  D3D11_TEXTURE2D_DESC got{};
  imported->GetDesc(&got);
  if (!expect(got.Width == width && got.Height == height && got.MiscFlags == D3D11_RESOURCE_MISC_SHARED,
              "imported texture description changed"))
    return 1;
  auto first = contents(device.Get(), context.Get(), imported.Get());
  if (!expect(first == pattern(5), "the creator's initial bytes did not arrive (%u bytes, first %02x)",
              (unsigned)first.size(), first.empty() ? 0u : first[0]))
    return 1;
  write_update(context.Get(), imported.Get(), pattern(7));
  if (!expect(drained(device.Get(), context.Get()), "the child's bytes did not land"))
    ExitProcess(1);
  if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(ready);
  if (!expect(WaitForSingleObject(go, 60000) == WAIT_OBJECT_0, "the parent never finished with the handle"))
    ExitProcess(1);
  CloseHandle(go);
  return verdict();
}

// the late child: the creator of the share whose handle this process inherited has released every resource of it,
// so this handle is all that is left of the share. the texture it names and the mutex it was made with both have
// to still be there, at the key the creator left them, because the handle is a handle to the share's own object
static int
child_late(char **argv) {
  HANDLE kept = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(device.As(&device1));
  ComPtr<ID3D11Texture2D> imported;
  CHECK(device1->OpenSharedResource1(kept, IID_PPV_ARGS(&imported)));
  ComPtr<IDXGIKeyedMutex> mutex;
  CHECK(imported.As(&mutex));
  CloseHandle(kept);
  // a fresh mutex is free with the initial key, so taking the initial key is what a mutex that lost its state does:
  // "If the mutex is not released within the time-out interval, the method returns" (Microsoft Learn,
  // IDXGIKeyedMutex::AcquireSync)
  if (!expect(mutex->AcquireSync(0, 0) == WAIT_TIMEOUT, "the late open took the initial key, so it came back fresh"))
    return 1;
  if (!expect(mutex->AcquireSync(late_key, wait_ms) == S_OK, "the late open lost the key the share was left with"))
    return 1;
  auto got = contents(device.Get(), context.Get(), imported.Get());
  if (!expect(got == pattern(11), "the late open's bytes changed (%u bytes, first %02x)", (unsigned)got.size(),
              got.empty() ? 0u : got[0]))
    return 1;
  if (!expect(mutex->ReleaseSync(2 * late_key) == S_OK, "the late open could not release the mutex"))
    return 1;
  return verdict();
}

// the named child: open the texture by the name its creator published, on a device of a process of its own
static int
child_named(char **argv) {
  WCHAR published[64];
  share_name(strtoul(argv[2], nullptr, 10), L"", published);
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(device.As(&device1));
  ComPtr<ID3D11Texture2D> imported;
  CHECK(device1->OpenSharedResourceByName(
      published, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&imported)
  ));
  ComPtr<IDXGIKeyedMutex> mutex;
  CHECK(imported.As(&mutex));
  if (!expect(mutex->AcquireSync(0, 0) == WAIT_TIMEOUT, "the named open took the initial key, so it came back fresh"))
    return 1;
  if (!expect(mutex->AcquireSync(named_key, wait_ms) == S_OK, "the named open lost the key the share was left with"))
    return 1;
  auto got = contents(device.Get(), context.Get(), imported.Get());
  if (!expect(got == pattern(21), "the named open's bytes did not arrive (%u bytes, first %02x)", (unsigned)got.size(),
              got.empty() ? 0u : got[0]))
    return 1;
  if (!expect(mutex->ReleaseSync(2 * named_key) == S_OK, "the named open could not release the mutex"))
    return 1;
  return verdict();
}

// the holding child: open the texture by the name its creator published and keep the resource, so the name stays
// published while the creator lets every resource of its own go
static int
child_named_hold(char **argv) {
  WCHAR published[64];
  share_name(strtoul(argv[2], nullptr, 10), L"_held", published);
  HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
  HANDLE go = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[4], nullptr, 10)));
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device1> device1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  CHECK(device.As(&device1));
  ComPtr<ID3D11Texture2D> imported;
  CHECK(device1->OpenSharedResourceByName(
      published, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&imported)
  ));
  ComPtr<IDXGIKeyedMutex> mutex;
  CHECK(imported.As(&mutex));
  if (!expect(mutex->AcquireSync(0, 0) == WAIT_TIMEOUT, "the holding open took the initial key, so it came back fresh"))
    return 1;
  if (!expect(mutex->AcquireSync(held_key, wait_ms) == S_OK, "the holding open lost the key the share had"))
    return 1;
  // handed back on the same key, so the open the creator makes next sees what this one saw
  if (!expect(mutex->ReleaseSync(held_key) == S_OK, "the holding open could not release the mutex"))
    return 1;
  auto got = contents(device.Get(), context.Get(), imported.Get());
  if (!expect(got == pattern(41), "the holding open's bytes did not arrive (%u bytes, first %02x)", (unsigned)got.size(),
              got.empty() ? 0u : got[0]))
    return 1;
  if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(ready);
  if (!expect(WaitForSingleObject(go, 60000) == WAIT_OBJECT_0, "the creator never finished with the name"))
    ExitProcess(1);
  CloseHandle(go);
  return verdict();
}

static int
parent() {
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                          nullptr, &context));

  char executable[MAX_PATH];
  if (!expect(!!GetModuleFileNameA(nullptr, executable, sizeof(executable)), "GetModuleFileName: %lu", GetLastError()))
    return verdict();

  step("keyed texture, inherited NT handle, two processes trade the keys");
  D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                                                           D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> texture;
  CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
  ComPtr<IDXGIResource1> resource;
  CHECK(texture.As(&resource));
  ComPtr<IDXGIKeyedMutex> mutex;
  CHECK(texture.As(&mutex));
  HANDLE shared = nullptr;
  CHECK(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared));
  HANDLE duplicate = nullptr;
  if (!expect(!!DuplicateHandle(GetCurrentProcess(), shared, GetCurrentProcess(), &duplicate, 0, TRUE,
                                DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
    return verdict();
  CloseHandle(shared);
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE ready = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  if (!expect(ready != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();
  std::string command = "\"" + std::string(executable) + "\" --keyed " +
                        std::to_string(reinterpret_cast<uintptr_t>(duplicate)) + " " +
                        std::to_string(reinterpret_cast<uintptr_t>(ready));
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &child), "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(child.hThread);
  CloseHandle(duplicate);
  if (!expect(WaitForSingleObject(ready, wait_ms) == WAIT_OBJECT_0, "the keyed child did not open the texture")) {
    TerminateProcess(child.hProcess, 1);
    return verdict();
  }
  CloseHandle(ready);
  for (unsigned turn : {1u, 3u})
    if (keyed_turn(device.Get(), context.Get(), mutex.Get(), texture.Get(), turn, false))
      return 1;
  if (!expect(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0, "the keyed child did not exit")) {
    TerminateProcess(child.hProcess, 1);
    return verdict();
  }
  DWORD code = 1;
  GetExitCodeProcess(child.hProcess, &code);
  expect(code == 0, "the keyed child exited %lu", code);
  CloseHandle(child.hProcess);
  step("the child died owning the mutex, so the mutex is abandoned");
  for (unsigned attempt = 0; attempt < 2; attempt++) {
    expect(mutex->AcquireSync(0, 0) == WAIT_ABANDONED, "the destroyed owner did not abandon the mutex");
    expect(mutex->ReleaseSync(0) == DXGI_ERROR_INVALID_CALL, "the abandoned mutex released without ownership");
  }
  expect(contents(device.Get(), context.Get(), texture.Get()) == pattern(4),
         "the dead child's last bytes did not survive it");
  mutex.Reset();
  resource.Reset();
  expect(texture.Reset() == 0, "the keyed texture retained an extra reference");

  step("keyed texture, a child killed while it owns the mutex, and the mutex it leaves behind");
  D3D11_TEXTURE2D_DESC crash_desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> crash_texture;
  CHECK(device->CreateTexture2D(&crash_desc, nullptr, &crash_texture));
  ComPtr<IDXGIResource1> crash_resource;
  CHECK(crash_texture.As(&crash_resource));
  ComPtr<IDXGIKeyedMutex> crash_mutex;
  CHECK(crash_texture.As(&crash_mutex));
  HANDLE crash_handle = nullptr;
  CHECK(crash_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                           &crash_handle));
  HANDLE inherited = nullptr;
  if (!expect(!!DuplicateHandle(GetCurrentProcess(), crash_handle, GetCurrentProcess(), &inherited, 0, TRUE,
                                DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
    return verdict();
  CloseHandle(crash_handle);
  HANDLE taken = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE die = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  if (!expect(taken != nullptr && die != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();
  command = "\"" + std::string(executable) + "\" --keyed-crash " +
            std::to_string(reinterpret_cast<uintptr_t>(inherited)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(taken)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(die));
  PROCESS_INFORMATION crash_child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &crash_child), "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(crash_child.hThread);
  CloseHandle(inherited);
  if (!expect(WaitForSingleObject(taken, wait_ms) == WAIT_OBJECT_0, "the crashing child did not take the mutex")) {
    TerminateProcess(crash_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(taken);
  // an owner that still exists, however far away, is not an abandoned one
  expect(crash_mutex->AcquireSync(0, 0) == WAIT_TIMEOUT, "a live owner's mutex read as abandoned");
  if (!expect(!!SetEvent(die), "SetEvent: %lu", GetLastError()))
    return verdict();
  CloseHandle(die);
  if (!expect(WaitForSingleObject(crash_child.hProcess, 60000) == WAIT_OBJECT_0, "the crashing child did not exit")) {
    TerminateProcess(crash_child.hProcess, 1);
    return verdict();
  }
  GetExitCodeProcess(crash_child.hProcess, &code);
  CloseHandle(crash_child.hProcess);
  step("the child died owning the mutex without unwinding anything, so the mutex is abandoned");
  for (unsigned attempt = 0; attempt < 2; attempt++) {
    expect(crash_mutex->AcquireSync(0, 0) == WAIT_ABANDONED, "the killed owner did not abandon the mutex");
    expect(crash_mutex->ReleaseSync(0) == DXGI_ERROR_INVALID_CALL, "the abandoned mutex released without ownership");
  }
  crash_mutex.Reset();
  crash_resource.Reset();
  expect(crash_texture.Reset() == 0, "the keyed texture retained an extra reference");

  step("shared texture, marshalled legacy handle, its lifetime outlives its creator");
  D3D11_TEXTURE2D_DESC shared_desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
  ComPtr<ID3D11Texture2D> shared_texture;
  auto initial_bytes = pattern(5);
  D3D11_SUBRESOURCE_DATA initial{initial_bytes.data(), width * 4, 0};
  CHECK(device->CreateTexture2D(&shared_desc, &initial, &shared_texture));
  if (!expect(drained(device.Get(), context.Get()), "the creator's initial bytes did not land"))
    return 1;
  if (!expect(contents(device.Get(), context.Get(), shared_texture.Get()) == initial_bytes,
              "the creator's own texture does not hold its initial bytes"))
    return 1;
  ComPtr<IDXGIResource1> shared_resource;
  CHECK(shared_texture.As(&shared_resource));
  HANDLE invalid = GetCurrentProcess();
  expect(shared_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                             nullptr, &invalid) == E_INVALIDARG &&
             !invalid,
         "the legacy-only texture handed out an NT handle");
  HANDLE legacy = nullptr;
  CHECK(shared_resource->GetSharedHandle(&legacy));
  ready = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE go = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  if (!expect(ready != nullptr && go != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();
  command = "\"" + std::string(executable) + "\" --shared " + std::to_string(reinterpret_cast<uintptr_t>(legacy)) +
            " " + std::to_string(reinterpret_cast<uintptr_t>(ready)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(go));
  PROCESS_INFORMATION shared_child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &shared_child), "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(shared_child.hThread);
  if (!expect(WaitForSingleObject(ready, wait_ms) == WAIT_OBJECT_0, "the shared child did not open the texture")) {
    TerminateProcess(shared_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(ready);
  if (!expect(contents(device.Get(), context.Get(), shared_texture.Get()) == pattern(7),
             "the child's bytes did not arrive through the marshalled handle"))
    return 1;
  step("the handle stays valid while the child holds the resource, and dies with it");
  shared_resource.Reset();
  expect(shared_texture.Reset() == 0, "the creator texture retained an extra reference");
  ComPtr<ID3D11Texture2D> reopened;
  CHECK(device->OpenSharedResource(legacy, IID_PPV_ARGS(&reopened)));
  expect(contents(device.Get(), context.Get(), reopened.Get()) == pattern(7),
         "the reopened texture's bytes changed");
  expect(reopened.Reset() == 0, "the reopened texture retained an extra reference");
  if (!expect(!!SetEvent(go), "SetEvent: %lu", GetLastError()))
    return verdict();
  if (!expect(WaitForSingleObject(shared_child.hProcess, 60000) == WAIT_OBJECT_0, "the shared child did not exit")) {
    TerminateProcess(shared_child.hProcess, 1);
    return verdict();
  }
  GetExitCodeProcess(shared_child.hProcess, &code);
  expect(code == 0, "the shared child exited %lu", code);
  CloseHandle(shared_child.hProcess);
  CloseHandle(go);
  ComPtr<ID3D11Texture2D> dead;
  HRESULT hr = device->OpenSharedResource(legacy, IID_PPV_ARGS(&dead));
  expect(FAILED(hr) && !dead, "the released sharing chain kept its handle valid, hr %#lx", hr);

  step("an NT handle kept while every resource of its share goes carries the mutex it was made with");
  D3D11_TEXTURE2D_DESC late_desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> late_texture;
  CHECK(device->CreateTexture2D(&late_desc, nullptr, &late_texture));
  ComPtr<IDXGIResource1> late_resource;
  CHECK(late_texture.As(&late_resource));
  ComPtr<IDXGIKeyedMutex> late_mutex;
  CHECK(late_texture.As(&late_mutex));
  write_update(context.Get(), late_texture.Get(), pattern(11));
  if (!expect(drained(device.Get(), context.Get()), "the late texture's bytes did not land"))
    return 1;
  if (!expect(late_mutex->AcquireSync(0, wait_ms) == S_OK, "the late texture's mutex did not take its initial key"))
    return 1;
  // the share is left released at a key of its own, which is the mutex a later open has to come back with
  if (!expect(late_mutex->ReleaseSync(late_key) == S_OK, "the late texture's mutex did not release its key"))
    return 1;
  HANDLE kept = nullptr;
  CHECK(late_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &kept));
  // every resource of the share goes, and the handle is all that is left of it
  late_mutex.Reset();
  late_resource.Reset();
  expect(late_texture.Reset() == 0, "the late texture retained an extra reference");
  {
    second_device other;
    if (!expect(other.open(), "a second device could not be made"))
      return verdict();
    ComPtr<ID3D11Texture2D> reopened;
    expect(SUCCEEDED(other.device1->OpenSharedResource1(kept, IID_PPV_ARGS(&reopened))),
           "the kept handle no longer opened a texture once every resource of the share went");
    // the rest of this step says what the reopened texture has to be, so it only runs on one that opened
    if (reopened) {
      ComPtr<IDXGIKeyedMutex> reopened_mutex;
      expect(SUCCEEDED(reopened.As(&reopened_mutex)), "the reopened texture has no mutex");
      if (reopened_mutex) {
        // a mutex that came back fresh is free with the initial key, and one that came back as its share's is not
        expect(reopened_mutex->AcquireSync(0, 0) == WAIT_TIMEOUT,
               "the reopened texture's mutex took the initial key, so it came back fresh");
        expect(reopened_mutex->AcquireSync(late_key, wait_ms) == S_OK,
               "the reopened texture's mutex lost the key it had");
        // handed on at the key it was taken at, so the child that opens the handle next finds the same one
        expect(reopened_mutex->ReleaseSync(late_key) == S_OK, "the reopened texture's mutex could not release");
        reopened_mutex.Reset();
      }
      expect(contents(other.device.Get(), other.context.Get(), reopened.Get()) == pattern(11),
             "the reopened texture's bytes changed");
      expect(reopened.Reset() == 0, "the reopened texture retained an extra reference");
    }
  }
  // and the same handle in a process of its own, which never held a resource of the share either
  {
    HANDLE inherited = nullptr;
    if (!expect(!!DuplicateHandle(GetCurrentProcess(), kept, GetCurrentProcess(), &inherited, 0, TRUE,
                                  DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
      return verdict();
    std::string command = "\"" + std::string(executable) + "\" --late " +
                          std::to_string(reinterpret_cast<uintptr_t>(inherited));
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                                 &child), "CreateProcess: %lu", GetLastError()))
      return verdict();
    CloseHandle(child.hThread);
    CloseHandle(inherited);
    if (!expect(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0, "the late child did not exit")) {
      TerminateProcess(child.hProcess, 1);
      return verdict();
    }
    DWORD late_code = 1;
    GetExitCodeProcess(child.hProcess, &late_code);
    CloseHandle(child.hProcess);
    expect(late_code == 0, "the late child exited %lu", late_code);
  }
  CloseHandle(kept);

  step("a name a share published is a name no other share can take");
  WCHAR published_name[64];
  share_name(GetCurrentProcessId(), L"", published_name);
  D3D11_TEXTURE2D_DESC named_desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0,
                                   D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> named_texture;
  CHECK(device->CreateTexture2D(&named_desc, nullptr, &named_texture));
  ComPtr<IDXGIResource1> named_resource;
  CHECK(named_texture.As(&named_resource));
  ComPtr<IDXGIKeyedMutex> named_mutex;
  CHECK(named_texture.As(&named_mutex));
  write_update(context.Get(), named_texture.Get(), pattern(21));
  if (!expect(drained(device.Get(), context.Get()), "the named texture's bytes did not land"))
    return 1;
  if (!expect(named_mutex->AcquireSync(0, wait_ms) == S_OK, "the named texture's mutex did not take its initial key"))
    return 1;
  if (!expect(named_mutex->ReleaseSync(named_key) == S_OK, "the named texture's mutex did not release its key"))
    return 1;
  SECURITY_ATTRIBUTES named_attributes{sizeof(named_attributes), nullptr, TRUE};
  HANDLE named = nullptr;
  CHECK(named_resource->CreateSharedHandle(
      &named_attributes, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, published_name, &named
  ));
  {
    ComPtr<ID3D11Texture2D> clash_texture;
    CHECK(device->CreateTexture2D(&named_desc, nullptr, &clash_texture));
    ComPtr<IDXGIResource1> clash_resource;
    CHECK(clash_texture.As(&clash_resource));
    HANDLE clash = nullptr;
    HRESULT clash_hr = clash_resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, published_name, &clash
    );
    expect(clash_hr == DXGI_ERROR_NAME_ALREADY_EXISTS && !clash, "a name in use was published twice, hr %#lx",
           clash_hr);
    clash_resource.Reset();
    expect(clash_texture.Reset() == 0, "the clashing texture retained an extra reference");
    DWORD flags = 0;
    expect(!!GetHandleInformation(named, &flags) && (flags & HANDLE_FLAG_INHERIT),
           "the published handle did not inherit as it was asked to");
  }

  step("a named handle publishes its name, so a device and a process of its own open the texture by it");
  {
    second_device other;
    if (!expect(other.open(), "a second device could not be made"))
      return verdict();
    ComPtr<ID3D11Texture2D> opened;
    expect(SUCCEEDED(other.device1->OpenSharedResourceByName(
               published_name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&opened)
           )),
           "the name a share published did not open on another device");
    if (opened) {
      ComPtr<IDXGIKeyedMutex> opened_mutex;
      expect(SUCCEEDED(opened.As(&opened_mutex)), "the named open has no mutex");
      if (opened_mutex) {
        expect(opened_mutex->AcquireSync(0, 0) == WAIT_TIMEOUT,
               "the named open took the initial key, so it came back fresh");
        expect(opened_mutex->AcquireSync(named_key, wait_ms) == S_OK, "the named open lost the key the share had");
        // handed on at the key it was taken at, so the child that opens the name next finds the same one
        expect(opened_mutex->ReleaseSync(named_key) == S_OK, "the named open could not release the mutex");
        opened_mutex.Reset();
      }
      expect(contents(other.device.Get(), other.context.Get(), opened.Get()) == pattern(21),
             "the named open's bytes changed");
      expect(opened.Reset() == 0, "the named open retained an extra reference");
    }
    WCHAR nobody[64];
    share_name(GetCurrentProcessId() + 1, L"_nobody", nobody);
    ComPtr<ID3D11Texture2D> nothing;
    expect(FAILED(other.device1->OpenSharedResourceByName(
               nobody, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&nothing)
           )),
           "a name nothing published opened a texture");
  }
  {
    std::string command = "\"" + std::string(executable) + "\" --named " +
                          std::to_string((unsigned long)GetCurrentProcessId());
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                                 &child), "CreateProcess: %lu", GetLastError()))
      return verdict();
    CloseHandle(child.hThread);
    if (!expect(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0, "the named child did not exit")) {
      TerminateProcess(child.hProcess, 1);
      return verdict();
    }
    DWORD named_code = 1;
    GetExitCodeProcess(child.hProcess, &named_code);
    CloseHandle(child.hProcess);
    expect(named_code == 0, "the named child exited %lu", named_code);
  }
  named_mutex.Reset();
  named_resource.Reset();
  expect(named_texture.Reset() == 0, "the named texture retained an extra reference");
  CloseHandle(named);

  step("a share published for reading is opened for reading and refused for writing");
  WCHAR read_name[64];
  share_name(GetCurrentProcessId(), L"_read", read_name);
  D3D11_TEXTURE2D_DESC read_desc{width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> read_texture;
  CHECK(device->CreateTexture2D(&read_desc, nullptr, &read_texture));
  ComPtr<IDXGIResource1> read_resource;
  CHECK(read_texture.As(&read_resource));
  write_update(context.Get(), read_texture.Get(), pattern(31));
  if (!expect(drained(device.Get(), context.Get()), "the read-only texture's bytes did not land"))
    return 1;
  HANDLE read_handle = nullptr;
  CHECK(read_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, read_name, &read_handle));
  {
    second_device other;
    if (!expect(other.open(), "a second device could not be made"))
      return verdict();
    ComPtr<ID3D11Texture2D> refused;
    HRESULT refused_hr = other.device1->OpenSharedResourceByName(
        read_name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&refused)
    );
    expect(refused_hr == E_ACCESSDENIED && !refused, "a share published for reading opened for writing, hr %#lx",
           refused_hr);
    ComPtr<ID3D11Texture2D> allowed;
    expect(SUCCEEDED(other.device1->OpenSharedResourceByName(read_name, DXGI_SHARED_RESOURCE_READ,
                                                             IID_PPV_ARGS(&allowed))),
           "a share published for reading did not open for reading");
    if (allowed) {
      expect(contents(other.device.Get(), other.context.Get(), allowed.Get()) == pattern(31),
             "the reading open's bytes changed");
      expect(allowed.Reset() == 0, "the reading open retained an extra reference");
    }
  }
  CloseHandle(read_handle);
  read_resource.Reset();
  expect(read_texture.Reset() == 0, "the read-only texture retained an extra reference");

  step("a published name outlives its creator: a peer's resource keeps it open");
  WCHAR held_name[64];
  share_name(GetCurrentProcessId(), L"_held", held_name);
  ComPtr<ID3D11Texture2D> held_texture;
  CHECK(device->CreateTexture2D(&named_desc, nullptr, &held_texture));
  ComPtr<IDXGIResource1> held_resource;
  CHECK(held_texture.As(&held_resource));
  ComPtr<IDXGIKeyedMutex> held_mutex;
  CHECK(held_texture.As(&held_mutex));
  write_update(context.Get(), held_texture.Get(), pattern(41));
  if (!expect(drained(device.Get(), context.Get()), "the held texture's bytes did not land"))
    return 1;
  if (!expect(held_mutex->AcquireSync(0, wait_ms) == S_OK, "the held texture's mutex did not take its initial key"))
    return 1;
  if (!expect(held_mutex->ReleaseSync(held_key) == S_OK, "the held texture's mutex did not release its key"))
    return 1;
  HANDLE held = nullptr;
  CHECK(held_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, held_name,
                                          &held));
  HANDLE holder_ready = CreateEventA(&named_attributes, FALSE, FALSE, nullptr);
  HANDLE holder_go = CreateEventA(&named_attributes, FALSE, FALSE, nullptr);
  if (!expect(holder_ready != nullptr && holder_go != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();
  std::string holder_command = "\"" + std::string(executable) + "\" --named-hold " +
                               std::to_string((unsigned long)GetCurrentProcessId()) + " " +
                               std::to_string(reinterpret_cast<uintptr_t>(holder_ready)) + " " +
                               std::to_string(reinterpret_cast<uintptr_t>(holder_go));
  PROCESS_INFORMATION holder{};
  if (!expect(!!CreateProcessA(executable, holder_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &holder), "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(holder.hThread);
  if (!expect(WaitForSingleObject(holder_ready, wait_ms) == WAIT_OBJECT_0, "the holding child did not open the name")) {
    TerminateProcess(holder.hProcess, 1);
    return verdict();
  }
  CloseHandle(holder_ready);
  // the creator now lets every resource of the share go, so only the peer's resource holds the name
  held_mutex.Reset();
  held_resource.Reset();
  expect(held_texture.Reset() == 0, "the held texture retained an extra reference");
  CloseHandle(held);
  {
    second_device other;
    if (!expect(other.open(), "a second device could not be made"))
      return verdict();
    ComPtr<ID3D11Texture2D> late_named;
    expect(SUCCEEDED(other.device1->OpenSharedResourceByName(
               held_name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&late_named)
           )),
           "the name died with the creator even though a peer held a resource of the share");
    if (late_named) {
      ComPtr<IDXGIKeyedMutex> late_named_mutex;
      expect(SUCCEEDED(late_named.As(&late_named_mutex)), "the name-open has no mutex");
      if (late_named_mutex) {
        expect(late_named_mutex->AcquireSync(held_key, wait_ms) == S_OK,
               "the name-open lost the key the share had");
        expect(late_named_mutex->ReleaseSync(held_key) == S_OK, "the name-open could not release the mutex");
        late_named_mutex.Reset();
      }
      expect(contents(other.device.Get(), other.context.Get(), late_named.Get()) == pattern(41),
             "the name-open's bytes changed");
      expect(late_named.Reset() == 0, "the name-open retained an extra reference");
    }
  }
  if (!expect(!!SetEvent(holder_go), "SetEvent: %lu", GetLastError())) {
    TerminateProcess(holder.hProcess, 1);
    return verdict();
  }
  CloseHandle(holder_go);
  if (!expect(WaitForSingleObject(holder.hProcess, 60000) == WAIT_OBJECT_0, "the holding child did not exit")) {
    TerminateProcess(holder.hProcess, 1);
    return verdict();
  }
  DWORD held_code = 1;
  GetExitCodeProcess(holder.hProcess, &held_code);
  CloseHandle(holder.hProcess);
  expect(held_code == 0, "the holding child exited %lu", held_code);
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "--keyed"))
    return child_keyed(argv);
  if (argc > 4 && !strcmp(argv[1], "--keyed-crash"))
    return child_keyed_crash(argv);
  if (argc > 2 && !strcmp(argv[1], "--shared"))
    return child_shared(argv);
  if (argc > 2 && !strcmp(argv[1], "--late"))
    return child_late(argv);
  if (argc > 2 && !strcmp(argv[1], "--named"))
    return child_named(argv);
  if (argc > 4 && !strcmp(argv[1], "--named-hold"))
    return child_named_hold(argv);
  return parent();
}
