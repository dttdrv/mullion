// contract: callbacks follow the object's full lifetime and share its COM reference count. Microsoft Learn,
// RegisterDestructionCallback: "Registers a user-defined callback to be invoked on destruction of the object".
// UnregisterDestructionCallback removes that registration; vkd3d-proton's test_destruction_notifier_callback
// records DXGI_ERROR_INVALID_CALL for a null function and DXGI_ERROR_NOT_FOUND for an unknown ID.
#include "d3d12_test.hpp"
#include <array>
#include <thread>

static void STDMETHODCALLTYPE
called(void *data) {
  ++*static_cast<UINT *>(data);
}

int
main(int argc, char **argv) {
  const UINT seed = argc > 2 ? strtoul(argv[2], nullptr, 0) : 1;
  ComPtr<ID3D12Device> device;
  CHECK(make_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  for (UINT count : {0u, 1u, 2u, 17u}) {
    step("seed %u, buffer callbacks %u", seed, count);
    auto resource = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    if (!expect(bool(resource), "resource not created"))
      continue;
    ComPtr<ID3DDestructionNotifier> notifier, again;
    if (!expect(SUCCEEDED(resource.As(&notifier)) && SUCCEEDED(resource.As(&again)), "no destruction notifier"))
      continue;
    expect(notifier == again, "notifier identity changed");
    expect(notifier->AddRef() == 4 && notifier->Release() == 3, "notifier has a separate reference count");
    std::vector<UINT> calls(count), ids(count);
    UINT invalid = seed;
    expect(notifier->RegisterDestructionCallback(nullptr, nullptr, &invalid) == DXGI_ERROR_INVALID_CALL &&
               invalid == seed,
           "invalid callback changed its ID");
    for (UINT i = 0; i < count; i++) {
      CHECK(notifier->RegisterDestructionCallback(called, &calls[i], &ids[i]));
      for (UINT j = 0; j < i; j++)
        expect(ids[i] != ids[j], "live callbacks %u and %u share an ID", i, j);
    }
    UINT anonymous = 0;
    CHECK(notifier->RegisterDestructionCallback(called, &anonymous, nullptr));
    for (UINT i = 0; i < count; i += 2) {
      CHECK(notifier->UnregisterDestructionCallback(ids[i]));
      expect(notifier->UnregisterDestructionCallback(ids[i]) == DXGI_ERROR_NOT_FOUND, "ID removed twice");
    }
    // independent registrations race on the same notifier; each retains a reference until it has unregistered.
    std::array<UINT, 2> concurrent{};
    std::array<std::thread, 2> threads;
    std::array<HRESULT, 2> results{};
    for (UINT i = 0; i < threads.size(); i++)
      threads[i] = std::thread([&, i, retained = notifier] {
        UINT id;
        results[i] = retained->RegisterDestructionCallback(called, &concurrent[i], &id);
        if (SUCCEEDED(results[i]))
          results[i] = retained->UnregisterDestructionCallback(id);
      });
    for (auto &thread : threads)
      thread.join();
    for (auto result : results)
      expect(result == S_OK, "concurrent registration failed: %08lx", result);
    again.Reset();
    resource.Reset();
    expect(anonymous == 0, "callback ran while notifier retained the object");
    notifier.Reset();
    expect(anonymous == 1, "anonymous callback ran incorrectly");
    for (auto calls : concurrent)
      expect(calls == 0, "removed concurrent callback ran");
    for (UINT i = 0; i < count; i++)
      expect(calls[i] == i % 2, "callback %u ran %u times", i, calls[i]);
  }
  return verdict();
}
