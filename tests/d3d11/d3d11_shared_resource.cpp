// contract: a valid shared texture opens with the matching handle type and one reference, and a keyed mutex
// gives only its owner access to the same texture bytes on either device. Microsoft Learn, GetSharedHandle:
// "you must create the resource as shared and specify that it uses NT handles" for CreateSharedHandle.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiresource-getsharedhandle
// GetSharedHandle: "GetSharedHandle can also return handles for resources that were passed into
// ID3D11Device::OpenSharedResource to open those resources." an opened resource keeps that handle valid after
// its creator is released, until the last resource is released.
// D3D11_RESOURCE_MISC_FLAG: "D3D11_RESOURCE_MISC_SHARED and D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX are mutually
// exclusive." Wine's Windows witness test_shared_resource additionally requires a sharing mode with NTHANDLE
// and rejects these RGBA shared textures below feature level 10_0 (d3d11/tests/d3d11.c). 10Level9 CreateTexture2D:
// "Format cannot be DXGI_FORMAT_R8G8B8A8_UNORM or DXGI_FORMAT_R8G8B8A8_UNORM_SRGB."
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-10level9-device
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_resource_misc_flag
// AcquireSync: "A keyed mutex does not support recursive calls to the AcquireSync method." the key may be any
// UINT64; the initial key is zero and subsequent keys are those released. Wine's Windows test_keyed_mutex
// observes DXGI_ERROR_INVALID_CALL for recursive acquire and unowned release, although Learn says E_FAIL.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync
// D3D11.3 6.1: "D3D11 will continue to rely on shared resources to achieve fully parallel GPU usage or multi-GPU
// usage". every sharing flag combination and every key bit are exercised; there are no shaders or random inputs.
// queued mutex work survives releasing the texture. an EVENT is signaled after earlier commands finish and
// "the value of the BOOL is always TRUE" (D3D11.3 20.4.2); the D3D10_QUERY_EVENT contract is the same.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/ne-d3d10-d3d10_query
// the lifetime cases stop CPU encoding before the queued acquire or release can retain its event, drop the last
// texture reference, then resume. completion returns TRUE; the runner also rejects any zombie message.
// D3D10-to-D3D11 and return writes are compared byte for byte after completed uploads. OpenSharedResource:
// "If a shared texture is updated on one device ID3D10Device::Flush must be called on that device."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d10/nf-d3d10-id3d10device-opensharedresource
#include "d3d11_test.hpp"
#include <d3d11_1.h>
#include <d3d10_1.h>
#include <limits>

static UINT64(__cdecl *command_buffer)(UINT64);
static HANDLE encoding_stopped, encoding_resume;
static volatile LONG stop_encoding;
static DWORD issuing_thread;

static UINT64 __cdecl
paused_command_buffer(UINT64 queue) {
  // uploads create command buffers on the issuing thread; only pause the asynchronous encoder
  if (GetCurrentThreadId() != issuing_thread && InterlockedCompareExchange(&stop_encoding, 0, 1) == 1) {
    SetEvent(encoding_stopped);
    if (WaitForSingleObject(encoding_resume, INFINITE) != WAIT_OBJECT_0)
      ExitProcess(1);
  }
  return command_buffer(queue);
}

// the key reaches the kernel through D3DKMTAcquireKeyedMutex, whose Key is a UINT64 on Windows; Wine's server keeps
// 32 bits of it (server/protocol.def, d3dkmt_mutex_acquire and d3dkmt_mutex_release in Wine-cx-26.2 and Rakia).
// server/d3dkmt.c's d3dkmt_mutex.key_value is also unsigned int. a key the host itself cannot tell
// from zero is the host's defect: it is measured on the host's own interface, named in the log and not exercised.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_acquirekeyedmutex
static UINT64
host_lost_key_bits() {
  struct Create {
    UINT64 InitialValue;
    UINT hSharedHandle, hKeyedMutex;
  } created{};
  struct Acquire {
    UINT hKeyedMutex;
    UINT64 Key;
    LARGE_INTEGER *pTimeout;
    UINT64 FenceValue;
  };
  struct Release {
    UINT hKeyedMutex;
    UINT64 Key, FenceValue;
  };
  HMODULE gdi = LoadLibraryA("gdi32.dll");
  auto create = reinterpret_cast<LONG(WINAPI *)(Create *)>((void *)GetProcAddress(gdi, "D3DKMTCreateKeyedMutex"));
  auto acquire = reinterpret_cast<LONG(WINAPI *)(Acquire *)>((void *)GetProcAddress(gdi, "D3DKMTAcquireKeyedMutex"));
  auto release = reinterpret_cast<LONG(WINAPI *)(Release *)>((void *)GetProcAddress(gdi, "D3DKMTReleaseKeyedMutex"));
  auto destroy = reinterpret_cast<LONG(WINAPI *)(UINT *)>((void *)GetProcAddress(gdi, "D3DKMTDestroyKeyedMutex"));
  if (!create || !acquire || !release || !destroy || create(&created))
    return 0;
  UINT64 lost = 0;
  LARGE_INTEGER now{};
  Acquire owned{created.hKeyedMutex, 0, &now};
  bool measured = !acquire(&owned);
  for (unsigned bit = 0; measured && bit < std::numeric_limits<UINT64>::digits; bit++) {
    Release released{created.hKeyedMutex, UINT64(1) << bit, owned.FenceValue + 1};
    measured = !release(&released);
    owned.Key = 0;
    if (measured && !acquire(&owned))
      lost |= released.Key;
    else if (measured)
      measured = !acquire(&(owned = {created.hKeyedMutex, released.Key, &now}));
  }
  destroy(&created.hKeyedMutex);
  return measured ? lost : 0;
}

int
main() {
  const UINT64 lost_key_bits = host_lost_key_bits();
  if (lost_key_bits)
    printf("host: D3DKMT keyed mutexes lose key bits %#llx; keys made only of them are not exercised\n",
           lost_key_bits);
  const DWORD access = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
  const UINT modes = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  const UINT width = std::numeric_limits<UINT64>::digits + 1;
  D3D11_TEXTURE2D_DESC desc{
      width, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
  for (D3D_FEATURE_LEVEL level :
       {D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1}) {
    ComPtr<ID3D11Device> device, other, third;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, nullptr));
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other,
                            nullptr, nullptr));
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &third,
                            nullptr, nullptr));
    for (bool shared : {false, true})
      for (bool keyed : {false, true})
        for (bool nt : {false, true})
          for (auto format :
               {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}) {
            desc.Format = format;
            desc.MiscFlags = (shared ? D3D11_RESOURCE_MISC_SHARED : 0) |
                             (keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : 0) |
                             (nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
            step("feature level %#x, flags %#x, format %u", level, desc.MiscFlags, format);
            bool valid = !desc.MiscFlags ||
                         (shared != keyed && (level >= D3D_FEATURE_LEVEL_10_0 || format == DXGI_FORMAT_B8G8R8A8_UNORM));
            ComPtr<ID3D11Texture2D> texture;
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
            expect(hr == (valid ? S_OK : E_INVALIDARG), "CreateTexture2D returned %#lx", hr);
            expect(valid ? bool(texture) : !texture, "CreateTexture2D output is wrong");
            if (!valid || FAILED(hr) || !texture)
              continue;
            ComPtr<IDXGIKeyedMutex> mutex;
            hr = texture.As(&mutex);
            expect(hr == (keyed ? S_OK : E_NOINTERFACE), "QueryInterface keyed mutex returned %#lx", hr);
            mutex.Reset();
            ComPtr<IDXGIResource1> resource;
            CHECK(texture.As(&resource));
            HANDLE legacy = GetCurrentProcess();
            hr = resource->GetSharedHandle(&legacy);
            expect(hr == (nt ? E_INVALIDARG : S_OK), "GetSharedHandle returned %#lx", hr);
            expect(nt ? legacy == GetCurrentProcess() : (desc.MiscFlags ? legacy != nullptr : legacy == nullptr),
                   "GetSharedHandle output is wrong");
            HANDLE handle = GetCurrentProcess();
            hr = resource->CreateSharedHandle(nullptr, access | GENERIC_ALL, nullptr, &handle);
            expect(hr == (nt ? S_OK : E_INVALIDARG), "CreateSharedHandle returned %#lx", hr);
            expect(nt ? handle != nullptr : handle == nullptr, "CreateSharedHandle output is wrong");
            if (nt && FAILED(hr))
              continue;
            if (!(desc.MiscFlags & modes))
              continue;
            if (!nt)
              handle = legacy;
            if (level == D3D_FEATURE_LEVEL_11_0 && shared && !keyed && !nt &&
                format == DXGI_FORMAT_R8G8B8A8_UNORM && legacy) {
              HANDLE truncated = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(static_cast<uint32_t>(
                  reinterpret_cast<uintptr_t>(legacy))));
              ComPtr<ID3D11Texture2D> from32;
              hr = other->OpenSharedResource(truncated, IID_PPV_ARGS(&from32));
              expect(SUCCEEDED(hr) && from32,
                     "a 32-bit truncation of the legacy handle did not open the same share, hr %#lx", hr);
            }
            HANDLE duplicate = nullptr;
            BOOL duplicated = DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, FALSE,
                                              DUPLICATE_SAME_ACCESS);
            expect(nt ? !!duplicated : !duplicated && GetLastError() == ERROR_INVALID_HANDLE,
                   "DuplicateHandle returned %d, error %lu", duplicated, GetLastError());
            if (duplicated)
              CloseHandle(duplicate);
            for (auto target : {device.Get(), other.Get()}) {
              ComPtr<ID3D11Device1> target1;
              CHECK(target->QueryInterface(IID_PPV_ARGS(&target1)));
              ComPtr<ID3D11Texture2D> imported, invalid;
              ComPtr<ID3D11Buffer> wrong_interface;
              hr = nt ? target1->OpenSharedResource1(handle, IID_PPV_ARGS(&wrong_interface))
                      : target->OpenSharedResource(handle, IID_PPV_ARGS(&wrong_interface));
              expect(hr == E_NOINTERFACE && !wrong_interface, "texture opened as a buffer, hr %#lx", hr);
              hr = nt ? target->OpenSharedResource(handle, IID_PPV_ARGS(&invalid))
                      : target1->OpenSharedResource1(handle, IID_PPV_ARGS(&invalid));
              expect(hr == E_INVALIDARG && !invalid, "wrong handle type returned %#lx", hr);
              hr = nt ? target1->OpenSharedResource1(handle, IID_PPV_ARGS(&imported))
                      : target->OpenSharedResource(handle, IID_PPV_ARGS(&imported));
              expect(hr == S_OK && imported, "OpenSharedResource returned %#lx", hr);
              if (!imported)
                continue;
              D3D11_TEXTURE2D_DESC got{};
              imported->GetDesc(&got);
              expect(got.Width == desc.Width && got.Height == desc.Height && got.Format == desc.Format &&
                         got.MiscFlags == desc.MiscFlags,
                     "imported texture description changed");
              if (!nt && target == other.Get()) {
                step("feature level %#x, flags %#x, format %u, creator-first sharing chain", level, desc.MiscFlags,
                     format);
                resource.Reset();
                expect(texture.Reset() == 0, "creator texture retained an extra reference");
                CHECK(imported.As(&resource));
                HANDLE forwarded_handle = nullptr;
                CHECK(resource->GetSharedHandle(&forwarded_handle));
                expect(forwarded_handle == handle, "imported sharing identity changed");
                ComPtr<ID3D11Texture2D> forwarded;
                CHECK(third->OpenSharedResource(forwarded_handle, IID_PPV_ARGS(&forwarded)));
                resource.Reset();
                expect(imported.Reset() == 0, "middle texture retained an extra reference");
                CHECK(device->OpenSharedResource(handle, IID_PPV_ARGS(&imported)));
                expect(imported.Reset() == 0, "reopened texture retained an extra reference");
                expect(forwarded.Reset() == 0, "last texture retained an extra reference");
                hr = device->OpenSharedResource(handle, IID_PPV_ARGS(&invalid));
                expect(hr == E_INVALIDARG && !invalid, "released sharing chain retained its handle, hr %#lx", hr);
              }
              expect(imported.Reset() == 0, "imported texture retained an extra reference");
            }
            BOOL closed = CloseHandle(handle);
            expect(nt ? !!closed : !closed && GetLastError() == ERROR_INVALID_HANDLE,
                   "CloseHandle returned %d, error %lu", closed, GetLastError());
          }
    expect(other.Reset() == 0, "other device retained a shared texture");
    expect(third.Reset() == 0, "third device retained a shared texture");
    expect(device.Reset() == 0, "device retained a shared texture");
  }

  for (bool nt : {false, true}) {
    step("keyed texture bytes, NT handle %d", nt);
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> device, other;
    ComPtr<ID3D11DeviceContext> context, other_context;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, &context));
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other,
                            nullptr, &other_context));
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | (nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
    ComPtr<ID3D11Texture2D> texture, imported, staging, other_staging;
    CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
    ComPtr<IDXGIResource1> resource;
    CHECK(texture.As(&resource));
    HANDLE handle = nullptr;
    CHECK(nt ? resource->CreateSharedHandle(nullptr, access, nullptr, &handle) : resource->GetSharedHandle(&handle));
    ComPtr<ID3D11Device1> other1;
    CHECK(other.As(&other1));
    CHECK(nt ? other1->OpenSharedResource1(handle, IID_PPV_ARGS(&imported))
             : other->OpenSharedResource(handle, IID_PPV_ARGS(&imported)));
    if (nt)
      CloseHandle(handle);
    auto readable = desc;
    readable.MiscFlags = readable.BindFlags = 0;
    readable.Usage = D3D11_USAGE_STAGING;
    readable.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&readable, nullptr, &staging));
    CHECK(other->CreateTexture2D(&readable, nullptr, &other_staging));
    ComPtr<IDXGIKeyedMutex> mutex, peer;
    CHECK(texture.As(&mutex));
    CHECK(imported.As(&peer));
    for (UINT64 key : {UINT64(0), std::numeric_limits<UINT64>::max()}) {
      expect(mutex->ReleaseSync(key) == DXGI_ERROR_INVALID_CALL, "unowned release accepted key %llu", key);
      expect(peer->ReleaseSync(key) == DXGI_ERROR_INVALID_CALL, "peer unowned release accepted key %llu", key);
    }
    expect(mutex->AcquireSync(1, 0) == WAIT_TIMEOUT, "initial key was not zero");
    if (!expect(mutex->AcquireSync(0, 0) == S_OK, "initial acquire failed"))
      return verdict();
    for (UINT64 key : {UINT64(0), std::numeric_limits<UINT64>::max()}) {
      expect(mutex->AcquireSync(key, 0) == DXGI_ERROR_INVALID_CALL, "recursive acquire accepted key %llu", key);
      expect(peer->AcquireSync(key, 0) == WAIT_TIMEOUT, "peer acquired an owned mutex with key %llu", key);
      expect(peer->ReleaseSync(key) == DXGI_ERROR_INVALID_CALL, "non-owner released key %llu", key);
    }
    std::vector<UINT> words(width);
    for (unsigned bit = 0; bit <= std::numeric_limits<UINT64>::digits; bit++) {
      UINT64 key = bit == std::numeric_limits<UINT64>::digits ? std::numeric_limits<UINT64>::max() : UINT64(1) << bit;
      if (!(key & ~lost_key_bits))
        continue;
      step("NT handle %d, key bit %u, key %llu, two-way texture bytes", nt, bit, key);
      for (UINT i = 0; i < width; i++)
        words[i] = (bit + 1) * (i + 1);
      context->UpdateSubresource(texture.Get(), 0, nullptr, words.data(), width * sizeof(words[0]), 0);
      CHECK(mutex->ReleaseSync(key));
      if (!expect(peer->AcquireSync(0, 0) == WAIT_TIMEOUT, "zero acquired a nonzero released key"))
        ExitProcess(1);
      if (!expect(peer->AcquireSync(key, 0) == S_OK, "released key did not acquire the mutex"))
        return verdict();
      other_context->CopyResource(other_staging.Get(), imported.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      CHECK(other_context->Map(other_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      expect(!memcmp(mapped.pData, words.data(), width * sizeof(words[0])), "peer read different texture bytes");
      other_context->Unmap(other_staging.Get(), 0);
      for (auto &word : words)
        word = ~word;
      other_context->UpdateSubresource(imported.Get(), 0, nullptr, words.data(), width * sizeof(words[0]), 0);
      CHECK(peer->ReleaseSync(0));
      if (!expect(mutex->AcquireSync(0, 0) == S_OK, "zero released key did not acquire the mutex"))
        return verdict();
      context->CopyResource(staging.Get(), texture.Get());
      CHECK(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      expect(!memcmp(mapped.pData, words.data(), width * sizeof(words[0])), "owner read different returned bytes");
      context->Unmap(staging.Get(), 0);
    }
    step("NT handle %d, destroying the owner abandons its acquired mutex", nt);
    mutex.Reset();
    resource.Reset();
    texture.Reset();
    staging.Reset();
    context.Reset();
    expect(device.Reset() == 0, "owner device retained a reference");
    for (unsigned attempt = 0; attempt < 2; attempt++) {
      expect(peer->AcquireSync(0, 0) == WAIT_ABANDONED, "destroyed owner did not abandon the mutex");
      expect(peer->ReleaseSync(0) == DXGI_ERROR_INVALID_CALL, "abandoned mutex released without ownership");
    }
    peer.Reset();
    expect(imported.Reset() == 0, "keyed import retained an extra reference");
  }

  step("stop encoding before the queued keyed event is consumed");
  issuing_thread = GetCurrentThreadId();
  encoding_stopped = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  encoding_resume = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(encoding_stopped && encoding_resume, "no encoding synchronization events"))
    return verdict();
  auto image = (BYTE *)GetModuleHandleA("d3d11.dll");
  if (!expect(image != nullptr, "no d3d11 module"))
    return verdict();
  auto dos = (IMAGE_DOS_HEADER *)image;
  auto headers = (IMAGE_NT_HEADERS *)(image + dos->e_lfanew);
  auto imports = (IMAGE_IMPORT_DESCRIPTOR *)(
      image + headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  ULONG_PTR *hook = nullptr;
  for (; imports->Name; imports++) {
    auto names = (IMAGE_THUNK_DATA *)(image + imports->OriginalFirstThunk);
    auto functions = (IMAGE_THUNK_DATA *)(image + imports->FirstThunk);
    for (; names->u1.AddressOfData; names++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      auto name = (IMAGE_IMPORT_BY_NAME *)(image + names->u1.AddressOfData);
      if (!strcmp((const char *)name->Name, "MTLCommandQueue_commandBuffer"))
        hook = &functions->u1.Function;
    }
  }
  if (!expect(hook != nullptr, "no command-buffer import to pause"))
    return verdict();
  DWORD protection;
  if (!expect(VirtualProtect(hook, sizeof(*hook), PAGE_READWRITE, &protection), "cannot pause the encoder"))
    return verdict();
  command_buffer = reinterpret_cast<decltype(command_buffer)>(*hook);
  *hook = ULONG_PTR(paused_command_buffer);
  if (!expect(VirtualProtect(hook, sizeof(*hook), protection, &protection), "cannot restore import protection"))
    return verdict();
  for (bool nt : {false, true})
    for (bool release : {false, true}) {
      step("NT handle %d, queued acquire%s outlives its texture", nt, release ? " and release" : "");
      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> context;
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                              &device, nullptr, &context));
      desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | (nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE : 0);
      ComPtr<ID3D11Texture2D> texture;
      ComPtr<IDXGIKeyedMutex> mutex;
      CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
      CHECK(texture.As(&mutex));
      InterlockedExchange(&stop_encoding, 1);
      CHECK(mutex->AcquireSync(0, 0));
      if (!expect(WaitForSingleObject(encoding_stopped, INFINITE) == WAIT_OBJECT_0, "encoding did not stop"))
        ExitProcess(1);
      if (release)
        CHECK(mutex->ReleaseSync(0));
      mutex.Reset();
      expect(texture.Reset() == 0, "queued mutex work retained the texture");
      SetEvent(encoding_resume);
      const D3D11_QUERY_DESC event_desc{D3D11_QUERY_EVENT};
      ComPtr<ID3D11Query> completed;
      CHECK(device->CreateQuery(&event_desc, &completed));
      context->End(completed.Get());
      context->Flush();
      BOOL done = FALSE;
      HRESULT hr;
      while ((hr = context->GetData(completed.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
      }
      expect(hr == S_OK && done, "released texture's queued mutex work did not complete, hr %#lx", hr);
    }
  if (!expect(VirtualProtect(hook, sizeof(*hook), PAGE_READWRITE, &protection), "cannot restore the encoder"))
    return verdict();
  *hook = ULONG_PTR(command_buffer);
  if (!expect(VirtualProtect(hook, sizeof(*hook), protection, &protection), "cannot restore import protection"))
    return verdict();
  CloseHandle(encoding_stopped);
  CloseHandle(encoding_resume);

  auto module = LoadLibraryA("d3d10_1.dll");
  auto create = reinterpret_cast<decltype(&D3D10CreateDevice1)>((void *)GetProcAddress(module, "D3D10CreateDevice1"));
  if (!expect(create != nullptr, "D3D10CreateDevice1 is unavailable"))
    return verdict();
  for (bool keyed : {false, true}) {
    step("Direct3D 10 sharing, keyed %d", keyed);
    ComPtr<ID3D10Device1> device, other;
    CHECK(create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION,
                 &device));
    CHECK(
        create(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &other));
    D3D10_TEXTURE2D_DESC desc10{width, 1, 1, 1, desc.Format, {1, 0}, D3D10_USAGE_DEFAULT, D3D10_BIND_SHADER_RESOURCE, 0,
                               UINT(keyed ? D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D10_RESOURCE_MISC_SHARED)};
    ComPtr<ID3D10Texture2D> texture, imported;
    CHECK(device->CreateTexture2D(&desc10, nullptr, &texture));
    ComPtr<IDXGIResource1> resource;
    CHECK(texture.As(&resource));
    HANDLE handle = nullptr, invalid = GetCurrentProcess();
    CHECK(resource->GetSharedHandle(&handle));
    expect(resource->CreateSharedHandle(nullptr, access, nullptr, &invalid) == E_INVALIDARG && !invalid,
           "Direct3D 10 accepted NT sharing without NTHANDLE");
    CHECK(other->OpenSharedResource(handle, IID_PPV_ARGS(&imported)));
    {
      step("Direct3D 10/11 shared texture bytes, keyed %d", keyed);
      ComPtr<ID3D11Device> peer_device;
      ComPtr<ID3D11DeviceContext> peer_context;
      const D3D_FEATURE_LEVEL peer_level = D3D_FEATURE_LEVEL_10_1;
      CHECK(D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &peer_level, 1, D3D11_SDK_VERSION, &peer_device, nullptr,
          &peer_context
      ));
      ComPtr<ID3D11Texture2D> peer_texture, read11;
      CHECK(peer_device->OpenSharedResource(handle, IID_PPV_ARGS(&peer_texture)));
      auto read_desc10 = desc10;
      read_desc10.Usage = D3D10_USAGE_STAGING;
      read_desc10.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
      read_desc10.BindFlags = read_desc10.MiscFlags = 0;
      ComPtr<ID3D10Texture2D> read10;
      CHECK(device->CreateTexture2D(&read_desc10, nullptr, &read10));
      D3D11_TEXTURE2D_DESC read_desc11;
      peer_texture->GetDesc(&read_desc11);
      read_desc11.Usage = D3D11_USAGE_STAGING;
      read_desc11.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      read_desc11.BindFlags = read_desc11.MiscFlags = 0;
      CHECK(peer_device->CreateTexture2D(&read_desc11, nullptr, &read11));
      ComPtr<IDXGIKeyedMutex> lock10, lock11;
      if (keyed) {
        CHECK(texture.As(&lock10));
        CHECK(peer_texture.As(&lock11));
      }
      std::vector<UINT> words(width);
      for (UINT generation = 0; generation < 3; generation++) {
        if (keyed)
          CHECK(lock10->AcquireSync(0, INFINITE));
        for (UINT i = 0; i < width; i++)
          words[i] = (generation * width + i + 1) ^ std::numeric_limits<UINT>::max();
        device->UpdateSubresource(texture.Get(), 0, nullptr, words.data(), width * sizeof(UINT), 0);
        D3D10_QUERY_DESC event_desc10{D3D10_QUERY_EVENT};
        ComPtr<ID3D10Query> completed10;
        CHECK(device->CreateQuery(&event_desc10, &completed10));
        completed10->End();
        device->Flush();
        BOOL done = FALSE;
        HRESULT hr;
        while ((hr = completed10->GetData(&done, sizeof(done), 0)) == S_FALSE) {
        }
        expect(hr == S_OK && done, "shared D3D10 upload did not complete");
        if (keyed) {
          CHECK(lock10->ReleaseSync(1));
          CHECK(lock11->AcquireSync(1, INFINITE));
        }
        peer_context->CopyResource(read11.Get(), peer_texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped11{};
        CHECK(peer_context->Map(read11.Get(), 0, D3D11_MAP_READ, 0, &mapped11));
        expect(!memcmp(mapped11.pData, words.data(), width * sizeof(UINT)), "D3D11 read different D3D10 upload bytes");
        peer_context->Unmap(read11.Get(), 0);
        for (auto &word : words)
          word = ~word;
        peer_context->UpdateSubresource(peer_texture.Get(), 0, nullptr, words.data(), width * sizeof(UINT), 0);
        D3D11_QUERY_DESC event_desc11{D3D11_QUERY_EVENT};
        ComPtr<ID3D11Query> completed11;
        CHECK(peer_device->CreateQuery(&event_desc11, &completed11));
        peer_context->End(completed11.Get());
        peer_context->Flush();
        done = FALSE;
        while ((hr = peer_context->GetData(completed11.Get(), &done, sizeof(done), 0)) == S_FALSE) {
        }
        expect(hr == S_OK && done, "shared D3D11 upload did not complete");
        if (keyed) {
          CHECK(lock11->ReleaseSync(0));
          CHECK(lock10->AcquireSync(0, INFINITE));
        }
        device->CopyResource(read10.Get(), texture.Get());
        D3D10_MAPPED_TEXTURE2D mapped10{};
        CHECK(read10->Map(0, D3D10_MAP_READ, 0, &mapped10));
        expect(!memcmp(mapped10.pData, words.data(), width * sizeof(UINT)), "D3D10 read different D3D11 return bytes");
        read10->Unmap(0);
        if (keyed)
          CHECK(lock10->ReleaseSync(0));
      }
    }
    if (keyed) {
      ComPtr<IDXGIKeyedMutex> mutex, peer;
      CHECK(texture.As(&mutex));
      CHECK(imported.As(&peer));
      expect(mutex->ReleaseSync(0) == DXGI_ERROR_INVALID_CALL, "Direct3D 10 accepted unowned release");
      if (!expect(mutex->AcquireSync(0, 0) == S_OK, "Direct3D 10 initial acquire failed"))
        return verdict();
      expect(mutex->AcquireSync(0, 0) == DXGI_ERROR_INVALID_CALL, "Direct3D 10 accepted recursive acquire");
      step("Direct3D 10 releases to its peer");
      CHECK(mutex->ReleaseSync(std::numeric_limits<UINT64>::max()));
      step("Direct3D 10 peer acquires the released key");
      if (!expect(peer->AcquireSync(std::numeric_limits<UINT64>::max(), 0) == S_OK, "Direct3D 10 peer acquire failed"))
        return verdict();
      step("Direct3D 10 peer queues release and drops its texture");
      CHECK(peer->ReleaseSync(0));
      peer.Reset();
      expect(imported.Reset() == 0, "Direct3D 10 keyed import retained an extra reference");
      D3D10_QUERY_DESC event_desc{D3D10_QUERY_EVENT};
      ComPtr<ID3D10Query> completed;
      CHECK(other->CreateQuery(&event_desc, &completed));
      completed->End();
      other->Flush();
      BOOL done = FALSE;
      HRESULT hr;
      while ((hr = completed->GetData(&done, sizeof(done), 0)) == S_FALSE) {
      }
      expect(hr == S_OK && done, "Direct3D 10 released mutex work did not complete, hr %#lx", hr);
    }
    step("Direct3D 10 sharing, keyed %d, teardown", keyed);
    expect(imported.Reset() == 0, "Direct3D 10 import retained an extra reference");
  }
  FreeLibrary(module);
  return verdict();
}
