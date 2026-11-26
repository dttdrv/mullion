// contract: releasing the last device reference frees the device, including command lists queued for execution.
// "the implementation must free the object"; "The method returns the new reference count."
// (Microsoft Learn, IUnknown::Release).
// https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release
// a completed copy checks the list's work; replaying it without waiting leaves its queued references at teardown.
// a deferred context holds one reference to its device, as Windows does (Wine's test_create_deferred_context), and
// still one after FinishCommandList once the list is released: the list it records next holds none.
#include "d3d11_test.hpp"
#include <d3d10.h>
#include <d3d11_1.h>
#include <cstdlib>
#include <random>

int
main(int argc, char **argv) {
  const UINT seed = argc > 1 ? strtoul(argv[1], nullptr, 0) : 18;
  std::minstd_rand next(seed);
  {
    step("a deferred context's references to its device");
    ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                            nullptr, nullptr));
    auto references = [&] {
      device->AddRef();
      return device->Release();
    };
    const ULONG before = references();
    ComPtr<ID3D11DeviceContext> deferred;
    CHECK(device->CreateDeferredContext(0, &deferred));
    expect(references() == before + 1, "a new deferred context holds %lu device references, want 1",
           references() - before);
    ComPtr<ID3D11CommandList> list;
    CHECK(deferred->FinishCommandList(FALSE, &list));
    list.Reset();
    expect(references() == before + 1, "after FinishCommandList the context holds %lu device references, want 1",
           references() - before);
    deferred.Reset();
    expect(references() == before, "the released context left %lu device references", references() - before);
  }
  for (bool ten : {false, true}) {
    for (bool flush : {false, true}) {
      step("seed %u, D3D%u, flush %u: create device and record copy", seed, ten ? 10 : 11, flush);
      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> immediate, deferred;
      const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
      CHECK(D3D11CreateDevice(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
      ));
      CHECK(device->CreateDeferredContext(0, &deferred));
      ComPtr<ID3D10Device> device10;
      if (ten) {
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3DDeviceContextState> state;
        CHECK(device.As(&device1));
        CHECK(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D10Device), nullptr,
                                              &state));
        CHECK(device.As(&device10));
      }
      const UINT value = next();
      auto source = buffer(device.Get(), sizeof(value), 0, value);
      auto destination = buffer(device.Get(), sizeof(value), 0);
      if (!expect(source && destination, "copy buffers were not created"))
        return verdict();
      D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS};
      ComPtr<ID3D11Query> statistics;
      CHECK(device->CreateQuery(&desc, &statistics));
      deferred->Begin(statistics.Get());
      deferred->CopyResource(destination.Get(), source.Get());
      deferred->End(statistics.Get());
      ComPtr<ID3D11CommandList> list;
      CHECK(deferred->FinishCommandList(FALSE, &list));
      immediate->ExecuteCommandList(list.Get(), TRUE);
      auto words = read(device.Get(), immediate.Get(), destination.Get());
      expect(words.size() == 1 && words[0] == value, "copy did not return the seeded word %u", value);

      step("seed %u, D3D%u, flush %u: replay without waiting and release device", seed, ten ? 10 : 11, flush);
      immediate->ExecuteCommandList(list.Get(), FALSE);
      if (flush)
        immediate->Flush();
      list.Reset();
      statistics.Reset();
      source.Reset();
      destination.Reset();
      deferred.Reset();
      immediate.Reset();
      IUnknown *last = ten ? static_cast<IUnknown *>(device10.Detach()) : static_cast<IUnknown *>(device.Detach());
      device.Reset();
      expect(last->Release() == 0, "device still has public references");
    }
  }
  return verdict();
}
