// contract: a successful AcquireSync is ownership, and only the owner's own ReleaseSync ends it. Microsoft Learn,
// IDXGIKeyedMutex::AcquireSync: "the initial key is zero and subsequent keys are those released"; ReleaseSync:
// "Using this method, the texture owner can release the mutex to another device." an unowned release is
// DXGI_ERROR_INVALID_CALL, as Wine's Windows test_keyed_mutex observes, so a release that returns anything else is
// the releasing instance's own ownership being published. two instances of one mutex therefore take turns: the
// first releases the key the second asks for, the second releases the key the first asks for, and whichever of
// them holds the mutex at any moment must be able to release it. the state that decides this is shared between the
// instances (one mapping per mutex), so a release that clears the owner before it publishes the key it released
// lets the other instance take the *old* key in that window and have its ownership erased by the release that is
// still running. the second instance below watches for exactly that: every key it acquires it must be able to
// release. both instances bring one key - the released key and the acquired key are the same one - so the whole of
// every release, and not just the part of it before the key is published, is a window the second instance can take
// the mutex in. an erasure that happens too rarely to see at 400 turns shows up at MULLION_RACE_TURNS higher.
// there are no shaders.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-releasesync
#include "d3d11_test.hpp"
#include <atomic>
#include <cstdlib>
#include <d3d11_1.h>
#include <thread>

// long enough that only a lost turn, never a slow one, ends the loop
static const DWORD wait_ms = 2000;
// the owner's turns; every one of them is a chance for the second instance to be left without its ownership. the
// release's window is a few instructions wide, so a longer run catches it more often: MULLION_RACE_TURNS raises it
static unsigned
Turns() {
  const char *env = getenv("MULLION_RACE_TURNS");
  return env && atoi(env) > 0 ? (unsigned)atoi(env) : 20000;
}
// the key both instances bring: one key keeps the two the same, so every release is a chance for the second
static const UINT64 key = 0;

int
main() {
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  ComPtr<ID3D11Device> device, other;
  ComPtr<ID3D11DeviceContext> context, other_context;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other, nullptr,
                          &other_context));

  D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE, 0,
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> texture, imported;
  CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
  ComPtr<IDXGIResource1> resource;
  CHECK(texture.As(&resource));
  HANDLE handle = nullptr;
  CHECK(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
  ComPtr<ID3D11Device1> other1;
  CHECK(other.As(&other1));
  CHECK(other1->OpenSharedResource1(handle, IID_PPV_ARGS(&imported)));
  CloseHandle(handle);
  ComPtr<IDXGIKeyedMutex> mutex, peer;
  CHECK(texture.As(&mutex));
  CHECK(imported.As(&peer));

  step("one instance takes each key its release published while another hunts for the key it is leaving");
  std::atomic<bool> stop{false};
  std::atomic<unsigned> taken{0};
  std::atomic<bool> lost{false};
  std::atomic<unsigned long> lostKey{0}, lostHr{0};
  std::atomic<unsigned long> ownerKey{0}, ownerHr{0};
  std::atomic<unsigned long> attempts{0};
  std::thread hunter([&] {
    for (;;) {
      if (stop.load(std::memory_order_relaxed))
        break;
      attempts.fetch_add(1u, std::memory_order_relaxed);
      HRESULT hr = peer->AcquireSync(key, 0);
      if (hr == WAIT_TIMEOUT)
        continue;
      if (hr != S_OK) {
        lostHr.store((unsigned long)hr, std::memory_order_relaxed);
        lostKey.store((unsigned long)key, std::memory_order_relaxed);
        lost.store(true, std::memory_order_relaxed);
        break;
      }
      taken.fetch_add(1u, std::memory_order_relaxed);
      // the turn moves on by releasing the key, which is the key both instances bring
      hr = peer->ReleaseSync(key);
      if (hr != S_OK) {
        lostHr.store((unsigned long)hr, std::memory_order_relaxed);
        lostKey.store((unsigned long)key, std::memory_order_relaxed);
        lost.store(true, std::memory_order_relaxed);
        break;
      }
    }
  });

  const unsigned turns = Turns();
  unsigned played = 0;
  for (; played < turns; played++) {
    HRESULT hr = mutex->AcquireSync(key, wait_ms);
    if (hr != S_OK) {
      ownerKey.store((unsigned long)key, std::memory_order_relaxed);
      ownerHr.store((unsigned long)hr, std::memory_order_relaxed);
      break;
    }
    hr = mutex->ReleaseSync(key);
    if (hr != S_OK) {
      ownerKey.store((unsigned long)key, std::memory_order_relaxed);
      ownerHr.store((unsigned long)hr, std::memory_order_relaxed);
      break;
    }
    if (lost.load(std::memory_order_relaxed))
      break;
  }
  stop.store(true, std::memory_order_relaxed);
  hunter.join();

  if (lost.load(std::memory_order_relaxed))
    expect(false, "an acquired key %lu could not be released (%#lx): its ownership was erased while it held it",
           lostKey.load(std::memory_order_relaxed), lostHr.load(std::memory_order_relaxed));
  if (ownerHr.load(std::memory_order_relaxed))
    expect(false, "key %lu did not stay the owner's turn (%#lx)", ownerKey.load(std::memory_order_relaxed),
           ownerHr.load(std::memory_order_relaxed));
  expect(played == turns, "the owner's turns stopped after %u of %u", played, turns);
  // a run in which the second instance never took the mutex proves nothing about its ownership
  expect(taken.load(std::memory_order_relaxed) > 0, "the second instance never acquired the mutex");
  step("the second instance took the mutex %u times in %lu attempts over %u turns", taken.load(std::memory_order_relaxed),
       attempts.load(std::memory_order_relaxed), played);
  return verdict();
}
