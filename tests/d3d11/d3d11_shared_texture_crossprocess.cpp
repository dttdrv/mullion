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
  hr = device1->OpenSharedResource1(HANDLE(uintptr_t(0x4000000000000001ull)), IID_PPV_ARGS(&wrong));
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

// the blocked child: the parent owns the mutex when this process asks for the key it will release, so an infinite
// acquire can only return once that release lands; a wrong key then gives up at its own time-out, not before
static int
child_blocked(char **argv) {
  HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
  HANDLE go = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[4], nullptr, 10)));
  HANDLE go2 = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[5], nullptr, 10)));
  HANDLE die = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[6], nullptr, 10)));
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
  if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(ready);
  if (!expect(WaitForSingleObject(go, wait_ms) == WAIT_OBJECT_0, "the parent never asked for the blocked acquire"))
    ExitProcess(1);
  CloseHandle(go);
  // the parent owns the mutex and has not released key 1 yet: this acquire cannot return before it does
  ULONGLONG started = GetTickCount64();
  HRESULT hr = mutex->AcquireSync(1, INFINITE);
  ULONGLONG waited = GetTickCount64() - started;
  if (!expect(hr == S_OK, "the infinite acquire returned %#lx instead of taking the released key", hr))
    ExitProcess(1);
  if (!expect(waited >= 200, "the infinite acquire returned after %llu ms, before the release it waits for",
              (unsigned long long)waited))
    ExitProcess(1);
  if (!expect(mutex->ReleaseSync(2) == S_OK, "the woken owner could not release the mutex"))
    ExitProcess(1);
  if (!expect(!!SetEvent(go2), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(go2);
  if (!expect(WaitForSingleObject(die, wait_ms) == WAIT_OBJECT_0, "the parent never took the mutex back"))
    ExitProcess(1);
  CloseHandle(die);
  // the parent owns it again: a key nobody released waits out its whole time-out
  started = GetTickCount64();
  hr = mutex->AcquireSync(probe_salt, 150);
  waited = GetTickCount64() - started;
  if (!expect(hr == WAIT_TIMEOUT, "a wrong key returned %#lx instead of waiting out its time-out", hr))
    ExitProcess(1);
  if (!expect(waited >= 120, "the time-out returned after %llu ms, before its 150 ms", (unsigned long long)waited))
    ExitProcess(1);
  return verdict();
}

// the owner child: take the mutex and die holding it, so its destruction abandons the mutex for whoever waits
static int
child_owner(char **argv) {
  HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
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
  if (!expect(mutex->AcquireSync(0, wait_ms) == S_OK, "the owner could not take the initial key"))
    ExitProcess(1);
  if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(ready);
  if (!expect(WaitForSingleObject(die, wait_ms) == WAIT_OBJECT_0, "the parent never ended the owner"))
    ExitProcess(1);
  CloseHandle(die);
  return verdict(); // the references die here, owning the mutex
}

// the abandoning waiter: the mutex is owned when this process blocks on it, so only the death of that owner can end
// the wait, and it must end with WAIT_ABANDONED rather than waiting for a release that will never come
static int
child_abandoned(char **argv) {
  HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
  HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
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
  if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
    ExitProcess(1);
  CloseHandle(ready);
  ULONGLONG started = GetTickCount64();
  HRESULT hr = mutex->AcquireSync(0, INFINITE);
  ULONGLONG waited = GetTickCount64() - started;
  if (!expect(hr == WAIT_ABANDONED, "the death of the owner woke the blocked acquire as %#lx, not WAIT_ABANDONED", hr))
    ExitProcess(1);
  if (!expect(waited >= 150, "the blocked acquire woke after %llu ms, before the owner it waits for died",
              (unsigned long long)waited))
    ExitProcess(1);
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
  if (!expect(!!modulePath(executable, sizeof(executable)), "GetModuleFileName: %lu", GetLastError()))
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

  step("an infinite acquire returns only when the release it waits for lands");
  ComPtr<ID3D11Texture2D> waiting_texture;
  CHECK(device->CreateTexture2D(&desc, nullptr, &waiting_texture));
  ComPtr<IDXGIResource1> waiting_resource;
  CHECK(waiting_texture.As(&waiting_resource));
  ComPtr<IDXGIKeyedMutex> waiting_mutex;
  CHECK(waiting_texture.As(&waiting_mutex));
  HANDLE waiting_handle = nullptr;
  CHECK(waiting_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                             &waiting_handle));
  HANDLE waiting_duplicate = nullptr;
  if (!expect(!!DuplicateHandle(GetCurrentProcess(), waiting_handle, GetCurrentProcess(), &waiting_duplicate, 0, TRUE,
                                DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
    return verdict();
  CloseHandle(waiting_handle);
  HANDLE blocked_ready = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE blocked_go = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE blocked_go2 = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE blocked_die = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  if (!expect(blocked_ready && blocked_go && blocked_go2 && blocked_die, "CreateEvent: %lu", GetLastError()))
    return verdict();
  // the parent owns the mutex while the child asks for key 1: nothing but its release can end that wait
  CHECK(waiting_mutex->AcquireSync(0, wait_ms));
  command = "\"" + std::string(executable) + "\" --blocked " +
            std::to_string(reinterpret_cast<uintptr_t>(waiting_duplicate)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(blocked_ready)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(blocked_go)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(blocked_go2)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(blocked_die));
  PROCESS_INFORMATION blocked_child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &blocked_child), "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(blocked_child.hThread);
  CloseHandle(waiting_duplicate);
  if (!expect(WaitForSingleObject(blocked_ready, wait_ms) == WAIT_OBJECT_0, "the blocked child did not open the texture")) {
    TerminateProcess(blocked_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(blocked_ready);
  if (!expect(!!SetEvent(blocked_go), "SetEvent: %lu", GetLastError()))
    return verdict();
  Sleep(250); // the child is waiting on a release this process has not made yet
  if (!expect(waiting_mutex->ReleaseSync(1) == S_OK, "the owner could not release the key the child waits for"))
    return verdict();
  if (!expect(WaitForSingleObject(blocked_go2, wait_ms) == WAIT_OBJECT_0, "the blocked acquire never returned")) {
    TerminateProcess(blocked_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(blocked_go2);
  if (!expect(waiting_mutex->AcquireSync(2, wait_ms) == S_OK, "the parent could not take the key the child released"))
    return verdict();
  if (!expect(!!SetEvent(blocked_die), "SetEvent: %lu", GetLastError()))
    return verdict();
  if (!expect(WaitForSingleObject(blocked_child.hProcess, 60000) == WAIT_OBJECT_0, "the blocked child did not exit")) {
    TerminateProcess(blocked_child.hProcess, 1);
    return verdict();
  }
  GetExitCodeProcess(blocked_child.hProcess, &code);
  expect(code == 0, "the blocked child exited %lu", code);
  CloseHandle(blocked_child.hProcess);
  CloseHandle(blocked_die);

  step("the death of an owner wakes an acquire blocked on it");
  ComPtr<ID3D11Texture2D> abandoning_texture;
  CHECK(device->CreateTexture2D(&desc, nullptr, &abandoning_texture));
  ComPtr<IDXGIResource1> abandoning_resource;
  CHECK(abandoning_texture.As(&abandoning_resource));
  HANDLE abandoning_handle = nullptr;
  CHECK(abandoning_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                nullptr, &abandoning_handle));
  HANDLE abandoning_duplicate = nullptr;
  if (!expect(!!DuplicateHandle(GetCurrentProcess(), abandoning_handle, GetCurrentProcess(), &abandoning_duplicate, 0,
                                TRUE, DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
    return verdict();
  CloseHandle(abandoning_handle);
  HANDLE owner_ready = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE owner_die = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  HANDLE waiter_ready = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  if (!expect(owner_ready && owner_die && waiter_ready, "CreateEvent: %lu", GetLastError()))
    return verdict();
  command = "\"" + std::string(executable) + "\" --owner " +
            std::to_string(reinterpret_cast<uintptr_t>(abandoning_duplicate)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(owner_ready)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(owner_die));
  PROCESS_INFORMATION owner_child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &owner_child), "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(owner_child.hThread);
  if (!expect(WaitForSingleObject(owner_ready, wait_ms) == WAIT_OBJECT_0, "the owner child did not take the mutex")) {
    TerminateProcess(owner_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(owner_ready);
  // the owner is alive and holds the mutex: only its death can end the waiter's infinite acquire
  command = "\"" + std::string(executable) + "\" --abandoned " +
            std::to_string(reinterpret_cast<uintptr_t>(abandoning_duplicate)) + " " +
            std::to_string(reinterpret_cast<uintptr_t>(waiter_ready));
  PROCESS_INFORMATION waiter_child{};
  if (!expect(!!CreateProcessA(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                               &waiter_child), "CreateProcess: %lu", GetLastError())) {
    TerminateProcess(owner_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(waiter_child.hThread);
  CloseHandle(abandoning_duplicate);
  if (!expect(WaitForSingleObject(waiter_ready, wait_ms) == WAIT_OBJECT_0, "the waiter child did not open the texture")) {
    TerminateProcess(owner_child.hProcess, 1);
    TerminateProcess(waiter_child.hProcess, 1);
    return verdict();
  }
  CloseHandle(waiter_ready);
  Sleep(200); // the waiter is blocked on an owner that is about to die holding the mutex
  if (!expect(!!SetEvent(owner_die), "SetEvent: %lu", GetLastError()))
    return verdict();
  CloseHandle(owner_die);
  if (!expect(WaitForSingleObject(owner_child.hProcess, 60000) == WAIT_OBJECT_0, "the owner child did not exit")) {
    TerminateProcess(owner_child.hProcess, 1);
    TerminateProcess(waiter_child.hProcess, 1);
    return verdict();
  }
  GetExitCodeProcess(owner_child.hProcess, &code);
  expect(code == 0, "the owner child exited %lu", code);
  CloseHandle(owner_child.hProcess);
  if (!expect(WaitForSingleObject(waiter_child.hProcess, 60000) == WAIT_OBJECT_0, "the waiter child did not exit")) {
    TerminateProcess(waiter_child.hProcess, 1);
    return verdict();
  }
  GetExitCodeProcess(waiter_child.hProcess, &code);
  expect(code == 0, "the waiter child exited %lu", code);
  CloseHandle(waiter_child.hProcess);

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
  return verdict();
}

int
main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "--keyed"))
    return child_keyed(argv);
  if (argc > 2 && !strcmp(argv[1], "--blocked"))
    return child_blocked(argv);
  if (argc > 2 && !strcmp(argv[1], "--owner"))
    return child_owner(argv);
  if (argc > 2 && !strcmp(argv[1], "--abandoned"))
    return child_abandoned(argv);
  if (argc > 2 && !strcmp(argv[1], "--shared"))
    return child_shared(argv);
  return parent();
}
